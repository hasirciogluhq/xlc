// Lowering: array, tuple and struct literals, and `new`.

#include "sema/lower/lower.hpp"

namespace xlang::lower {

Lowerer::EP Lowerer::arrayExpr(const Expr& e, const Type* expected) {
    const Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
    const Type* elem_hint = want.kind == TypeKind::Array && want.elem ? want.elem.get() : nullptr;
    if (e.kind == Expr::Kind::ArrayRepeat) {
        EP v = expr(*e.lhs, elem_hint);
        const auto n = s_.evalConst(*e.rhs);
        if (!v || !n) {
            return nullptr;
        }
        if (n->kind != ConstValue::Kind::Int || n->int_value < 0) {
            err("E0707",
                e.rhs->span,
                "array length must be a non-negative integer constant",
                "invalid length");
            return nullptr;
        }
        auto len = std::make_shared<Expr>();
        len->kind = Expr::Kind::IntLiteral;
        len->int_value = static_cast<std::uint64_t>(n->int_value);
        len->span = e.rhs->span;
        if (elem_hint != nullptr) {
            v = coerce(std::move(v), *elem_hint, e.lhs->span);
            if (!v) {
                return nullptr;
            }
        }
        EP out = mk(hir::ExprKind::ArrayRepeat, Type::makeArray(v->type, len), e.span);
        out->int_value = len->int_value;
        out->lhs = std::move(v);
        return out;
    }
    std::vector<EP> items;
    Type elem;
    bool have = false;
    bool ok = true;
    for (const ExprPtr& a : e.args) {
        EP v = expr(*a, have ? &elem : elem_hint);
        if (v && elem_hint != nullptr) {
            v = coerce(std::move(v), *elem_hint, a->span);
        } else if (v && have) {
            v = coerce(std::move(v), elem, a->span);
        }
        if (!v) {
            ok = false;
            continue;
        }
        if (!have) {
            elem = elem_hint != nullptr ? *elem_hint : v->type;
            have = true;
        }
        items.push_back(std::move(v));
    }
    if (!ok) {
        return nullptr;
    }
    if (!have) {
        if (elem_hint == nullptr) {
            err("E0708",
                e.span,
                "cannot infer the element type of an empty array",
                "write a type: `local a: [T; 0]`");
            return nullptr;
        }
        elem = *elem_hint;
    }
    auto len = std::make_shared<Expr>();
    len->kind = Expr::Kind::IntLiteral;
    len->int_value = items.size();
    len->span = e.span;
    EP out = mk(hir::ExprKind::ArrayList, Type::makeArray(elem, len), e.span);
    out->args = std::move(items);
    return out;
}

Lowerer::EP Lowerer::tupleExpr(const Expr& e, const Type* expected) {
    // `(a, b)`: every element takes its type from the expected tuple's matching element (so an
    // untyped literal like `0` becomes `f64` when the destination says so) or stands alone.
    const Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
    const bool hinted = want.kind == TypeKind::Tuple && want.args.size() == e.args.size();
    std::vector<EP> items;
    bool ok = true;
    for (std::size_t i = 0; i < e.args.size(); ++i) {
        EP v = expr(*e.args[i], hinted ? &want.args[i] : nullptr);
        if (v && hinted) {
            v = coerce(std::move(v), want.args[i], e.args[i]->span);
        }
        // The tuple owns its elements: a droppable local moves in (its flag is cleared).
        v = moveOut(std::move(v));
        if (!v) {
            ok = false;
            continue;
        }
        items.push_back(std::move(v));
    }
    if (!ok) {
        return nullptr;
    }
    std::vector<Type> elements;
    for (const EP& v : items) {
        elements.push_back(v->type);
    }
    EP out = mk(hir::ExprKind::TupleLit, Type::makeTuple(std::move(elements)), e.span);
    out->args = std::move(items);
    return out;
}

void Lowerer::lowerFields(const Expr& e, const Type& agg, std::vector<hir::FieldInit>& out) {
    std::vector<FieldInfo> fields;
    collectFields(agg.path.front(), fields);
    std::set<std::string> seen;
    for (const FieldInit& init : e.fields) {
        std::size_t index = fields.size();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].decl->name == init.name) {
                index = i;
            }
        }
        if (index == fields.size()) {
            err("E0709",
                init.span,
                std::format("`{}` has no field `{}`", typeToString(agg), init.name),
                "unknown field");
            if (init.value) {
                (void)expr(*init.value, nullptr);
            }
            continue;
        }
        if (!seen.insert(init.name).second) {
            err("E0709",
                init.span,
                std::format("field `{}` is initialized twice", init.name),
                "duplicate field");
            continue;
        }
        const Type ft = resolvedIn(*fields[index].owner, fields[index].decl->type);
        EP v = init.value ? moveOut(coerce(expr(*init.value, &ft), ft, init.value->span)) : nullptr;
        if (!v) {
            continue;
        }
        hir::FieldInit h;
        h.name = init.name;
        h.index = static_cast<std::uint32_t>(index);
        h.value = std::move(v);
        out.push_back(std::move(h));
    }
}

