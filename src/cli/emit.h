#pragma once

#include "xlang/hir/hir.h"
#include "xlang/lang/config.h"
#include "xlang/sema/compilation.h"
#include "xlang/sema/sema.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

#include <filesystem>
#include <string>
#include <vector>

namespace xlang::cli {

/// What to write and where: `xlc [options] input.xl [inputs...] -o out --emit=KIND`.
struct EmitRequest {
    std::filesystem::path root;                 ///< the `.xl` file being compiled
    std::filesystem::path output;               ///< `-o`; empty = next to `root`, named by `emit`
    std::string emit{"exe"};                    ///< exe|obj|ir|bc|header|static|shared
    bool incremental{false};                    ///< one object per module, reused from `cache_dir`
    std::filesystem::path cache_dir;            ///< empty = `.xlc-cache` next to the output
    std::vector<std::filesystem::path> inputs;  ///< prebuilt objects/libraries handed to the linker
};

/// Code generation for the lowered program `module`, then (for exe/static/shared) archive or link.
/// Problems are reported on `diagnostics`; link and I/O failures throw `XlangError`.
void emitOutput(Compilation& compilation, hir::Module& module, const BuildConfig& config,
                SourceManager& sources, DiagnosticEngine& diagnostics, const EmitRequest& request);

}  // namespace xlang::cli
