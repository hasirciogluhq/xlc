// Lowering: the module scope: imported and declared names, name lookup and dotted chains.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

// ---------------------------------------------------------------------------------------------
// Module scope, registrations, impls

ScopeEntry Lowerer::entryOf(Sema::EntityRef ref, const std::string& local, const Span& span) {
    if (ref.sym->kind == Sema::Symbol::Kind::FnAlias) {
        // An alias stands for its target: the same function, overloads and link name.
        const auto& alias = std::get<FnAliasDecl>(ref.sym->item->decl);
        if (const auto target = ref.owner->resolveFnAlias(alias.target, alias.target_span)) {
            return entryOf(*target, local, span);
        }
        ScopeEntry failed;
        failed.name = local;
        failed.span = span;
        return failed;
    }
    ScopeEntry e;
    e.name = local;
    e.span = span;
    e.item = ref.sym->item;
    e.owner = ref.owner;
    e.qname = ref.qname();
    switch (ref.sym->kind) {
    case Sema::Symbol::Kind::Fn:
        e.kind = ScopeEntry::Kind::Function;
        e.overloads = ref.sym->overloads;
        break;
    case Sema::Symbol::Kind::Global: {
        const auto& decl = std::get<GlobalDecl>(ref.sym->item->decl);
        e.kind = ScopeEntry::Kind::Global;
        e.type = globalType(*ref.owner, decl);
        e.readonly = decl.is_const;
        break;
    }
    case Sema::Symbol::Kind::Define:
        e.kind = ScopeEntry::Kind::Constant;
        break;
    default:
        e.kind = ScopeEntry::Kind::Type;
        break;
    }
    return e;
}

void Lowerer::buildModuleScope() {
    scopes_.push(ScopeKind::Module);
    {
        ScopeEntry e;
        e.kind = ScopeEntry::Kind::Type;
        e.name = "MemoryOrder";
        e.item = &s_.memory_order_item_;
        e.owner = &s_;
        e.qname = "MemoryOrder";
        (void)scopes_.declare(std::move(e));
    }
    for (const Item& item : s_.program_.items) {
        const DeclBase& base = declBase(item);
        if (item.modifiers.registered) {
            registered_names_.insert(base.name);
        }
        if (const auto* imp = std::get_if<ImportDecl>(&item.decl)) {
            if (s_.comp_ != nullptr) {
                continue; // bindings come from the compilation below
            }
            // Standalone module: imports stay external until module loading is available.
            const auto bind = [&](ScopeEntry entry) {
                if (const ScopeEntry* prev = scopes_.declare(entry)) {
                    Diagnostic d;
                    d.code = "E0401";
                    d.severity = Severity::Error;
                    d.message = std::format("`{}` is bound twice", entry.name);
                    d.labels.push_back({prev->span, "first binding"});
                    d.labels.push_back({entry.span, "second binding"});
                    d.fix = "rename one of them with `as`";
                    s_.diag_.report(std::move(d));
                }
            };
            switch (imp->form) {
            case ImportDecl::Form::Module:
            case ImportDecl::Form::StarAs: {
                ScopeEntry ns;
                ns.kind = ScopeEntry::Kind::Namespace;
                ns.name = imp->alias.empty() ? imp->module : imp->alias;
                ns.module = imp->module;
                ns.span = imp->module_span;
                bind(std::move(ns));
                break;
            }
            case ImportDecl::Form::Star:
                star_import_ = true;
                break;
            case ImportDecl::Form::Names:
                for (const ImportName& n : imp->names) {
                    ScopeEntry ext;
                    ext.kind = ScopeEntry::Kind::External;
                    ext.name = n.alias.empty() ? n.name : n.alias;
                    ext.module = imp->module;
                    ext.member = n.name;
                    ext.span = n.span;
                    bind(std::move(ext));
                }
                break;
            }
            continue;
        }
        if (const auto* alias = std::get_if<FnAliasDecl>(&item.decl)) {
            if (const auto target = s_.resolveFnAlias(alias->target, alias->target_span)) {
                (void)scopes_.declare(entryOf(*target, base.name, base.name_span));
            }
            continue;
        }
        ScopeEntry e;
        e.name = base.name;
        e.span = base.name_span;
        e.item = &item;
        e.owner = &s_;
        e.qname = s_.qualify(base.name);
        if (std::holds_alternative<FnDecl>(item.decl)) {
            e.kind = ScopeEntry::Kind::Function;
            if (const Sema::Symbol* sym = s_.find(e.name)) {
                e.overloads = sym->overloads;
            }
        } else if (const auto* g = std::get_if<GlobalDecl>(&item.decl)) {
            e.kind = ScopeEntry::Kind::Global;
            e.type = globalType(s_, *g);
            e.readonly = g->is_const;
        } else if (std::holds_alternative<DefineConstDecl>(item.decl)) {
            e.kind = ScopeEntry::Kind::Constant;
        } else if (std::holds_alternative<StructDecl>(item.decl) ||
                   std::holds_alternative<ClassDecl>(item.decl) ||
                   std::holds_alternative<InterfaceDecl>(item.decl) ||
                   std::holds_alternative<EnumDecl>(item.decl) ||
                   std::holds_alternative<TypeAliasDecl>(item.decl)) {
            e.kind = ScopeEntry::Kind::Type;
        } else {
            continue;
        }
        // Duplicate definitions were already reported by `Sema::collect` (E0401).
        (void)scopes_.declare(std::move(e));
    }
    // Imports bound by the compilation.
    for (const auto& [name, b] : s_.imports_) {
        if (scopes_.lookupModule(name) != nullptr) {
            continue;
        }
        if (b.kind == Sema::ImportBinding::Kind::Namespace) {
            ScopeEntry ns;
            ns.kind = ScopeEntry::Kind::Namespace;
            ns.name = name;
            ns.module = b.module_key;
            ns.span = b.span;
            (void)scopes_.declare(std::move(ns));
            continue;
        }
        Sema* owner = s_.comp_->semaOf(b.module_key);
        const Sema::Symbol* sym = owner != nullptr ? owner->findLocal(b.name) : nullptr;
        if (sym == nullptr) {
            continue;
        }
        ScopeEntry e = entryOf(Sema::EntityRef{owner, sym, b.name}, name, b.span);
        (void)scopes_.declare(std::move(e));
    }
}

