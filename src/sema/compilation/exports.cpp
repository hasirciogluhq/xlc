#include "sema/compilation/compilation.hpp"

#include <algorithm>
#include <format>

namespace xlang {

// Exports of one module (plan 4.6a): the names other modules can import or re-export. Own
// `export`ed declarations first, then `export ... from` re-exports, resolved depth-first so a
// re-export sees the exports of the module it re-exports from.

void Compilation::resolveExports(ModuleInfo& m) {
    if (m.exports_done || m.exports_in_progress) {
        return;
    }
    m.exports_in_progress = true;

    const auto add = [&](const std::string& name, ModuleInfo::Export e) {
        const auto [it, inserted] = m.exports.emplace(name, e);
        if (inserted) {
            return;
        }
        const ModuleInfo::Export& old = it->second;
        if (old.module == e.module && old.name == e.name && old.is_namespace == e.is_namespace) {
            return; // the same entity exported twice
        }
        Diagnostic d;
        d.code = "E0401";
        d.severity = Severity::Error;
        d.message = std::format("`{}` is exported twice", name);
        d.labels.push_back({old.span, "first export"});
        d.labels.push_back({e.span, "second export"});
        d.fix = "resolve with `except`, `as`, or `export * as ns`";
        diag_.report(std::move(d));
    };

    for (const Item& item : m.program.items) {
        const bool exported =
            item.modifiers.exported || item.modifiers.export_c || item.modifiers.registered;
        if (!exported) {
            continue;
        }
        if (std::holds_alternative<ImportDecl>(item.decl) ||
            std::holds_alternative<ExportFromDecl>(item.decl) ||
            std::holds_alternative<ImplDecl>(item.decl) ||
            std::holds_alternative<MacroCallDecl>(item.decl) ||
            std::holds_alternative<TopLevelCallDecl>(item.decl)) {
            continue;
        }
        const DeclBase& base = declBase(item);
        ModuleInfo::Export e;
        e.module = &m;
        e.name = base.name;
        e.span = base.name_span;
        add(base.name, e);
    }

    for (const Item& item : m.program.items) {
        const auto* ex = std::get_if<ExportFromDecl>(&item.decl);
        if (ex == nullptr) {
            continue;
        }
        const auto it = targets_.find({ex->module, m.key});
        if (it == targets_.end() || it->second == nullptr) {
            continue;
        }
        ModuleInfo& target = *it->second;
        resolveExports(target);
        switch (ex->form) {
        case ExportFromDecl::Form::Star: {
            for (const auto& [name, e] : target.exports) {
                const bool skipped = std::ranges::any_of(
                    ex->except, [&](const ImportName& x) { return x.name == name; });
                if (skipped) {
                    continue;
                }
                ModuleInfo::Export copy = e;
                copy.span = ex->module_span;
                add(name, copy);
            }
            break;
        }
        case ExportFromDecl::Form::StarAs: {
            ModuleInfo::Export e;
            e.is_namespace = true;
            e.module = &target;
            e.span = ex->module_span;
            add(ex->alias, e);
            break;
        }
        case ExportFromDecl::Form::Names: {
            for (const ImportName& n : ex->names) {
                const auto found = target.exports.find(n.name);
                if (found == target.exports.end()) {
                    diag_.error("E0711",
                                n.span,
                                std::format("`{}` is not exported by `{}`", n.name, ex->module),
                                "only `export`ed names can be re-exported");
                    continue;
                }
                ModuleInfo::Export copy = found->second;
                copy.span = n.span;
                add(n.alias.empty() ? n.name : n.alias, copy);
            }
            break;
        }
        }
    }
    m.exports_in_progress = false;
    m.exports_done = true;
}

}  // namespace xlang
