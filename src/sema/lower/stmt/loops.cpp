// Lowering: `break`/`continue`, `while`, C-style `for`, `for i in lo..hi` and `for x, i in s`.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

void Lowerer::lowerJump(const Stmt& s, std::vector<hir::Stmt>& out) {
    const bool is_break = s.kind == Stmt::Kind::Break;
    const char* word = is_break ? "break" : "continue";
    if (loops_.empty()) {
        err("E0709",
            s.span,
            std::format("`{}` outside of a loop", word),
            "there is no enclosing loop");
        return;
    }
    std::size_t target = loops_.size() - 1;
    if (!s.name.empty()) {
        bool found = false;
        for (std::size_t i = loops_.size(); i-- > 0;) {
            if (loops_[i].label == s.name) {
                target = i;
                found = true;
                break;
            }
        }
        if (!found) {
            err("E0709",
                s.span,
                std::format("no enclosing loop is labeled `{}`", s.name),
                "write `label: while ...` on the loop first");
            return;
        }
    }
    emitDefers(loops_[target].defer_mark, out);
    hir::Stmt h;
    h.span = s.span;
    h.kind = is_break ? hir::StmtKind::Break : hir::StmtKind::Continue;
    h.depth = static_cast<std::uint32_t>(loops_.size() - 1 - target);
    out.push_back(std::move(h));
}

void Lowerer::lowerFor(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back(); // the header scope (no defers can land here, but keeps frames aligned)
    if (s.init) {
        lowerStmt(*s.init, block.body);
    }
    hir::Stmt loop;
    loop.kind = hir::StmtKind::While;
    loop.span = s.span;
    std::vector<hir::Stmt> pre;
    EP cond = s.expr ? loopCondition(*s.expr, pre) : boolConst(true, s.span);
    loops_.push_back({s.label, defers_.size()});
    lowerBlock(*s.body, loop.body, true);
    loops_.pop_back();
    if (s.step) {
        lowerStmt(*s.step, loop.step);
    }
    defers_.pop_back();
    scopes_.pop();
    if (!cond) {
        return;
    }
    guardLoop(loop, std::move(pre), std::move(cond));
    block.body.push_back(std::move(loop));
    out.push_back(std::move(block));
}

std::optional<bool> Lowerer::dropTest(const Expr& cond) {
    const bool negated = cond.kind == Expr::Kind::Unary && cond.unary_op == UnaryOp::Not && cond.lhs;
    const Expr& call = negated ? *cond.lhs : cond;
    if (call.kind != Expr::Kind::Call || !call.lhs || call.lhs->kind != Expr::Kind::Name ||
        call.lhs->text != "__builtin_needsDrop") {
        return std::nullopt;
    }
    const EP v = expr(call, nullptr);
    if (!v || v->kind != hir::ExprKind::BoolConst) {
        return std::nullopt;
    }
    return v->bool_value != negated;
}

Lowerer::EP Lowerer::loopCondition(const Expr& cond, std::vector<hir::Stmt>& pre) {
    const Type boolean = Type::makeBool();
    const auto saved = std::exchange(prelude_, in_defer_ != 0 ? prelude_ : &pre);
    EP out = coerce(expr(cond, &boolean), boolean, cond.span);
    prelude_ = saved;
    return out;
}

void Lowerer::guardLoop(hir::Stmt& loop, std::vector<hir::Stmt> pre, EP cond) {
    if (pre.empty()) {
        loop.value = std::move(cond);
        return;
    }
    hir::Stmt leave;
    leave.kind = hir::StmtKind::If;
    leave.span = loop.span;
    EP n = mk(hir::ExprKind::Unary, Type::makeBool(), loop.span);
    n->unary_op = UnaryOp::Not;
    n->lhs = std::move(cond);
    leave.value = std::move(n);
    hir::Stmt brk;
    brk.kind = hir::StmtKind::Break;
    brk.span = loop.span;
    leave.body.push_back(std::move(brk));
    pre.push_back(std::move(leave));
    loop.value = boolConst(true, loop.span);
    if (loop.post_test) {
        // `do { } while c`: the test follows the body, and `continue` reaches it through the step
        // (a `do` loop has no step of its own). A `break` in the step leaves the loop.
        std::ranges::move(pre, std::back_inserter(loop.step));
        loop.post_test = false;
        // The first pass must not test: `while true` with the test in the step runs the body
        // first, exactly like the post-test loop.
        return;
    }
    std::ranges::move(loop.body, std::back_inserter(pre));
    loop.body = std::move(pre);
}

