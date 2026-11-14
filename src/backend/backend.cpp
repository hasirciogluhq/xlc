// The backend behind the C API of include/xlang/backend_c.h: one session drives the whole C++
// pipeline (compilation, lowering, analyses, emission) exactly as src/cli/main.cpp arranged it
// before the CLI moved to xlang. The entry points convert the flat boundary values, keep the
// ordering guarantees of the old `run()`, and turn every `XlangError` into a reported diagnostic
// and a failing exit code.
#include "xlang/backend_c.h"

#include "cli/emit.h"
#include "cli/style.h"
#include "xlang/analysis/analysis.h"
#include "xlang/hir/hir.h"
#include "xlang/lang/config.h"
#include "xlang/lang/frontend.h"
#include "xlang/mir/build.h"
#include "xlang/sema/compilation.h"
#include "xlang/sema/sema.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/error.h"
#include "xlang/util/source.h"

#include <cstdint>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace xlang {

namespace fs = std::filesystem;

namespace {

/// The bytes of an empty string: the boundary wants a valid pointer even for length zero.
const char kEmptyBytes[1] = {'\0'};

std::string_view textOf(XlcStr s) {
    return std::string_view(s.bytes == nullptr ? kEmptyBytes : s.bytes, s.len);
}

std::string stringOf(XlcStr s) {
    return std::string(textOf(s));
}

std::vector<std::string> listOf(XlcStrList list) {
    std::vector<std::string> out;
    out.reserve(list.count);
    for (std::uint32_t i = 0; i < list.count; ++i) {
        out.emplace_back(textOf(list.items[i]));
    }
    return out;
}

/// The diagnostic configuration the C++ CLI derived from the parsed options.
DiagnosticOptions diagnosticOptions(const XlcConfig& config) {
    DiagnosticOptions out;
    out.allow = listOf(config.allow);
    out.deny = listOf(config.deny);
    out.warn_all = config.warn_all != 0;
    out.allow_all = config.allow_all != 0;
    out.color = cli::useColor(host::kStderrFd);
    out.context_errors = static_cast<std::size_t>(config.context_errors);
    out.context_warnings = static_cast<std::size_t>(config.context_warnings);
    out.log_path = stringOf(config.log_path);
    out.format = textOf(config.diag_format) == "json" ? DiagFormat::Json : DiagFormat::Text;
    out.log_level = textOf(config.diag_log_level) == "error"   ? DiagLogLevel::Error
                    : textOf(config.diag_log_level) == "warn" ? DiagLogLevel::Warn
                                                              : DiagLogLevel::All;
    return out;
}

/// The build configuration the C++ CLI derived from the parsed options; throws `XlangError` on
/// a bad target or definition, like the CLI did.
BuildConfig buildConfig(const XlcConfig& raw) {
    BuildConfig config;
    const std::string target = stringOf(raw.target);
    if (!target.empty()) {
        auto parsed = parseTargetTriple(target);
        if (!parsed) {
            throw XlangError(std::format("unsupported target `{}` (x86_64/aarch64 on "
                                         "linux/macos/windows)",
                                         target));
        }
        config.target = std::move(*parsed);
    }
    const std::string opt = stringOf(raw.opt);
    if (!opt.empty()) {
        config.opt_level = std::stoi(opt);
    }
    config.debug = raw.debug != 0;
    config.cpu = stringOf(raw.cpu);
    config.mangle = stringOf(raw.mangle);
    config.safety = stringOf(raw.safety);
    config.layout_scheme = stringOf(raw.layout_scheme);
    config.layout_history = stringOf(raw.layout_history);
    config.crt_link = raw.crt != 0;
    for (const std::string& def : listOf(raw.defines)) {
        const auto [name, value] = std::pair{def.substr(0, def.find('=')),
                                             def.find('=') == std::string::npos
                                                 ? ""
                                                 : def.substr(def.find('=') + 1)};
        if (name.empty()) {
            throw XlangError("empty name in -D");
        }
        config.defines[name] = ConfigValue::parse(value);
    }
    for (const std::string& name : listOf(raw.undefs)) {
        if (config.defines.erase(name) == 0 && config.lookup(name)) {
            throw XlangError(std::format(
                "`{}` is a builtin definition and cannot be undefined", name));
        }
    }
    return config;
}

void printError(std::string_view message) {
    const cli::Palette c{cli::useColor(host::kStderrFd)};
    std::cerr << std::format("{}error:{} {}\n", c.boldRed(), c.reset(), message);
}

/// The state of one compilation: everything `run()` of the old C++ CLI held between the stages
/// the CLI now drives one call at a time.
class Session {
public:
    explicit Session(const XlcConfig& raw) : config_(buildConfig(raw)) {
        // The color mode of the CLI decides what `useColor` reports everywhere below.
        cli::colorMode() = textOf(raw.color_mode) == "auto" ? "" : stringOf(raw.color_mode);
        diagnostics_.configure(diagnosticOptions(raw));
    }

    DiagnosticEngine& diagnostics() { return diagnostics_; }
    const BuildConfig& config() const { return config_; }

