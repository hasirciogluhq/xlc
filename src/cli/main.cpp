// xlc: the xlang compiler. One command, like `clang++`: it translates the source it is given and
// writes the result (or prints it); it never runs, packages or fetches anything.
//
//   xlc [options] main.xl [inputs...] [-o out] [--emit=exe|obj|ir|bc|header|static|shared]
//
// The first `.xl` file is the program root; every other input is a prebuilt object or library that
// is handed to the linker.

#include "cli/emit.h"
#include "cli/style.h"
#include "xlang/hir/hir.h"
#include "xlang/lang/ast.h"
#include "xlang/lang/config.h"
#include "xlang/lang/frontend.h"
#include "xlang/sema/compilation.h"
#include "xlang/sema/sema.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/error.h"
#include "xlang/util/source.h"

#include <CLI/CLI.hpp>

#include <algorithm>
#include <filesystem>
#include <format>
#include <iostream>
#include <print>
#include <ranges>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

/// Every command-line option, in the order `--help` shows them.
struct Options {
    // What to do.
    std::vector<std::string> inputs;
    fs::path output;
    std::string emit{"exe"};
    bool check{false};
    bool dump_ast{false};
    bool dump_hir{false};
    std::string dump_layout;
    bool layout_record{false};
    bool print_cfg{false};
    bool incremental{false};
    fs::path cache_dir;
    // Modules.
    std::vector<std::string> module_roots;
    std::vector<std::string> imports;
    // Build configuration.
    std::vector<std::string> defines;
    std::vector<std::string> undefs;
    std::string target;
    std::string opt;
    bool debug{false};
    std::string cpu{"generic"};
    std::string mangle{"xl"};
    std::string safety{"checked"};
    std::string layout_scheme;
    std::string layout_history;
    // Diagnostics.
    std::vector<std::string> allow;
    std::vector<std::string> deny;
    bool warn_all{false};
    bool allow_all{false};
    std::size_t context{0};
    std::size_t context_errors{10};
    std::size_t context_warnings{2};
    std::string log_path;
    std::string diag_format{"text"};
    std::string diag_log_level{"all"};
    std::string color{"auto"};
    std::string explain;
};

void declareOptions(CLI::App& app, Options& o) {
    app.add_option("inputs", o.inputs, "Program root (.xl) followed by prebuilt objects/libraries to link");
    app.add_option("-o,--output", o.output, "Output file (default: next to the input)");
    app.add_option("--emit", o.emit, "exe|obj|ir|bc|header|static|shared (default exe)")
        ->check(CLI::IsMember({"exe", "obj", "ir", "bc", "header", "static", "shared"}));
    app.add_flag("--check", o.check, "Parse, analyze and lower; write nothing (like -fsyntax-only)");
    app.add_flag("--dump-ast", o.dump_ast, "Parse only: print the AST summary");
    app.add_flag("--dump-hir", o.dump_hir, "Analyze and lower: print the typed HIR");
    app.add_option("--dump-layout", o.dump_layout, "Print offset/size/align of every struct and class, or of TYPE")
        ->expected(0, 1)
        ->default_str("");
    app.add_flag("--layout-record", o.layout_record, "With --dump-layout: write the layouts to the history file");
    app.add_flag("--print-cfg", o.print_cfg, "Print every resolved definition and exit");
    app.add_flag("--incremental", o.incremental, "One object per module, reused from the cache when unchanged");
    app.add_option("--cache-dir", o.cache_dir, "Object cache directory (default: .xlc-cache next to the output)");

    app.add_option("-M,--module-root", o.module_roots,
                   "NAME=DIR: `import \"NAME\"` and `\"NAME/x\"` resolve inside DIR; its modules form "
                   "the library group (repeatable)");
    app.add_option("--import", o.imports, "Load module SPEC into the program though nothing imports it (repeatable)");

    app.add_option("-D,--define", o.defines, "NAME or NAME=VALUE (repeatable)");
    app.add_option("-U,--undef", o.undefs, "Remove a user definition (repeatable)");
    app.add_option("--target", o.target, "Target triple, e.g. aarch64-apple-darwin");
    app.add_option("-O", o.opt, "Optimization level 0..3 (`-O2`)")->check(CLI::IsMember({"0", "1", "2", "3"}));
    app.add_flag("-g", o.debug, "Debug info (`debug` = true)");
    app.add_option("--cpu", o.cpu, "Target CPU (default generic)");
    app.add_option("--mangle", o.mangle, "xl|none")->check(CLI::IsMember({"xl", "none"}));
    app.add_option("--safety", o.safety, "checked|unchecked")->check(CLI::IsMember({"checked", "unchecked"}));
    app.add_option("--layout-scheme", o.layout_scheme, "Pin the default layout scheme (v1)");
    app.add_option("--layout-history", o.layout_history, "Layout history file (default xl.layout next to the input)");

    app.add_option("--allow", o.allow, "Silence warning CODE (repeatable); `-Wno-CODE` is the same");
    app.add_option("--deny", o.deny, "Turn warning CODE into an error (repeatable)");
    app.add_flag("--warn-all", o.warn_all, "Enable every warning, ignoring --allow and @allow");
    app.add_flag("--allow-all", o.allow_all, "Silence every warning");
    app.add_option("--context", o.context, "Context lines around marks for errors and warnings")
        ->check(CLI::NonNegativeNumber);
    app.add_option("--context-errors", o.context_errors, "Context lines for errors (default 10)");
    app.add_option("--context-warnings", o.context_warnings, "Context lines for warnings (default 2)");
    app.add_option("--diag-log", o.log_path, "Write every diagnostic (suppressed ones included) to FILE");
    app.add_option("--diag-format", o.diag_format, "Diagnostic format: text|json")
        ->check(CLI::IsMember({"text", "json"}));
    app.add_option("--diag-log-level", o.diag_log_level, "Log level: all|warn|error")
        ->check(CLI::IsMember({"all", "warn", "error"}));
    app.add_option("--color", o.color, "Colored output: auto (terminals; NO_COLOR turns it off), always or never")
        ->check(CLI::IsMember({"auto", "always", "never"}));
    app.add_option("--explain", o.explain, "Print the long explanation of a diagnostic code");
}

