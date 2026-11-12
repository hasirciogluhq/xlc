// Deep copies of declarations for generic instantiation: every type parameter named in a
// TypeEnv is replaced by its concrete type, everything else is copied unchanged.
#include "xlang/lang/ast.h"

#include "xlang/util/error.h"

namespace xlang {

namespace {

ExprPtr cloneExpr(const Expr& e, const TypeEnv& env);
Block cloneBlock(const Block& b, const TypeEnv& env);

Stmt cloneStmt(const Stmt& s, const TypeEnv& env) {
    Stmt c;
    c.kind = s.kind;
    c.span = s.span;
    c.attributes = s.attributes;
    c.name = s.name;
    c.names = s.names;
    c.label = s.label;
    c.has_type = s.has_type;
    c.type = substituteType(s.type, env);
    if (s.target) {
        c.target = cloneExpr(*s.target, env);
    }
    if (s.expr) {
        c.expr = cloneExpr(*s.expr, env);
    }
    if (s.expr2) {
        c.expr2 = cloneExpr(*s.expr2, env);
    }
    c.has_compound_op = s.has_compound_op;
    c.is_incdec = s.is_incdec;
    c.range_inclusive = s.range_inclusive;
    c.compound_op = s.compound_op;
    if (s.body) {
        c.body = std::make_unique<Block>(cloneBlock(*s.body, env));
    }
    if (s.else_body) {
        c.else_body = std::make_unique<Block>(cloneBlock(*s.else_body, env));
    }
    c.else_is_if = s.else_is_if;
    if (s.init) {
        c.init = std::make_unique<Stmt>(cloneStmt(*s.init, env));
    }
    if (s.step) {
        c.step = std::make_unique<Stmt>(cloneStmt(*s.step, env));
    }
    for (const SwitchCase& sc : s.cases) {
        SwitchCase n;
        n.span = sc.span;
        n.is_default = sc.is_default;
        for (const ExprPtr& v : sc.values) {
            n.values.push_back(cloneExpr(*v, env));
        }
        if (sc.body) {
            n.body = std::make_unique<Block>(cloneBlock(*sc.body, env));
        }
        c.cases.push_back(std::move(n));
    }
    return c;
}

Block cloneBlock(const Block& b, const TypeEnv& env) {
    Block c;
    c.span = b.span;
    for (const Stmt& s : b.statements) {
        c.statements.push_back(cloneStmt(s, env));
    }
    return c;
}

ExprPtr cloneExpr(const Expr& e, const TypeEnv& env) {
    auto c = std::make_unique<Expr>();
    c->kind = e.kind;
    c->span = e.span;
    c->int_value = e.int_value;
    c->float_value = e.float_value;
    c->bool_value = e.bool_value;
    c->text = e.text;
    c->unary_op = e.unary_op;
    c->binary_op = e.binary_op;
    c->type = substituteType(e.type, env);
    // A type parameter used as a value (`T(args)`, `T.f`) names the type it stands for.
    if (e.kind == Expr::Kind::Name) {
        if (const auto it = env.find(e.text); it != env.end()) {
            c->text = typeToString(it->second);
            c->type = it->second;  // the resolved type, so it is not looked up again by name
        }
    }
    if (e.lhs) {
        c->lhs = cloneExpr(*e.lhs, env);
    }
    if (e.rhs) {
        c->rhs = cloneExpr(*e.rhs, env);
    }
    for (const ExprPtr& a : e.args) {
        c->args.push_back(cloneExpr(*a, env));
    }
    for (const FieldInit& f : e.fields) {
        FieldInit n;
        n.name = f.name;
        n.span = f.span;
        if (f.value) {
            n.value = cloneExpr(*f.value, env);
        }
        c->fields.push_back(std::move(n));
    }
    c->has_call_args = e.has_call_args;
    for (const Type& t : e.generic_args) {
        c->generic_args.push_back(substituteType(t, env));
    }
    c->macro_args = e.macro_args;
    if (e.closure) {
        auto d = std::make_shared<ClosureData>();
        d->params = e.closure->params;
        for (Param& p : d->params) {
            p.type = substituteType(p.type, env);
        }
        d->has_return_type = e.closure->has_return_type;
        d->return_type = substituteType(e.closure->return_type, env);
        d->is_move = e.closure->is_move;
        d->expr_body = e.closure->expr_body;
        d->body = cloneBlock(e.closure->body, env);
        c->closure = std::move(d);
    }
    return c;
}

FnDecl cloneFn(const FnDecl& f, const TypeEnv& env) {
    FnDecl c;
    static_cast<DeclBase&>(c) = f;  // name, attributes, spans
    c.params = f.params;
    for (Param& p : c.params) {
        p.type = substituteType(p.type, env);
    }
    c.has_return_type = f.has_return_type;
    c.return_type = substituteType(f.return_type, env);
    c.has_body = f.has_body;
    c.body = cloneBlock(f.body, env);
    c.is_virtual = f.is_virtual;
    c.is_override = f.is_override;
    return c;
}

/// A method of a generic type's instance: the type's parameters are replaced, and a method that
/// is generic itself stays generic (`fn map<U>` of `Box<T>` is `fn map<U>` of `Box<i32>`), so it
/// is instantiated per call like the method of a plain class. A method parameter that shadows a
/// type parameter keeps its own meaning.
FnDecl cloneMethod(const FnDecl& m, const TypeEnv& env) {
    TypeEnv own = env;
    for (const std::string& p : m.generic_params) {
        own.erase(p);
    }
    FnDecl c = cloneFn(m, own);
    c.generic_params = m.generic_params;
    return c;
}

}  // namespace

Type substituteType(const Type& type, const TypeEnv& env) {
    if (type.kind == TypeKind::Named && type.path.size() == 1 && type.args.empty()) {
        if (const auto it = env.find(type.path.front()); it != env.end()) {
            Type out = it->second;
            out.span = type.span;
            return out;
        }
    }
    Type out = type;
    if (type.elem) {
        out.elem = std::make_shared<const Type>(substituteType(*type.elem, env));
    }
    if (type.ret) {
        out.ret = std::make_shared<const Type>(substituteType(*type.ret, env));
    }
    for (Type& p : out.params) {
        p = substituteType(p, env);
    }
    for (Type& a : out.args) {
        a = substituteType(a, env);
    }
    return out;
}

FnDecl instantiateFn(const FnDecl& generic, const TypeEnv& env) {
    return cloneFn(generic, env);  // cloneFn does not copy the type parameters
}

Block copyBlock(const Block& block) {
    return cloneBlock(block, TypeEnv{});
}

ExprPtr copyExpr(const Expr& expr) {
    return cloneExpr(expr, TypeEnv{});
}

Item instantiateItem(const Item& generic, const TypeEnv& env, const std::string& name) {
    Item out;
    out.modifiers = generic.modifiers;
    if (const auto* f = std::get_if<FnDecl>(&generic.decl)) {
        FnDecl c = cloneFn(*f, env);
        c.name = name;
        out.decl = std::move(c);
    } else if (const auto* s = std::get_if<StructDecl>(&generic.decl)) {
        StructDecl c;
        static_cast<DeclBase&>(c) = *s;
        c.name = name;
        for (const FieldDecl& f : s->fields) {
            FieldDecl n = f;
            n.type = substituteType(f.type, env);
            c.fields.push_back(std::move(n));
        }
        c.opaque = s->opaque;
        c.is_union = s->is_union;
        c.is_payload = s->is_payload;
        // The variant list of a payload enum is data of the type, not of its text: an instance
        // `Result<i32, E>` needs it to be matched like the declaration, with its payload types
        // substituted (`Ok(T)` is `Ok(i32)` there).
        for (const SumVariant& v : s->variants) {
            SumVariant n;
            n.name = v.name;
            for (const Type& p : v.payload) {
                n.payload.push_back(substituteType(p, env));
            }
            c.variants.push_back(std::move(n));
        }
        out.decl = std::move(c);
    } else if (const auto* cl = std::get_if<ClassDecl>(&generic.decl)) {
        ClassDecl c;
        static_cast<DeclBase&>(c) = *cl;
        c.name = name;
        c.has_base = cl->has_base;
        c.base = cl->base;
        for (const FieldDecl& f : cl->fields) {
            FieldDecl n = f;
            n.type = substituteType(f.type, env);
            c.fields.push_back(std::move(n));
        }
        for (const FnDecl& m : cl->methods) {
            c.methods.push_back(cloneMethod(m, env));
        }
        c.opaque = cl->opaque;
        out.decl = std::move(c);
    } else if (const auto* in = std::get_if<InterfaceDecl>(&generic.decl)) {
        InterfaceDecl c;
        static_cast<DeclBase&>(c) = *in;
        c.name = name;
        for (const FnDecl& m : in->methods) {
            c.methods.push_back(cloneMethod(m, env));
        }
        c.opaque = in->opaque;
        out.decl = std::move(c);
    } else if (const auto* impl = std::get_if<ImplDecl>(&generic.decl)) {
        ImplDecl c;
        static_cast<DeclBase&>(c) = *impl;
        c.name = name;
        c.interface_path = impl->interface_path;
        for (const Type& a : impl->interface_args) {
            c.interface_args.push_back(substituteType(a, env));
        }
        c.target = substituteType(impl->target, env);
        for (const FnDecl& m : impl->methods) {
            c.methods.push_back(cloneMethod(m, env));
        }
        out.decl = std::move(c);
    } else if (const auto* e = std::get_if<EnumDecl>(&generic.decl)) {
        EnumDecl c;
        static_cast<DeclBase&>(c) = *e;
        c.name = name;
        c.has_underlying = e->has_underlying;
        c.underlying = substituteType(e->underlying, env);
        for (const EnumMember& m : e->members) {
            EnumMember n;
            n.name = m.name;
            n.has_payload = m.has_payload;
            n.attributes = m.attributes;
            n.span = m.span;
            for (const Type& p : m.payload) {
                n.payload.push_back(substituteType(p, env));
            }
            if (m.value) {
                n.value = cloneExpr(*m.value, env);
            }
            c.members.push_back(std::move(n));
        }
        out.decl = std::move(c);
    } else {
        throw XlangError(
            "internal: only fn, struct, class, interface, impl and enum declarations are instantiated");
    }
    return out;
}

}  // namespace xlang
