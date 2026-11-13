// Lowering: conversions: `as`, implicit coercions, interface boxing and the stack-interface check.

#include "sema/lower/lower.hpp"

namespace xlang::lower {

// Expressions, calls, overload resolution and conversions.

// ---------------------------------------------------------------------------------------------
// Conversions

bool Lowerer::castable(const Type& from, const Type& to) const {
    if (lower::isUnknown(from) || lower::isUnknown(to) || lower::sameType(from, to)) {
        return true;
    }
    const auto scalar = [](const Type& t) {
        return t.kind == TypeKind::Int || t.kind == TypeKind::Float || t.kind == TypeKind::Bool;
    };
    const auto addr = [](const Type& t) {
        return t.kind == TypeKind::Pointer || t.kind == TypeKind::FnPtr;
    };
    if (from.kind == TypeKind::Enum && from.elem) {
        return castable(*from.elem, to);
    }
    if (to.kind == TypeKind::Enum && to.elem) {
        return castable(from, *to.elem);
    }
    if (scalar(from) && scalar(to)) {
        return true;
    }
    if (addr(from) && addr(to)) {
        return true;
    }
    if ((addr(from) && to.kind == TypeKind::Int) || (from.kind == TypeKind::Int && addr(to))) {
        return true;
    }
    return false;
}

Lowerer::EP Lowerer::castTo(EP value, const Type& to, bool implicit) {
    if (!value || lower::sameType(value->type, to)) {
        return value;
    }
    EP c = mk(hir::ExprKind::Cast, to, value->span);
    c->implicit = implicit;
    c->lhs = std::move(value);
    return c;
}

Lowerer::EP Lowerer::toInterface(EP value, const Type& iface, const Span& at) {
    // `value` is a struct/class value (an lvalue) or a pointer to one.
    const std::string iface_q = iface.path.empty() ? std::string{} : iface.path.front();
    EP pointer;
    std::string type_q;
    if (value->type.kind == TypeKind::Pointer && value->type.elem &&
        lower::isAggregate(*value->type.elem)) {
        type_q = value->type.elem->path.front();
        pointer = std::move(value);
    } else {
        type_q = value->type.path.front();
        if (!value->lvalue) {
            err("E0709",
                at,
                "cannot convert a temporary to an interface",
                "the interface value points at the object; store it in a variable first");
            return nullptr;
        }
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(value->type), at);
        addr->lhs = std::move(value);
        pointer = std::move(addr);
    }
    if (findImpl(type_q, iface_q) == nullptr) {
        Diagnostic d;
        d.code = "E0504";
        d.severity = Severity::Error;
        d.message = std::format("`{}` does not implement `{}`", type_q, iface_q);
        d.labels.push_back({at, std::format("`{}` -> `{}` needs an `impl`", type_q, iface_q)});
        d.detail = std::format("No `impl {} for {}` was found.", iface_q, type_q);
        d.reason = "Interfaces are nominal; matching method names never count.";
        d.fix = std::format("add `impl {} for {} {{ ... }}`", iface_q, type_q);
        s_.diag_.report(std::move(d));
        return nullptr;
    }
    EP out = mk(hir::ExprKind::ToInterface, iface, at);
    out->text = type_q;
    out->lhs = std::move(pointer);
    return out;
}

namespace {

/// True when the place `e` lives in the current frame: a local or parameter, a temporary
/// materialized for a droppable value, or a field/element of one (not reached through a pointer).
[[nodiscard]] bool onStack(const hir::Expr& e) {
    switch (e.kind) {
    case hir::ExprKind::LocalRef:
    case hir::ExprKind::Materialize: return true;
    case hir::ExprKind::Field: return !e.through_pointer && e.lhs && onStack(*e.lhs);
    case hir::ExprKind::Index:
        return e.lhs && e.lhs->type.kind == TypeKind::Array && onStack(*e.lhs);
    default: return false;
    }
}

/// The first `ToInterface` under `e` (through casts, selects, tuples and struct literals) whose
/// object is on the stack, or null.
[[nodiscard]] const hir::Expr* stackInterface(const hir::Expr& e) {
    const auto first = [](const auto& range, auto&& proj) -> const hir::Expr* {
        for (const auto& item : range) {
            const hir::Expr* part = proj(item);
            if (const hir::Expr* sub = part ? stackInterface(*part) : nullptr) {
                return sub;
            }
        }
        return nullptr;
    };
    const auto id = [](const hir::ExprPtr& p) -> const hir::Expr* { return p.get(); };
    switch (e.kind) {
    case hir::ExprKind::ToInterface: {
        const hir::Expr* obj = e.lhs.get();
        return obj && obj->kind == hir::ExprKind::AddrOf && obj->lhs && onStack(*obj->lhs) ? &e
                                                                                          : nullptr;
    }
    case hir::ExprKind::Cast: return e.lhs ? stackInterface(*e.lhs) : nullptr;
    case hir::ExprKind::Select:
        if (const hir::Expr* sub = e.rhs ? stackInterface(*e.rhs) : nullptr) {
            return sub;
        }
        return first(e.args, id);
    case hir::ExprKind::TupleLit: return first(e.args, id);
    case hir::ExprKind::StructLit:
        return first(e.fields, [](const hir::FieldInit& f) { return f.value.get(); });
    default: return nullptr;
    }
}

} // namespace

