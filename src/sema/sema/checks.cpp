#include "sema/sema/sema.hpp"

#include "xlang/sema/compilation.h"

#include "sema/lower/lower.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <numeric>
#include <random>

namespace xlang {

// Whole-module checks and the `analyze` entry point: aliases, enums, array lengths in every
// type, `staticAssert`, layouts of every aggregate, and the bodies of non-generic functions.

// ---------------------------------------------------------------------------------------------
// Whole-module checks

void Sema::checkType(const Type& type, const Span& at) {
    auto resolved = resolve(type);
    if (!resolved) {
        return;
    }
    // Array lengths anywhere in the tree must be constants.
    const std::function<void(const Type&)> walk = [&](const Type& t) {
        if (t.kind == TypeKind::Array) {
            if (t.length != nullptr) {
                const auto v = evalConst(*t.length);
                if (v && (v->kind != ConstValue::Kind::Int || v->int_value < 0)) {
                    error("E0707",
                          t.length->span,
                          "array length must be a non-negative integer constant",
                          "invalid length");
                }
            }
            if (t.elem) {
                walk(*t.elem);
            }
        } else if (t.kind == TypeKind::Pointer) {
            if (t.elem) {
                walk(*t.elem);
            }
        } else if (t.kind == TypeKind::FnPtr) {
            for (const Type& p : t.params) {
                walk(p);
            }
            if (t.ret) {
                walk(*t.ret);
            }
        } else if (t.kind == TypeKind::Tuple) {
            for (const Type& a : t.args) {
                walk(a);
            }
        }
    };
    walk(type);
    (void)at;
}

void Sema::checkExpr(const Expr& e) {
    if (e.kind == Expr::Kind::Cast || e.kind == Expr::Kind::New ||
        e.kind == Expr::Kind::StructLiteral) {
        checkType(e.type, e.span);
    }
    if (e.lhs) {
        checkExpr(*e.lhs);
    }
    if (e.rhs) {
        checkExpr(*e.rhs);
    }
    for (const ExprPtr& a : e.args) {
        checkExpr(*a);
    }
    for (const FieldInit& f : e.fields) {
        if (f.value) {
            checkExpr(*f.value);
        }
    }
    if (e.closure) {
        for (const Param& p : e.closure->params) {
            if (p.has_type) {
                checkType(p.type, p.span);
            }
        }
        if (e.closure->has_return_type) {
            checkType(e.closure->return_type, e.span);
        }
        checkStatements(e.closure->body);
    }
}

void Sema::checkStatements(const Block& block) {
    for (const Stmt& s : block.statements) {
        if (s.has_type) {
            checkType(s.type, s.span);
        }
        if (s.target) {
            checkExpr(*s.target);
        }
        if (s.expr) {
            checkExpr(*s.expr);
        }
        if (s.body) {
            checkStatements(*s.body);
        }
        if (s.else_body) {
            checkStatements(*s.else_body);
        }
    }
}

void Sema::checkStaticAssert(const TopLevelCallDecl& call) {
    checkStaticAssertCall(*call.call);
}

void Sema::checkStaticAssertCall(const Expr& c) {
    if (c.lhs->kind != Expr::Kind::Name || c.lhs->text != "staticAssert") {
        return;
    }
    if (c.args.empty() || c.args.size() > 2) {
        error("E0702",
              c.span,
              "`staticAssert` takes a condition and an optional message",
              "wrong arguments");
        return;
    }
    const auto value = evalConst(*c.args[0]);
    if (!value) {
        return;
    }
    if (value->kind != ConstValue::Kind::Bool) {
        error("E0702",
              c.args[0]->span,
              "`staticAssert` condition must be a bool constant",
              "not a bool");
        return;
    }
    if (!value->bool_value) {
        std::string message = "static assertion failed";
        if (c.args.size() == 2 && c.args[1]->kind == Expr::Kind::StringLiteral) {
            message += ": " + c.args[1]->text;
        }
        error("E0702", c.span, std::move(message), "this condition is false");
    }
}

void Sema::checkAggregate(const Item& item) {
    const DeclBase& base = declBase(item);
    for (const Attribute& a : base.attributes) {
        (void)a;
    }
    (void)layoutOfName(base.name, base.name_span);
}

void Sema::analyze() {
    for (const Item& item : program_.items) {
        const DeclBase& base = declBase(item);
        if (const auto* e = std::get_if<EnumDecl>(&item.decl)) {
            // A generic enum is checked per instance, when it is instantiated.
            if (e->generic_params.empty()) {
                checkEnum(*e);
            }
        } else if (const auto* a = std::get_if<TypeAliasDecl>(&item.decl)) {
            // A generic alias has no concrete target until it is used; each use resolves it.
            if (a->generic_params.empty()) {
                (void)resolve(a->target);
                checkType(a->target, base.span);
                Type self = Type::makeNamed({base.name});
                self.span = base.name_span;
                (void)resolve(self); // cyclic aliases
            }
        } else if (std::holds_alternative<DefineConstDecl>(item.decl)) {
            Expr name;
            name.kind = Expr::Kind::Name;
            name.text = base.name;
            name.span = base.name_span;
            if (!config_.lookup(base.name)) {
                (void)evalConst(name);
            }
        } else if (const auto* g = std::get_if<GlobalDecl>(&item.decl)) {
            if (g->has_type) {
                checkType(g->type, base.span);
            }
            if (g->init) {
                checkExpr(*g->init);
            }
        } else if (isPackFn(item)) {
            // A pack function is checked here for its declaration and its uses of the pack; its
            // body is checked per element count, when a call instantiates it.
            checkPackFn(item);
        } else if (const auto* f = std::get_if<FnDecl>(&item.decl);
                   f != nullptr && f->generic_params.empty()) {
            // Generic declarations are checked per instance, when they are instantiated.
            for (const Param& p : f->params) {
                if (!p.is_self) {
                    checkType(p.type, p.span);
                }
            }
            checkType(f->return_type, base.span);
            if (f->has_body) {
                checkStatements(f->body);
            }
        } else if ((std::holds_alternative<StructDecl>(item.decl) ||
                    std::holds_alternative<ClassDecl>(item.decl)) &&
                   genericParamsOf(item).empty()) {
            // A payload struct of a generic enum (`Result$Ok<T, E>`) has the enum's parameters in
            // its fields but none of its own; it is checked with the enum's instance.
            const bool instantiated_part =
                std::holds_alternative<StructDecl>(item.decl) &&
                std::get<StructDecl>(item.decl).is_payload &&
                !genericParamsOf(genericEnumOf(declBase(item).name)).empty();
            if (instantiated_part) {
                continue;
            }
            const auto* fields = std::holds_alternative<StructDecl>(item.decl)
                                     ? &std::get<StructDecl>(item.decl).fields
                                     : &std::get<ClassDecl>(item.decl).fields;
            for (const FieldDecl& f : *fields) {
                checkType(f.type, f.span);
            }
            if (const auto* c = std::get_if<ClassDecl>(&item.decl)) {
                for (const FnDecl& m : c->methods) {
                    checkNoPackMethod(m);
                    // A generic method has no body to check until an instance is made.
                    if (m.has_body && m.generic_params.empty()) {
                        checkStatements(m.body);
                    }
                }
            }
            const bool opaque = std::holds_alternative<StructDecl>(item.decl)
                                    ? std::get<StructDecl>(item.decl).opaque
                                    : std::get<ClassDecl>(item.decl).opaque;
            if (!opaque) {
                checkAggregate(item);
            }
        } else if (const auto* impl = std::get_if<ImplDecl>(&item.decl)) {
            for (const FnDecl& m : impl->methods) {
                checkNoPackMethod(m);
                if (m.has_body) {
                    checkStatements(m.body);
                }
            }
        } else if (const auto* iface = std::get_if<InterfaceDecl>(&item.decl)) {
            for (const FnDecl& m : iface->methods) {
                checkNoPackMethod(m);
            }
        } else if (const auto* call = std::get_if<TopLevelCallDecl>(&item.decl)) {
            checkStaticAssert(*call);
        }
    }
}

hir::Module Sema::lower() {
    // A standalone module (no `Compilation`): it is its own group, so it is the only module and the
    // only one that can run `@init` functions.
    return lowerProgram({this}, {this}, {});
}

} // namespace xlang