void Lowerer::lowerForRange(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    // An untyped integer literal on either side takes its type from the other bound, so `0..n`
    // works for any integer type of `n` without a written `as`. A typed bound wins; when both are
    // literals the lower one's default type (i32, or i64 when it does not fit) is used.
    const bool lo_literal = s.expr->kind == Expr::Kind::IntLiteral;
    const bool hi_literal = s.expr2->kind == Expr::Kind::IntLiteral;
    Type hint;
    bool have_hint = false;
    if (lo_literal && !hi_literal) {
        EP probe = expr(*s.expr2, nullptr);
        if (probe && !lower::isUnknown(probe->type) && probe->type.kind == TypeKind::Int) {
            hint = probe->type;
            have_hint = true;
        }
    }
    EP lo = expr(*s.expr, have_hint ? &hint : nullptr);
    if (!lo) {
        (void)expr(*s.expr2, nullptr);
        return;
    }
    const Type t = lo->type;
    EP hi = expr(*s.expr2, &t);
    if (!lower::isUnknown(t) && t.kind != TypeKind::Int) {
        err("E0708",
            s.expr->span,
            std::format("a range needs integer bounds, found `{}`", typeToString(t)),
            "write `for i in 0..n`");
        return;
    }
    hi = hi ? coerce(std::move(hi), t, s.expr2->span) : nullptr;
    if (!hi || lower::isUnknown(t)) {
        return;
    }
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();

    const hir::LocalId var = addLocal(s.name, t, false, s.span);
    const hir::LocalId end = hiddenLocal("end", t, s.span);
    {
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = var;
        d.value = std::move(lo);
        block.body.push_back(std::move(d));
        hir::Stmt e;
        e.kind = hir::StmtKind::Local;
        e.span = s.span;
        e.local = end;
        e.value = std::move(hi);
        block.body.push_back(std::move(e));
    }
    bindLocal(s.name, var, t, s.span);

    const auto ref = [&](hir::LocalId id) {
        EP r = mk(hir::ExprKind::LocalRef, t, s.span);
        r->local = id;
        r->lvalue = true;
        return r;
    };
    hir::Stmt loop;
    loop.kind = hir::StmtKind::While;
    loop.span = s.span;
    EP cond = mk(hir::ExprKind::Binary, boolean, s.span);
    // `in a..b` runs while `i < b`; `in a..=b` runs while `i <= b`.
    cond->binary_op = s.range_inclusive ? BinaryOp::Le : BinaryOp::Lt;
    cond->lhs = ref(var);
    cond->rhs = ref(end);
    loop.value = std::move(cond);
    loops_.push_back({s.label, defers_.size()});
    lowerBlock(*s.body, loop.body, true);
    loops_.pop_back();
    {
        hir::Stmt step;
        step.kind = hir::StmtKind::Assign;
        step.span = s.span;
        step.target = ref(var);
        EP add = mk(hir::ExprKind::Binary, t, s.span);
        add->binary_op = BinaryOp::Add;
        add->lhs = ref(var);
        add->rhs = intConst(1, t, s.span);
        step.value = std::move(add);
        loop.step.push_back(std::move(step));
    }
    defers_.pop_back();
    scopes_.pop();
    block.body.push_back(std::move(loop));
    out.push_back(std::move(block));
}

