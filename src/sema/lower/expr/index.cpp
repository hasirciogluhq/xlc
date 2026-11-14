// Lowering: indexing and slicing (with `--safety=checked` bounds checks).

#include "sema/lower/lower.hpp"

namespace xlang::lower {

/// @invariant a bounds check exists only under `--safety=checked`; an unchecked build emits the access with no branch.
Lowerer::EP Lowerer::indexExpr(const Expr& e) {
    const std::size_t mark = preludeMark();
    EP base = expr(*e.lhs, nullptr);
    EP idx = expr(*e.rhs, nullptr);
    if (!base || !idx) {
        return nullptr;
    }
    if (lower::isUnknown(base->type)) {
        EP i = mk(hir::ExprKind::Index, lower::unknownType(), e.span);
        i->lhs = std::move(base);
        i->rhs = std::move(idx);
        return i;
    }
    if (lower::isAggregate(base->type)) {
        return userIndex(e, base->type, mark); // `operator[]`
    }
    if (!lower::isUnknown(idx->type) && idx->type.kind != TypeKind::Int) {
        err("E0708",
            e.rhs->span,
            std::format("index must be an integer, found `{}`", typeToString(idx->type)),
            "integer expected");
        return nullptr;
    }
    if (base->type.kind == TypeKind::Str) {
        // Reads one byte; checked against `len` at run time under --safety=checked. Never a place.
        EP i = mk(hir::ExprKind::Index, Type::makeInt(8, false), e.span);
        i->checked = s_.config_.safety == "checked";
        i->lvalue = false;
        i->lhs = std::move(base);
        i->rhs = std::move(idx);
        return i;
    }
    if (base->type.kind == TypeKind::Slice) {
        // A slice is `{ ptr, len }` over `elem`: check against `len`, then index `elem` at `ptr`.
        // A `[]mut T` element is a place (a write lands in the owner's storage); a `[]T` element is
        // a copy out of the view, like a `str` byte, so it is not a place.
        if (!base->type.elem) {
            return nullptr;
        }
        EP i = mk(hir::ExprKind::Index, *base->type.elem, e.span);
        i->checked = s_.config_.safety == "checked";
        i->lvalue = base->type.is_mutable && base->lvalue;
        i->readonly = base->readonly;
        i->lhs = std::move(base);
        i->rhs = std::move(idx);
        return i;
    }
    if (base->type.kind != TypeKind::Array && base->type.kind != TypeKind::Pointer) {
        err("E0709",
            e.span,
            std::format("`{}` cannot be indexed", typeToString(base->type)),
            "only arrays and pointers can be indexed");
        return nullptr;
    }
    const bool is_array = base->type.kind == TypeKind::Array;
    if (!base->type.elem) {
        return nullptr;
    }
    if (!is_array && base->type.elem->isVoid()) {
        err("E0709", e.span, "cannot index `*void`", "cast it to a typed pointer first");
        return nullptr;
    }
    if (is_array && e.rhs->kind == Expr::Kind::IntLiteral) {
        if (const auto len = arrayLength(s_, base->type); len && e.rhs->int_value >= *len) {
            err("E0701",
                e.rhs->span,
                std::format("index {} is out of bounds for `{}` (length {})",
                            e.rhs->int_value,
                            typeToString(base->type),
                            *len),
                "constant index out of range");
            return nullptr;
        }
    }
    EP i = mk(hir::ExprKind::Index, *base->type.elem, e.span);
    i->checked = is_array && s_.config_.safety == "checked";
    i->lvalue = is_array ? base->lvalue : true;
    i->readonly = is_array && base->readonly;
    i->lhs = std::move(base);
    i->rhs = std::move(idx);
    return i;
}

/// `base[lo..hi]` / `base[lo..]` / `base[..hi]` / `base[..]`: a `str` view over the bytes `lo` up
/// to `hi`, never a copy. `str` and `[u8; N]` are the bases: a `str` is already a byte view, and
/// an array of bytes borrows its storage. Both bounds are optional and default to 0 and the length.
/// Under --safety=checked the node carries `checked`, so both bounds are compared against the base
/// length at run time (`E0` at compile time only for constant bounds, below).
Lowerer::EP Lowerer::sliceExpr(const Expr& e) {
    EP base = expr(*e.lhs, nullptr);
    if (!base) {
        return nullptr;
    }
    const auto slicing_is_bytes = [](const Type& t) {
        return t.kind == TypeKind::Str || (t.kind == TypeKind::Array && t.elem &&
                                           t.elem->kind == TypeKind::Int && t.elem->bits == 8);
    };
    // Slicing yields a slice type: `str`/`[u8; N]` keeps its old `str` result for source
    // compatibility, while an array or an existing slice yields `[]T` over its element type.
    bool yields_str = false;
    Type result = Type::makeStr();
    if (!lower::isUnknown(base->type)) {
        if (slicing_is_bytes(base->type)) {
            yields_str = true;
            result = Type::makeStr();
        } else if (base->type.kind == TypeKind::Array && base->type.elem) {
            // A slice is writable only when the owner is a writable place and not `const`.
            result = Type::makeSlice(*base->type.elem, base->lvalue && !base->readonly);
        } else if (base->type.kind == TypeKind::Slice && base->type.elem) {
            result = Type::makeSlice(*base->type.elem,
                                     base->type.is_mutable && base->lvalue && !base->readonly);
        } else {
            err("E0709",
                e.span,
                std::format("`{}` cannot be sliced", typeToString(base->type)),
                "only `str`, arrays and slices can be sliced");
            return nullptr;
        }
    }
    const Type usize = Type::makeInt(0, false);
    EP lo = e.args.empty() ? nullptr : expr(*e.args[0], &usize);
    lo = lo ? coerce(std::move(lo), usize, e.args[0]->span) : nullptr;
    EP hi = e.rhs ? expr(*e.rhs, &usize) : nullptr;
    hi = hi ? coerce(std::move(hi), usize, e.rhs->span) : nullptr;
    if ((!e.args.empty() && !lo) || (e.rhs != nullptr && !hi)) {
        return nullptr;
    }
    if (lower::isUnknown(base->type)) {
        EP s = mk(hir::ExprKind::Slice, lower::unknownType(), e.span);
        s->lhs = std::move(base);
        s->rhs = std::move(hi);
        if (lo) {
            s->args.push_back(std::move(lo));
        }
        return s;
    }
    // Constant bounds on a constant-length array are a compile-time range check, like indexing.
    if (base->type.kind == TypeKind::Array) {
        if (const auto len = arrayLength(s_, base->type); len && e.rhs != nullptr &&
                                                          e.rhs->kind == Expr::Kind::IntLiteral &&
                                                          e.rhs->int_value > *len) {
            err("E0701",
                e.rhs->span,
                std::format("slice end {} is past the length {} of `{}`",
                            e.rhs->int_value,
                            *len,
                            typeToString(base->type)),
                "the end bound counts from the start");
            return nullptr;
        }
    }
    (void)yields_str;
    EP s = mk(hir::ExprKind::Slice, result, e.span);
    s->checked = s_.config_.safety == "checked";
    s->lvalue = false;
    s->rhs = std::move(hi);
    s->lhs = std::move(base);
    if (lo) {
        s->args.push_back(std::move(lo));
    }
    return s;
}

} // namespace xlang::lower