const ScopeEntry* Lowerer::lookupName(const std::string& name) {
    if (const ScopeEntry* e = scopes_.lookup(name)) {
        return e;
    }
    if (s_.comp_ != nullptr) {
        if (const auto it = extra_entries_.find("reg:" + name); it != extra_entries_.end()) {
            return &it->second;
        }
        if (const auto ref = s_.comp_->findRegistered(name)) {
            return &(extra_entries_["reg:" + name] =
                         entryOf(*ref, name, declBase(*ref->sym->item).name_span));
        }
    }
    return nullptr;
}

/// The entry a name chain (`a`, `a.b`, `a.b.c`) names: the last step is looked up inside the
/// namespace the step before it named. A chain that starts outside a namespace (a local, a field of
/// a struct) has no entry, and a chain whose base is not a namespace has none either.
const ScopeEntry* Lowerer::chainEntry(const Expr& e) {
    if (e.kind == Expr::Kind::Name) {
        return lookupName(e.text);
    }
    if (e.kind == Expr::Kind::Field) {
        const ScopeEntry* base = chainEntry(*e.lhs);
        if (base != nullptr && base->kind == ScopeEntry::Kind::Namespace) {
            return namespaceMember(*base, e.text, e.span, true);
        }
    }
    return nullptr;
}

const ScopeEntry* Lowerer::namespaceMember(const ScopeEntry& ns,
                                           const std::string& name,
                                           const Span& at,
                                           bool report) {
    if (s_.comp_ == nullptr) {
        // Standalone namespace: members are external names.
        const std::string key = "ext:" + ns.module + "." + name;
        ScopeEntry& e = extra_entries_[key];
        e.kind = ScopeEntry::Kind::External;
        e.name = name;
        e.module = ns.module;
        e.member = name;
        e.span = at;
        return &e;
    }
    ModuleInfo* m = s_.comp_->moduleOf(ns.module);
    if (m == nullptr) {
        return nullptr;
    }
    const auto it = m->exports.find(name);
    if (it == m->exports.end()) {
        if (report) {
            err("E0706",
                at,
                std::format("module `{}` has no exported name `{}`", ns.name, name),
                "only `export`ed names are visible");
        }
        return nullptr;
    }
    const std::string key = "ns:" + ns.module + "." + name;
    if (const auto cached = extra_entries_.find(key); cached != extra_entries_.end()) {
        return &cached->second;
    }
    ScopeEntry e;
    if (it->second.is_namespace) {
        e.kind = ScopeEntry::Kind::Namespace;
        e.name = name;
        e.module = it->second.module->key;
        e.span = at;
    } else if (const auto ref = s_.comp_->exportedEntity(ns.module, name)) {
        e = entryOf(*ref, name, at);
    } else {
        return nullptr;
    }
    return &(extra_entries_[key] = std::move(e));
}

} // namespace xlang::lower
