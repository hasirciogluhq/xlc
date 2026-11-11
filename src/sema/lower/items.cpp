#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

// Module scope, registrations, impls, vtables and every declaration (`lower_items` part of the
// lowering pass).

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

namespace {

[[nodiscard]] bool isVoidPointer(const Type& t) {
    return t.kind == TypeKind::Pointer && t.elem && t.elem->kind == TypeKind::Void;
}

} // namespace

void Lowerer::checkRegistrations() {
    for (const Item& item : s_.program_.items) {
        if (!item.modifiers.registered || item.modifiers.register_role.empty()) {
            continue;
        }
        const DeclBase& base = declBase(item);
        const std::string& role = item.modifiers.register_role;
        const auto* fn = std::get_if<FnDecl>(&item.decl);
        bool ok = false;
        std::string why;
        if (role == "panic") {
            ok = fn != nullptr && fn->params.size() == 1 && !fn->params[0].is_self &&
                 fn->return_type.isVoid();
            if (ok) {
                const Type p = resolved(fn->params[0].type);
                ok = p.kind == TypeKind::Pointer && p.elem && p.elem->kind == TypeKind::Int &&
                     p.elem->bits == 8 && !p.elem->is_signed;
            }
            why = "`register(panic)` needs `fn(msg: *u8)` with no return type";
        } else if (role == "alloc") {
            ok = fn != nullptr && fn->params.size() == 2 && !fn->params[0].is_self &&
                 !fn->params[1].is_self && isVoidPointer(resolved(fn->return_type));
            if (ok) {
                const Type a = resolved(fn->params[0].type);
                const Type b = resolved(fn->params[1].type);
                ok = a.kind == TypeKind::Int && !a.is_signed && b.kind == TypeKind::Int &&
                     !b.is_signed && a.bits == 0 && b.bits == 0; // `usize`
            }
            why = "`register(alloc)` needs `fn(size: usize, align: usize) -> *void`";
        } else if (role == "free") {
            ok = fn != nullptr && fn->params.size() == 1 && !fn->params[0].is_self &&
                 fn->return_type.isVoid() && isVoidPointer(resolved(fn->params[0].type));
            why = "`register(free)` needs `fn(p: *void)` with no return type";
        } else if (role == "unwind") {
            ok = fn != nullptr && fn->params.size() == 3 && fn->return_type.isVoid();
            if (ok) {
                const Type a = resolved(fn->params[0].type);
                const Type b = resolved(fn->params[1].type);
                const Type c = resolved(fn->params[2].type);
                const auto bytes = [](const Type& t) {
                    return t.kind == TypeKind::Pointer && t.elem && t.elem->kind == TypeKind::Int &&
                           t.elem->bits == 8 && !t.elem->is_signed;
                };
                ok = a.kind == TypeKind::Int && a.bits == 32 && a.is_signed &&
                     b.kind == TypeKind::Pointer && b.elem && b.elem->kind == TypeKind::Int &&
                     b.elem->bits == 0 && !b.elem->is_signed && bytes(c); // `*usize`, `*u8`
            }
            why = "`register(unwind)` needs `fn(op: i32, snapshot: *usize, message: *u8)` with no "
                  "return type";
        } else {
            why = std::format("unknown role `{}`; known roles: panic, alloc, free, unwind", role);
        }
        if (!ok) {
            err("E0503",
                base.name_span,
                std::format("`{}` does not fit role `{}`", base.name, role),
                why);
            continue;
        }
        const std::string qname = s_.qualify(base.name);
        const LowerShared::Role info{
            base.name_span, base.name, qname, is_library_, &s_, s_.findLocal(base.name)};
        const auto [it, inserted] = sh_.roles.emplace(role, info);
        if (!inserted) {
            if (it->second.from_library == is_library_) {
                Diagnostic d;
                d.code = "E0502";
                d.severity = Severity::Error;
                d.message = std::format("role `{}` is registered twice", role);
                d.labels.push_back({it->second.span, "first registration"});
                d.labels.push_back({base.name_span, "second registration"});
                d.fix = "keep one registration per role in your program; a library default is "
                        "replaced by registering the role in your own code";
                s_.diag_.report(std::move(d));
                continue;
            }
            // A library default yields to the program; a library registration never replaces one.
            if (is_library_) {
                continue;
            }
            it->second = info;
        }
        if (role == "panic") {
            mod_.panic_function = qname;
        } else if (role == "alloc") {
            mod_.alloc_function = qname;
        } else if (role == "free") {
            mod_.free_function = qname;
        } else {
            mod_.unwind_function = qname;
        }
    }
}

void Lowerer::checkHookFunctions() {
    // Attribute name, and the code reported when it is put on something that cannot be that hook.
    struct Hook {
        std::string_view attr;
        const char* code;
    };
    constexpr std::array<Hook, 3> hooks{{{"init", "E0729"}, {"fini", "E0729"}, {"entry", "E0752"}}};
    for (const Item& item : s_.program_.items) {
        const DeclBase& base = declBase(item);
        if (const auto* cls = std::get_if<ClassDecl>(&item.decl)) {
            for (const FnDecl& m : cls->methods) {
                for (const Hook& h : hooks) {
                    if (const Attribute* at = lower::findAttribute(m.attributes, h.attr)) {
                        err(h.code,
                            at->span,
                            std::format("`@{}` on method `{}.{}` is not allowed",
                                        h.attr,
                                        base.name,
                                        m.name),
                            std::format("`@{}` is for module-level functions", h.attr));
                    }
                }
            }
        }
        const auto* fn = std::get_if<FnDecl>(&item.decl);
        for (const Hook& h : hooks) {
            const Attribute* at = lower::findAttribute(base.attributes, h.attr);
            if (at == nullptr) {
                continue;
            }
            // Init and fini run without arguments; the entry's signature is checked with its types.
            const bool no_params = h.attr != "entry";
            if (fn == nullptr || !fn->generic_params.empty() || !fn->has_body ||
                (no_params && (!fn->params.empty() || !fn->return_type.isVoid()))) {
                err(h.code,
                    at->span,
                    std::format("`@{}` on `{}` needs a plain function", h.attr, base.name),
                    no_params
                        ? std::format("write `@{} fn name() {{ ... }}`: no generics, parameters, "
                                      "return type or declaration without a body",
                                      h.attr)
                        : "write `@entry fn main()`: no generics, methods or declaration without a "
                          "body");
                continue;
            }
            const std::string qname = s_.qualify(base.name);
            if (h.attr == "init") {
                sh_.init_fns.insert(qname);
                init_order_.push_back(qname);
            } else if (h.attr == "fini") {
                sh_.fini_fns.insert(qname);
                fini_order_.push_back(qname);
            } else if (!sh_.entry_fn.empty()) {
                err("E0753",
                    at->span,
                    std::format("`@entry` on `{}` but the program already has `@entry` on `{}`",
                                base.name,
                                sh_.entry_fn),
                    "a program has exactly one `@entry` function: remove the second one");
            } else {
                sh_.entry_fn = qname;
            }
        }
    }
}

void Lowerer::checkEntrySignature(const FnDecl& f,
                                  const std::vector<Type>& params,
                                  const Type& ret) {
    const auto isUi8 = [](const Type& t) {
        return t.kind == TypeKind::Int && t.bits == 8 && !t.is_signed;
    };
    const auto isPtrTo = [](const Type& t, const auto& pred) {
        return t.kind == TypeKind::Pointer && t.elem != nullptr && pred(*t.elem);
    };
    const bool ret_ok =
        ret.isVoid() || (ret.kind == TypeKind::Int && ret.bits == 32 && ret.is_signed);
    const bool argc_ok = params.size() >= 1 && params[0].kind == TypeKind::Int &&
                         params[0].bits == 32 && params[0].is_signed;
    const bool argv_ok =
        params.size() >= 2 && isPtrTo(params[1], [&](const Type& t) { return isPtrTo(t, isUi8); });
    const bool params_ok = params.empty() || (params.size() == 2 && argc_ok && argv_ok);
    if (!ret_ok || !params_ok) {
        err("E0752",
            f.name_span,
            std::format("`@entry` function `{}` has an invalid signature", f.name),
            "write `@entry fn name()` or `@entry fn name(argc: i32, argv: **u8) -> i32`: "
            "the return type is void or i32");
    }
}

