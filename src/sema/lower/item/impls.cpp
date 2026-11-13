// Lowering: `impl` blocks: collection, interface checks, generic impl instances and lowering.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

void Lowerer::collectImpls() {
    for (const Item& item : s_.program_.items) {
        const auto* impl = std::get_if<ImplDecl>(&item.decl);
        if (impl == nullptr) {
            continue;
        }
        if (!impl->generic_params.empty()) {
            // Every type parameter must be a direct argument of the target, so that matching the
            // target against an instance fixes it.
            for (const std::string& p : impl->generic_params) {
                const bool direct = std::ranges::any_of(impl->target.args, [&p](const Type& a) {
                    return a.kind == TypeKind::Named && a.path.size() == 1 && a.path.front() == p &&
                           a.args.empty();
                });
                if (!direct) {
                    err("E0732",
                        impl->span,
                        std::format("type parameter `{}` is not a direct argument of the target",
                                    p),
                        std::format("write it as an argument of the target, such as `{}<{}>`",
                                    lower::joinPath(impl->target.path),
                                    p));
                }
            }
            sh_.impl_templates.push_back(LowerShared::ImplTemplate{&item, &s_});
            continue;
        }
        const Type target = resolved(impl->target);
        if (!lower::isAggregate(target)) {
            if (!lower::isUnknown(target)) {
                err("E0706",
                    impl->span,
                    std::format("`{}` cannot implement an interface", typeToString(target)),
                    "only structs and classes have `impl`");
            }
            continue;
        }
        Type iface = Type::makeNamed(impl->interface_path);
        iface.args = impl->interface_args;
        iface.span = impl->span;
        const auto ir = s_.resolve(iface);
        if (!ir) {
            continue;
        }
        if (ir->kind != TypeKind::Interface) {
            err("E0706",
                impl->span,
                std::format("`{}` is not an interface", lower::joinPath(impl->interface_path)),
                "not declared as an interface");
            continue;
        }
        ImplInfo info;
        info.decl = impl;
        info.owner = &s_;
        info.item = &item;
        info.type_q = target.path.front();
        info.iface_q = ir->path.front();
        info.prefix = info.type_q + "." + info.iface_q + ".";
        info.span = impl->span;
        if (findImplExact(info.type_q, info.iface_q) != nullptr) {
            err("E0731",
                impl->span,
                std::format("`{}` implements `{}` twice", info.type_q, info.iface_q),
                "keep one `impl` for this type and interface");
            continue;
        }
        sh_.impls.push_back(std::move(info));
    }
}

void Lowerer::checkImpl(const ImplInfo& info) {
    const auto iref = s_.entityByQName(info.iface_q);
    if (!iref) {
        return;
    }
    const auto& idecl = std::get<InterfaceDecl>(iref->sym->item->decl);
    std::set<std::string> iface_names;
    for (const FnDecl& im : idecl.methods) {
        iface_names.insert(im.name);
        const FnDecl* found = nullptr;
        for (const FnDecl& m : info.decl->methods) {
            if (m.name == im.name) {
                found = &m;
            }
        }
        if (found == nullptr) {
            err("E0713",
                info.decl->span,
                std::format("`impl {} for {}` is missing `{}`", info.iface_q, info.type_q, im.name),
                "every interface method must be implemented");
            continue;
        }
        const Type want = fnTypeOf(*iref->owner, im);
        const Type have = fnTypeOf(s_, *found);
        if (!lower::sameType(want, have)) {
            err("E0713",
                found->name_span,
                std::format("`{}` has signature `{}`, the interface declares `{}`",
                            im.name,
                            typeToString(have),
                            typeToString(want)),
                "signature differs from the interface");
        }
    }
    for (const FnDecl& m : info.decl->methods) {
        if (!iface_names.contains(m.name)) {
            err("E0713",
                m.name_span,
                std::format("`{}` is not a method of `{}`", m.name, info.iface_q),
                "not part of the interface");
        }
    }
}

void Lowerer::lowerImpls() {
    // Indexed: lowering an impl can add impl instances to `sh_.impls`.
    for (std::size_t i = 0; i < sh_.impls.size(); ++i) {
        const ImplInfo& info = sh_.impls[i];
        if (info.owner != &s_ || info.instance) {
            continue;
        }
        lowerImplInfo(info);
    }
}

