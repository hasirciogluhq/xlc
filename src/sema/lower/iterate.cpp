#include "sema/lower/lower.hpp"

namespace xlang::lower {

// Methods of structs, and the iterator protocol of `for x in value`.
//
// A struct has no methods of its own: `impl T { fn name(self: *T, ...) }` makes the function
// `T.name` (see parser), and `value.name(...)` on a struct that has no field and no class method
// of that name calls it. The compiler finds `iter` and `next` by that rule alone; it knows no
// library type.
//
// `for x in value` over a struct or class value:
//
//   - `next(self: *I) -> Option<T>` alone: the value is its own iterator. The loop calls `next` on
//     the value itself (through its address), so a second `for` over the same variable goes on
//     where the first one stopped.
//   - `iter(self: *C) -> I` and `next` on `I`: every loop takes a new iterator from the value and
//     starts from the beginning. `iter` wins when the value has both.
//   - `iter` alone does not iterate: there is nothing to call in the loop (E0776).
//
// `next` returns an enum with two variants, one carrying the item and one without data (`Some(T)`
// / `None`, in either order); the variant without data ends the loop. `x` is the item itself.
//
//   { local $src = &value                      // the place, or a temporary kept alive
//     local $it  = $src.iter()                 // only with `iter`
//     while true {
//         local $n = $it.next()
//         if $n is the end { break }
//         local x = $n.item
//         ...body...
//     } }

namespace {

[[nodiscard]] ExprPtr deref(ExprPtr place, const Span& span) {
    return astUnary(UnaryOp::Deref, std::move(place), span);
}

} // namespace

std::string Lowerer::methodFunctionName(const Type& agg, const std::string& name) {
    std::string base = agg.path.empty() ? std::string() : agg.path.front();
    base = base.substr(0, base.find('<'));
    if (const std::size_t cut = base.rfind("::"); cut != std::string::npos) {
        base.erase(0, cut + 2);
    }
    return base + "." + name;
}

bool Lowerer::hasMethod(const Type& agg, const std::string& name) {
    return (agg.kind == TypeKind::Class && findMethod(agg, name)) ||
           hasOperator(methodFunctionName(agg, name), 1, {agg});
}

Lowerer::EP Lowerer::callMethod(ExprPtr receiver,
                                const Type& agg,
                                const std::string& name,
                                std::vector<ExprPtr> rest,
                                const Span& span,
                                bool& found) {
    const std::string fn = agg.kind == TypeKind::Class && findMethod(agg, name)
                               ? name
                               : methodFunctionName(agg, name);
    return operatorCall(fn, std::move(receiver), false, std::move(rest), {agg}, span, found);
}

void Lowerer::lowerIterate(const Stmt& s, EP value, std::vector<hir::Stmt>& out) {
    const Span span = s.span;
    const bool through = value->type.kind == TypeKind::Pointer && value->type.elem != nullptr;
    const Type owner = through ? *value->type.elem : value->type;
    const std::string shown = typeToString(owner);
    if (!isAggregate(owner)) {
        err("E0709",
            s.expr->span,
            std::format("`{}` cannot be iterated with `for ... in`", typeToString(value->type)),
            "iterate an array, a slice, `str` or a type with `next` / `iter`; a range is "
            "`for i in a..b`");
        return;
    }
    const bool has_iter = hasMethod(owner, "iter");
    const bool has_next = hasMethod(owner, "next");
    if (!has_iter && !has_next) {
        err("E0775",
            s.expr->span,
            std::format("`{}` cannot be iterated: it has neither `next` nor `iter`", shown),
            std::format("give it `fn next(self: *{0}) -> Option<T>` (it is its own iterator), or "
                        "`fn iter(self: *{0}) -> I` where `I` has `next`",
                        shown));
        return;
    }

    // Where the iterable lives: a pointer to its place, or to a temporary that lives to the end of
    // the loop.
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();
    const auto leave = [this] {
        defers_.pop_back();
        scopes_.pop();
    };
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = span;
    const auto declare = [&](hir::LocalId id, EP init, std::vector<hir::Stmt>& into) {
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = span;
        d.local = id;
        d.value = std::move(init);
        into.push_back(std::move(d));
    };
    const auto bindHidden = [&](const char* prefix, const Type& t) {
        const hir::LocalId id = hiddenLocal(prefix, t, span);
        const std::string name = fn_->locals[id].name;
        bindLocal(name, id, t, span);
        return std::pair{id, name};
    };
    const Type owner_ptr = Type::makePointer(owner);
    EP address;
    if (through) {
        address = std::move(value);
    } else {
        if (!value->lvalue) {
            if (needsDrop(owner)) {
                value = materializeTemp(std::move(value), span);
            } else {
                const auto [tmp, tmp_name] = bindHidden("iterable", owner);
                declare(tmp, std::move(value), block.body);
                value = localRef(tmp, owner, span);
                value->lvalue = true;
            }
        }
        address = mk(hir::ExprKind::AddrOf, owner_ptr, span);
        address->lhs = std::move(value);
    }
    const auto [src_id, src_name] = bindHidden("src", owner_ptr);
    declare(src_id, std::move(address), block.body);
    const auto source = [&] { return deref(astName(src_name, span), span); };

    // The iterator: the value itself, or what `iter` returns.
    Type iterator = owner;
    std::function<ExprPtr()> receiver = source;
    if (has_iter) {
        bool found = false;
        EP made = callMethod(source(), owner, "iter", {}, span, found);
        if (!made || lower::isUnknown(made->type)) {
            leave();
            return;
        }
        const bool by_pointer = made->type.kind == TypeKind::Pointer && made->type.elem != nullptr;
        iterator = by_pointer ? *made->type.elem : made->type;
        if (!isAggregate(iterator)) {
            err("E0776",
                s.expr->span,
                std::format("`iter` of `{}` returns `{}`, which is not an iterator", shown,
                            typeToString(made->type)),
                "`iter` returns a struct or class that has `next`");
            leave();
            return;
        }
        if (!hasMethod(iterator, "next")) {
            err("E0776",
                s.expr->span,
                std::format("`iter` of `{}` returns `{}`, which has no `next`",
                            shown,
                            typeToString(iterator)),
                std::format("add `fn next(self: *{0}) -> Option<T>` to `{0}`; `for` calls it until "
                            "it returns the variant without data",
                            typeToString(iterator)));
            leave();
            return;
        }
        if (has_next && typeToString(iterator) != shown) {
            s_.diag_.warning(
                "W0771",
                s.expr->span,
                std::format("`{}` has both `iter` and `next`: `for` uses `iter`", shown),
                "every loop starts from the beginning with a new iterator, so the position that "
                "`next` keeps is not used here; call `next` yourself to continue a partial walk");
        }
        const auto [it_id, it_name] = bindHidden("iter", made->type);
        declare(it_id, moveOut(std::move(made)), block.body);
        receiver = by_pointer ? std::function<ExprPtr()>([=] { return deref(astName(it_name, span), span); })
                              : std::function<ExprPtr()>([=] { return astName(it_name, span); });
    }

    // `next` that takes `self` by value works on a copy, so the loop would never advance.
    if (iterator.kind != TypeKind::Class) {
        const auto entries = operatorEntries(methodFunctionName(iterator, "next"), {iterator}, span);
        const bool by_value = std::ranges::any_of(entries, [&](const ScopeEntry& entry) {
            return std::ranges::any_of(candidatesOf(entry), [&](const OverloadCandidate& c) {
                return c.params.size() == 1 && c.params.front().kind != TypeKind::Pointer;
            });
        });
        if (by_value) {
            s_.diag_.warning(
                "W0770",
                s.expr->span,
                std::format("`next` of `{}` takes `self` by value: every call works on a copy",
                            typeToString(iterator)),
                std::format("the position never advances and the loop does not end; write "
                            "`fn next(self: *{})`",
                            typeToString(iterator)));
        }
    }

    // The loop.
    const Type usize = Type::makeInt(0, false);
    hir::LocalId index_id{};
    if (!s.names.empty()) {
        index_id = hiddenLocal("n", usize, span);
        declare(index_id, intConst(0, usize, span), block.body);
    }
    hir::Stmt loop;
    loop.kind = hir::StmtKind::While;
    loop.span = span;
    loop.value = boolConst(true, span);
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();
    const auto leaveLoop = [this, &leave] {
        defers_.pop_back();
        scopes_.pop();
        leave();
    };
    bool found = false;
    EP next = callMethod(receiver(), iterator, "next", {}, span, found);
    if (!next || lower::isUnknown(next->type)) {
        leaveLoop();
        return;
    }
    const auto sum = sumOfType(next->type);
    std::optional<std::size_t> item_variant;
    std::optional<std::size_t> end_variant;
    if (sum && sum->decl->variants.size() == 2) {
        for (std::size_t i = 0; i < 2; ++i) {
            const std::size_t payload = sum->decl->variants[i].payload.size();
            if (payload == 1) {
                item_variant = i;
            } else if (payload == 0) {
                end_variant = i;
            }
        }
    }
    if (!item_variant || !end_variant) {
        err("E0777",
            s.expr->span,
            std::format("`next` of `{}` returns `{}`, which cannot end a loop",
                        typeToString(iterator),
                        typeToString(next->type)),
            "`next` returns an enum with two variants: one carrying the item and one without data "
            "(`Some(T)` / `None`); a `Result` has no end to stop at");
        leaveLoop();
        return;
    }
    const Type held_type = next->type;
    const hir::LocalId held = hiddenLocal("next", held_type, span);
    declare(held, moveOut(std::move(next)), loop.body);

    // if ($next is the end) { break }
    {
        EP tag = mk(hir::ExprKind::Field, sumPart(*sum, "$Tag", span), span);
        tag->text = "tag";
        tag->index = 0;
        tag->lvalue = true;
        tag->lhs = localRef(held, held_type, span);
        tag->lhs->lvalue = true;
        EP at_end = mk(hir::ExprKind::Binary, Type::makeBool(), span);
        at_end->binary_op = BinaryOp::Eq;
        at_end->lhs = std::move(tag);
        at_end->rhs = intConst(*end_variant, sumPart(*sum, "$Tag", span), span);
        hir::Stmt stop;
        stop.kind = hir::StmtKind::If;
        stop.span = span;
        stop.value = std::move(at_end);
        hir::Stmt brk;
        brk.kind = hir::StmtKind::Break;
        brk.span = span;
        stop.body.push_back(std::move(brk));
        loop.body.push_back(std::move(stop));
    }

    // local x = $next.item
    EP item = moveOut(sumPayloadRead(*sum, held, held_type, *item_variant, 0, span), true);
    const Type item_type = item->type;
    const hir::LocalId elem = addLocal(s.name, item_type, false, span);
    if (needsDrop(item_type)) {
        registerDrop(elem, item_type, true, span, loop.body);
    }
    declare(elem, std::move(item), loop.body);
    bindLocal(s.name, elem, item_type, span);
    if (!s.names.empty()) { // `for x, i in value`: the index of the item
        const hir::LocalId shown_index = addLocal(s.names[0], usize, false, span);
        declare(shown_index, localRef(index_id, usize, span), loop.body);
        bindLocal(s.names[0], shown_index, usize, span);
        hir::Stmt step;
        step.kind = hir::StmtKind::Assign;
        step.span = span;
        step.target = localRef(index_id, usize, span);
        EP add = mk(hir::ExprKind::Binary, usize, span);
        add->binary_op = BinaryOp::Add;
        add->lhs = localRef(index_id, usize, span);
        add->rhs = intConst(1, usize, span);
        step.value = std::move(add);
        loop.step.push_back(std::move(step));
    }
    loops_.push_back({s.label, defers_.size()});
    lowerBlock(*s.body, loop.body, true);
    loops_.pop_back();
    leaveLoop();
    block.body.push_back(std::move(loop));
    out.push_back(std::move(block));
}

} // namespace xlang::lower
