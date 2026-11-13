// Lowering: calls of functions, constructors and function values, and argument lowering.

#include "sema/lower/lower.hpp"

namespace xlang::lower {

bool Lowerer::lowerArgs(const Expr& call,
                        const std::vector<Type>& params,
                        std::vector<EP>& out,
                        const std::string& callee) {
    if (call.args.size() != params.size()) {
        err("E0710",
            call.span,
            std::format(
                "`{}` takes {} argument(s), {} given", callee, params.size(), call.args.size()),
            "wrong number of arguments");
        for (const ExprPtr& a : call.args) {
            (void)expr(*a, nullptr);
        }
        return false;
    }
    bool ok = true;
    for (std::size_t i = 0; i < params.size(); ++i) {
        EP a = moveOut(coerce(expr(*call.args[i], &params[i]), params[i], call.args[i]->span));
        ok = ok && a != nullptr;
        out.push_back(std::move(a));
    }
    return ok;
}

Lowerer::EP Lowerer::callEntry(const Expr& e, const ScopeEntry& entry, const std::string& name) {
    if (entry.kind == ScopeEntry::Kind::Function) {
        // Overloaded or generic calls lower their arguments once here; the choice is made from
        // their cached types.
        std::vector<const Item*> items = entry.overloads;
        if (items.empty()) {
            items.push_back(entry.item);
        }
        bool generic = false;
        bool pack = false;
        for (const Item* item : items) {
            generic = generic || !s_.genericParamsOf(*item).empty();
            pack = pack || Sema::isPackFn(*item);
        }
        const bool many = items.size() > 1 || generic || pack;
        std::vector<PreArg> pre;
        if (many) {
            pre = preLowerArgs(e);
        }
        auto cands = candidatesOf(entry);
        for (OverloadCandidate& g : genericCandidates(entry, e, pre)) {
            cands.push_back(std::move(g));
        }
        // A pack instance is made only when no fixed-arity overload has the call's argument count,
        // so an exact overload is always preferred over the pack.
        const bool fixed_arity = std::ranges::any_of(
            cands, [&e](const OverloadCandidate& c) { return c.params.size() == e.args.size(); });
        if (pack && !fixed_arity) {
            for (OverloadCandidate& p : packCandidates(entry, e, pre)) {
                cands.push_back(std::move(p));
            }
        }
        if (cands.empty()) {
            err("E0404",
                e.span,
                std::format("no `{}` fits this call", name),
                "check the arguments and the type arguments");
            return nullptr;
        }
        std::size_t pick = 0;
        if (many) {
            const auto chosen = resolveOverload(e, name, cands, pre);
            if (!chosen) {
                return nullptr;
            }
            pick = *chosen;
        }
        const OverloadCandidate& cand = cands[pick];
        if (sh_.isHook(cand.key)) {
            err("E0727",
                e.span,
                std::format("`{}` is an `@init`, `@fini` or `@entry` function and cannot be called",
                            name),
                "hook functions are started by the generated code; move the shared code into a "
                "normal "
                "function and call that");
            return nullptr;
        }
        std::vector<EP> args;
        const bool lowered = many ? lowerArgsPre(e, cand.params, pre, args, name)
                                  : lowerArgs(e, cand.params, args, name);
        if (!lowered) {
            return nullptr;
        }
        EP c = mk(hir::ExprKind::Call, cand.ret, e.span);
        c->text = cand.key;
        c->args = std::move(args);
        return c;
    }
    if (entry.kind == ScopeEntry::Kind::External) {
        EP c = mk(hir::ExprKind::ExternalCall, lower::unknownType(), e.span);
        c->text = entry.module;
        c->member = entry.member;
        for (const ExprPtr& a : e.args) {
            c->args.push_back(expr(*a, nullptr));
        }
        return c;
    }
    return nullptr;
}

Lowerer::EP Lowerer::callExpr(const Expr& e, const Type* expected) {
    const Expr& callee = *e.lhs;
    (void)expected;
    if (callee.kind == Expr::Kind::Name) {
        const std::string& name = callee.text;
        const ScopeEntry* entry = lookupName(name);
        const bool shadowed = entry != nullptr && entry->kind != ScopeEntry::Kind::Type;
        if (!shadowed) {
            if (name == "syscall") {
                return syscallExpr(e);
            }
            if (name == "sizeOf" || name == "alignOf" || name == "offsetOf") {
                const Type size_t_type = Type::makeInt(0, false);
                return constExpr(e, &size_t_type);
            }
            if (name == "staticAssert") {
                err("E0709", e.span, "`staticAssert` is a statement", "it has no value");
                return nullptr;
            }
            if (name.starts_with("__builtin_")) {
                return builtinCall(e, name);
            }
        }
        if (entry != nullptr && namesClass(*entry)) {
            return classConstruct(e, callee, *entry); // `T(args)`, `Vec<i32>(args)`
        }
        if (entry != nullptr) {
            if (EP c = callEntry(e, *entry, name)) {
                return c;
            }
            if (entry->kind == ScopeEntry::Kind::Function ||
                entry->kind == ScopeEntry::Kind::External) {
                return nullptr; // diagnosed
            }
        } else if (star_import_) {
            EP c = mk(hir::ExprKind::ExternalCall, lower::unknownType(), e.span);
            c->text = "*";
            c->member = name;
            for (const ExprPtr& a : e.args) {
                c->args.push_back(expr(*a, nullptr));
            }
            return c;
        }
    } else if (callee.kind == Expr::Kind::Field) {
        // `ns.fn(...)` (module namespace), `Enum.Variant(...)` or `value.method(...)`.
        if (const ScopeEntry* head = chainEntry(*callee.lhs); head != nullptr) {
            if (const auto sum = sumFor(*head, expected, e.span)) {
                return sumVariantCall(*sum, e);
            }
            if (isGenericPayloadEnum(*head)) {
                return nullptr; // diagnosed by `sumFor`
            }
        }
        if (const ScopeEntry* head = chainEntry(*callee.lhs);
            head != nullptr && head->kind == ScopeEntry::Kind::Namespace) {
            const ScopeEntry* member = namespaceMember(*head, callee.text, callee.span, true);
            if (member == nullptr) {
                return nullptr;
            }
            if (namesClass(*member)) {
                return classConstruct(e, callee, *member); // `ns.T(args)`, `ns.Vec<i32>(args)`
            }
            if (EP c = callEntry(e, *member, callee.text)) {
                return c;
            }
            if (member->kind == ScopeEntry::Kind::Function ||
                member->kind == ScopeEntry::Kind::External) {
                return nullptr;
            }
        }
        if (const ScopeEntry* ent = chainEntry(callee); ent == nullptr ||
                                                        ent->kind == ScopeEntry::Kind::Local ||
                                                        ent->kind == ScopeEntry::Kind::Global) {
            return methodCall(e, *callee.lhs);
        }
    }
    // Anything else: the callee must be a function pointer value.
    EP f = expr(callee, nullptr);
    if (!f) {
        return nullptr;
    }
    if (lower::isUnknown(f->type)) {
        EP c = mk(hir::ExprKind::CallIndirect, lower::unknownType(), e.span);
        c->lhs = std::move(f);
        for (const ExprPtr& a : e.args) {
            c->args.push_back(expr(*a, nullptr));
        }
        return c;
    }
    if (lower::isAggregate(f->type)) {
        return userCall(e, f->type); // `operator()`, and every closure that captures
    }
    if (f->type.kind != TypeKind::FnPtr || !f->type.ret) {
        err("E0709",
            e.span,
            std::format("`{}` is not callable", typeToString(f->type)),
            "expected a function or function pointer");
        return nullptr;
    }
    std::vector<EP> args;
    const std::string label = f->kind == hir::ExprKind::LocalRef ? f->text : "function pointer";
    if (!lowerArgs(e, f->type.params, args, label)) {
        return nullptr;
    }
    EP c = mk(f->kind == hir::ExprKind::FuncRef ? hir::ExprKind::Call : hir::ExprKind::CallIndirect,
              *f->type.ret,
              e.span);
    if (f->kind == hir::ExprKind::FuncRef) {
        c->text = f->text;
    } else {
        c->lhs = std::move(f);
    }
    c->args = std::move(args);
    return c;
}

} // namespace xlang::lower