    void addModuleRoot(std::string name, fs::path dir) {
        module_roots_.emplace_back(std::move(name), std::move(dir));
    }

    void addImplicitImport(std::string spec) { imports_.push_back(std::move(spec)); }

    void loadRoot(const fs::path& root) {
        // The layout history sits next to the root unless `--layout-history` named a file; the
        // compilation borrows it, so it lives in the session.
        const std::string history_path = config_.layout_history.empty()
                                             ? (root.parent_path() / "xl.layout").string()
                                             : config_.layout_history;
        history_ = std::make_unique<LayoutHistory>(LayoutHistory::load(history_path));
        history_path_ = history_path;
        compilation_ = std::make_unique<Compilation>(sources_, config_, diagnostics_,
                                                     history_.get());
        for (auto& [name, dir] : module_roots_) {
            compilation_->addModuleRoot(name, dir);
        }
        for (const std::string& spec : imports_) {
            compilation_->addImplicitImport(spec);
        }
        compilation_->loadRoot(root);
        root_ = root;
    }

    void analyze() {
        compilation_->finish();
        if (!diagnostics_.hasErrors()) {
            compilation_->analyzeAll();
        }
    }

    /// Lowers once: HIR, then MIR, then the static analyses, in the order of the old `run()`.
    /// Never runs on a program with errors.
    void ensureLowered() {
        if (lowered_ || diagnostics_.hasErrors()) {
            return;
        }
        hir_module_ = compilation_->lowerAll();
        mir_module_ = mir::build(hir_module_);
        if (!diagnostics_.hasErrors()) {
            analysis::analyze(hir_module_, *mir_module_, diagnostics_);
        }
        lowered_ = true;
    }

    hir::Module& hirModule() { return hir_module_; }
    mir::Module& mirModule() { return *mir_module_; }
    Compilation& compilation() { return *compilation_; }
    SourceManager& sources() { return sources_; }
    const std::string& historyPath() const { return history_path_; }
    LayoutHistory& history() { return *history_; }
    const fs::path& root() const { return root_; }

private:
    BuildConfig config_;
    SourceManager sources_;
    DiagnosticEngine diagnostics_{sources_, std::cerr};
    std::vector<std::pair<std::string, fs::path>> module_roots_;
    std::vector<std::string> imports_;
    std::unique_ptr<LayoutHistory> history_;
    std::string history_path_;
    std::unique_ptr<Compilation> compilation_;
    fs::path root_;
    bool lowered_{false};
    hir::Module hir_module_;
    std::optional<mir::Module> mir_module_;
};

/// The guard every entry point uses: report an `XlangError` (or anything else) and fail. An
/// empty message means the failure was already reported in its own words.
template <typename F>
int32_t guard(F&& body) {
    try {
        body();
        return 0;
    } catch (const XlangError& error) {
        if (error.what()[0] != '\0') {
            printError(error.what());
        }
        return 1;
    } catch (const std::exception& error) {
        printError(error.what());
        return 1;
    }
}

cli::EmitRequest emitRequestOf(const XlcEmitRequest& raw) {
    cli::EmitRequest request;
    request.root = stringOf(raw.root);
    request.output = stringOf(raw.output);
    request.emit = stringOf(raw.emit);
    request.incremental = raw.incremental != 0;
    request.cache_dir = stringOf(raw.cache_dir);
    for (const std::string& input : listOf(raw.inputs)) {
        request.inputs.emplace_back(input);
    }
    for (const std::string& arg : listOf(raw.link_args)) {
        request.link_args.push_back(arg);
    }
    return request;
}

}  // namespace

}  // namespace xlang

using namespace xlang;

int32_t XLC_BACKEND_CALL xlcExplain(XlcStr code) {
    return guard([&] {
        const std::string text = DiagnosticEngine::explain(stringOf(code));
        if (text.empty()) {
            printError(std::format("unknown diagnostic code `{}`", textOf(code)));
            throw XlangError("");
        }
        std::cout << text;
        std::cout.flush();
    });
}

void XLC_BACKEND_CALL xlcVersion(void) {
    std::cout << XLANG_VERSION << "\n";
    std::cout.flush();
}

int32_t XLC_BACKEND_CALL xlcPrintCfg(const XlcConfig* config) {
    return guard([&] {
        const BuildConfig built = buildConfig(*config);
        std::cout << std::format("target = \"{}\"\n", built.target.triple);
        for (const auto& [name, value] : built.resolved()) {
            std::cout << std::format("{} = {}\n", name, value.toString());
        }
        std::cout.flush();
    });
}

int32_t XLC_BACKEND_CALL xlcDumpAst(const XlcConfig* config, XlcStr path) {
    return guard([&] {
        BuildConfig built = buildConfig(*config);
        SourceManager sources;
        DiagnosticEngine diagnostics(sources, std::cerr);
        diagnostics.configure(diagnosticOptions(*config));
        const FileId file = sources.loadFile(stringOf(path));
        const Program program = parseSource(sources, file, built, diagnostics);
        diagnostics.flush();
        if (diagnostics.hasErrors()) {
            const cli::Palette c{cli::useColor(host::kStderrFd)};
            std::cerr << std::format("{}{} error(s), {} warning(s){}\n",
                                     diagnostics.hasErrors() ? c.boldRed() : c.boldYellow(),
                                     diagnostics.errorCount(), diagnostics.warningCount(),
                                     c.reset());
            throw XlangError("");
        }
        dumpProgram(std::cout, program, true);
        std::cout.flush();
    });
}

