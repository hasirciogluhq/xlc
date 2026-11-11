#include "sema/compilation/compilation.hpp"

#include <format>
#include <map>

namespace xlang {

// Import bindings of one module (plan 4.6a): each imported name becomes a `Sema::ImportBinding`
// in the importing module. A conflict is reported on the import line, against the local
// definition or the earlier binding; the same entity through two paths is only a warning.

using compilation_detail::lastSegment;

void Compilation::linkImports(ModuleInfo& m) {
    struct Bound {
        Span span;
        const ModuleInfo* module; // defining module / namespace target
        std::string name;
        bool is_namespace;
    };
    std::map<std::string, Bound> bound;

    const auto bind = [&](const std::string& local,
                          Sema::ImportBinding b,
                          const ModuleInfo* module,
                          const std::string& name,
                          bool is_ns,
                          const Span& span) {
        if (const Sema::Symbol* own = m.sema->findLocal(local)) {
            Diagnostic d;
            d.code = "E0401";
            d.severity = Severity::Error;
            d.message = std::format("`{}` is defined here and imported", local);
            d.labels.push_back({declBase(*own->item).name_span, "local definition"});
            d.labels.push_back({span, "imported here"});
            d.fix = "rename the import with `as`";
            diag_.report(std::move(d));
            return;
        }
        const auto [it, inserted] = bound.emplace(local, Bound{span, module, name, is_ns});
        if (!inserted) {
            const Bound& old = it->second;
            Diagnostic d;
            d.severity = Severity::Error;
            d.labels.push_back({old.span, "first binding"});
            d.labels.push_back({span, "second binding"});
            if (old.module == module && old.name == name && old.is_namespace == is_ns) {
                d.code = "W0401";
                d.severity = Severity::Warning;
                d.message = std::format("`{}` is imported twice as the same entity", local);
                d.fix = "import it through one path, or silence with `@allow(W0401)`";
            } else {
                d.code = "E0401";
                d.message = std::format("different entities are bound to `{}`", local);
                d.fix = "rename one with `as`, or use `import * as ns from ...`";
                diag_.report(std::move(d));
                return;
            }
            diag_.report(std::move(d));
            return;
        }
        m.sema->bindImport(local, std::move(b));
    };

    const auto bindExport =
        [&](const std::string& local, const ModuleInfo::Export& e, const Span& span) {
            Sema::ImportBinding b;
            b.span = span;
            if (e.is_namespace) {
                b.kind = Sema::ImportBinding::Kind::Namespace;
                b.module_key = e.module->key;
                bind(local, std::move(b), e.module, {}, true, span);
            } else {
                b.kind = Sema::ImportBinding::Kind::Entity;
                b.module_key = e.module->key;
                b.name = e.name;
                bind(local, std::move(b), e.module, e.name, false, span);
            }
        };

    for (const Item& item : m.program.items) {
        const auto* imp = std::get_if<ImportDecl>(&item.decl);
        if (imp == nullptr) {
            continue;
        }
        const auto tit = targets_.find({imp->module, m.key});
        if (tit == targets_.end() || tit->second == nullptr) {
            continue;
        }
        ModuleInfo& target = *tit->second;
        resolveExports(target);
        switch (imp->form) {
        case ImportDecl::Form::Module:
        case ImportDecl::Form::StarAs: {
            ModuleInfo::Export ns;
            ns.is_namespace = true;
            ns.module = &target;
            const std::string name = !imp->alias.empty() ? imp->alias : lastSegment(imp->module);
            bindExport(name, ns, imp->module_span);
            break;
        }
        case ImportDecl::Form::Star:
            for (const auto& [name, e] : target.exports) {
                bindExport(name, e, imp->module_span);
            }
            break;
        case ImportDecl::Form::Names:
            for (const ImportName& n : imp->names) {
                const auto found = target.exports.find(n.name);
                if (found == target.exports.end()) {
                    diag_.error("E0711",
                                n.span,
                                std::format("`{}` is not exported by `{}`", n.name, imp->module),
                                "only names marked `export` can be imported");
                    continue;
                }
                bindExport(n.alias.empty() ? n.name : n.alias, found->second, n.span);
            }
            break;
        }
    }
}

}  // namespace xlang
