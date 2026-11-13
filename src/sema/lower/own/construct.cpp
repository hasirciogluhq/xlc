// Lowering: construction: `init` lookup, constructor calls and generic construction.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

bool Lowerer::hasInit(const std::string& qname) {
    const auto ref = s_.entityByQName(qname);
    const auto* c = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
    return c != nullptr &&
           std::ranges::any_of(c->methods, [](const FnDecl& m) { return m.name == "init"; });
}

std::string Lowerer::zeroArgInitOf(const std::string& qname) {
    const auto ref = s_.entityByQName(qname);
    const auto* c = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
    if (c == nullptr) {
        return {};
    }
    for (const FnDecl& m : c->methods) {
        if (m.name == "init" &&
            std::ranges::all_of(m.params, [](const Param& p) { return p.is_self; })) {
            return qname + ".init";
        }
    }
    return {};
}

std::string Lowerer::baseInitKey(const ClassDecl& cl, Sema& owner) {
    if (!cl.has_base) {
        return {};
    }
    const auto base = owner.resolve(Type::makeNamed(cl.base));
    if (!base || base->kind != TypeKind::Class) {
        return {};
    }
    return zeroArgInitOf(base->path.front());
}

Lowerer::EP Lowerer::genericConstruct(const Expr& e, const Expr& callee, const ScopeEntry& entry) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    const std::string local = declBase(*entry.item).name;
    const Sema::Symbol* sym = owner.find(local);
    if (sym == nullptr || callee.generic_args.empty()) {
        err("E0724",
            e.span,
            std::format("cannot infer the type arguments of `{}`", local),
            std::format("write them: `{}<...>(...)`", local));
        return nullptr;
    }
    std::vector<Type> targs;
    for (const Type& a : callee.generic_args) {
        targs.push_back(resolved(a));
    }
    const auto inst = owner.instantiate(Sema::EntityRef{&owner, sym, local}, targs, e.span);
    if (!inst) {
        return nullptr;
    }
    return constructCall(e, entryOf(*inst, entry.name, e.span));
}

bool Lowerer::lowerInitCall(const Expr& e, const Type& t, std::string& key, std::vector<EP>& args) {
    const auto ref = s_.entityByQName(t.path.front());
    const auto* cls = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
    key.clear();
    args.clear();
    if (cls == nullptr) {
        return false;
    }
    MethodRef m;
    m.kind = MethodRef::Kind::Direct;
    m.cls = cls;
    m.cls_q = t.path.front();
    m.owner = ref->owner;
    const auto cands = methodCandidates(m, "init");
    if (cands.empty()) {
        if (!e.args.empty()) {
            err("E0710",
                e.span,
                std::format("`{}` has no `init`; it takes no arguments", cls->name),
                "write an `init` method to take arguments");
            return false;
        }
        return true;
    }
    std::size_t pick = 0;
    if (cands.size() > 1) {
        std::vector<PreArg> pre = preLowerArgs(e);
        const auto chosen = resolveOverload(e, cls->name, cands, pre);
        if (!chosen) {
            return false;
        }
        pick = *chosen;
        if (!lowerArgsPre(e, cands[pick].params, pre, args, cls->name)) {
            return false;
        }
    } else if (!lowerArgs(e, cands[pick].params, args, cls->name)) {
        return false;
    }
    key = cands[pick].key;
    return true;
}

Lowerer::EP Lowerer::constructCall(const Expr& e, const ScopeEntry& entry) {
    const auto& cls = std::get<ClassDecl>(entry.item->decl);
    const std::string qname = entry.qname.empty() ? cls.name : entry.qname;
    Type t = Type::makeNamed({qname});
    t.kind = TypeKind::Class;
    t.span = e.span;
    std::string key;
    std::vector<EP> args;
    if (!lowerInitCall(e, t, key, args)) {
        return nullptr;
    }
    EP out = mk(hir::ExprKind::Construct, t, e.span);
    out->target = t;
    out->text = key;
    out->args = std::move(args);
    return out;
}

} // namespace xlang::lower