[[nodiscard]] xlang::DiagnosticOptions diagnosticOptions(const CLI::App& app, const Options& o) {
    xlang::DiagnosticOptions out;
    out.allow = o.allow;
    out.deny = o.deny;
    out.warn_all = o.warn_all;
    out.allow_all = o.allow_all;
    out.color = xlang::cli::useColor(xlang::host::kStderrFd);
    out.context_errors = o.context_errors;
    out.context_warnings = o.context_warnings;
    if (app.count("--context") != 0) {
        out.context_errors = out.context_warnings = o.context;
    }
    if (app.count("--context-errors") != 0) {
        out.context_errors = o.context_errors;
    }
    if (app.count("--context-warnings") != 0) {
        out.context_warnings = o.context_warnings;
    }
    out.log_path = o.log_path;
    out.format = o.diag_format == "json" ? xlang::DiagFormat::Json : xlang::DiagFormat::Text;
    out.log_level = o.diag_log_level == "error"  ? xlang::DiagLogLevel::Error
                    : o.diag_log_level == "warn" ? xlang::DiagLogLevel::Warn
                                                 : xlang::DiagLogLevel::All;
    return out;
}

[[nodiscard]] xlang::BuildConfig buildConfig(const Options& o) {
    xlang::BuildConfig config;
    if (!o.target.empty()) {
        auto target = xlang::parseTargetTriple(o.target);
        if (!target) {
            throw xlang::XlangError(
                std::format("unsupported target `{}` (x86_64/aarch64 on linux/macos/windows)", o.target));
        }
        config.target = std::move(*target);
    }
    if (!o.opt.empty()) {
        config.opt_level = std::stoi(o.opt);
    }
    config.debug = o.debug;
    config.cpu = o.cpu;
    config.mangle = o.mangle;
    config.safety = o.safety;
    config.layout_scheme = o.layout_scheme;
    config.layout_history = o.layout_history;
    for (const std::string& def : o.defines) {
        const auto [name, value] = std::pair{def.substr(0, def.find('=')),
                                             def.find('=') == std::string::npos ? "" : def.substr(def.find('=') + 1)};
        if (name.empty()) {
            throw xlang::XlangError("empty name in -D");
        }
        config.defines[name] = xlang::ConfigValue::parse(value);
    }
    for (const std::string& name : o.undefs) {
        if (config.defines.erase(name) == 0 && config.lookup(name)) {
            throw xlang::XlangError(std::format("`{}` is a builtin definition and cannot be undefined", name));
        }
    }
    return config;
}

/// "N error(s), M warning(s)" on stderr, red when there are errors.
void printSummary(const xlang::DiagnosticEngine& diagnostics) {
    const xlang::cli::Palette c{xlang::cli::useColor(xlang::host::kStderrFd)};
    std::println(std::cerr, "{}{} error(s), {} warning(s){}",
                 diagnostics.hasErrors() ? c.boldRed() : c.boldYellow(), diagnostics.errorCount(),
                 diagnostics.warningCount(), c.reset());
}

void printError(std::string_view message) {
    const xlang::cli::Palette c{xlang::cli::useColor(xlang::host::kStderrFd)};
    std::println(std::cerr, "{}error:{} {}", c.boldRed(), c.reset(), message);
}

/// Splits `NAME=DIR` of `--module-root`.
[[nodiscard]] std::pair<std::string, fs::path> moduleRoot(const std::string& spec) {
    const std::size_t eq = spec.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == spec.size()) {
        throw xlang::XlangError(std::format("`--module-root {}` must be NAME=DIR", spec));
    }
    const fs::path dir = spec.substr(eq + 1);
    if (!fs::is_directory(dir)) {
        throw xlang::XlangError(std::format("`--module-root {}`: `{}` is not a directory", spec, dir.string()));
    }
    return {spec.substr(0, eq), dir};
}

