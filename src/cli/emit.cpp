#include "cli/emit.h"

#include "xlang/codegen/codegen.h"
#include "xlang/codegen/link.h"
#include "xlang/hir/prune.h"
#include "xlang/util/error.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <format>
#include <fstream>
#include <functional>
#include <map>
#include <ranges>

namespace xlang::cli {

namespace {

namespace fs = std::filesystem;

/// Default output path: next to the input, named after it (`main.xl` -> `main`, `main.exe`, `libmain.a`...).
[[nodiscard]] fs::path defaultOutput(const fs::path& root, std::string_view emit, std::string_view os) {
    const bool windows = os == "windows";
    const std::string stem = root.stem().string();
    const fs::path dir = root.parent_path();
    if (emit == "exe") {
        return dir / (stem + (windows ? ".exe" : ""));
    }
    if (emit == "obj") {
        return dir / (stem + (windows ? ".obj" : ".o"));
    }
    if (emit == "ir") {
        return dir / (stem + ".ll");
    }
    if (emit == "bc") {
        return dir / (stem + ".bc");
    }
    if (emit == "header") {
        return dir / (stem + ".h");
    }
    if (emit == "static") {
        return dir / (windows ? stem + ".lib" : "lib" + stem + ".a");
    }
    return dir / ((windows ? "" : "lib") + stem + (windows ? ".dll" : os == "macos" ? ".dylib" : ".so"));
}

/// `@library("x")` imports of the program, for the Windows linker step.
void collectImports(const hir::Module& module, codegen::LinkRequest& req) {
    for (const auto& f : module.functions) {
        if (f.declared && !f.import_library.empty()) {
            req.imports.emplace_back(f.import_library, f.symbol);
        }
    }
}

/// FNV-1a step over `s` (with a separator), used for the incremental cache keys.
[[nodiscard]] std::uint64_t mix(std::uint64_t h, std::string_view s) {
    for (const unsigned char ch : s) {
        h = (h ^ ch) * 1099511628211ULL;
    }
    return (h ^ 0xff) * 1099511628211ULL;
}

/// Reports the entry-point rules (`@entry` exists for an executable, never for a library or object,
/// takes no arguments for a DLL, and a Windows program takes them only with `register(args)`). True
/// when the output may be produced.
[[nodiscard]] bool checkEntry(const hir::Module& module, const BuildConfig& config, DiagnosticEngine& diagnostics,
                              const EmitRequest& request) {
    const hir::Function* entry = nullptr;
    for (const hir::Function& f : module.functions) {
        entry = f.is_entry ? &f : entry;
    }
    const std::string& emit = request.emit;
    const codegen::Output output = codegen::outputOf(emit);
    if (emit == "exe" && entry == nullptr) {
        diagnostics.error("E0739", Span{},
                          std::format("`{}` has no entry point: no `@entry` function", request.root.filename().string()),
                          "mark one module-level function `@entry fn main()`, or build the file as a library with "
                          "`--emit=obj`, `--emit=static` or `--emit=shared`");
    } else if (entry != nullptr && (emit == "static" || emit == "obj")) {
        diagnostics.error("E0754", entry->span,
                          std::format("`@entry` on `{}` in a static library or object file", entry->name),
                          "remove `@entry`: a library has no entry point, use `@init` for startup work or `@fini` "
                          "for teardown");
    } else if (entry != nullptr && entry->param_count > 0 &&
               (output == codegen::Output::Shared ||
                (config.target.os == "windows" && module.args_function.empty()))) {
        const bool shared = output == codegen::Output::Shared;
        diagnostics.error("E0755", entry->span,
                          std::format("`@entry` on `{}` takes arguments, which {}", entry->name,
                                      shared ? "a shared library does not pass"
                                             : "a Windows program gets only from `register(args)`"),
                          shared ? "write `@entry fn name()` with no parameters"
                                 : "link the standard library (it registers the role), or write "
                                   "`register(args) fn name(argc: *i32) -> **u8` that parses the "
                                   "command line");
    } else {
        return true;
    }
    return false;
}

/// Archives (`static`) or links (`exe`, `shared`) `objects` plus the prebuilt inputs into `out`.
void archiveOrLink(const EmitRequest& request, const fs::path& out, std::vector<fs::path> objects,
                   const hir::Module& module, const BuildConfig& config) {
    if (request.emit == "static") {
        codegen::archive(objects, out, config);
        return;
    }
    codegen::LinkRequest req;
    req.objects = std::move(objects);
    req.objects.insert(req.objects.end(), request.inputs.begin(), request.inputs.end());
    req.output = out;
    req.kind = request.emit == "shared" ? codegen::LinkKind::Shared : codegen::LinkKind::Executable;
    req.config = &config;
    collectImports(module, req);
    codegen::link(req);
}

/// Plan 8.1/8.2: one object per module, keyed by the content hash of the module, its dependencies
/// and the build configuration; an object that is already in the cache is not generated again.
void emitIncremental(Compilation& compilation, hir::Module& module, const BuildConfig& config,
                     SourceManager& sources, DiagnosticEngine& diagnostics, const EmitRequest& request,
                     const fs::path& out, const codegen::LayoutQuery& layoutOf) {
    const bool windows = config.target.os == "windows";
    const std::string stem = request.root.stem().string();
    const fs::path dir = request.cache_dir.empty() ? out.parent_path() / ".xlc-cache" : request.cache_dir;
    fs::create_directories(dir);

    std::string cfg = std::format("xlc-{}|{}|{}|{}|O{}|{}|{}|{}|{}|{}|{}", XLANG_VERSION, config.target.triple,
                                  config.target.os, config.target.arch, config.opt_level, config.debug ? "g" : "-",
                                  config.cpu, config.mangle, config.safety, config.layout_scheme, request.emit);
    for (const auto& [k, v] : config.defines) {
        cfg += "|D:" + k + "=" + v.toString();
    }

    std::map<const ModuleInfo*, std::uint64_t> base;
    const std::function<std::uint64_t(const ModuleInfo&)> baseHash = [&](const ModuleInfo& m) -> std::uint64_t {
        if (const auto it = base.find(&m); it != base.end()) {
            return it->second;
        }
        base[&m] = 0; // import cycles terminate here
        std::uint64_t h = mix(mix(mix(14695981039346656037ULL, cfg), m.key), sources.file(m.file).text());
        for (const ModuleInfo* d : m.deps) {
            h = mix(h, std::to_string(baseHash(*d)));
        }
        return base[&m] = h;
    };

    // Registered names (panic, allocator, ...) are visible everywhere: they salt every module hash.
    std::string salt;
    for (const ModuleInfo* m : compilation.order()) {
        if (std::ranges::any_of(m->program.items, [](const Item& i) { return i.modifiers.registered; })) {
            salt += std::to_string(baseHash(*m)) + ";";
        }
    }

    std::vector<fs::path> objects;
    for (ModuleInfo* m : compilation.order()) {
        const bool has_code = m->key.empty() ||
                              std::ranges::any_of(module.functions, [&](const auto& f) { return f.module == m->key; }) ||
                              std::ranges::any_of(module.globals, [&](const auto& g) { return g.module == m->key; });
        if (!has_code) {
            continue;
        }
        std::string name = m->key.empty() ? stem : m->key;
        std::ranges::replace(name, '/', '.');
        const fs::path obj = dir / std::format("{}-{:016x}{}", name, mix(baseHash(*m), salt), windows ? ".obj" : ".o");
        if (!fs::exists(obj)) {
            codegen::CodegenInput cg{module, config, sources, diagnostics, layoutOf, name, codegen::outputOf(request.emit)};
            cg.only_module = m->key;
            auto artifact = codegen::generate(cg);
            if (!artifact) {
                // The back end failed without reporting why: say so, so the build does not end with no error at all.
                if (!diagnostics.hasErrors()) {
                    diagnostics.error("E0714", Span{},
                                      std::format("code generation for module `{}` failed without a diagnostic", name),
                                      "internal error: the back end returned no object");
                }
                return;
            }
            artifact->optimize(config.opt_level);
            artifact->writeObject(obj);
        }
        objects.push_back(obj);
    }
    if (!diagnostics.hasErrors()) {
        archiveOrLink(request, out, std::move(objects), module, config);
    }
}

} // namespace

void emitOutput(Compilation& compilation, hir::Module& module, const BuildConfig& config, SourceManager& sources,
                DiagnosticEngine& diagnostics, const EmitRequest& request) {
    // Plan 24: only what the roots reach is code-generated and linked. Libraries and objects keep
    // every `export` as part of their interface; an executable keeps its `export(c)` functions only
    // when it links prebuilt objects (they may call back into it), and nothing else.
    hir::pruneUnreachable(module,
                          request.emit != "exe"     ? hir::KeepExports::All
                          : request.inputs.empty() ? hir::KeepExports::None
                                                   : hir::KeepExports::C);
    Sema& root = *compilation.rootSema();
    const codegen::LayoutQuery layoutOf = [&root](const Type& t) { return root.layoutOf(t, Span{}); };
    const fs::path out =
        request.output.empty() ? defaultOutput(request.root, request.emit, config.target.os) : request.output;
    const std::string stem = request.root.stem().string();

    if (!checkEntry(module, config, diagnostics, request)) {
        return;
    }
    if (request.emit == "header") {
        std::string guard = "XLANG_" + stem + "_H";
        std::ranges::transform(guard, guard.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
        std::ofstream(out) << codegen::generateHeader(module, layoutOf, guard);
        return;
    }
    const bool linked = request.emit == "exe" || request.emit == "static" || request.emit == "shared";
    if (request.incremental && linked) {
        emitIncremental(compilation, module, config, sources, diagnostics, request, out, layoutOf);
        return;
    }

    codegen::CodegenInput cg{module, config, sources, diagnostics, layoutOf, stem, codegen::outputOf(request.emit)};
    auto artifact = codegen::generate(cg);
    if (!artifact) {
        return;
    }
    artifact->optimize(config.opt_level);
    if (request.emit == "ir") {
        artifact->writeIR(out);
    } else if (request.emit == "bc") {
        artifact->writeBitcode(out);
    } else if (request.emit == "obj") {
        artifact->writeObject(out);
    } else {
        // The intermediate object sits next to the output (same directory, so the same permissions
        // and file system) and is removed whether or not the link succeeds.
        fs::path obj = out;
        obj += config.target.os == "windows" ? ".tmp.obj" : ".tmp.o";
        artifact->writeObject(obj);
        try {
            archiveOrLink(request, out, {obj}, module, config);
        } catch (...) {
            fs::remove(obj);
            throw;
        }
        fs::remove(obj);
    }
}

} // namespace xlang::cli
