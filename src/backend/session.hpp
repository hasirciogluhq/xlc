#pragma once

// The state of one compilation behind the backend's C API: the source manager, the diagnostics,
// the compilation (sema), the lowering to HIR and MIR and the analyses. The frontend entry points
// (src/backend/backend.cpp) and the code generation entry points (src/backend/codegen.cpp) share
// it; both are C ABI wrappers, so everything here is C++ and never leaves the backend.

#include "xlang/backend_c.h"
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
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xlang::backend {

namespace fs = std::filesystem;

/// The bytes of an empty string: the boundary wants a valid pointer even for length zero.
extern const char kEmptyBytes[1];

/// Boundary string -> view and back.
[[nodiscard]] XlcStr viewOf(const std::string& s);
[[nodiscard]] std::string_view textOf(XlcStr s);
[[nodiscard]] std::string stringOf(XlcStr s);
[[nodiscard]] std::vector<std::string> listOf(XlcStrList list);

/// Whether the backend colors stderr; the CLI decides once with `xlcSetColor`.
void setColor(bool on);

/// `error: MESSAGE` on stderr, in the CLI's shape.
void printError(std::string_view message);

/// The bold-red or bold-yellow `N error(s), M warning(s)` line, as the CLI always printed it.
void printSummary(const DiagnosticEngine& diagnostics);

/// The diagnostic configuration the C++ CLI derived from the parsed options.
[[nodiscard]] DiagnosticOptions diagnosticOptions(const XlcConfig& config);

/// The build configuration the C++ CLI derived from the parsed options; throws `XlangError` on a
/// bad target or definition, like the CLI did.
[[nodiscard]] BuildConfig buildConfig(const XlcConfig& raw);

class Session {
public:
    explicit Session(const XlcConfig& raw);

    DiagnosticEngine& diagnostics() { return diagnostics_; }
    const BuildConfig& config() const { return config_; }

    void addModuleRoot(std::string name, fs::path dir);
    void addImplicitImport(std::string spec);
    void loadRoot(const fs::path& root);
    void analyze();

    /// Lowers once: HIR, then MIR, then the static analyses, in the order of the old `run()`.
    /// Never runs on a program with errors.
    void ensureLowered();

    hir::Module& hirModule() { return hir_module_; }
    mir::Module& mirModule() { return *mir_module_; }
    Compilation& compilation() { return *compilation_; }
    SourceManager& sources() { return sources_; }
    const std::string& historyPath() const { return history_path_; }
    LayoutHistory& history() { return *history_; }
    const fs::path& root() const { return root_; }

    /// Erases every function and global the lists do not name; the lists are indices into the
    /// current module, ascending. Used once, by the output step's reachability prune.
    void keep(const std::vector<std::uint32_t>& functions, const std::vector<std::uint32_t>& globals);

    /// The source modules in dependency order, built on first use and kept with the session.
    const std::vector<XlcModule>& modules();

    /// The size of the aggregate `name` in the root module's sema, or nothing when it is not a
    /// struct or class.
    [[nodiscard]] std::optional<std::uint64_t> sizeOf(const std::string& name);

    /// The incremental cache key of this configuration for `emit`; kept until the next call.
    const std::string& configKey(const std::string& emit);

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
    std::vector<XlcModule> modules_;
    std::vector<std::vector<std::uint32_t>> module_deps_;
    bool modules_built_{false};
    std::string config_key_;
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

}  // namespace xlang::backend