void Lowerer::lowerImplInfo(const ImplInfo& info) {
    checkImpl(info);
    Type target_type;
    if (const auto tref = s_.entityByQName(info.type_q)) {
        target_type.kind =
            tref->sym->kind == Sema::Symbol::Kind::Class ? TypeKind::Class : TypeKind::Struct;
        target_type.path = {info.type_q};
    }
    const Type self = Type::makePointer(target_type);
    const auto iref = s_.entityByQName(info.iface_q);
    hir::Impl out;
    out.interface = info.iface_q;
    out.type = info.type_q;
    out.span = info.span;
    for (const FnDecl& m : info.decl->methods) {
        if (!m.generic_params.empty()) {
            err("E0733",
                m.name_span,
                std::format("generic method `{}` is only supported in a class", m.name),
                "move the method into the class");
            continue;
        }
        lowerFunction(m,
                      Modifiers{},
                      info.prefix + m.name,
                      info.prefix + m.name,
                      m.attributes,
                      info.type_q,
                      &self);
    }
    if (iref) {
        for (const FnDecl& im : std::get<InterfaceDecl>(iref->sym->item->decl).methods) {
            out.methods.push_back(info.prefix + im.name);
        }
    }
    mod_.impls.push_back(std::move(out));
}

const Lowerer::ImplInfo* Lowerer::findImpl(const std::string& type_q, const std::string& iface_q) {
    if (const ImplInfo* found = findImplExact(type_q, iface_q)) {
        return found;
    }
    if (instantiateImpls()) {
        return findImplExact(type_q, iface_q);
    }
    return nullptr;
}

bool Lowerer::instantiateImpls() {
    bool any = false;
    for (bool created = true; created;) {
        created = false;
        for (const LowerShared::ImplTemplate& t : sh_.impl_templates) {
            const auto& decl = std::get<ImplDecl>(t.item->decl);
            // The generic aggregate the template's target names (E0706 was reported in collect).
            const auto ref = decl.target.path.size() == 1
                                 ? t.owner->lookupEntity(decl.target.path.front())
                                 : t.owner->lookupQualified(decl.target.path);
            if (!ref || ref->sym->item == nullptr || s_.genericParamsOf(*ref->sym->item).empty()) {
                continue;
            }
            const std::string generic = ref->qname();
            for (Sema* m : sh_.modules) {
                for (const auto& [local, info] : m->instances()) {
                    if (info.generic != generic || info.args.size() != decl.target.args.size()) {
                        continue;
                    }
                    if (!sh_.impl_done.insert({t.item, m->qualify(local)}).second) {
                        continue;
                    }
                    created = true;
                    instantiateImpl(t, *m, local, info);
                }
            }
        }
        any = any || created;
    }
    return any;
}

void Lowerer::instantiateImpl(const LowerShared::ImplTemplate& t,
                              Sema& m,
                              const std::string& local,
                              const Sema::InstanceInfo& info) {
    const auto& decl = std::get<ImplDecl>(t.item->decl);
    std::map<std::string, Type> bound;
    for (std::size_t i = 0; i < info.args.size(); ++i) {
        if (!unifyParam(decl.target.args[i], info.args[i], decl.generic_params, bound, *t.owner)) {
            return; // this impl does not apply to the instance
        }
    }
    Type target;
    target.kind =
        m.findLocal(local)->kind == Sema::Symbol::Kind::Class ? TypeKind::Class : TypeKind::Struct;
    target.path = {m.qualify(local)};
    target.args = info.args;
    target.span = decl.span;
    Type iface = Type::makeNamed(decl.interface_path);
    iface.span = decl.span;
    for (const Type& a : decl.interface_args) {
        iface.args.push_back(substituteType(a, bound));
    }
    const auto ir = t.owner->resolve(iface);
    if (!ir) {
        return;
    }
    if (ir->kind != TypeKind::Interface) {
        err("E0706",
            decl.span,
            std::format("`{}` is not an interface", lower::joinPath(decl.interface_path)),
            "not declared as an interface");
        return;
    }
    const std::string type_q = target.path.front();
    const std::string iface_q = ir->path.front();
    if (findImplExact(type_q, iface_q) != nullptr) {
        err("E0731",
            decl.span,
            std::format("`{}` implements `{}` twice", type_q, iface_q),
            "keep one `impl` for this type and interface");
        return;
    }
    Item inst = instantiateItem(*t.item, bound, type_q + "." + iface_q);
    std::get<ImplDecl>(inst.decl).target = target;
    const Item* item = t.owner->addImplInstance(std::move(inst));
    ImplInfo out;
    out.decl = &std::get<ImplDecl>(item->decl);
    out.owner = t.owner;
    out.item = item;
    out.type_q = type_q;
    out.iface_q = iface_q;
    out.prefix = type_q + "." + iface_q + ".";
    out.span = decl.span;
    out.instance = true;
    sh_.impls.push_back(std::move(out));
}

} // namespace xlang::lower