Lowerer::EP Lowerer::structLiteral(const Expr& e) {
    const Type t = resolved(e.type);
    if (lower::isUnknown(t)) {
        return nullptr;
    }
    if (!lower::isAggregate(t)) {
        err("E0709",
            e.span,
            std::format("`{}` is not a struct or class", typeToString(t)),
            "cannot build it with `{ ... }`");
        return nullptr;
    }
    if (t.kind == TypeKind::Class && hasInit(t.path.front())) {
        err("E0723",
            e.span,
            std::format(
                "`{}` has an `init`; build it with `{}(...)`", typeToString(t), typeToString(t)),
            "a class literal would skip the constructor");
        return nullptr;
    }
    EP out = mk(hir::ExprKind::StructLit, t, e.span);
    lowerFields(e, t, out->fields);
    return out;
}

Lowerer::EP Lowerer::newExpr(const Expr& e) {
    const Type t = resolved(e.type);
    if (lower::isUnknown(t)) {
        return nullptr;
    }
    if (!lower::isAggregate(t)) {
        err("E0709",
            e.span,
            std::format("`new` needs a struct or class, found `{}`", typeToString(t)),
            "not allocatable");
        return nullptr;
    }
    EP out = mk(hir::ExprKind::New, Type::makePointer(t), e.span);
    out->target = t;
    if (t.kind == TypeKind::Class && (e.has_call_args || hasInit(t.path.front()))) {
        if (!e.has_call_args) {
            err("E0723",
                e.span,
                std::format("`{}` has an `init`; allocate it with `new {}(...)`",
                            typeToString(t),
                            typeToString(t)),
                "a field literal would skip the constructor");
            return nullptr;
        }
        if (!lowerInitCall(e, t, out->text, out->args)) {
            return nullptr;
        }
    } else if (e.has_call_args) {
        err("E0709",
            e.span,
            std::format("`new {}(...)` needs a class with an `init`", typeToString(t)),
            "use `new T { ... }` for structs");
        return nullptr;
    }
    lowerFields(e, t, out->fields);
    if (e.lhs) {
        // Placement: `new(p) T { ... }` initializes the memory `p` points at; no allocation
        // happens.
        EP a = expr(*e.lhs, nullptr);
        if (!a) {
            return nullptr;
        }
        if (!lower::isUnknown(a->type) && a->type.kind != TypeKind::Pointer) {
            err("E0709",
                e.lhs->span,
                std::format("`new(p) T` needs a pointer to the memory, found `{}`",
                            typeToString(a->type)),
                "pass the memory as a pointer, for example `new(allocator.alloc(sizeOf(T), "
                "alignOf(T))) T { }`");
            return nullptr;
        }
        out->lhs = std::move(a);
    } else {
        if (mod_.alloc_function.empty()) {
            Diagnostic d;
            d.code = "E0501";
            d.severity = Severity::Error;
            d.message = "`new` has no registered `alloc` function";
            d.labels.push_back({e.span, "no `register(alloc)` function in this compilation"});
            d.reason = "The compiler binds no allocator by name.";
            d.fix = "write `new(p) T { ... }` with your own memory or register an `alloc` function";
            s_.diag_.report(std::move(d));
            return nullptr;
        }
        out->registered = true;
    }
    return out;
}

} // namespace xlang::lower
