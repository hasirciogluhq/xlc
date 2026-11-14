// Lowering: shared helpers: diagnostics, HIR node factories, type resolution, field layout, constant checks.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

void Lowerer::err(const char* code, const Span& at, std::string message, std::string label) {
    s_.error(code, at, std::move(message), std::move(label));
}

Lowerer::EP Lowerer::mk(hir::ExprKind kind, Type type, const Span& span) const {
    auto e = std::make_unique<hir::Expr>();
    e->kind = kind;
    e->type = std::move(type);
    e->span = span;
    return e;
}

/// @invariant a negative literal of a type narrower than 64 bits keeps only its low bits of that width, which is why `-1 as u8` is 255 and not a sign-extended 64-bit value.
Lowerer::EP Lowerer::intConst(std::uint64_t v, Type type, const Span& span) const {
    EP e = mk(hir::ExprKind::IntConst, std::move(type), span);
    e->int_value = v;
    return e;
}

Type Lowerer::resolvedIn(Sema& sema, const Type& t) {
    // A part of a generic payload enum names its type: `Sema::resolve` needs the arguments written
    // (`Result<i32, E>`), so the enum's instance is filled in here (`Result<i32, E>$Ok`).
    const Type named = sumPartResolved(sema, t);
    auto r = sema.resolve(named);
    if (!r) {
        return lower::unknownType();
    }
    return normalized(sema, *r);
}

/// `t` with the envelope of a generic payload enum applied: `Result$Ok<A, B>` names the part of
/// the instance `Result<A, B>`, so it reads `Result<A, B>$Ok`. Every other type is returned
/// unchanged.
Type Lowerer::sumPartResolved(Sema& sema, const Type& t) {
    if (t.kind != TypeKind::Named || t.path.size() != 1 || t.args.empty()) {
        return t;
    }
    const std::string& name = t.path.front();
    // The envelope is `Result<A, B>` of `Result$Part<A, B>`; the declaration's arguments are its
    // parameters (`A`, `B`), so the search is textual and is skipped for user types that carry
    // arguments of their own but have no such base.
    const std::size_t cut = name.rfind('$');
    if (cut == std::string::npos) {
        return t;
    }
    const auto ref = sema.entityByQName(name.substr(0, cut));
    if (!ref || ref->sym->item == nullptr ||
        !std::holds_alternative<StructDecl>(ref->sym->item->decl)) {
        return t;
    }
    if (std::get<StructDecl>(ref->sym->item->decl).variants.empty()) {
        return t; // not a payload enum: nothing to move inside the name
    }
    Type inst = t;
    inst.path = {ref->qname() + "$" + name.substr(cut + 1)};
    return inst;
}

Type Lowerer::globalType(Sema& sema, const GlobalDecl& g) {
    if (g.has_type) {
        return resolvedIn(sema, g.type);
    }
    if (g.init) {
        if (const auto t = lower::literalConstType(sema, *g.init)) {
            return *t;
        }
    }
    return lower::unknownType();
}

/// HIR array types carry a literal length, never a `define` name.
Type Lowerer::normalized(Sema& sema, const Type& t) {
    Type out = t;
    if (t.kind == TypeKind::Array && t.length != nullptr) {
        if (const auto n = arrayLength(sema, t)) {
            auto len = std::make_shared<Expr>();
            len->kind = Expr::Kind::IntLiteral;
            len->int_value = *n;
            len->span = t.length->span;
            out.length = len;
        }
    }
    if ((t.kind == TypeKind::Array || t.kind == TypeKind::Pointer || t.kind == TypeKind::Enum) &&
        t.elem) {
        out.elem = std::make_shared<const Type>(normalized(sema, *t.elem));
    }
    if (t.kind == TypeKind::FnPtr) {
        for (Type& p : out.params) {
            p = normalized(sema, p);
        }
        if (t.ret) {
            out.ret = std::make_shared<const Type>(normalized(sema, *t.ret));
        }
    }
    return out;
}

std::optional<std::uint64_t> Lowerer::arrayLength(Sema& sema, const Type& t) {
    if (t.length == nullptr) {
        return std::nullopt;
    }
    const auto v = sema.evalConst(*t.length);
    if (!v || v->kind != ConstValue::Kind::Int || v->int_value < 0) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(v->int_value);
}