bool Lowerer::rejectStackInterface(const hir::Expr& value) {
    const hir::Expr* bad = stackInterface(value);
    if (bad == nullptr) {
        return false;
    }
    err("E0760",
        bad->span,
        std::format("cannot return a stack `{}` as `{}`: the fat pointer would dangle",
                    bad->text,
                    typeToString(bad->type)),
        "allocate the object on the heap with `new` and return that pointer");
    return true;
}

Lowerer::EP Lowerer::coerce(EP value, const Type& to, const Span& at) {
    if (!value) {
        return nullptr;
    }
    if (lower::isUnknown(value->type) || lower::isUnknown(to)) {
        return value;
    }
    // `[]mut T` degrades to `[]T` with no code: the two are the same two-word layout, only the
    // permission differs. The reverse (`[]T` to `[]mut T`) is not implicit and needs `as`.
    if (value->type.kind == TypeKind::Slice && to.kind == TypeKind::Slice &&
        value->type.is_mutable && !to.is_mutable && value->type.elem && to.elem &&
        lower::sameType(*value->type.elem, *to.elem)) {
        EP v = std::move(value);
        v->type = to;
        return v;
    }
    if (value->kind == hir::ExprKind::Nil) {
        if (to.kind == TypeKind::Pointer || to.kind == TypeKind::FnPtr) {
            value->type = to;
            return value;
        }
        err("E0708",
            at,
            std::format("`nil` cannot be used as `{}`", typeToString(to)),
            "only pointers can be `nil`");
        return nullptr;
    }
    if (to.kind == TypeKind::Interface) {
        if (value->type.kind == TypeKind::Interface) {
            if (lower::sameType(value->type, to)) {
                return value;
            }
            err("E0708",
                at,
                std::format(
                    "expected `{}`, found `{}`", typeToString(to), typeToString(value->type)),
                "different interfaces");
            return nullptr;
        }
        const bool agg = lower::isAggregate(value->type) ||
                         (value->type.kind == TypeKind::Pointer && value->type.elem &&
                          lower::isAggregate(*value->type.elem));
        if (agg) {
            return toInterface(std::move(value), to, at);
        }
    }
    const ConversionResult r = implicitConversion(value->type, to, inUnsafe(), ptrBits());
    switch (r.status) {
    case ConversionStatus::Ok:
        return castTo(std::move(value), to, true);
    case ConversionStatus::Warn:
        s_.diag_.warning("W0601",
                         at,
                         std::format("`{}` is implicitly converted to `{}`, precision may be lost",
                                     typeToString(value->type),
                                     typeToString(to)),
                         "write `as` to silence");
        return castTo(std::move(value), to, true);
    case ConversionStatus::Signedness:
        err("E0601",
            at,
            std::format("implicit conversion from `{}` to `{}` changes signedness",
                        typeToString(value->type),
                        typeToString(to)),
            "write `as`, or use `unsafe { }`");
        return nullptr;
    case ConversionStatus::NeedsCast:
        err("E0708",
            at,
            std::format("expected `{}`, found `{}`", typeToString(to), typeToString(value->type)),
            r.note.empty() ? "an explicit `as` is required" : r.note);
        return nullptr;
    case ConversionStatus::Invalid:
        err("E0708",
            at,
            std::format("expected `{}`, found `{}`", typeToString(to), typeToString(value->type)),
            "no conversion exists");
        return nullptr;
    }
    return nullptr;
}

} // namespace xlang::lower
