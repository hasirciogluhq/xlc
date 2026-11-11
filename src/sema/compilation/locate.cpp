#include "sema/compilation/compilation.hpp"

#include "xlang/lang/frontend.h"

#include <algorithm>
#include <format>
#include <set>
#include <sstream>

namespace xlang {

// Module resolution and loading: import spec -> file on disk (or virtual file), and file -> parsed
// `ModuleInfo` with its `Sema`. Every later stage (exports, imports, analysis, lowering) works on
// the modules registered here in `order_`.

using compilation_detail::normalizedKey;

void Compilation::addVirtualFile(const std::string& path, std::string text) {
    virtual_files_[normalizedKey(path)] = std::move(text);
}

void Compilation::addModuleRoot(std::string name, std::filesystem::path dir) {
    roots_.emplace_back(std::move(name), std::move(dir));
    // Longest name first, so `a/b` beats `a`.
    std::ranges::stable_sort(roots_, std::ranges::greater{}, [](const auto& r) { return r.first.size(); });
}

const std::pair<std::string, std::filesystem::path>* Compilation::rootOf(const std::string& spec) const {
    const auto it = std::ranges::find_if(roots_, [&](const auto& r) {
        return spec == r.first || (spec.size() > r.first.size() && spec.starts_with(r.first) && spec[r.first.size()] == '/');
    });
    return it == roots_.end() ? nullptr : &*it;
}

bool Compilation::fileExists(const std::filesystem::path& p) const {
    if (virtual_files_.contains(normalizedKey(p))) {
        return true;
    }
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

std::string Compilation::keyFor(const std::filesystem::path& path, bool is_root) const {
    if (is_root) {
        return {};
    }
    std::filesystem::path p = path.lexically_normal();
    const auto stripIndex = [](std::string s) {
        if (s.ends_with("/index")) {
            s.resize(s.size() - 6);
        } else if (s == "index") {
            s.clear();
        }
        return s;
    };
    const auto relativeTo = [&](const std::filesystem::path& base) -> std::optional<std::string> {
        if (base.empty()) {
            return std::nullopt;
        }
        const std::filesystem::path rel = p.lexically_relative(base.lexically_normal());
        if (rel.empty() || *rel.begin() == "..") {
            return std::nullopt;
        }
        std::filesystem::path stem = rel;
        stem.replace_extension();
        return stripIndex(stem.generic_string());
    };
    // Module roots may live outside or inside the program's directory, so they are checked before
    // it. Their keys are import paths: `name/sub`.
    for (const auto& [name, dir] : roots_) {
        if (const auto r = relativeTo(dir)) {
            return r->empty() ? name : name + "/" + *r;
        }
    }
    if (const auto r = relativeTo(root_dir_)) {
        return *r;
    }
    std::filesystem::path stem = p;
    stem.replace_extension();
    return stripIndex(stem.generic_string());
}

std::optional<std::filesystem::path>
Compilation::locate(const std::string& spec, const ModuleInfo* from, const Span& at) {
    std::vector<std::filesystem::path> bases;
    std::string rest = spec;
    if (const auto* root = rootOf(spec)) {
        bases.push_back(root->second);
        rest = spec.size() == root->first.size() ? std::string{} : spec.substr(root->first.size() + 1);
    }
    if (bases.empty()) {
        if (from != nullptr && !from->path.empty()) {
            bases.push_back(from->path.parent_path());
        }
        if (!root_dir_.empty()) {
            bases.push_back(root_dir_);
        }
        if (bases.empty()) {
            bases.push_back(std::filesystem::path{});
        }
    }
    for (const std::filesystem::path& base : bases) {
        const std::filesystem::path as_file =
            rest.empty() ? base / "index.xl" : base / (rest + ".xl");
        const std::filesystem::path as_dir =
            rest.empty() ? base / "index.xl" : base / rest / "index.xl";
        const bool f = fileExists(as_file);
        const bool d = fileExists(as_dir);
        if (f && d && as_file != as_dir) {
            diag_.error("E0711",
                        at,
                        std::format("module path `{}` is ambiguous", spec),
                        std::format("both `{}` and `{}` exist",
                                    as_file.generic_string(),
                                    as_dir.generic_string()));
            return std::nullopt;
        }
        if (f) {
            return as_file;
        }
        if (d) {
            return as_dir;
        }
    }
    return std::nullopt;
}

ModuleInfo* Compilation::importTarget(ModuleInfo& from, const std::string& spec, const Span& at) {
    const auto key = std::make_pair(spec, from.key);
    if (const auto it = targets_.find(key); it != targets_.end()) {
        return it->second;
    }
    const auto path = locate(spec, &from, at);
    ModuleInfo* target = nullptr;
    if (path) {
        target = loadModule(*path, false);
    } else {
        diag_.error("E0711",
                    at,
                    std::format("module `{}` not found", spec),
                    "no `.xl` file or `index.xl` directory with that path");
    }
    targets_[key] = target;
    return target;
}

void Compilation::collectMacros(ModuleInfo& dep,
                                MacroTable& table,
                                std::set<const ModuleInfo*>& seen) {
    if (!seen.insert(&dep).second) {
        return;
    }
    for (const Item& item : dep.program.items) {
        if (const auto* m = std::get_if<DefineMacroDecl>(&item.decl);
            m != nullptr && item.modifiers.exported) {
            MacroDef def;
            def.name = m->name;
            def.params = m->params;
            def.body = m->body;
            def.span = m->name_span;
            table.macros.emplace(def.name, std::move(def));
        }
        if (const auto* e = std::get_if<ExportFromDecl>(&item.decl)) {
            const auto it = targets_.find({e->module, dep.key});
            if (it != targets_.end() && it->second != nullptr) {
                collectMacros(*it->second, table, seen);
            }
        }
    }
}

ModuleInfo* Compilation::loadModule(const std::filesystem::path& path, bool is_root) {
    const std::string norm = normalizedKey(path);
    if (const auto it = by_path_.find(norm); it != by_path_.end()) {
        return it->second.get();
    }
    auto owned = std::make_unique<ModuleInfo>();
    ModuleInfo* m = owned.get();
    by_path_[norm] = std::move(owned);
    m->path = path;
    if (is_root) {
        root_ = m;
        root_dir_ = path.parent_path();
    }
    m->key = keyFor(path, is_root);
    m->is_library = std::ranges::any_of(
        roots_, [&](const auto& r) { return norm.starts_with(normalizedKey(r.second) + "/"); });
    {
        std::string name = m->key;
        if (name.empty()) {
            name = path.stem().string();
        }
        std::replace(name.begin(), name.end(), '/', '.');
        m->mangle_name = name;
    }
    by_key_[m->key] = m;

    if (const auto it = virtual_files_.find(norm); it != virtual_files_.end()) {
        m->file = sources_.addFile(path, it->second);
    } else {
        m->file = sources_.loadFile(path);
    }

    // Pass 0 (silent): find the imports so exported macros of the dependencies can be seeded.
    std::ostringstream sink;
    DiagnosticEngine quiet(sources_, sink);
    MacroTable discard; // pass 0 also collects the module's own macros: keep them out of the seed
    Program first = parseSource(sources_, m->file, config_, quiet, discard);
    MacroTable seed;
    std::set<const ModuleInfo*> seen;
    seen.insert(m);
    for (const Item& item : first.items) {
        std::string spec;
        Span at;
        if (const auto* imp = std::get_if<ImportDecl>(&item.decl)) {
            spec = imp->module;
            at = imp->module_span;
        } else if (const auto* ex = std::get_if<ExportFromDecl>(&item.decl)) {
            spec = ex->module;
            at = ex->module_span;
        } else {
            continue;
        }
        if (ModuleInfo* dep = importTarget(*m, spec, at)) {
            collectMacros(*dep, seed, seen);
            if (std::find(m->deps.begin(), m->deps.end(), dep) == m->deps.end()) {
                m->deps.push_back(dep);
            }
        }
    }

    // Pass 1: the real parse with the imported macros known.
    m->program = parseSource(sources_, m->file, config_, diag_, seed);
    m->sema = std::make_unique<Sema>(m->program, config_, diag_, history_);
    m->sema->attach(this, m->key);
    order_.push_back(m);
    return m;
}

ModuleInfo* Compilation::loadRoot(const std::filesystem::path& path) {
    return loadModule(path, true);
}

}  // namespace xlang