[[nodiscard]] int run(CLI::App& app, const Options& o) {
    if (!o.explain.empty()) {
        const std::string text = xlang::DiagnosticEngine::explain(o.explain);
        if (text.empty()) {
            printError(std::format("unknown diagnostic code `{}`", o.explain));
            return 1;
        }
        std::cout << text;
        return 0;
    }

    xlang::BuildConfig config = buildConfig(o);
    if (o.print_cfg) {
        std::println("target = \"{}\"", config.target.triple);
        for (const auto& [name, value] : config.resolved()) {
            std::println("{} = {}", name, value.toString());
        }
        return 0;
    }
    if (o.inputs.empty()) {
        std::cout << app.help();
        return 0;
    }

    // The first `.xl` input is the program; the rest are link inputs.
    const auto is_source = [](const std::string& f) { return fs::path(f).extension() == ".xl"; };
    const auto source = std::ranges::find_if(o.inputs, is_source);
    if (source == o.inputs.end()) {
        throw xlang::XlangError("no input: give a `.xl` file");
    }
    const fs::path root = *source;
    xlang::cli::EmitRequest request{.root = root, .output = o.output, .emit = o.emit,
                                    .incremental = o.incremental, .cache_dir = o.cache_dir};
    for (const std::string& f : o.inputs) {
        if (&f != &*source) {
            request.inputs.emplace_back(f);
        }
    }

    xlang::SourceManager sources;
    xlang::DiagnosticEngine diagnostics(sources, std::cerr);
    diagnostics.configure(diagnosticOptions(app, o));

    if (o.dump_ast) {
        const xlang::FileId file = sources.loadFile(root);
        const xlang::Program program = xlang::parseSource(sources, file, config, diagnostics);
        diagnostics.flush();
        if (diagnostics.hasErrors()) {
            printSummary(diagnostics);
            return 1;
        }
        xlang::dumpProgram(std::cout, program, true);
        return 0;
    }

    const std::string history_path =
        config.layout_history.empty() ? (root.parent_path() / "xl.layout").string() : config.layout_history;
    xlang::LayoutHistory history = xlang::LayoutHistory::load(history_path);

    // Whole program: root + imports + `--import` modules, then analysis and lowering.
    xlang::Compilation compilation(sources, config, diagnostics, &history);
    for (const std::string& spec : o.module_roots) {
        auto [name, dir] = moduleRoot(spec);
        compilation.addModuleRoot(std::move(name), std::move(dir));
    }
    for (const std::string& spec : o.imports) {
        compilation.addImplicitImport(spec);
    }
    compilation.loadRoot(root);
    compilation.finish();
    if (!diagnostics.hasErrors()) {
        compilation.analyzeAll();
    }

    const bool wants_layout = app.count("--dump-layout") != 0;
    const bool emits = !o.check && !o.dump_hir && !wants_layout;
    if (!diagnostics.hasErrors()) {
        xlang::hir::Module module = compilation.lowerAll();
        if (o.dump_hir && !diagnostics.hasErrors()) {
            diagnostics.flush();
            xlang::hir::dumpModule(std::cout, module);
        }
        if (emits && !diagnostics.hasErrors()) {
            xlang::cli::emitOutput(compilation, module, config, sources, diagnostics, request);
        }
    }

    if (wants_layout && !diagnostics.hasErrors()) {
        xlang::Sema& sema = *compilation.rootSema();
        const std::vector<std::string> names =
            o.dump_layout.empty() ? sema.aggregateNames() : std::vector<std::string>{o.dump_layout};
        for (const std::string& name : names) {
            if (auto layout = sema.layoutOfName(name, xlang::Span{})) {
                xlang::LayoutRecord record{name, *layout};
                std::cout << xlang::formatLayoutRecord(record);
                if (o.layout_record) {
                    history.put(std::move(record));
                }
            }
        }
        if (o.layout_record && !diagnostics.hasErrors()) {
            history.save(history_path);
        }
    }

    diagnostics.flush();
    if (diagnostics.hasErrors()) {
        printSummary(diagnostics);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // `-Wno-CODE` is spelled like a single flag; rewrite it to `--allow=CODE` for CLI11.
    std::vector<std::string> args(argv, argv + argc);
    for (std::string& arg : args | std::views::drop(1)) {
        if (arg.starts_with("-Wno-") && arg.size() > 5) {
            arg = "--allow=" + arg.substr(5);
        }
    }
    std::vector<char*> raw;
    for (std::string& a : args) {
        raw.push_back(a.data());
    }

    CLI::App app{"xlc: the xlang compiler"};
    app.set_version_flag("--version", XLANG_VERSION);
    Options options;
    declareOptions(app, options);
    try {
        app.parse(static_cast<int>(raw.size()), raw.data());
    } catch (const CLI::ParseError& e) {
        return app.exit(e);
    }
    xlang::cli::colorMode() = options.color == "auto" ? "" : options.color;

    try {
        return run(app, options);
    } catch (const xlang::XlangError& error) {
        printError(error.what());
        return 1;
    }
}
