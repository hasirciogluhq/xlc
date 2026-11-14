// Lowering: statement dispatch: `local`, `return`, `if`, `defer`, expression statements.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

void Lowerer::lowerStmt(const Stmt& s, std::vector<hir::Stmt>& out) {
    // The statements of the kinds below evaluate their expressions once, in order, before they
    // act: a `?` in them is hoisted in front. A loop condition runs every pass, so it hoists into
    // the loop body instead (`guardLoop`); a `defer` body runs later and refuses `?`.
    using K = Stmt::Kind;
    const bool hoists = std::ranges::contains(std::array{K::Local,
                                                         K::Assign,
                                                         K::Return,
                                                         K::Expr,
                                                         K::If,
                                                         K::Switch,
                                                         K::Delete,
                                                         K::ForEach,
                                                         K::ForRange},
                                              s.kind);
    std::vector<hir::Stmt> prelude;
    std::vector<hir::Stmt> body;
    const auto saved = std::exchange(prelude_, hoists ? &prelude : nullptr);
    lowerStmtBody(s, body);
    prelude_ = saved;
    std::ranges::move(prelude, std::back_inserter(out));
    std::ranges::move(body, std::back_inserter(out));
}

void Lowerer::lowerStmtBody(const Stmt& s, std::vector<hir::Stmt>& out) {
    hir::Stmt h;
    h.span = s.span;
    const Type boolean = Type::makeBool();
    switch (s.kind) {
    case Stmt::Kind::Local: {
        if (!s.names.empty()) {
            lowerTupleLocal(s, out);
            return;
        }
        Type t;
        EP init;
        if (s.has_type) {
            t = resolved(s.type);
            if (s.expr) {
                init = coerce(expr(*s.expr, &t), t, s.expr->span);
            }
        } else if (s.expr) {
            init = expr(*s.expr, nullptr);
            t = init ? init->type : lower::unknownType();
            if (init && !t.hasValue()) {
                err("E0708",
                    s.expr->span,
                    std::format("`{}` cannot be bound to a `{}` value", s.name, typeToString(t)),
                    "the expression has no value");
                init.reset();
                t = lower::unknownType();
            }
        } else {
            err("E0708",
                s.span,
                std::format("cannot infer the type of `{}`", s.name),
                "write a type (`local x: T`) or an initial value");
            t = lower::unknownType();
        }
        init = moveOut(std::move(init));
        h.kind = hir::StmtKind::Local;
        h.local = addLocal(s.name, t, false, s.span);
        rejectDroppablePlace(t, s.span, "a local");
        if (needsDrop(t)) {
            registerDrop(h.local, t, init != nullptr, s.span, out);
        }
        h.value = std::move(init);
        bindLocal(
            s.name, h.local, t, s.span); // after the initializer: `local x = x` sees the outer `x`
        break;
    }
    case Stmt::Kind::Const:
        lowerLocalConst(s);
        return;
/// @ownership `delete` drops the object through its class drop glue first, then calls the registered `free`; with no `free` role it is E0501, never a silent leak.
    case Stmt::Kind::Delete: {
        // Drop the object (when its class needs drop), then free it through the allocator.
        EP p = expr(*s.expr, nullptr);
        if (!p) {
            return;
        }
        if (p->type.kind != TypeKind::Pointer || !p->type.elem ||
            !lower::isAggregate(*p->type.elem)) {
            err("E0709",
                s.expr->span,
                std::format("`delete` needs a pointer to a struct or class, found `{}`",
                            typeToString(p->type)),
                "only `new` objects can be deleted");
            return;
        }
        const Type t = *p->type.elem;
        const Type ptr = p->type;
        const hir::LocalId tmp = hiddenLocal("del", ptr, s.span);
        hir::Stmt store;
        store.kind = hir::StmtKind::Local;
        store.span = s.span;
        store.local = tmp;
        store.value = std::move(p);
        out.push_back(std::move(store));
        if (dropsThroughPointer(t)) {
            hir::Stmt drop;
            drop.kind = hir::StmtKind::Expr;
            drop.span = s.span;
            drop.value = dropThrough(localRef(tmp, ptr, s.span), t, s.span);
            out.push_back(std::move(drop));
        }
        if (mod_.free_function.empty()) {
            err("E0501",
                s.span,
                "`delete` has no registered `free` function",
                "register one with `register(free) fn name(p: *void)`");
            return;
        }
        EP call = mk(hir::ExprKind::Call, Type::makeVoid(), s.span);
        call->text = mod_.free_function;
        EP raw = mk(hir::ExprKind::Cast, Type::makePointer(Type::makeVoid()), s.span);
        raw->lhs = localRef(tmp, ptr, s.span);
        call->args.push_back(std::move(raw));
        hir::Stmt free_stmt;
        free_stmt.kind = hir::StmtKind::Expr;
        free_stmt.span = s.span;
        free_stmt.value = std::move(call);
        out.push_back(std::move(free_stmt));
        return;
    }
    case Stmt::Kind::Assign:
        if (s.has_compound_op) {
            lowerCompound(s, out);
        } else {
            lowerAssign(*s.target, *s.expr, s.span, out);
        }
        return;
    case Stmt::Kind::Return: {
        h.kind = hir::StmtKind::Return;
        if (ret_.isNever()) {
            err("E0765", s.span, "a `-> never` function cannot return",
                "end the path with an endless loop or a `-> never` call");
            if (s.expr) {
                (void)expr(*s.expr, nullptr);
            }
            return;
        }
        if (s.expr && inferring_ && lower::isUnknown(ret_)) {
            // A closure without `->` and without a `fn(...)` context: the first `return` gives
            // it its return type (a void value is only run).
            EP v = expr(*s.expr, nullptr);
            if (!v) {
                return;
            }
            ret_ = v->type;
            if (ret_.isVoid()) {
                hir::Stmt run;
                run.kind = hir::StmtKind::Expr;
                run.span = s.span;
                run.value = std::move(v);
                out.push_back(std::move(run));
            } else {
                h.value = moveOut(std::move(v));
            }
        } else if (s.expr) {
            if (ret_.isVoid()) {
                err("E0708",
                    s.expr->span,
                    "a `void` function cannot return a value",
                    "remove the value or add `-> T`");
                (void)expr(*s.expr, nullptr);
            } else {
                h.value = moveOut(coerce(expr(*s.expr, &ret_), ret_, s.expr->span));
                if (!h.value) {
                    return;
                }
                (void)rejectStackInterface(*h.value); // reported; lowering goes on (no W0701 noise)
            }
        } else if (!ret_.isVoid() && !lower::isUnknown(ret_)) {
            err("E0708",
                s.span,
                std::format("`return` needs a value of type `{}`", typeToString(ret_)),
                "missing value");
        }
        const bool pending = std::ranges::any_of(defers_, [](const auto& f) { return !f.empty(); });
        if (pending) {
            // The returned value is computed before the deferred statements run.
            if (h.value) {
                const Type vt = h.value->type;
                const hir::LocalId id = hiddenLocal("ret", vt, s.span);
                hir::Stmt keep;
                keep.kind = hir::StmtKind::Local;
                keep.span = s.span;
                keep.local = id;
                keep.value = std::move(h.value);
                out.push_back(std::move(keep));
                EP ref = mk(hir::ExprKind::LocalRef, vt, s.span);
                ref->local = id;
                ref->lvalue = true;
                h.value = std::move(ref);
            }
            emitDefers(0, out);
        }
        break;
    }
    case Stmt::Kind::DoWhile: {
        h.kind = hir::StmtKind::While;
        h.post_test = true;
        loops_.push_back({s.label, defers_.size()});
        lowerBlock(*s.body, h.body, true);
        loops_.pop_back();
        std::vector<hir::Stmt> pre;
        EP cond = loopCondition(*s.expr, pre);
        if (!cond) {
            return;
        }
        guardLoop(h, std::move(pre), std::move(cond));
        break;
    }
    case Stmt::Kind::For:
        lowerFor(s, out);
        return;
    case Stmt::Kind::ForRange:
        lowerForRange(s, out);
        return;
    case Stmt::Kind::ForPack:
        // A pack loop is unrolled when its pack function is instantiated; reaching it here means
        // it is outside such a function (the checker reports the same uses in pack functions).
        err("E0735",
            s.expr->span,
            "`for ... in` needs a parameter pack",
            "only the parameter pack of a function can be iterated");
        return;
    case Stmt::Kind::ForEach:
        lowerForEach(s, out);
        return;
    case Stmt::Kind::Switch:
        lowerSwitch(s, out);
        return;
    case Stmt::Kind::Defer: {
        const Stmt& inner = *s.step;
        switch (inner.kind) {
        case Stmt::Kind::Return:
        case Stmt::Kind::Break:
        case Stmt::Kind::Continue:
        case Stmt::Kind::Defer:
        case Stmt::Kind::Local:
        case Stmt::Kind::Const:
            err("E0709",
                inner.span,
                "this statement cannot be deferred",
                "a `defer` runs when the block is left; it cannot return, jump, declare a variable "
                "or defer again");
            return;
        default:
            break;
        }
        std::vector<hir::Stmt> lowered;
        ++in_defer_; // a deferred body runs at block exit: a `?` in it has no place to hoist to
        lowerStmt(inner, lowered);
        --in_defer_;
        // The deferred statement runs under a flag set here, so a panic unwinding through the
        // function runs exactly the defers that were registered and not yet run.
        const hir::LocalId flag = hiddenLocal("dfr", Type::makeBool(), s.span);
        entry_flags_.push_back(flag);
        hir::Stmt arm;
        arm.kind = hir::StmtKind::Assign;
        arm.span = s.span;
        arm.target = localRef(flag, Type::makeBool(), s.span);
        arm.value = boolConst(true, s.span);
        out.push_back(std::move(arm));
        hir::Stmt guard;
        guard.kind = hir::StmtKind::If;
        guard.span = s.span;
        guard.value = localRef(flag, Type::makeBool(), s.span);
        hir::Stmt clear;
        clear.kind = hir::StmtKind::Assign;
        clear.span = s.span;
        clear.target = localRef(flag, Type::makeBool(), s.span);
        clear.value = boolConst(false, s.span);
        guard.body.push_back(std::move(clear));
        for (hir::Stmt& d : lowered) {
            guard.body.push_back(std::move(d));
        }
        unwind_entries_.push_back(lower::cloneHirStmt(guard));
        defers_.back().push_back(std::move(guard));
        return;
    }
    case Stmt::Kind::Expr: {
        if (s.expr->kind == Expr::Kind::Call && s.expr->lhs->kind == Expr::Kind::Name &&
            s.expr->lhs->text == "staticAssert" && lookupName("staticAssert") == nullptr) {
            s_.checkStaticAssertCall(*s.expr);
            return;
        }
        if (s.expr->kind == Expr::Kind::Call && s.expr->lhs->kind == Expr::Kind::Name &&
            s.expr->lhs->text == "__builtin_write") {
            lowerRawWrite(*s.expr, s.span, out);
            return;
        }
        EP v = expr(*s.expr, nullptr);
        if (!v) {
            return;
        }
        if (needsDrop(v->type)) {
            // A discarded droppable value is dropped right after the statement.
            const Type t = v->type;
            const hir::LocalId tmp = hiddenLocal("disc", t, s.span);
            hir::Stmt store;
            store.kind = hir::StmtKind::Local;
            store.span = s.span;
            store.local = tmp;
            store.value = std::move(v);
            out.push_back(std::move(store));
            EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(t), s.span);
            addr->lhs = localRef(tmp, t, s.span);
            v = dropCallOn(std::move(addr), t, s.span);
        }
        h.kind = hir::StmtKind::Expr;
        h.value = std::move(v);
        break;
    }
    case Stmt::Kind::If: {
        if (const std::optional<bool> fixed = dropTest(*s.expr)) {
            // `if __builtin_needsDrop<T>() { } else { }`: only the branch that runs is lowered, so
            // each branch may use what only that kind of `T` has (`p.clone()` on a class).
            h.kind = hir::StmtKind::Block;
            if (*fixed) {
                lowerBlock(*s.body, h.body, true);
            } else if (s.else_body) {
                lowerBlock(*s.else_body, h.body, true);
            }
            break;
        }
        h.kind = hir::StmtKind::If;
        h.value = coerce(expr(*s.expr, &boolean), boolean, s.expr->span);
        lowerBlock(*s.body, h.body, true);
        if (s.else_body) {
            lowerBlock(*s.else_body, h.else_body, true);
        }
        if (!h.value) {
            return;
        }
        break;
    }
    case Stmt::Kind::While: {
        h.kind = hir::StmtKind::While;
        std::vector<hir::Stmt> pre;
        EP cond = loopCondition(*s.expr, pre);
        loops_.push_back({s.label, defers_.size()});
        lowerBlock(*s.body, h.body, true);
        loops_.pop_back();
        if (!cond) {
            return;
        }
        guardLoop(h, std::move(pre), std::move(cond));
        break;
    }
    case Stmt::Kind::Continue:
    case Stmt::Kind::Break:
        lowerJump(s, out);
        return;
    case Stmt::Kind::Try:
        lowerTry(s, out);
        return;
    case Stmt::Kind::Unsafe:
        ++unsafe_;
        h.kind = hir::StmtKind::Block;
        lowerBlock(*s.body, h.body, true);
        --unsafe_;
        break;
    case Stmt::Kind::Block:
        h.kind = hir::StmtKind::Block;
        lowerBlock(*s.body, h.body, true);
        break;
    }
    out.push_back(std::move(h));
}

} // namespace xlang::lower
