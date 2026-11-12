#pragma once

#include "xlang/hir/hir.h"
#include "xlang/lang/config.h"
#include "xlang/sema/layout.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace xlang::codegen {

/// Size/alignment of a type as the semantic phase computed it (structs, classes, arrays,
/// interface values); the generator never lays out aggregates itself.
using LayoutQuery = std::function<std::optional<TypeLayout>(const Type&)>;

/// What the build produces. It decides which entry code is generated.
enum class Output {
    Executable, ///< `_start` (OS entry stub) calls `_xl_initAll`, then `main` with the process arguments
    Shared,     ///< `_xl_initAll` is a constructor (ELF `.init_array`, Mach-O `__mod_init_func`); no `main` needed
    Static,     ///< `_xl_initAll` is an exported symbol the host calls; no entry code
    Object,     ///< same as Static, for one object file
};

/// The output kind of an `--emit=` value (`exe`, `shared`, `static`; everything else is an object).
[[nodiscard]] constexpr Output outputOf(std::string_view emit) {
    if (emit == "exe") {
        return Output::Executable;
    }
    if (emit == "shared") {
        return Output::Shared;
    }
    return emit == "static" ? Output::Static : Output::Object;
}

struct CodegenInput {
    const hir::Module& module;
    const BuildConfig& config;
    const SourceManager& sources;
    DiagnosticEngine& diagnostics;
    LayoutQuery layout;
    std::string module_name;     ///< name of the LLVM module (root file stem)
    Output output{Output::Executable};
    /// Per-module compilation (plan 8.1): only this module's functions/globals are defined, the
    /// rest are external declarations; vtables/itables are weak definitions in every module.
    std::optional<std::string> only_module;
};

/// The result of code generation: an LLVM module plus the target machine it was built for.
class Artifact {
public:
    struct Impl;
    explicit Artifact(std::unique_ptr<Impl> impl);
    ~Artifact();
    Artifact(Artifact&&) noexcept;
    Artifact& operator=(Artifact&&) noexcept;

    /// Optimizes with the pipeline for `config.opt_level` (plan 5.2).
    void optimize(int opt_level);
    /// `--emit=ir`: textual LLVM IR (`.ll`) with triple and data layout.
    void writeIR(const std::filesystem::path& path) const;
    /// The same text as `writeIR`, in memory.
    [[nodiscard]] std::string ir() const;
    /// `--emit=bc`
    void writeBitcode(const std::filesystem::path& path) const;
    /// `--emit=obj`; throws XlangError when the backend cannot emit the file.
    void writeObject(const std::filesystem::path& path) const;
    [[nodiscard]] std::string triple() const;
    [[nodiscard]] bool hasEntry() const;

private:
    std::unique_ptr<Impl> impl_;
};

/// HIR -> LLVM (plan 4.7). Returns nullptr after reporting errors.
[[nodiscard]] std::unique_ptr<Artifact> generate(const CodegenInput& input);

/// `--emit=header`: `extern "C"` header for `export(c)` functions/globals and `@repr(c)` types (5.7).
[[nodiscard]] std::string generateHeader(const hir::Module& module, const LayoutQuery& layout, const std::string& guard);

}  // namespace xlang::codegen
