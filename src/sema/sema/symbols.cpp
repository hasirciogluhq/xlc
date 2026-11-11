#include "sema/sema/sema.hpp"

#include "xlang/sema/compilation.h"

#include <algorithm>
#include <format>

namespace xlang {

// Name table of one module (plan 2.4): setup, diagnostics with instantiation notes, `collect`
// (every top-level declaration becomes a symbol, duplicates are `E0401`/`E0405`), link names and
// `fn alias` resolution.

using sema_detail::findAttr;

Sema::Sema(const Program& program,
           const BuildConfig& config,
           DiagnosticEngine& diagnostics,
           const LayoutHistory* history)
    : program_(program), config_(config), diag_(diagnostics), history_(history) {
    // Builtin `MemoryOrder` is an ordinary enum (plan 6.1 / 2.6).
    EnumDecl order;
    order.name = "MemoryOrder";
    order.has_underlying = true;
    order.underlying = Type::makeInt(8, false);
    for (const char* member : {"relaxed", "acquire", "release", "acqRel", "seqCst"}) {
        EnumMember m;
        m.name = member;
        order.members.push_back(std::move(m));
    }
    memory_order_item_.decl = std::move(order);
    collect();
}

void Sema::error(const char* code, const Span& at, std::string message, std::string label) {
    const std::string key =
        std::format("{}:{}:{}:{}:{}", code, at.file, at.line, at.column, message);
    if (!reported_.insert(key).second) {
        return;
    }
    diag_.error(code, at, std::move(message), std::move(label), instantiationNotes());
}

std::vector<Note> Sema::instantiationNotes() const {
    constexpr std::size_t max_levels = 8;
    std::vector<Note> notes;
    const Sema* owner = this;
    std::string local = current_instance_;
    while (!local.empty() && owner != nullptr && notes.size() < max_levels) {
        const InstanceInfo* info = owner->instanceInfo(local);
        if (info == nullptr) {
            break;
        }
        if (info->requested_at.valid()) {
            notes.push_back({info->requested_at,
                             std::format("in instantiation of `{}` requested here", local)});
        }
        owner = info->parent_owner;
        local = info->parent;
    }
    return notes;
}

void Sema::notConstant(const Span& at, std::string why) {
    error("E0707", at, "expression is not a compile-time constant", std::move(why));
}

void Sema::setCurrentInstance(const Item* item) {
    const auto it = item_instance_.find(item);
    current_instance_ = it == item_instance_.end() ? std::string{} : it->second;
}

void Sema::collect() {
    symbols_["MemoryOrder"] = Symbol{Symbol::Kind::Enum, &memory_order_item_};
    for (const Item& item : program_.items) {
        Symbol sym;
        sym.item = &item;
        const DeclBase& base = declBase(item);
        bool named = true;
        if (std::holds_alternative<StructDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Struct;
            if (genericParamsOf(item).empty()) {
                aggregate_order_.push_back(base.name);
            }
        } else if (std::holds_alternative<ClassDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Class;
            if (genericParamsOf(item).empty()) {
                aggregate_order_.push_back(base.name);
            }
        } else if (std::holds_alternative<InterfaceDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Interface;
        } else if (std::holds_alternative<EnumDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Enum;
        } else if (std::holds_alternative<TypeAliasDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Alias;
        } else if (std::holds_alternative<DefineConstDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Define;
        } else if (std::holds_alternative<GlobalDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Global;
        } else if (std::holds_alternative<FnDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Fn;
        } else if (std::holds_alternative<FnAliasDecl>(item.decl)) {
            sym.kind = Symbol::Kind::FnAlias;
        } else if (std::holds_alternative<DefineMacroDecl>(item.decl)) {
            sym.kind = Symbol::Kind::Macro;
        } else {
            named = false;
        }
        if (!named) {
            continue;
        }
        const auto [it, inserted] = symbols_.emplace(base.name, sym);
        if (sym.kind == Symbol::Kind::Fn) {
            // Functions may share a name (overloads); the signatures are checked when lowered.
            if (inserted) {
                it->second.overloads.push_back(&item);
            } else if (it->second.kind == Symbol::Kind::Fn) {
                it->second.overloads.push_back(&item);
                continue;
            }
        }
        if (!inserted && it->second.item != &memory_order_item_) {
            const DeclBase& first = declBase(*it->second.item);
            Diagnostic d;
            d.code = "E0401";
            d.severity = Severity::Error;
            d.message = std::format("`{}` is defined twice", base.name);
            d.labels.push_back({first.name_span, "first definition"});
            d.labels.push_back({base.name_span, "second definition"});
            d.fix = "rename one of them";
            diag_.report(std::move(d));
        }
    }
    // A name with overloads may only name ordinary functions.
    for (const auto& [name, sym] : symbols_) {
        if (sym.kind != Symbol::Kind::Fn || sym.overloads.size() < 2) {
            continue;
        }
        for (const Item* f : sym.overloads) {
            std::string why;
            if (name == "main") {
                why = "`main` is the entry point";
            } else if (f->modifiers.registered) {
                why = "a `register`ed function fills a role";
            } else if (f->modifiers.export_c) {
                why = "`export(c)` names one C symbol";
            } else if (f->modifiers.declared) {
                why = "a `declare`d function is one external symbol";
            }
            if (!why.empty()) {
                diag_.error("E0405",
                            declBase(*f).name_span,
                            std::format("`{}` cannot be overloaded: {}", name, why),
                            "keep one declaration with this name");
            }
        }
    }
}

std::string Sema::linkName(const std::vector<Attribute>& attrs,
                           bool export_c,
                           bool declared,
                           const std::string& local_name,
                           const std::vector<Type>& params,
                           bool overloaded) const {
    if (!overloaded) {
        // Priority: export(c) > @mangle on the item > @file(mangle) > --mangle.
        if (export_c) {
            return local_name;
        }
        if (const Attribute* m = findAttr(attrs, "mangle"); m != nullptr && !m->args.empty()) {
            const AttributeArg& a = m->args[0];
            return a.kind == AttributeArg::Kind::String ? a.text : local_name;
        }
        bool none = config_.mangle == "none";
        if (const Attribute* f = findAttr(program_.file_attributes, "file")) {
            for (const AttributeArg& a : f->args) {
                if (a.key == "mangle") {
                    none = a.text == "none";
                }
            }
        }
        if (none || declared) {
            return local_name;
        }
    }
    // `_xl$<module>$<name>$<param types>`; the module is the mangling name of the defining module.
    std::string sig;
    for (const Type& p : params) {
        sig += "$" + mangleType(p);
    }
    std::string module = key_;
    if (module.empty() && comp_ != nullptr && comp_->root() != nullptr) {
        module = comp_->root()->mangle_name;
    }
    if (module.empty()) {
        module = "main";
    }
    std::replace(module.begin(), module.end(), '/', '.');
    // A generic instance is named `f<i32, str>`: its link name spells the brackets `G`/`E`.
    std::string name;
    for (const char c : local_name) {
        switch (c) {
        case '<': name += 'G'; break;
        case '>': name += 'E'; break;
        case ',':
        case ' ': break;
        case '.':
        case ':':
        case '*':
        case '[':
        case ']':
        case ';':
        case '(':
        case ')': name += '_'; break;
        default: name += c; break;
        }
    }
    return "_xl$" + module + "$" + name + sig;
}

std::optional<Sema::EntityRef> Sema::resolveFnAlias(const std::vector<std::string>& path,
                                                    const Span& at) {
    std::set<std::string> seen;
    Sema* where = this;
    std::vector<std::string> cur = path;
    Span span = at;
    while (true) {
        std::string shown;
        for (std::size_t i = 0; i < cur.size(); ++i) {
            shown += (i != 0 ? "." : "") + cur[i];
        }
        const auto ref = cur.size() == 1 ? where->lookupEntity(cur.front())
                                         : where->lookupQualified(cur);
        if (!ref) {
            error("E0706", span, std::format("unknown function `{}`", shown), "not declared here");
            return std::nullopt;
        }
        if (ref->sym->kind == Symbol::Kind::Fn) {
            return ref;
        }
        if (ref->sym->kind != Symbol::Kind::FnAlias) {
            error("E0706", span, std::format("`{}` is not a function", shown), "a function is expected");
            return std::nullopt;
        }
        if (!seen.insert(ref->qname()).second) {
            error("E0704", span, std::format("function alias `{}` refers to itself", shown), "cyclic alias");
            return std::nullopt;
        }
        const auto& alias = std::get<FnAliasDecl>(ref->sym->item->decl);
        where = ref->owner;
        cur = alias.target;
        span = alias.target_span;
    }
}

std::vector<std::string> Sema::genericParamsOf(const Item& item) const {
    // An instance names its type parameters nowhere: it is the declaration with the arguments
    // already substituted. The generated parts of a payload enum have no parameters of their own
    // either; they exist only inside an instance of the enum.
    if (const auto it = item_instance_.find(&item); it != item_instance_.end()) {
        return {};
    }
    if (const auto* f = std::get_if<FnDecl>(&item.decl)) {
        return f->generic_params;
    }
    if (const auto* s = std::get_if<StructDecl>(&item.decl)) {
        return s->generic_params;
    }
    if (const auto* c = std::get_if<ClassDecl>(&item.decl)) {
        return c->generic_params;
    }
    if (const auto* a = std::get_if<TypeAliasDecl>(&item.decl)) {
        return a->generic_params;
    }
    if (const auto* i = std::get_if<InterfaceDecl>(&item.decl)) {
        return i->generic_params;
    }
    if (const auto* e = std::get_if<EnumDecl>(&item.decl)) {
        return e->generic_params;
    }
    // A payload enum's payload struct (`E$V`, the fields are `_0`, `_1`, ...) is only ever
    // instantiated as part of `E`'s instance, which substitutes these fields; a bare `E$V` names
    // the declaration, whose field types are the enum's parameters.
    return {};
}

}  // namespace xlang
