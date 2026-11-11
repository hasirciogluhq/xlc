#include "sema/compilation/compilation.hpp"

#include "sema/lower/lower.hpp"

#include <format>
#include <set>
#include <vector>

namespace xlang {

// Driver of the semantic phases: module loading lives in `locate.cpp`, exports in `exports.cpp`,
// import bindings in `imports.cpp`, role registrations in `registered.cpp`. This part owns the
// object lifecycle, the phase order (`finish` -> `analyzeAll` -> `lowerAll`) and the whole-program
// lowering: `@init` order and the `W0301` check for `declare`d symbols nothing defines.

using compilation_detail::initPostOrder;
using compilation_detail::normalizedKey;

Compilation::Compilation(SourceManager& sources,
                         const BuildConfig& config,
                         DiagnosticEngine& diagnostics,
                         const LayoutHistory* history)
    : sources_(sources), config_(config), diag_(diagnostics), history_(history) {}

Compilation::~Compilation() = default;

// ---------------------------------------------------------------------------------------------
// Services for `Sema` / `Lowerer`

Sema* Compilation::semaOf(const std::string& key) {
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? nullptr : it->second->sema.get();
}

ModuleInfo* Compilation::moduleOf(const std::string& key) {
    const auto it = by_key_.find(key);
    return it == by_key_.end() ? nullptr : it->second;
}

std::optional<Sema::EntityRef> Compilation::exportedEntity(const std::string& module_key,
                                                           const std::string& name) {
    ModuleInfo* m = moduleOf(module_key);
    if (m == nullptr) {
        return std::nullopt;
    }
    const auto it = m->exports.find(name);
    if (it == m->exports.end() || it->second.is_namespace || it->second.module == nullptr) {
        return std::nullopt;
    }
    Sema* owner = it->second.module->sema.get();
    const Sema::Symbol* sym = owner->findLocal(it->second.name);
    if (sym == nullptr) {
        return std::nullopt;
    }
    return Sema::EntityRef{owner, sym, it->second.name};
}

std::optional<Sema::EntityRef> Compilation::findRegistered(const std::string& name) {
    const auto it = registered_.find(name);
    if (it == registered_.end()) {
        return std::nullopt;
    }
    return it->second;
}

// ---------------------------------------------------------------------------------------------
// Phase order

void Compilation::finish() {
    for (const std::string& spec : implicit_) {
        if (const auto path = locate(spec, root_, Span{})) {
            implicit_modules_.push_back(loadModule(*path, false));
        } else {
            diag_.error("E0711", Span{}, std::format("module `{}` not found", spec),
                        "`--import` takes a module path: a `--module-root` name, or a path next to the root file");
        }
    }
    for (ModuleInfo* m : order_) {
        resolveExports(*m);
    }
    for (ModuleInfo* m : order_) {
        linkImports(*m);
    }
    collectRegistered();
}

void Compilation::analyzeAll() {
    for (ModuleInfo* m : order_) {
        m->sema->analyze();
    }
}

hir::Module Compilation::lowerAll() {
    std::vector<Sema*> modules;
    std::set<const Sema*> library_modules;
    for (ModuleInfo* m : order_) {
        modules.push_back(m->sema.get());
        if (m->is_library) {
            library_modules.insert(m->sema.get());
        }
    }
    // `@init` order: the library group first, then the rest. Each group starts at its entry modules
    // (the implicit imports, the root) and takes their dependencies first; leftovers follow `order_`.
    std::vector<ModuleInfo*> starts = implicit_modules_;
    for (ModuleInfo* m : order_) {
        if (m->is_library) {
            starts.push_back(m);
        }
    }
    if (root_ != nullptr) {
        starts.push_back(root_);
    }
    for (ModuleInfo* m : order_) {
        starts.push_back(m);
    }
    std::set<const ModuleInfo*> done;
    std::vector<Sema*> init_order;
    for (ModuleInfo* m : starts) {
        initPostOrder(m, done, init_order);
    }
    hir::Module out = lowerProgram(modules, init_order, library_modules);
    // Plan 4.6b: a `declare`d symbol nothing in this compilation defines is only a warning (it is
    // looked up at link time; with `--emit=obj` that is normal).
    std::set<std::string> defined;
    for (const hir::Function& f : out.functions) {
        if (!f.declared && f.has_body) {
            defined.insert(f.symbol);
        }
    }
    for (const hir::Global& g : out.globals) {
        if (!g.declared) {
            defined.insert(g.symbol);
        }
    }
    const auto warn = [&](const std::string& symbol, const Span& span, const char* what) {
        Diagnostic d;
        d.code = "W0301";
        d.severity = Severity::Warning;
        d.message =
            std::format("declared {} `{}` is not defined in this compilation", what, symbol);
        d.labels.push_back({span, "resolved at link time"});
        d.detail = "No module of this compilation defines this symbol.";
        d.reason = "The linker must find it in another object or library; with `--emit=obj` this "
                   "is expected.";
        d.fix = "link the object/library that defines it, or silence with `@allow(W0301)`";
        diag_.report(std::move(d));
    };
    for (const hir::Function& f : out.functions) {
        // A `@library` import is resolved by the OS library, not by another object.
        if (f.declared && f.import_library.empty() && !defined.contains(f.symbol)) {
            warn(f.symbol, f.span, "function");
        }
    }
    for (const hir::Global& g : out.globals) {
        if (g.declared && !defined.contains(g.symbol)) {
            warn(g.symbol, g.span, "global");
        }
    }
    return out;
}

}  // namespace xlang