Type Lowerer::fnTypeOf(Sema& owner, const FnDecl& f) {
    std::vector<Type> params;
    for (const Param& p : f.params) {
        if (!p.is_self) {
            params.push_back(resolvedIn(owner, p.type));
        }
    }
    return Type::makeFnPtr(std::move(params), resolvedIn(owner, f.return_type));
}

std::optional<Sema::EntityRef> Lowerer::entityOfType(const Type& t) {
    if (t.path.empty()) {
        return std::nullopt;
    }
    return s_.entityByQName(t.path.front());
}

void Lowerer::collectFields(const std::string& qname, std::vector<FieldInfo>& out) {
    const auto ref = s_.entityByQName(qname);
    if (!ref) {
        return;
    }
    if (const auto* s = std::get_if<StructDecl>(&ref->sym->item->decl)) {
        for (const FieldDecl& f : s->fields) {
            out.push_back({&f, ref->owner});
        }
        return;
    }
    const auto* c = std::get_if<ClassDecl>(&ref->sym->item->decl);
    if (c == nullptr) {
        return;
    }
    if (c->has_base) {
        Type base = Type::makeNamed(c->base);
        if (const auto r = ref->owner->resolve(base);
            r && r->kind == TypeKind::Class && r->path.front() != qname) {
            collectFields(r->path.front(), out);
        }
    }
    for (const FieldDecl& f : c->fields) {
        out.push_back({&f, ref->owner});
    }
}

const Lowerer::ImplInfo* Lowerer::findImplExact(const std::string& type_q,
                                                const std::string& iface_q) const {
    for (const ImplInfo& i : sh_.impls) {
        if (i.type_q == type_q && i.iface_q == iface_q) {
            return &i;
        }
    }
    return nullptr;
}

bool Lowerer::isConstValue(const hir::Expr& e, const hir::Expr** offender) const {
    const auto fail = [&](const hir::Expr& at) {
        if (offender != nullptr && *offender == nullptr) {
            *offender = &at;
        }
        return false;
    };
    switch (e.kind) {
    case hir::ExprKind::IntConst:
    case hir::ExprKind::FloatConst:
    case hir::ExprKind::BoolConst:
    case hir::ExprKind::StrConst:
    case hir::ExprKind::Nil:
    case hir::ExprKind::FuncRef:
    case hir::ExprKind::External:
        return true;
    case hir::ExprKind::Unary:
    case hir::ExprKind::Cast:
    case hir::ExprKind::ToInterface:
        return e.lhs && isConstValue(*e.lhs, offender);
    case hir::ExprKind::Binary:
        return e.lhs && e.rhs && isConstValue(*e.lhs, offender) && isConstValue(*e.rhs, offender);
    case hir::ExprKind::ArrayList:
    case hir::ExprKind::TupleLit:
        return std::ranges::all_of(
            e.args, [&](const hir::ExprPtr& a) { return isConstValue(*a, offender); });
    case hir::ExprKind::ArrayRepeat:
        return e.lhs && isConstValue(*e.lhs, offender);
    case hir::ExprKind::StructLit:
        return std::ranges::all_of(
            e.fields, [&](const hir::FieldInit& f) { return isConstValue(*f.value, offender); });
    case hir::ExprKind::AddrOf: {
        // `&global`, `&global[const]`, `&global.field`
        const hir::Expr* cur = e.lhs.get();
        while (cur != nullptr) {
            if (cur->kind == hir::ExprKind::GlobalRef) {
                return true;
            }
            if (cur->kind == hir::ExprKind::Index) {
                if (cur->rhs == nullptr || cur->rhs->kind != hir::ExprKind::IntConst) {
                    return fail(*cur);
                }
                cur = cur->lhs.get();
            } else if (cur->kind == hir::ExprKind::Field && !cur->through_pointer) {
                cur = cur->lhs.get();
            } else {
                return fail(e);
            }
        }
        return fail(e);
    }
    default:
        return fail(e);
    }
}

} // namespace xlang::lower
