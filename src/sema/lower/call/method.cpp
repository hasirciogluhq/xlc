// Lowering: method lookup and method calls (virtual, interface and inherent).

#include "sema/lower/lower.hpp"

namespace xlang::lower {

std::optional<Lowerer::MethodRef> Lowerer::findMethod(const Type& agg, const std::string& name) {
    if (agg.kind == TypeKind::Interface) {
        const auto ref = entityOfType(agg);
        if (!ref || !std::holds_alternative<InterfaceDecl>(ref->sym->item->decl)) {
            return std::nullopt;
        }
        const auto& decl = std::get<InterfaceDecl>(ref->sym->item->decl);
        for (std::size_t i = 0; i < decl.methods.size(); ++i) {
            if (decl.methods[i].name == name) {
                MethodRef m;
                m.kind = MethodRef::Kind::Itable;
                m.decl = &decl.methods[i];
                m.owner = ref->owner;
                m.index = static_cast<std::uint32_t>(i);
                return m;
            }
        }
        return std::nullopt;
    }
    std::string cur = agg.path.front();
    std::set<std::string> seen;
    while (!cur.empty() && seen.insert(cur).second) {
        const auto ref = s_.entityByQName(cur);
        const ClassDecl* c = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
        if (c != nullptr) {
            for (const FnDecl& m : c->methods) {
                if (m.name == name) {
                    MethodRef r;
                    r.kind = (m.is_virtual || m.is_override) ? MethodRef::Kind::Virtual
                                                             : MethodRef::Kind::Direct;
                    r.fn_name = cur + "." + name;
                    r.decl = &m;
                    r.owner = ref->owner;
                    r.cls = c;
                    r.cls_q = cur;
                    if (r.kind == MethodRef::Kind::Virtual) {
                        const auto& slots = vtableOf(agg.path.front());
                        for (std::size_t i = 0; i < slots.size(); ++i) {
                            if (slots[i].slot.method == name) {
                                r.index = static_cast<std::uint32_t>(i);
                                r.fn_name = slots[i].slot.function;
                            }
                        }
                    }
                    return r;
                }
            }
        }
        instantiateImpls();
        for (const ImplInfo& i : sh_.impls) {
            if (i.type_q != cur) {
                continue;
            }
            for (const FnDecl& m : i.decl->methods) {
                if (m.name == name) {
                    MethodRef r;
                    r.fn_name = i.prefix + name;
                    r.decl = &m;
                    r.owner = i.owner;
                    r.from_impl = true;
                    return r;
                }
            }
        }
        cur.clear();
        if (c != nullptr && c->has_base) {
            Type base = Type::makeNamed(c->base);
            if (const auto r = ref->owner->resolve(base); r && r->kind == TypeKind::Class) {
                cur = r->path.front();
            }
        }
    }
    return std::nullopt;
}

Lowerer::EP Lowerer::methodCall(const Expr& e, const Expr& recv_ast) {
    const Expr& callee = *e.lhs;
    const std::size_t mark = preludeMark();
    EP recv = expr(recv_ast, nullptr);
    if (!recv) {
        for (const ExprPtr& a : e.args) {
            (void)expr(*a, nullptr);
        }
        return nullptr;
    }
    if (lower::isUnknown(recv->type)) {
        EP c = mk(hir::ExprKind::CallIndirect, lower::unknownType(), e.span);
        c->lhs = std::move(recv);
        for (const ExprPtr& a : e.args) {
            c->args.push_back(expr(*a, nullptr));
        }
        return c;
    }
    if (!recv->lvalue && needsDrop(recv->type)) {
        recv = materializeTemp(std::move(recv), recv_ast.span);
    }
    bool through = false;
    Type agg = recv->type;
    if (agg.kind == TypeKind::Pointer && agg.elem) {
        through = true;
        agg = *agg.elem;
    }
    if (agg.kind != TypeKind::Interface && !lower::isAggregate(agg)) {
        err("E0709",
            callee.span,
            std::format("`{}` has no method `{}`", typeToString(recv->type), callee.text),
            "only structs, classes and interfaces have methods");
        return nullptr;
    }
    // `value.field(args)` where `field` holds a function pointer: an indirect call.
    if (lower::isAggregate(agg)) {
        std::vector<FieldInfo> fields;
        collectFields(agg.path.front(), fields);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].decl->name != callee.text) {
                continue;
            }
            const Type ft = resolvedIn(*fields[i].owner, fields[i].decl->type);
            if (ft.kind != TypeKind::FnPtr || !ft.ret) {
                break;
            }
            EP f = mk(hir::ExprKind::Field, ft, callee.span);
            f->text = callee.text;
            f->index = static_cast<std::uint32_t>(i);
            f->through_pointer = through;
            f->lvalue = true;
            f->lhs = std::move(recv);
            std::vector<EP> args;
            if (!lowerArgs(e, ft.params, args, callee.text)) {
                return nullptr;
            }
            EP c = mk(hir::ExprKind::CallIndirect, *ft.ret, e.span);
            c->lhs = std::move(f);
            c->args = std::move(args);
            return c;
        }
    }
    auto m = findMethod(agg, callee.text);
    if (!m && lower::isAggregate(agg)) {
        // A method of a struct (or a class without it) from an inherent `impl`: `T.name(self, ...)`.
        cutPrelude(mark);
        ExprPtr receiver = cloneAst(recv_ast);
        if (through) {
            receiver = lower::astUnary(UnaryOp::Deref, std::move(receiver), recv_ast.span);
        }
        std::vector<ExprPtr> rest;
        for (const ExprPtr& a : e.args) {
            rest.push_back(cloneAst(*a));
        }
        bool found = false;
        EP c = callMethod(std::move(receiver), agg, callee.text, std::move(rest), e.span, found);
        if (found) {
            return c;
        }
    }
    if (!m) {
        err("E0709",
            callee.span,
            std::format("`{}` has no method `{}`", typeToString(agg), callee.text),
            "unknown method");
        for (const ExprPtr& a : e.args) {
            (void)expr(*a, nullptr);
        }
        return nullptr;
    }
    std::vector<EP> args;
    bool overloaded_args = false;
    if (!m->decl->generic_params.empty()) {
        // A generic method: its instance is chosen by the written or inferred type arguments.
        if (m->kind != MethodRef::Kind::Direct || m->cls == nullptr) {
            err("E0733",
                callee.span,
                std::format("generic method `{}` is only supported in a class", callee.text),
                "move the method into the class");
            return nullptr;
        }
        const std::vector<std::string>& names = m->decl->generic_params;
        std::vector<PreArg> pre = preLowerArgs(e);
        std::vector<Type> targs;
        if (!callee.generic_args.empty()) {
            if (callee.generic_args.size() != names.size()) {
                err("E0724",
                    callee.span,
                    std::format("`{}` takes {} type argument(s), {} given",
                                callee.text,
                                names.size(),
                                callee.generic_args.size()),
                    "write one type per type parameter");
                return nullptr;
            }
            for (const Type& a : callee.generic_args) {
                targs.push_back(resolved(a));
            }
        } else {
            std::map<std::string, Type> bound;
            bool ok = true;
            std::size_t i = 0;
            for (const Param& p : m->decl->params) {
                if (p.is_self) {
                    continue;
                }
                if (i >= pre.size() || lower::isUnknown(pre[i].type) ||
                    !unifyParam(p.type, pre[i].type, names, bound, *m->owner)) {
                    ok = false;
                    break;
                }
                ++i;
            }
            for (const std::string& n : names) {
                if (!ok || !bound.contains(n)) {
                    err("E0724",
                        callee.span,
                        std::format("cannot infer `{}` of `{}`", n, callee.text),
                        std::format("write the type arguments: `.{}<...>(...)`", callee.text));
                    return nullptr;
                }
                targs.push_back(bound.at(n));
            }
        }
        const Sema::MethodInstance* inst =
            m->owner->instantiateMethod(*m->decl, m->cls->name, m->cls_q, targs);
        std::vector<Type> params;
        for (const Param& p : inst->decl.params) {
            if (!p.is_self) {
                params.push_back(resolvedIn(*m->owner, p.type));
            }
        }
        m->decl = &inst->decl;
        m->fn_name = inst->key;
        if (!lowerArgsPre(e, params, pre, args, callee.text)) {
            return nullptr;
        }
        overloaded_args = true;
    }
    if (m->kind == MethodRef::Kind::Direct && m->cls != nullptr && !overloaded_args) {
        // Non-virtual methods of one class may overload each other.
        const auto cands = methodCandidates(*m, callee.text);
        if (cands.size() > 1) {
            std::vector<PreArg> pre = preLowerArgs(e);
            const auto chosen = resolveOverload(e, callee.text, cands, pre);
            if (!chosen) {
                return nullptr;
            }
            m->decl = cands[*chosen].decl;
            m->fn_name = cands[*chosen].key;
            overloaded_args = true;
            if (!lowerArgsPre(e, cands[*chosen].params, pre, args, callee.text)) {
                return nullptr;
            }
        }
    }
    std::vector<Type> params;
    for (const Param& p : m->decl->params) {
        if (!p.is_self) {
            params.push_back(resolvedIn(*m->owner, p.type));
        }
    }
    const Type ret = resolvedIn(*m->owner, m->decl->return_type);
    if (!overloaded_args && !lowerArgs(e, params, args, callee.text)) {
        return nullptr;
    }

    if (m->kind == MethodRef::Kind::Itable) {
        if (through) { // pointer to an interface value: call through the value
            EP d = mk(hir::ExprKind::Deref, agg, recv->span);
            d->lvalue = true;
            d->lhs = std::move(recv);
            recv = std::move(d);
        }
        EP c = mk(hir::ExprKind::CallItable, ret, e.span);
        c->text = callee.text;
        c->index = m->index;
        c->lhs = std::move(recv);
        c->args = std::move(args);
        return c;
    }

    EP self;
    if (through) {
        self = std::move(recv);
    } else {
        if (!recv->lvalue) {
            err("E0709",
                recv_ast.span,
                "a method needs an addressable receiver",
                "store the value in a variable first");
            return nullptr;
        }
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(agg), recv->span);
        addr->lhs = std::move(recv);
        self = std::move(addr);
    }
    EP c =
        mk(m->kind == MethodRef::Kind::Virtual ? hir::ExprKind::CallVirtual : hir::ExprKind::Call,
           ret,
           e.span);
    c->text = m->fn_name;
    c->index = m->index;
    c->member = callee.text;
    c->args.push_back(std::move(self));
    for (EP& a : args) {
        c->args.push_back(std::move(a));
    }
    return c;
}

} // namespace xlang::lower