// ---------------------------------------------------------------------------------------------
// Vtables (plan 4.4)

const std::vector<LowerShared::SlotInfo>& Lowerer::vtableOf(const std::string& qname) {
    static const std::vector<SlotInfo> empty;
    if (const auto it = sh_.vtables.find(qname); it != sh_.vtables.end()) {
        return it->second;
    }
    const auto ref = s_.entityByQName(qname);
    const ClassDecl* cd = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
    if (cd == nullptr || !sh_.vtables_in_progress.insert(qname).second) {
        return empty;
    }
    Sema& owner = *ref->owner;
    std::vector<SlotInfo> slots;
    if (cd->has_base) {
        Type base = Type::makeNamed(cd->base);
        base.span = cd->name_span;
        if (const auto r = owner.resolve(base); r && r->kind == TypeKind::Class) {
            slots = vtableOf(r->path.front());
        }
    }
    const auto signature = [&](Sema& sema, const FnDecl& f) {
        std::string s;
        for (const Param& p : f.params) {
            if (!p.is_self) {
                s += typeToString(resolvedIn(sema, p.type)) + ",";
            }
        }
        return s + "->" + typeToString(resolvedIn(sema, f.return_type));
    };
    for (const FnDecl& m : cd->methods) {
        const std::string fname = qname + "." + m.name;
        std::size_t idx = slots.size();
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].slot.method == m.name) {
                idx = i;
            }
        }
        const bool found = idx != slots.size();
        if (m.is_override) {
            if (!found) {
                owner.error("E0712",
                            m.name_span,
                            std::format("`{}` overrides nothing", m.name),
                            "no virtual method with this name in a base class");
                continue;
            }
            if (signature(owner, m) != signature(*slots[idx].owner, *slots[idx].decl)) {
                owner.error("E0712",
                            m.name_span,
                            std::format("override `{}` has a different signature", m.name),
                            "parameters and return type must match the base method");
            }
            slots[idx] = SlotInfo{{m.name, fname}, &m, &owner};
        } else if (m.is_virtual) {
            if (found) {
                owner.error("E0712",
                            m.name_span,
                            std::format("`{}` is already virtual in a base class", m.name),
                            "write `override fn` to replace it");
                continue;
            }
            slots.push_back(SlotInfo{{m.name, fname}, &m, &owner});
        } else if (found) {
            owner.error("E0712",
                        m.name_span,
                        std::format("`{}` hides a virtual method of a base class", m.name),
                        "write `override fn` to replace it");
        }
    }
    sh_.vtables_in_progress.erase(qname);
    return sh_.vtables[qname] = std::move(slots);
}

// ---------------------------------------------------------------------------------------------
// Items

bool Lowerer::cCompatible(Sema& sema, const Type& t) {
    switch (t.kind) {
    case TypeKind::Void:
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::FnPtr:
    case TypeKind::Pointer:
        return true;
    case TypeKind::Enum:
        return t.elem && t.elem->kind == TypeKind::Int;
    case TypeKind::Struct: {
        const auto ref = sema.entityByQName(t.path.front());
        if (!ref) {
            return false;
        }
        const Attribute* repr = lower::findAttribute(declBase(*ref->sym->item).attributes, "repr");
        return repr != nullptr &&
               std::ranges::any_of(repr->args, [](const AttributeArg& a) { return a.text == "c"; });
    }
    default:
        return false;
    }
}

void Lowerer::assignSymbol(const std::vector<Attribute>& attrs,
                           bool export_c,
                           bool declared,
                           const std::string& local_name,
                           const std::vector<Type>& params,
                           bool overloaded,
                           std::string& symbol,
                           const Span& at) {
    if (!overloaded && !export_c) {
        if (const Attribute* m = lower::findAttribute(attrs, "mangle");
            m != nullptr && !m->args.empty() && m->args[0].kind == AttributeArg::Kind::Ident &&
            m->args[0].text != "none") {
            err("E0705", m->args[0].span, "`@mangle` takes `none` or a string", "invalid argument");
        }
    }
    symbol = s_.linkName(attrs, export_c, declared, local_name, params, overloaded);
    if (symbol == "_start" || symbol == "mainCRTStartup" || symbol == "_xl_initAll") {
        err("E0726",
            at,
            std::format("symbol `{}` is reserved for the compiler's entry point", symbol),
            "rename it: the compiler generates this symbol to start the program");
    }
    const auto [it, inserted] = sh_.link_names.emplace(symbol, at);
    // Plain names must be unique; mangled names are unique by their parameter types.
    if (!inserted && !declared && symbol.find("_xl$") != 0) {
        Diagnostic d;
        d.code = "E0401";
        d.severity = Severity::Error;
        d.message = std::format("symbol `{}` is defined twice", symbol);
        d.labels.push_back({it->second, "first definition"});
        d.labels.push_back({at, "second definition"});
        d.fix = "enable name mangling or rename one with `@mangle(\"...\")`";
        s_.diag_.report(std::move(d));
    }
}

std::string Lowerer::overloadKey(const std::string& qname, const std::vector<Type>& params) {
    std::string key = qname;
    for (const Type& p : params) {
        key += "$" + mangleType(p);
    }
    return key;
}

std::string Lowerer::signatureOf(const std::string& name, const std::vector<Type>& params) {
    std::string out = name + "(";
    for (std::size_t i = 0; i < params.size(); ++i) {
        out += (i != 0 ? ", " : "") + typeToString(params[i]);
    }
    return out + ")";
}

std::vector<Lowerer::OverloadCandidate> Lowerer::methodCandidates(const MethodRef& m,
                                                                  const std::string& name) {
    const std::string qname = m.cls_q + "." + name;
    std::vector<OverloadCandidate> out;
    for (const FnDecl& fm : m.cls->methods) {
        if (fm.name != name) {
            continue;
        }
        OverloadCandidate c;
        c.decl = &fm;
        for (const Param& p : fm.params) {
            if (!p.is_self) {
                c.params.push_back(resolvedIn(*m.owner, p.type));
            }
        }
        c.ret = resolvedIn(*m.owner, fm.return_type);
        out.push_back(std::move(c));
    }
    const bool overloaded = out.size() > 1;
    for (OverloadCandidate& c : out) {
        c.key = overloaded ? overloadKey(qname, c.params) : qname;
    }
    return out;
}

bool Lowerer::unifyParam(const Type& pattern,
                         const Type& actual,
                         const std::vector<std::string>& names,
                         std::map<std::string, Type>& bound,
                         Sema& owner) {
    if (pattern.kind == TypeKind::Named && pattern.path.size() == 1 && pattern.args.empty() &&
        std::ranges::find(names, pattern.path.front()) != names.end()) {
        const auto it = bound.find(pattern.path.front());
        if (it != bound.end()) {
            return lower::sameType(it->second, actual);
        }
        bound.emplace(pattern.path.front(), actual);
        return true;
    }
    if (pattern.kind == TypeKind::Pointer || pattern.kind == TypeKind::Array) {
        return actual.kind == pattern.kind && pattern.elem && actual.elem &&
               unifyParam(*pattern.elem, *actual.elem, names, bound, owner);
    }
    if (pattern.kind == TypeKind::FnPtr) {
        if (actual.kind != TypeKind::FnPtr || pattern.params.size() != actual.params.size() ||
            !pattern.ret || !actual.ret) {
            return false;
        }
        for (std::size_t i = 0; i < pattern.params.size(); ++i) {
            if (!unifyParam(pattern.params[i], actual.params[i], names, bound, owner)) {
                return false;
            }
        }
        return unifyParam(*pattern.ret, *actual.ret, names, bound, owner);
    }
    if (pattern.kind == TypeKind::Named && !pattern.args.empty()) {
        // `Vec<T>` against an instance of a generic class: the instance must come from `Vec`. The
        // generic name is looked up rather than resolved, because a bare generic name is E0724.
        std::optional<Sema::EntityRef> base;
        if (pattern.path.size() == 1) {
            base = pattern.path.front().find("::") != std::string::npos
                       ? owner.entityByQName(pattern.path.front())
                       : owner.lookupEntity(pattern.path.front());
        } else if (pattern.path.size() == 2) {
            base = owner.lookupQualified(pattern.path);
        }
        if (!base || (actual.kind != TypeKind::Class && actual.kind != TypeKind::Struct) ||
            actual.path.empty()) {
            return false;
        }
        const auto ref = s_.entityByQName(actual.path.front());
        const Sema::InstanceInfo* info = ref ? ref->owner->instanceInfo(ref->local) : nullptr;
        if (info == nullptr || info->generic != base->qname() ||
            info->args.size() != pattern.args.size()) {
            return false;
        }
        for (std::size_t i = 0; i < pattern.args.size(); ++i) {
            if (!unifyParam(pattern.args[i], info->args[i], names, bound, owner)) {
                return false;
            }
        }
        return true;
    }
    const auto concrete = owner.resolve(pattern);
    return concrete && lower::sameType(*concrete, actual);
}

