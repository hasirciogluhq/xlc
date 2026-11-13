// Lowering: the per-module driver: items, pending generic instances, lazy methods, batches.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

void Lowerer::lowerItems() {
    std::vector<const Item*> items;
    for (const Item& item : s_.program_.items) {
        items.push_back(&item);
    }
    lowerBatch(items);
    lowerImpls();
    lowerPendingInstances();
}

bool Lowerer::lowerPendingInstances() {
    bool any = false;
    for (std::vector<const Item*> batch = s_.takePendingInstances(); !batch.empty();
         batch = s_.takePendingInstances()) {
        any = true;
        lowerBatch(batch);
    }
    for (std::vector<const Sema::MethodInstance*> methods = s_.takePendingMethods();
         !methods.empty();
         methods = s_.takePendingMethods()) {
        any = true;
        for (const Sema::MethodInstance* m : methods) {
            const Type self = Type::makePointer(resolved(Type::makeNamed({m->cls})));
            lowerFunction(m->decl,
                          Modifiers{},
                          m->key,
                          m->cls + "." + m->decl.name,
                          m->decl.attributes,
                          m->cls_q,
                          &self,
                          false);
        }
    }
    s_.setCurrentInstance(nullptr);
    return any;
}

void Lowerer::lowerLazyMethod(const LowerShared::LazyMethod& m, const std::string& key) {
    s_.setCurrentInstance(m.item);
    lowerFunction(
        *m.decl, m.item->modifiers, key, m.local_name, m.decl->attributes, m.cls_q, &m.self, m.overloaded);
    s_.setCurrentInstance(nullptr);
}

void Lowerer::lowerBatch(const std::vector<const Item*>& items) {
    for (const Item* ip : items) {
        const Item& item = *ip;
        s_.setCurrentInstance(ip);
        if (!s_.genericParamsOf(item).empty() || Sema::isPackFn(item)) {
            continue; // a generic or pack declaration is lowered per instance
        }
        if (!s_.genericParamsOf(s_.genericEnumOf(declBase(item).name)).empty() &&
            std::holds_alternative<StructDecl>(item.decl) &&
            std::get<StructDecl>(item.decl).is_payload) {
            continue; // a payload struct of a generic enum is lowered per instance
        }
        if (std::holds_alternative<ImplDecl>(item.decl)) {
            // Only a generic impl instance is lowered here; the module's impls are in lowerImpls.
            const ImplInfo* found = nullptr;
            for (const ImplInfo& info : sh_.impls) {
                if (info.item == ip && info.instance) {
                    found = &info;
                }
            }
            if (found != nullptr) {
                lowerImplInfo(*found);
            }
            continue;
        }
        if (const auto* e = std::get_if<EnumDecl>(&item.decl)) {
            lowerEnum(*e);
        } else if (std::holds_alternative<StructDecl>(item.decl) ||
                   std::holds_alternative<ClassDecl>(item.decl)) {
            lowerAggregate(item);
        } else if (const auto* in = std::get_if<InterfaceDecl>(&item.decl)) {
            hir::Interface i;
            i.name = s_.qualify(in->name);
            i.span = in->span;
            for (const FnDecl& m : in->methods) {
                hir::InterfaceMethod im;
                im.name = m.name;
                for (const Param& p : m.params) {
                    if (!p.is_self) {
                        im.params.push_back(resolved(p.type));
                    }
                }
                im.ret = resolved(m.return_type);
                i.methods.push_back(std::move(im));
            }
            mod_.interfaces.push_back(std::move(i));
        } else if (const auto* g = std::get_if<GlobalDecl>(&item.decl)) {
            lowerGlobal(*g, item.modifiers);
        } else if (const auto* f = std::get_if<FnDecl>(&item.decl)) {
            const std::string qname = s_.qualify(f->name);
            const Sema::Symbol* sym = s_.find(f->name);
            const bool overloaded = sym != nullptr && sym->overloads.size() > 1;
            std::vector<Type> params;
            for (const Param& p : f->params) {
                params.push_back(resolved(p.type));
            }
            const std::string key = overloaded ? overloadKey(qname, params) : qname;
            if (!fn_keys_.insert(key).second) {
                // Two overloads with the same parameter types (E0401).
                Diagnostic d;
                d.code = "E0401";
                d.severity = Severity::Error;
                d.message = std::format("`{}` is defined twice", signatureOf(f->name, params));
                d.labels.push_back({f->name_span, "second definition"});
                d.fix = "give the overloads different parameter types";
                s_.diag_.report(std::move(d));
                continue;
            }
            lowerFunction(*f, item.modifiers, key, f->name, f->attributes, {}, nullptr, overloaded);
        }
    }
    // Methods after all signatures/types exist.
    for (const Item* ip : items) {
        const Item& item = *ip;
        s_.setCurrentInstance(ip);
        if (!s_.genericParamsOf(item).empty()) {
            continue;
        }
        if (const auto* cl = std::get_if<ClassDecl>(&item.decl)) {
            const std::string cq = s_.qualify(cl->name);
            const Type self = Type::makePointer(resolved(Type::makeNamed({cl->name})));
            for (const FnDecl& m : cl->methods) {
                const bool overloaded = std::ranges::count_if(cl->methods, [&](const FnDecl& o) {
                                            return o.name == m.name;
                                        }) > 1;
                if (!m.generic_params.empty()) {
                    // A generic method is lowered per instance, when a call names it (methodCall).
                    if (m.is_virtual || m.is_override) {
                        err("E0730",
                            m.name_span,
                            std::format("generic method `{}` cannot be virtual", m.name),
                            "remove `virtual`/`override`: a vtable slot has one fixed signature");
                    } else if (overloaded) {
                        err("E0405",
                            m.name_span,
                            std::format("generic method `{}` cannot be overloaded", m.name),
                            "keep one declaration with this name");
                    }
                    continue;
                }
                std::string key = cq + "." + m.name;
                if (overloaded) {
                    if (m.is_virtual || m.is_override) {
                        err("E0405",
                            m.name_span,
                            std::format("`{}` cannot be overloaded: virtual methods have one slot",
                                        m.name),
                            "keep one declaration with this name");
                    }
                    std::vector<Type> params;
                    for (const Param& p : m.params) {
                        if (!p.is_self) {
                            params.push_back(resolved(p.type));
                        }
                    }
                    key = overloadKey(key, params);
                }
                if (!fn_keys_.insert(key).second) {
                    err("E0401",
                        m.name_span,
                        std::format("`{}.{}` is defined twice", cl->name, m.name),
                        "give the overloads different parameter types");
                    continue;
                }
                if (m.name == "init") {
                    base_init_call_ = baseInitKey(*cl, s_);
                }
                if (s_.instanceInfo(cl->name) != nullptr && !m.is_virtual && !m.is_override &&
                    m.name != "init" && m.name != "drop") {
                    sh_.lazy_methods.emplace(
                        key,
                        LowerShared::LazyMethod{
                            &s_, ip, &m, cl->name + "." + m.name, cq, self, overloaded});
                    continue;
                }
                lowerFunction(m,
                              item.modifiers,
                              key,
                              cl->name + "." + m.name,
                              m.attributes,
                              cq,
                              &self,
                              overloaded);
            }
        }
    }
}

void Lowerer::run() {
    buildModuleScope();
    lowerItems();
}

void Lowerer::finish() {
    scopes_.pop();
}

bool Lowerer::isRegisteredName(const std::string& name) const {
    if (s_.comp_ != nullptr) {
        return s_.comp_->findRegistered(name).has_value();
    }
    return registered_names_.contains(name);
}

} // namespace xlang::lower