XlcSession* XLC_BACKEND_CALL xlcSessionNew(const XlcConfig* config) {
    try {
        return reinterpret_cast<XlcSession*>(new Session(*config));
    } catch (const XlangError& error) {
        printError(error.what());
        return nullptr;
    } catch (const std::exception& error) {
        printError(error.what());
        return nullptr;
    }
}

void XLC_BACKEND_CALL xlcSessionFree(XlcSession* session) {
    delete reinterpret_cast<Session*>(session);
}

int32_t XLC_BACKEND_CALL xlcSessionAddModuleRoot(XlcSession* session, XlcStr spec) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        const std::string text = stringOf(spec);
        const std::size_t eq = text.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == text.size()) {
            throw XlangError(std::format("`--module-root {}` must be NAME=DIR", text));
        }
        const fs::path directory = text.substr(eq + 1);
        if (!fs::is_directory(directory)) {
            throw XlangError(std::format("`--module-root {}`: `{}` is not a directory", text,
                                         directory.string()));
        }
        s->addModuleRoot(text.substr(0, eq), directory);
    });
}

int32_t XLC_BACKEND_CALL xlcSessionAddImplicitImport(XlcSession* session, XlcStr spec) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] { s->addImplicitImport(stringOf(spec)); });
}

int32_t XLC_BACKEND_CALL xlcSessionLoadRoot(XlcSession* session, XlcStr path) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] { s->loadRoot(stringOf(path)); });
}

int32_t XLC_BACKEND_CALL xlcSessionAnalyze(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] { s->analyze(); });
}

int32_t XLC_BACKEND_CALL xlcSessionDumpHir(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        s->ensureLowered();
        if (!s->diagnostics().hasErrors()) {
            s->diagnostics().flush();
            hir::dumpModule(std::cout, s->hirModule());
            std::cout.flush();
        }
    });
}

int32_t XLC_BACKEND_CALL xlcSessionDumpMir(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        s->ensureLowered();
        if (!s->diagnostics().hasErrors()) {
            s->diagnostics().flush();
            mir::dumpModule(std::cout, s->mirModule());
            std::cout.flush();
        }
    });
}

int32_t XLC_BACKEND_CALL xlcSessionDumpLayout(XlcSession* session, XlcStr type_name,
                                              int32_t record) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        Sema& sema = *s->compilation().rootSema();
        const std::string name = stringOf(type_name);
        const std::vector<std::string> names =
            name.empty() ? sema.aggregateNames() : std::vector<std::string>{name};
        for (const std::string& aggregate : names) {
            if (auto layout = sema.layoutOfName(aggregate, Span{})) {
                LayoutRecord layout_record{aggregate, *layout};
                std::cout << formatLayoutRecord(layout_record);
                if (record != 0) {
                    s->history().put(std::move(layout_record));
                }
            }
        }
        std::cout.flush();
        if (record != 0 && !s->diagnostics().hasErrors()) {
            s->history().save(s->historyPath());
        }
    });
}

int32_t XLC_BACKEND_CALL xlcSessionEmit(XlcSession* session, const XlcEmitRequest* request) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        s->ensureLowered();
        if (!s->diagnostics().hasErrors()) {
            cli::emitOutput(s->compilation(), s->hirModule(), s->config(), s->sources(),
                            s->diagnostics(), emitRequestOf(*request));
        }
    });
}

int32_t XLC_BACKEND_CALL xlcSessionHasErrors(XlcSession* session) {
    return reinterpret_cast<Session*>(session)->diagnostics().hasErrors() ? 1 : 0;
}

int64_t XLC_BACKEND_CALL xlcSessionErrorCount(XlcSession* session) {
    return static_cast<int64_t>(reinterpret_cast<Session*>(session)->diagnostics().errorCount());
}

int64_t XLC_BACKEND_CALL xlcSessionWarningCount(XlcSession* session) {
    return static_cast<int64_t>(
        reinterpret_cast<Session*>(session)->diagnostics().warningCount());
}

void XLC_BACKEND_CALL xlcSessionPrintSummary(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    s->diagnostics().flush();
    const cli::Palette c{cli::useColor(host::kStderrFd)};
    std::cerr << std::format("{}{} error(s), {} warning(s){}\n",
                             s->diagnostics().hasErrors() ? c.boldRed() : c.boldYellow(),
                             s->diagnostics().errorCount(), s->diagnostics().warningCount(),
                             c.reset());
}

void XLC_BACKEND_CALL xlcSessionFlush(XlcSession* session) {
    reinterpret_cast<Session*>(session)->diagnostics().flush();
}