/// `for x in s { }` / `for x, i in s { }`: a compile-time lowering to the index loop a caller
/// writes by hand, with no vtable and no `Iter` protocol. The iterable is an array, a slice or a
/// `str`; the element is a pointer into the storage (`*T`, so nothing is copied and a write reaches
/// the owner) and the index is a `usize`. The bound is read once. `Vec<T>` stays stdlib: it is
/// iterated once it can hand out a slice, so the compiler never names a runtime type (CLAUDE.md,
/// compiler/runtime).
void Lowerer::lowerForEach(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    const Type usize = Type::makeInt(0, false);
    // A `for x in <name>` whose iterable is a bare name that names nothing is the leftover of a
    // pack loop that was not unrolled (the pack rewrite happens before lowering); keep the old
    // `E0735`.
    if (s.expr->kind == Expr::Kind::Name && lookupName(s.expr->text) == nullptr &&
        !s_.config_.lookup(s.expr->text)) {
        err("E0735",
            s.expr->span,
            "`for ... in` needs a parameter pack",
            "only the parameter pack of a function can be iterated");
        return;
    }
    EP it = expr(*s.expr, nullptr);
    if (!it) {
        return;
    }
    if (lower::isUnknown(it->type)) {
        return;
    }
    const Type iterable = it->type;
    if (lower::isAggregate(iterable) ||
        (iterable.kind == TypeKind::Pointer && iterable.elem && lower::isAggregate(*iterable.elem))) {
        lowerIterate(s, std::move(it), out); // `next` / `iter`: the iterator protocol
        return;
    }

    const bool is_array = iterable.kind == TypeKind::Array;
    const bool is_view = iterable.kind == TypeKind::Str || iterable.kind == TypeKind::Slice;
    if (!is_array && !is_view) {
        err("E0709",
            s.expr->span,
            std::format("`{}` cannot be iterated with `for ... in`", typeToString(iterable)),
            "iterate an array, a slice or `str`; a range is `for i in a..b`");
        return;
    }
    Type elem_type = Type::makeInt(8, false); // a `str` walks bytes
    if (iterable.elem) {
        elem_type = *iterable.elem;
    }
    // A `str` is read-only, so its loop variable is the byte itself (`u8`); an array or a slice
    // yields the element's address (`*T`), so a write through it reaches the owner.
    const bool by_value = iterable.kind == TypeKind::Str;
    const Type elem_ptr = by_value ? elem_type : Type::makePointer(elem_type);

    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();

    const hir::LocalId idx = hiddenLocal("i", usize, s.span);
    const hir::LocalId end = hiddenLocal("n", usize, s.span);
    // An array is *not* copied: it is indexed through its address, so a write reaches the owner.
    // A slice or `str` is a two-word view, so copying it is correct and keeps the code simple.
    hir::LocalId src{};
    if (is_array) {
        src = hiddenLocal("arrp", Type::makePointer(iterable), s.span);
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(iterable), s.span);
        addr->lhs = std::move(it);
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = src;
        d.value = std::move(addr);
        block.body.push_back(std::move(d));
    } else {
        src = hiddenLocal("seq", iterable, s.span);
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = src;
        d.value = std::move(it);
        block.body.push_back(std::move(d));
    }
    const auto ref = [&](hir::LocalId id, const Type& t) {
        EP r = mk(hir::ExprKind::LocalRef, t, s.span);
        r->local = id;
        r->lvalue = true;
        return r;
    };
    // The base the index reads through: for an array it is `(*arrp)`, a place of the array type so
    // that `[i]` yields one element (not a copy of the whole array); otherwise the copied view.
    const auto baseRef = [&]() -> EP {
        if (!is_array) {
            return ref(src, iterable);
        }
        EP deref = mk(hir::ExprKind::Deref, iterable, s.span);
        deref->lhs = ref(src, Type::makePointer(iterable));
        deref->lvalue = true;
        return deref;
    };
    // `end`: a constant for an array, `len` for a view.
    {
        EP len;
        if (is_array) {
            const auto n = arrayLength(s_, iterable);
            len = intConst(n.value_or(0), usize, s.span);
        } else {
            EP field = mk(hir::ExprKind::Field, usize, s.span);
            field->text = "len";
            field->index = 1;
            field->lhs = baseRef();
            len = std::move(field);
        }
        hir::Stmt e;
        e.kind = hir::StmtKind::Local;
        e.span = s.span;
        e.local = end;
        e.value = std::move(len);
        block.body.push_back(std::move(e));
    }
    {
        hir::Stmt z;
        z.kind = hir::StmtKind::Local;
        z.span = s.span;
        z.local = idx;
        z.value = intConst(0, usize, s.span);
        block.body.push_back(std::move(z));
    }

    hir::Stmt loop;
    loop.kind = hir::StmtKind::While;
    loop.span = s.span;
    EP cond = mk(hir::ExprKind::Binary, boolean, s.span);
    cond->binary_op = BinaryOp::Lt;
    cond->lhs = ref(idx, usize);
    cond->rhs = ref(end, usize);
    loop.value = std::move(cond);

    // The body is wrapped so the element and the index names live only inside the loop: their
    // declarations come first, then the user's block, then the index step.
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();
    const hir::LocalId elem = addLocal(s.name, elem_ptr, false, s.span);
    {
        EP index = mk(hir::ExprKind::Index, elem_type, s.span);
        index->checked = s_.config_.safety == "checked";
        index->lvalue = true;
        index->lhs = baseRef();
        index->rhs = ref(idx, usize);
        EP value;
        if (by_value) {
            value = std::move(index);
        } else {
            value = mk(hir::ExprKind::AddrOf, elem_ptr, s.span);
            value->lhs = std::move(index);
        }
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = elem;
        d.value = std::move(value);
        loop.body.push_back(std::move(d));
    }
    bindLocal(s.name, elem, elem_ptr, s.span);
    if (!s.names.empty()) {
        const hir::LocalId ivid = addLocal(s.names[0], usize, false, s.span);
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = ivid;
        d.value = ref(idx, usize);
        loop.body.push_back(std::move(d));
        bindLocal(s.names[0], ivid, usize, s.span);
    }
    loops_.push_back({s.label, defers_.size()});
    lowerBlock(*s.body, loop.body, true);
    loops_.pop_back();
    defers_.pop_back();
    scopes_.pop();
    {
        hir::Stmt step;
        step.kind = hir::StmtKind::Assign;
        step.span = s.span;
        step.target = ref(idx, usize);
        EP add = mk(hir::ExprKind::Binary, usize, s.span);
        add->binary_op = BinaryOp::Add;
        add->lhs = ref(idx, usize);
        add->rhs = intConst(1, usize, s.span);
        step.value = std::move(add);
        loop.step.push_back(std::move(step));
    }
    defers_.pop_back();
    scopes_.pop();
    block.body.push_back(std::move(loop));
    out.push_back(std::move(block));
}

} // namespace xlang::lower