std::vector<Lowerer::PreArg> Lowerer::preLowerArgs(const Expr& call) {
    std::vector<PreArg> out;
    for (const ExprPtr& a : call.args) {
        PreArg p;
        switch (a->kind) {
        case Expr::Kind::IntLiteral:
            p.literal = true;
            p.type =
                a->int_value > 2147483647ULL ? Type::makeInt(64, true) : Type::makeInt(32, true);
            break;
        case Expr::Kind::FloatLiteral:
            p.literal = true;
            p.type = Type::makeFloat(64);
            break;
        case Expr::Kind::StringLiteral:
            p.literal = true;
            p.type = Type::makeStr();
            break;
        case Expr::Kind::Name:
            if (a->text == "nil") {
                p.literal = true;
                p.type = lower::unknownType();
                break;
            }
            [[fallthrough]];
        default:
            p.value = expr(*a, nullptr);
            p.type = p.value ? p.value->type : lower::unknownType();
            break;
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Lowerer::OverloadCandidate> Lowerer::genericCandidates(const ScopeEntry& entry,
                                                                   const Expr& call,
                                                                   const std::vector<PreArg>& pre) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    std::vector<const Item*> items = entry.overloads;
    if (items.empty()) {
        items.push_back(entry.item);
    }
    const std::vector<Type> no_args;
    std::vector<OverloadCandidate> out;
    for (const Item* item : items) {
        const std::vector<std::string> names = s_.genericParamsOf(*item);
        if (names.empty()) {
            continue;
        }
        const std::string local = declBase(*item).name;
        if (owner.find(local) == nullptr) {
            continue;
        }
        // The symbol of this overload (the name's own symbol shows its first declaration only).
        Sema::Symbol this_overload;
        this_overload.kind = Sema::Symbol::Kind::Fn;
        this_overload.item = item;
        const Sema::EntityRef gref{&owner, &this_overload, local};
        const auto& decl = std::get<FnDecl>(item->decl);
        std::size_t arity = 0;
        for (const Param& p : decl.params) {
            arity += p.is_self ? 0 : 1;
        }
        if (arity != call.args.size()) {
            continue;
        }
        std::vector<Type> targs;
        const std::vector<Type>& written = call.lhs ? call.lhs->generic_args : no_args;
        if (!written.empty()) {
            if (written.size() != names.size()) {
                continue;
            }
            for (const Type& a : written) {
                targs.push_back(resolved(a));
            }
        } else {
            std::map<std::string, Type> bound;
            bool ok = true;
            std::size_t i = 0;
            for (const Param& p : decl.params) {
                if (p.is_self) {
                    continue;
                }
                if (lower::isUnknown(pre[i].type) ||
                    !unifyParam(p.type, pre[i].type, names, bound, owner)) {
                    ok = false;
                    break;
                }
                ++i;
            }
            if (!ok) {
                continue;
            }
            for (const std::string& n : names) {
                const auto it = bound.find(n);
                if (it == bound.end()) {
                    err("E0724",
                        call.span,
                        std::format("cannot infer `{}`", n),
                        std::format("write the type arguments: `{}<...>(...)`", local));
                    return out;
                }
                targs.push_back(it->second);
            }
        }
        const auto inst = owner.instantiate(gref, targs, call.span);
        if (!inst) {
            continue;
        }
        const auto& idecl = std::get<FnDecl>(inst->sym->item->decl);
        OverloadCandidate c;
        c.decl = &idecl;
        for (const Param& p : idecl.params) {
            if (!p.is_self) {
                c.params.push_back(resolvedIn(owner, p.type));
            }
        }
        c.ret = resolvedIn(owner, idecl.return_type);
        c.key = inst->qname();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Lowerer::OverloadCandidate>
Lowerer::packCandidates(const ScopeEntry& entry, const Expr& call, const std::vector<PreArg>& pre) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    std::vector<const Item*> items = entry.overloads;
    if (items.empty()) {
        items.push_back(entry.item);
    }
    std::vector<OverloadCandidate> out;
    for (const Item* item : items) {
        if (!Sema::isPackFn(*item)) {
            continue;
        }
        const auto& decl = std::get<FnDecl>(item->decl);
        std::size_t fixed = 0;
        for (const Param& p : decl.params) {
            fixed += (p.is_self || p.is_pack) ? 0 : 1;
        }
        if (call.args.size() < fixed) {
            continue;
        }
        std::vector<Type> elements;
        bool known = true;
        for (std::size_t i = fixed; i < pre.size(); ++i) {
            known = known && !lower::isUnknown(pre[i].type);
            elements.push_back(pre[i].type);
        }
        if (!known) {
            continue; // an argument of unknown type (`nil`): no instance can be made for it
        }
        const std::string local = declBase(*item).name;
        const auto inst = owner.instantiatePack(*item, local, elements, call.span);
        if (!inst) {
            continue;
        }
        const auto& idecl = std::get<FnDecl>(inst->sym->item->decl);
        OverloadCandidate c;
        c.decl = &idecl;
        for (const Param& p : idecl.params) {
            if (!p.is_self) {
                c.params.push_back(resolvedIn(owner, p.type));
            }
        }
        c.ret = resolvedIn(owner, idecl.return_type);
        c.key = inst->qname();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Lowerer::OverloadCandidate> Lowerer::candidatesOf(const ScopeEntry& entry) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    std::vector<const Item*> items = entry.overloads;
    if (items.empty()) {
        items.push_back(entry.item);
    }
    const bool overloaded = items.size() > 1;
    const std::string qname = entry.qname.empty() ? entry.name : entry.qname;
    std::vector<OverloadCandidate> out;
    for (const Item* item : items) {
        if (!s_.genericParamsOf(*item).empty() || Sema::isPackFn(*item)) {
            continue; // generic and pack overloads are instantiated per call (genericCandidates,
                      // packCandidates)
        }
        OverloadCandidate c;
        c.decl = &std::get<FnDecl>(item->decl);
        for (const Param& p : c.decl->params) {
            c.params.push_back(resolvedIn(owner, p.type));
        }
        c.ret = resolvedIn(owner, c.decl->return_type);
        c.key = overloaded ? overloadKey(qname, c.params) : qname;
        out.push_back(std::move(c));
    }
    return out;
}

namespace {

/// Cost of passing `arg`, whose type is `from`, to a parameter of type `param` (lower is better).
/// Untyped literals use their own rules; a typed argument costs 0 when the types match, 1 for
/// integer widening or wrapping in an interface, 2 for an integer to float conversion and 3 for
/// other conversions.
[[nodiscard]] int argumentCost(const Expr& arg, const Type& from, const Type& param) {
    switch (arg.kind) {
    case Expr::Kind::IntLiteral:
        if (param.kind == TypeKind::Int) {
            return param.bits == 64 && param.is_signed ? 1 : 2;
        }
        return 3;
    case Expr::Kind::FloatLiteral:
        if (param.kind == TypeKind::Float) {
            return param.bits == 64 ? 1 : 2;
        }
        return 3;
    case Expr::Kind::StringLiteral:
        if (param.kind == TypeKind::Str) {
            return 0;
        }
        if (param.kind == TypeKind::Pointer) {
            return 1;
        }
        return param.kind == TypeKind::Array ? 2 : 3;
    case Expr::Kind::Name:
        if (arg.text == "nil") {
            return 1;
        }
        break;
    default:
        break;
    }
    if (lower::isUnknown(from) || lower::sameType(from, param)) {
        return 0;
    }
    if (param.kind == TypeKind::Int && from.kind == TypeKind::Int) {
        return 1;
    }
    if (param.kind == TypeKind::Interface) {
        return 1;
    }
    if (param.kind == TypeKind::Float &&
        (from.kind == TypeKind::Int || from.kind == TypeKind::Float)) {
        return 2;
    }
    return 3;
}

/// What an argument looks like in a diagnostic.
[[nodiscard]] std::string argumentText(const Expr& arg, const Type& from) {
    switch (arg.kind) {
    case Expr::Kind::IntLiteral:
        return "integer literal";
    case Expr::Kind::FloatLiteral:
        return "float literal";
    case Expr::Kind::StringLiteral:
        return "string literal";
    default:
        break;
    }
    return lower::isUnknown(from) ? "unknown" : typeToString(from);
}

} // namespace

bool Lowerer::convertible(const hir::Expr& value, const Type& to) {
    if (lower::isUnknown(value.type) || lower::isUnknown(to)) {
        return true;
    }
    if (to.kind == TypeKind::Interface) {
        if (value.type.kind == TypeKind::Interface) {
            return lower::sameType(value.type, to);
        }
        const bool pointer = value.type.kind == TypeKind::Pointer && value.type.elem &&
                             lower::isAggregate(*value.type.elem);
        if (pointer || lower::isAggregate(value.type)) {
            if (!pointer && !value.lvalue) {
                return false; // `coerce` reports a temporary (E0709)
            }
            const Type& object = pointer ? *value.type.elem : value.type;
            const std::string iface_q = to.path.empty() ? std::string{} : to.path.front();
            return findImpl(object.path.front(), iface_q) != nullptr;
        }
    }
    const ConversionResult r = implicitConversion(value.type, to, inUnsafe(), ptrBits());
    return r.status == ConversionStatus::Ok || r.status == ConversionStatus::Warn;
}

bool Lowerer::literalFits(const Expr& arg, const Type& param) {
    if (lower::isUnknown(param)) {
        return true;
    }
    switch (arg.kind) {
    case Expr::Kind::IntLiteral:
        return param.kind == TypeKind::Int || param.kind == TypeKind::Float ||
               param.kind == TypeKind::Enum;
    case Expr::Kind::FloatLiteral:
        return param.kind == TypeKind::Float;
    case Expr::Kind::StringLiteral:
        if (param.kind == TypeKind::Str || param.kind == TypeKind::Pointer) {
            return true;
        }
        if (param.kind == TypeKind::Enum) {
            return param.elem && param.elem->kind == TypeKind::Pointer;
        }
        if (param.kind == TypeKind::Array && param.elem && param.elem->kind == TypeKind::Int &&
            param.elem->bits == 8 && !param.elem->is_signed) {
            const auto n = arrayLength(s_, param);
            return n && arg.text.size() <= *n;
        }
        return false;
    case Expr::Kind::Name:
        return arg.text == "nil" &&
               (param.kind == TypeKind::Pointer || param.kind == TypeKind::FnPtr);
    default:
        return false;
    }
}

bool Lowerer::lowerArgsPre(const Expr& call,
                           const std::vector<Type>& params,
                           std::vector<PreArg>& pre,
                           std::vector<EP>& out,
                           const std::string& callee) {
    if (call.args.size() != params.size()) {
        err("E0710",
            call.span,
            std::format(
                "`{}` takes {} argument(s), {} given", callee, params.size(), call.args.size()),
            "wrong number of arguments");
        return false;
    }
    bool ok = true;
    for (std::size_t i = 0; i < params.size(); ++i) {
        EP v = pre[i].literal ? expr(*call.args[i], &params[i]) : std::move(pre[i].value);
        EP a = moveOut(coerce(std::move(v), params[i], call.args[i]->span));
        ok = ok && a != nullptr;
        out.push_back(std::move(a));
    }
    return ok;
}

std::optional<std::size_t> Lowerer::resolveOverload(const Expr& call,
                                                    const std::string& name,
                                                    const std::vector<OverloadCandidate>& cands,
                                                    const std::vector<PreArg>& pre) {
    // A non-literal argument that failed to lower has reported its error already.
    for (const PreArg& p : pre) {
        if (!p.literal && !p.value) {
            return std::nullopt;
        }
    }
    if (cands.size() == 1) {
        return 0; // a single candidate is checked by `lowerArgsPre`
    }
    struct Scored {
        std::size_t index;
        int cost;
    };
    // Costs and viability come from the cached types only: no lowering, no diagnostics.
    std::vector<Scored> viable;
    for (std::size_t c = 0; c < cands.size(); ++c) {
        const OverloadCandidate& cand = cands[c];
        if (cand.params.size() != call.args.size()) {
            continue;
        }
        int cost = 0;
        bool ok = true;
        for (std::size_t i = 0; i < call.args.size() && ok; ++i) {
            const Expr& arg = *call.args[i];
            const PreArg& p = pre[i];
            const Type& param = cand.params[i];
            ok = p.literal ? literalFits(arg, param) : convertible(*p.value, param);
            cost += argumentCost(arg, p.type, param);
        }
        if (ok) {
            viable.push_back({c, cost});
        }
    }
    if (viable.empty()) {
        std::string shown;
        std::string candidates;
        for (const OverloadCandidate& cand : cands) {
            candidates += "\n  " + signatureOf(name, cand.params);
        }
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            shown += (i != 0 ? ", " : "") + argumentText(*call.args[i], pre[i].type);
        }
        Diagnostic d;
        d.code = "E0404";
        d.severity = Severity::Error;
        d.message = std::format("no `{}` takes ({})", name, shown);
        d.labels.push_back({call.span, "no overload matches these arguments"});
        d.fix = "candidates:" + candidates;
        s_.diag_.report(std::move(d));
        return std::nullopt;
    }
    int best = viable.front().cost;
    for (const Scored& s : viable) {
        best = std::min(best, s.cost);
    }
    std::vector<std::size_t> tied;
    for (const Scored& s : viable) {
        if (s.cost == best) {
            tied.push_back(s.index);
        }
    }
    if (tied.size() > 1) {
        std::string list;
        for (const std::size_t t : tied) {
            list += "\n  " + signatureOf(name, cands[t].params);
        }
        Diagnostic d;
        d.code = "E0403";
        d.severity = Severity::Error;
        d.message = std::format("call to `{}` is ambiguous", name);
        d.labels.push_back({call.span, "several overloads match equally well"});
        d.fix = "write the argument types so one overload is closer, e.g. a typed literal:" + list;
        s_.diag_.report(std::move(d));
        return std::nullopt;
    }
    return tied.front();
}

void Lowerer::lowerEnum(const EnumDecl& decl) {
    hir::Enum out;
    out.name = s_.qualify(decl.name);
    out.underlying = resolved(decl.underlying);
    // The member values of an instance are computed by the module that owns the generic enum.
    Sema* owner_ref =
        s_.compilation() != nullptr ? s_.compilation()->semaOf(s_.genericOwnerKey(decl.name)) : &s_;
    Sema& owner = owner_ref != nullptr ? *owner_ref : s_;
    for (const EnumMember& m : decl.members) {
        hir::EnumMemberValue v;
        v.name = m.name;
        if (const auto c = owner.enumMember(decl.name, m.name, m.span)) {
            if (c->kind == ConstValue::Kind::Int) {
                v.int_value = static_cast<std::int64_t>(c->int_value);
            } else {
                v.text = c->toString();
            }
        }
        out.members.push_back(std::move(v));
    }
    mod_.enums.push_back(std::move(out));
}

// ---------------------------------------------------------------------------------------------
// Payload enums. The parser turns `enum E { A(i64), B }` into the struct `E { tag: E$Tag, data:
// E$Data }`, the tag enum `E$Tag`, the union `E$Data { A: E$A }` and one struct `E$A` per variant
// with data. Here the variants are built (`E.A(1)`, `E.B`) and matched (`switch`). A generic enum
// is the same shape inside its instance: `E<i32>` is the instance of `E`, and its parts are the
// instances `E<i32>$Tag`, `E<i32>$Data` and `E<i32>$A` (made by `Sema::instantiate`).

const StructDecl* Lowerer::sumDecl(const Item* item) {
    if (item == nullptr) {
        return nullptr;
    }
    const auto* decl = std::get_if<StructDecl>(&item->decl);
    return decl != nullptr && !decl->variants.empty() ? decl : nullptr;
}

bool Lowerer::sameVariants(const StructDecl& named, const StructDecl& scrutinee) {
    if (&named == &scrutinee) {
        return true;
    }
    // A case names the declaration (`Result.Ok`), the scrutinized value is one instance of it
    // (`Result<i32, E>`): same enum name before the arguments, same variants.
    const std::string base = scrutinee.name.substr(0, scrutinee.name.find('<'));
    return named.name == base && std::ranges::equal(named.variants,
                                                    scrutinee.variants,
                                                    std::equal_to<>(),
                                                    &SumVariant::name,
                                                    &SumVariant::name);
}

std::optional<Lowerer::Sum> Lowerer::sumOfEntry(const ScopeEntry& head) {
    if (head.kind != ScopeEntry::Kind::Type || head.item == nullptr) {
        return std::nullopt;
    }
    const StructDecl* decl = sumDecl(head.item);
    if (decl == nullptr) {
        return std::nullopt;
    }
    return Sum{head.owner != nullptr ? head.owner : &s_, decl};
}

std::optional<Lowerer::Sum> Lowerer::sumOfType(const Type& t) {
    // A generic enum: `Result<i32, E>` is a `Struct` whose name is the instance of `Result`; its
    // variant list came with the instance (see `instantiateItem`).
    if (t.kind != TypeKind::Struct && t.kind != TypeKind::Named) {
        return std::nullopt;
    }
    const auto ref = entityOfType(t);
    if (!ref) {
        return std::nullopt;
    }
    const StructDecl* decl = sumDecl(ref->sym->item);
    if (decl == nullptr) {
        return std::nullopt;
    }
    return Sum{ref->owner, decl};
}

Type Lowerer::sumPart(const Sum& sum, std::string_view suffix, const Span& span) {
    // An instance is named `Result<i32, E>`; its union and its payload structs keep the arguments
    // after the suffix (`Result$Data<i32, E>`, `Result$Ok<i32, E>`), while the tag enum is the same
    // value enum for every instance of the declaration (`Result$Tag`).
    const std::string base = sum.decl->name;
    const std::size_t cut = base.find('<');
    const bool per_instance = cut != std::string::npos && suffix != "$Tag";
    Type named =
        Type::makeNamed({per_instance ? base.substr(0, cut) + std::string(suffix) + base.substr(cut)
                                      : base.substr(0, cut) + std::string(suffix)});
    named.span = span;
    return resolvedIn(*sum.owner, named);
}

std::optional<std::size_t> Lowerer::sumVariantIndex(const Sum& sum, std::string_view name) {
    const auto it = std::ranges::find(sum.decl->variants, name, &SumVariant::name);
    if (it == sum.decl->variants.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - sum.decl->variants.begin());
}

/// Index of the variant's field inside `E$Data`: only variants with data have one.
std::size_t Lowerer::sumDataIndex(const Sum& sum, std::size_t variant) {
    return static_cast<std::size_t>(
        std::ranges::count_if(sum.decl->variants | std::views::take(variant),
                              [](const SumVariant& v) { return !v.payload.empty(); }));
}

Lowerer::EP Lowerer::sumConstruct(const Sum& sum,
                                  std::size_t variant,
                                  std::vector<EP> values,
                                  const Span& span) {
    const SumVariant& v = sum.decl->variants[variant];
    // The literal is the enum itself (`Result<i32, E>`, a struct), built from its two parts.
    Type whole = Type::makeNamed({sum.owner->qualify(sum.decl->name)});
    whole.kind = TypeKind::Struct;
    whole.span = span;
    EP lit = mk(hir::ExprKind::StructLit, whole, span);
    lit->fields.push_back({"tag", 0, intConst(variant, sumPart(sum, "$Tag", span), span)});
    if (v.payload.empty()) {
        return lit;
    }
    EP payload = mk(hir::ExprKind::StructLit, sumPart(sum, "$" + v.name, span), span);
    for (const std::size_t i : std::views::iota(std::size_t{0}, values.size())) {
        payload->fields.push_back(
            {std::format("_{}", i), static_cast<std::uint32_t>(i), std::move(values[i])});
    }
    EP data = mk(hir::ExprKind::StructLit, sumPart(sum, "$Data", span), span);
    data->fields.push_back(
        {v.name, static_cast<std::uint32_t>(sumDataIndex(sum, variant)), std::move(payload)});
    lit->fields.push_back({"data", 1, std::move(data)});
    return lit;
}

/// `E.B` used as a value: only a variant without data can stand alone.
Lowerer::EP Lowerer::sumVariantValue(const Sum& sum, const Expr& e) {
    const auto variant = sumVariantIndex(sum, e.text);
    if (!variant) {
        err("E0706",
            e.span,
            std::format("enum `{}` has no member `{}`", sum.decl->name, e.text),
            "unknown member");
        return nullptr;
    }
    const SumVariant& v = sum.decl->variants[*variant];
    if (!v.payload.empty()) {
        err("E0751",
            e.span,
            std::format(
                "variant `{}.{}` carries {} value(s)", sum.decl->name, v.name, v.payload.size()),
            std::format("write `{}.{}(...)` with its values", sum.decl->name, v.name));
        return nullptr;
    }
    return sumConstruct(sum, *variant, {}, e.span);
}

/// `E.A(x, y)`: every argument converts to the payload type at its position.
Lowerer::EP Lowerer::sumVariantCall(const Sum& sum, const Expr& call) {
    const Expr& callee = *call.lhs;
    const auto variant = sumVariantIndex(sum, callee.text);
    if (!variant) {
        err("E0706",
            callee.span,
            std::format("enum `{}` has no member `{}`", sum.decl->name, callee.text),
            "unknown member");
        return nullptr;
    }
    const SumVariant& v = sum.decl->variants[*variant];
    if (call.args.size() != v.payload.size()) {
        err("E0751",
            call.span,
            std::format("variant `{}.{}` takes {} value(s), {} given",
                        sum.decl->name,
                        v.name,
                        v.payload.size(),
                        call.args.size()),
            "the number of values must match the variant's payload");
        return nullptr;
    }
    std::vector<EP> values;
    bool ok = true;
    for (const auto& [arg, type] : std::views::zip(call.args, v.payload)) {
        const Type want = resolvedIn(*sum.owner, type);
        EP value = expr(*arg, &want);
        value = value ? coerce(std::move(value), want, arg->span) : nullptr;
        // The payload owns the value: a droppable argument moves into it (a copy would drop it
        // twice: once through the enum, once through the local).
        value = value ? moveOut(std::move(value)) : nullptr;
        ok = ok && value != nullptr;
        values.push_back(std::move(value));
    }
    return ok ? sumConstruct(sum, *variant, std::move(values), call.span) : nullptr;
}

/// The place `scrutinee.data.<Variant>._<index>`.
Lowerer::EP Lowerer::sumPayloadRead(const Sum& sum,
                                    hir::LocalId scrutinee,
                                    const Type& self,
                                    std::size_t variant,
                                    std::size_t index,
                                    const Span& span) {
    const SumVariant& v = sum.decl->variants[variant];
    const auto field = [&](EP base, std::string name, std::uint32_t at, Type type) {
        EP f = mk(hir::ExprKind::Field, std::move(type), span);
        f->text = std::move(name);
        f->index = at;
        f->lvalue = true;
        f->lhs = std::move(base);
        return f;
    };
    EP root = mk(hir::ExprKind::LocalRef, self, span);
    root->local = scrutinee;
    root->lvalue = true;
    EP data = field(std::move(root), "data", 1, sumPart(sum, "$Data", span));
    EP payload = field(std::move(data),
                       v.name,
                       static_cast<std::uint32_t>(sumDataIndex(sum, variant)),
                       sumPart(sum, "$" + v.name, span));
    return field(std::move(payload),
                 std::format("_{}", index),
                 static_cast<std::uint32_t>(index),
                 resolvedIn(*sum.owner, v.payload[index]));
}

/// `switch` on a payload enum. A case names variants: `case E.A(x, y) { }` binds the payload,
/// `case E.A, E.B { }` matches without binding, `E.C` for a variant without data. Without
/// `default` every variant must be covered (E0750).
void Lowerer::lowerSumSwitch(const Stmt& s, EP scrutinee, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    const Sum sum = *sumOfType(scrutinee->type);
    const Type self = scrutinee->type;
    const Type tag_type = sumPart(sum, "$Tag", s.span);
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    const hir::LocalId id = hiddenLocal("sw", self, s.span);
    {
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = id;
        d.value = std::move(scrutinee);
        block.body.push_back(std::move(d));
    }
    const auto tagIs = [&](std::size_t variant, const Span& at) {
        EP tag = mk(hir::ExprKind::Field, tag_type, at);
        tag->text = "tag";
        tag->index = 0;
        tag->lvalue = true;
        tag->lhs = mk(hir::ExprKind::LocalRef, self, at);
        tag->lhs->local = id;
        tag->lhs->lvalue = true;
        EP eq = mk(hir::ExprKind::Binary, boolean, at);
        eq->binary_op = BinaryOp::Eq;
        eq->lhs = std::move(tag);
        eq->rhs = intConst(variant, tag_type, at);
        return eq;
    };

    struct Arm {
        EP cond;
        std::vector<hir::Stmt> body;
    };
    std::vector<Arm> arms;
    std::vector<hir::Stmt> default_body;
    bool has_default = false;
    std::set<std::size_t> covered;
    for (const SwitchCase& c : s.cases) {
        if (c.is_default) {
            if (std::exchange(has_default, true)) {
                err("E0709",
                    c.span,
                    "a `switch` has more than one `default`",
                    "remove the duplicate");
            }
            lowerBlock(*c.body, default_body, true);
            continue;
        }
        Arm arm;
        scopes_.push(ScopeKind::Block);
        for (const ExprPtr& value : c.values) {
            const bool call = value->kind == Expr::Kind::Call;
            const Expr& pattern = call ? *value->lhs : *value;
            const ScopeEntry* head =
                pattern.kind == Expr::Kind::Field ? chainEntry(*pattern.lhs) : nullptr;
            const auto named = head != nullptr ? sumOfEntry(*head) : std::nullopt;
            // The pattern names the enum; the scrutinee may be one instance of it (`Result<i32, E>`
            // built its own aggregate), so the compared part is the declaration's variant list.
            if (!named) {
                // A name that is not an enum at all, such as `case 1`, names no variant of the
                // switched-on enum: the `case` syntax itself is wrong (E0749).
                err("E0749",
                    value->span,
                    std::format("`case` of a `{}` switch must name one of its variants",
                                sum.decl->name),
                    std::format("write `{}.Variant` or `{}.Variant(names...)`",
                                sum.decl->name,
                                sum.decl->name));
                continue;
            }
            if (!sameVariants(*named->decl, *sum.decl)) {
                // A pattern that names another enum: `case Other.A` in a `switch` on `E`.
                err("E0757",
                    value->span,
                    std::format("`case {}.{}` names a variant of `{}`, but the value is a `{}`",
                                named->decl->name,
                                pattern.text,
                                named->decl->name,
                                sum.decl->name),
                    std::format("write `{}.{}`", sum.decl->name, pattern.text));
                continue;
            }
            const auto variant = sumVariantIndex(sum, pattern.text);
            if (!variant) {
                err("E0706",
                    pattern.span,
                    std::format("enum `{}` has no member `{}`", sum.decl->name, pattern.text),
                    "unknown member");
                continue;
            }
            if (!covered.insert(*variant).second) {
                err("E0709",
                    value->span,
                    "duplicate `case` variant",
                    "this variant is already handled by an earlier `case`");
            }
            EP cond = tagIs(*variant, value->span);
            arm.cond = arm.cond ? [&] {
                EP both = mk(hir::ExprKind::Binary, boolean, value->span);
                both->binary_op = BinaryOp::Or;
                both->lhs = std::move(arm.cond);
                both->rhs = std::move(cond);
                return both;
            }()
                                : std::move(cond);
            if (!call) {
                continue;
            }
            const SumVariant& v = sum.decl->variants[*variant];
            if (c.values.size() != 1 || value->args.size() != v.payload.size() ||
                !std::ranges::all_of(
                    value->args, [](const ExprPtr& a) { return a->kind == Expr::Kind::Name; })) {
                err("E0749",
                    value->span,
                    std::format("pattern `{}.{}(...)` needs one name per payload value ({}), alone "
                                "in its `case`",
                                sum.decl->name,
                                v.name,
                                v.payload.size()),
                    "write `case E.A(x, y) { }`; use `_` to ignore a value");
                continue;
            }
            for (const std::size_t i : std::views::iota(std::size_t{0}, v.payload.size())) {
                const Expr& name = *value->args[i];
                if (name.text == "_") {
                    continue;
                }
                EP read = sumPayloadRead(sum, id, self, *variant, i, name.span);
                const Type field_type = read->type;
                // The binding owns the value: a droppable payload moves out of the enum. The
                // switch held the scrutinee; once its live payload is taken the holder is dropped
                // without it, so exactly one value exists.
                read = moveOut(std::move(read), true);
                if (read == nullptr) {
                    continue;
                }
                const hir::LocalId local = addLocal(name.text, field_type, false, name.span);
                hir::Stmt d;
                d.kind = hir::StmtKind::Local;
                d.span = name.span;
                d.local = local;
                d.value = std::move(read);
                arm.body.push_back(std::move(d));
                bindLocal(name.text, local, field_type, name.span);
            }
        }
        lowerBlock(*c.body, arm.body, true);
        scopes_.pop();
        if (arm.cond) {
            arms.push_back(std::move(arm));
        }
    }
    scopes_.pop();

    if (!has_default && covered.size() < sum.decl->variants.size()) {
        std::string missing;
        for (const auto& [i, v] :
             std::views::zip(std::views::iota(std::size_t{0}), sum.decl->variants)) {
            if (!covered.contains(i)) {
                missing +=
                    std::format("{}`{}.{}`", missing.empty() ? "" : ", ", sum.decl->name, v.name);
            }
        }
        err("E0750",
            s.span,
            std::format("`switch` on `{}` does not cover {}", sum.decl->name, missing),
            "add a `case` for each missing variant, or a `default`");
    }
    std::vector<hir::Stmt> chain = std::move(default_body);
    for (Arm& arm : arms | std::views::reverse) {
        hir::Stmt ifs;
        ifs.kind = hir::StmtKind::If;
        ifs.span = s.span;
        ifs.value = std::move(arm.cond);
        ifs.body = std::move(arm.body);
        ifs.else_body = std::move(chain);
        chain.clear();
        chain.push_back(std::move(ifs));
    }
    std::ranges::move(chain, std::back_inserter(block.body));
    out.push_back(std::move(block));
}

void Lowerer::lowerGlobal(const GlobalDecl& g, const Modifiers& mods) {
    hir::Global out;
    out.name = s_.qualify(g.name);
    out.module = s_.key_;
    out.constant = g.is_const;
    out.type = globalType(s_, g);
    rejectDroppablePlace(out.type, g.name_span, "a global");
    if (droppableSum(out.type)) {
        err("E0758",
            g.name_span,
            std::format("`{}` is a payload enum that needs drop and cannot be a global",
                        typeToString(out.type)),
            "a global is plain data and is never dropped; use a local or a class field");
    } else if (needsDrop(out.type)) {
        err("E0721",
            g.name_span,
            std::format("`{}` needs drop and cannot be a global", typeToString(out.type)),
            "use a local, or a class field or `Vec` for owned values");
    }
    if (!g.has_type && g.init && !lower::literalConstType(s_, *g.init)) {
        err("E0715",
            g.init->span,
            "untyped `const` needs a literal value",
            "write its type: `const NAME: T = ...`");
    }
    out.exported = mods.exported || mods.export_c || mods.registered;
    out.export_c = mods.export_c;
    out.declared = mods.declared;
    out.registered = mods.registered;
    out.register_role = mods.register_role;
    out.span = g.span;
    if (mods.export_c && !cCompatible(s_, out.type)) {
        err("E0302",
            g.name_span,
            std::format("`{}` has a type that C cannot express", typeToString(out.type)),
            "`export(c)` needs C-compatible types (integers, floats, bool, pointers, `@repr(c)` "
            "structs)");
    }
    assignSymbol(
        g.attributes, mods.export_c, mods.declared, g.name, {}, false, out.symbol, g.name_span);
    if (g.init) {
        EP init = g.has_type ? coerce(expr(*g.init, &out.type), out.type, g.init->span)
                             : expr(*g.init, nullptr);
        if (init && !g.has_type) {
            out.type = init->type;
        }
        if (init) {
            const hir::Expr* offender = nullptr;
            if (!isConstValue(*init, &offender)) {
                const hir::Expr& at = offender != nullptr ? *offender : *init;
                if (g.is_const) {
                    err("E0715",
                        at.span,
                        "`const` initializer is not a compile-time constant",
                        "a `const` holds literals, arithmetic on them, arrays and struct literals; "
                        "it cannot call functions or read other names");
                } else {
                    err("E0707",
                        at.span,
                        "global initializer is not a compile-time constant",
                        "globals are statically initialized: no calls, no reads of other globals");
                }
            }
            out.init = std::move(init);
        }
    }
    mod_.globals.push_back(std::move(out));
}

void Lowerer::lowerLocalConst(const Stmt& s) {
    EP init;
    Type t;
    if (s.has_type) {
        t = resolved(s.type);
        init = coerce(expr(*s.expr, &t), t, s.expr->span);
    } else {
        init = expr(*s.expr, nullptr);
        t = init ? init->type : lower::unknownType();
    }
    if (init) {
        const hir::Expr* offender = nullptr;
        if (!isConstValue(*init, &offender)) {
            const hir::Expr& at = offender != nullptr ? *offender : *init;
            err("E0715",
                at.span,
                "`const` initializer is not a compile-time constant",
                "a `const` holds literals, arithmetic on them, arrays and struct literals; "
                "it cannot call functions or read other names");
            init.reset();
        }
    }
    // A `const` is a private read-only global with a unique name; only the block sees the name.
    hir::Global g;
    g.name = std::format("{}${}${}", fn_->name, s.name, hidden_counter_++);
    g.module = s_.key_;
    g.symbol = g.name;
    g.type = t;
    g.constant = true;
    g.init = std::move(init);
    g.span = s.span;
    ScopeEntry e;
    e.kind = ScopeEntry::Kind::Global;
    e.name = s.name;
    e.type = t;
    e.span = s.span;
    e.owner = &s_;
    e.qname = g.name;
    e.readonly = true;
    mod_.globals.push_back(std::move(g));
    if (const ScopeEntry* prev = scopes_.declare(std::move(e))) {
        Diagnostic d;
        d.code = "E0401";
        d.severity = Severity::Error;
        d.message = std::format("`{}` is defined twice in this scope", s.name);
        d.labels.push_back({prev->span, "first definition"});
        d.labels.push_back({s.span, "second definition"});
        d.fix = "rename one of them";
        s_.diag_.report(std::move(d));
    }
}

void Lowerer::lowerFunction(const FnDecl& f,
                            const Modifiers& mods,
                            const std::string& qname,
                            const std::string& local_name,
                            const std::vector<Attribute>& attrs,
                            const std::string& owner,
                            const Type* self_type,
                            bool overloaded) {
    hir::Function out;
    out.name = qname;
    out.module = s_.key_;
    out.owner = owner;
    out.is_init = sh_.init_fns.contains(qname);
    out.is_fini = sh_.fini_fns.contains(qname);
    out.is_entry = sh_.entry_fn == qname;
    out.exported = mods.exported || mods.export_c || mods.registered;
    out.export_c = mods.export_c;
    out.declared = mods.declared;
    if (const Attribute* lib = lower::findAttribute(attrs, "library");
        lib != nullptr && mods.declared) {
        if (lib->args.size() == 1 && lib->args[0].kind == AttributeArg::Kind::String) {
            out.import_library = lib->args[0].text;
        } else {
            err("E0705",
                f.name_span,
                "`@library` takes one string (the OS library name)",
                "invalid argument");
        }
    }
    out.registered = mods.registered;
    out.register_role = mods.register_role;
    out.is_virtual = f.is_virtual;
    out.is_override = f.is_override;
    out.has_body = f.has_body;
    out.span = f.span;
    out.ret = resolved(f.return_type);

    fn_ = &out;
    in_ctor_ = !owner.empty() && self_type != nullptr && f.name == "init";
    ret_ = out.ret;
    unsafe_ = 0;
    loops_.clear();
    defers_.clear();
    scopes_.push(ScopeKind::Function);
    // Parameters that need drop are owned by the callee: their drops run on every exit path.
    std::vector<hir::Stmt> prologue;
    struct OwnedParam {
        hir::LocalId id;
        Type type;
        Span span;
    };
    std::vector<OwnedParam> owned_params;
    const std::string base_call = std::exchange(base_init_call_, {});
    entry_temps_.clear();
    entry_flags_.clear();
    unwind_entries_.clear();
    has_try_ = false;
    defers_.emplace_back();
    std::vector<Type> param_types;
    for (const Param& p : f.params) {
        Type t;
        if (p.is_self) {
            if (self_type == nullptr) {
                err("E0709",
                    p.span,
                    "`self` outside of a method",
                    "only class and `impl` methods have `self`");
                t = lower::unknownType();
            } else {
                t = *self_type;
            }
        } else {
            t = resolved(p.type);
        }
        param_types.push_back(t);
        const hir::LocalId id = addLocal(p.is_self ? "self" : p.name, t, true, p.span);
        bindLocal(p.is_self ? "self" : p.name, id, t, p.span);
        if (!p.is_self) {
            rejectDroppablePlace(t, p.span, "a parameter");
        }
        if (!p.is_self && needsDrop(t)) {
            owned_params.push_back({id, t, p.span});
        }
    }
    if (out.is_entry) {
        checkEntrySignature(f, param_types, out.ret);
    }
    out.param_count = static_cast<std::uint32_t>(out.locals.size());
    // The drop flags follow the parameters (the first `param_count` locals).
    for (const OwnedParam& p : owned_params) {
        registerDrop(p.id, p.type, true, p.span, prologue);
    }
    if (mods.export_c) {
        bool ok = cCompatible(s_, out.ret);
        for (const Type& t : param_types) {
            ok = ok && cCompatible(s_, t);
        }
        if (!ok) {
            err("E0302",
                f.name_span,
                "`export(c)` signature uses a type that C cannot express",
                "integers, floats, bool, pointers, function pointers, `@repr(c)` structs and "
                "integer enums only");
        }
    }
    assignSymbol(attrs,
                 mods.export_c,
                 mods.declared,
                 local_name,
                 param_types,
                 overloaded,
                 out.symbol,
                 f.name_span);
    if (f.has_body) {
        if (!base_call.empty()) {
            // A derived `init` first runs the base class constructor on `self`.
            EP self = localRef(0, out.locals.front().type, f.span);
            EP call = mk(hir::ExprKind::Call, Type::makeVoid(), f.span);
            call->text = base_call;
            call->args.push_back(std::move(self));
            hir::Stmt s;
            s.kind = hir::StmtKind::Expr;
            s.span = f.span;
            s.value = std::move(call);
            prologue.push_back(std::move(s));
        }
        lowerBlock(f.body, out.body, false);
        if (!out.ret.isVoid() && !lower::isUnknown(out.ret) &&
            !lower::terminates(out.body, mod_.panic_function)) {
            s_.diag_.warning(
                "W0701",
                f.name_span,
                std::format("`{}` can reach the end without returning a value", f.name),
                "not every path returns");
        }
    }
    // Falling off the end runs the parameter drops (a `return` already ran them); the temporaries
    // and their flags are declared first, so every path sees them initialized.
    const bool jumps = !out.body.empty() && out.body.back().kind == hir::StmtKind::Return;
    if (f.has_body && !jumps) {
        emitDefers(defers_.size() - 1, out.body);
    }
    defers_.pop_back();
    // Flags first: the prologue and the body set them.
    std::vector<hir::Stmt> head;
    for (const hir::LocalId flag : entry_flags_) {
        hir::Stmt live;
        live.kind = hir::StmtKind::Local;
        live.span = f.span;
        live.local = flag;
        live.value = boolConst(false, f.span);
        head.push_back(std::move(live));
    }
    entry_flags_.clear();
    for (const auto& [tmp, flag] : entry_temps_) {
        hir::Stmt decl;
        decl.kind = hir::StmtKind::Local;
        decl.span = f.span;
        decl.local = tmp;
        head.push_back(std::move(decl));
        hir::Stmt live;
        live.kind = hir::StmtKind::Local;
        live.span = f.span;
        live.local = flag;
        live.value = boolConst(false, f.span);
        head.push_back(std::move(live));
    }
    entry_temps_.clear();
    for (hir::Stmt& p : prologue) {
        head.push_back(std::move(p));
    }
    // The landing pad: every registered cleanup, newest first (needs the registered `unwind`).
    if (!mod_.unwind_function.empty() && f.has_body) {
        for (std::size_t i = unwind_entries_.size(); i-- > 0;) {
            out.unwind_body.push_back(lower::cloneHirStmt(unwind_entries_[i]));
        }
    }
    out.has_try = has_try_;
    unwind_entries_.clear();
    has_try_ = false;
    out.body.insert(out.body.begin(),
                    std::make_move_iterator(head.begin()),
                    std::make_move_iterator(head.end()));
    scopes_.pop();
    fn_ = nullptr;
    in_ctor_ = false;
    mod_.functions.push_back(std::move(out));
}

void Lowerer::lowerAggregate(const Item& item) {
    if (const auto* st = std::get_if<StructDecl>(&item.decl)) {
        hir::Aggregate a;
        a.name = s_.qualify(st->name);
        a.opaque = st->opaque;
        a.is_union = st->is_union;
        a.span = st->span;
        const Attribute* repr = lower::findAttribute(st->attributes, "repr");
        a.repr_c = repr != nullptr && std::ranges::any_of(repr->args, [](const AttributeArg& x) {
                       return x.text == "c";
                   });
        // A payload enum owns its payload: its glue drops only the live variant's values.
        if (!st->variants.empty()) {
            emitSumDropGlue(a.name, *st, st->span);
        }
        for (const FieldDecl& f : st->fields) {
            const Type ft = resolved(f.type);
            // A payload struct and the union that holds it are dropped by the enum's own glue,
            // which drops the live variant only, so a droppable payload is allowed there. Such a
            // struct is never a field of anything else and has no name a program can write.
            // A sum (`E` itself) and its parts are dropped by `E`'s glue, which drops the live
            // variant only, so a droppable payload is allowed there.
            const bool in_sum = !st->variants.empty() || st->is_payload || st->is_union;
            if (droppableSum(ft) && !in_sum) {
                err("E0758",
                    f.span,
                    std::format("`{}` is a payload enum that needs drop and cannot be a struct "
                                "field",
                                typeToString(ft)),
                    "use a class field instead; structs are plain data and drop nothing");
            } else if (needsDrop(ft) && !in_sum) {
                err("E0721",
                    f.span,
                    std::format("`{}` needs drop and cannot be a struct field", typeToString(ft)),
                    "use a class field instead; structs are plain data");
            }
            rejectDroppablePlace(ft, f.span, "a struct field");
            a.fields.push_back({f.name, ft});
        }
        mod_.aggregates.push_back(std::move(a));
    } else if (const auto* cl = std::get_if<ClassDecl>(&item.decl)) {
        hir::Aggregate a;
        a.name = s_.qualify(cl->name);
        a.is_class = true;
        for (const FnDecl& m : cl->methods) {
            const bool only_self =
                std::ranges::all_of(m.params, [](const Param& p) { return p.is_self; });
            if (m.name == "init" && m.has_return_type) {
                err("E0722", m.name_span, "`init` takes no return type", "remove the `-> T`");
            }
            if (m.name == "drop" &&
                (!only_self || m.has_return_type || m.is_virtual || m.is_override)) {
                err("E0722",
                    m.name_span,
                    "`drop` takes only `self` and returns nothing, and cannot be virtual",
                    "write `fn drop(self) { ... }`");
            }
        }
        if (cl->has_base && hasInit(s_.qualify(cl->name)) && baseInitKey(*cl, s_).empty()) {
            if (const auto base = s_.resolve(Type::makeNamed(cl->base));
                base && base->kind == TypeKind::Class && hasInit(base->path.front())) {
                err("E0722",
                    cl->name_span,
                    std::format("`{}` has an `init` but its base has none without arguments",
                                cl->name),
                    "give the base class an `init` that takes no arguments");
            }
        }
        emitDropGlue(a.name, *cl, cl->span);
        a.opaque = cl->opaque;
        a.span = cl->span;
        const Attribute* repr = lower::findAttribute(cl->attributes, "repr");
        a.repr_c = repr != nullptr && std::ranges::any_of(repr->args, [](const AttributeArg& x) {
                       return x.text == "c";
                   });
        if (cl->has_base) {
            Type base = Type::makeNamed(cl->base);
            base.span = cl->name_span;
            if (const auto r = s_.resolve(base); r && r->kind == TypeKind::Class) {
                a.base = r->path.front();
            }
        }
        for (const FieldDecl& f : cl->fields) {
            const Type ft = resolved(f.type);
            rejectDroppablePlace(ft, f.span, "a class field");
            a.fields.push_back({f.name, ft});
        }
        for (const FnDecl& m : cl->methods) {
            a.methods.push_back(a.name + "." + m.name);
        }
        for (const SlotInfo& slot : vtableOf(a.name)) {
            a.vtable.push_back(slot.slot);
        }
        mod_.aggregates.push_back(std::move(a));
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

namespace xlang {

hir::Module lowerProgram(const std::vector<Sema*>& modules,
                         const std::vector<Sema*>& init_order,
                         const std::set<const Sema*>& library_modules) {
    using lower::Lowerer;
    using lower::LowerShared;
    LowerShared shared;
    shared.modules = modules;
    std::vector<std::unique_ptr<Lowerer>> lowerers;
    std::map<const Sema*, Lowerer*> by_sema;
    for (Sema* s : modules) {
        lowerers.push_back(std::make_unique<Lowerer>(*s, shared, library_modules.contains(s)));
        by_sema[s] = lowerers.back().get();
    }
    for (auto& l : lowerers) {
        l->collectImpls();
    }
    for (auto& l : lowerers) {
        l->checkRegistrations();
    }
    for (auto& l : lowerers) {
        l->checkHookFunctions();
    }
    for (auto& l : lowerers) {
        l->run();
    }
    // Generic instances requested by one module can belong to another: lower until none is
    // left.
    bool more = true;
    while (more) {
        more = false;
        for (auto& l : lowerers) {
            more = l->lowerPendingInstances() || more;
        }
    }
    for (auto& l : lowerers) {
        l->finish();
    }
    for (Sema* s : init_order) {
        for (const std::string& name : by_sema.at(s)->initFunctions()) {
            shared.module.init_functions.push_back(name);
        }
        for (const std::string& name : by_sema.at(s)->finiFunctions()) {
            shared.module.fini_functions.push_back(name);
        }
    }
    return std::move(shared.module);
}

} // namespace xlang
