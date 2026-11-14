#pragma once

#include "xlang/hir/hir.h"
#include "xlang/lang/config.h"
#include "xlang/sema/compilation.h"
#include "xlang/sema/sema.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace xlang::cli {

/// What code generation writes and where. The CLI (src/cli/emit.xl) has resolved `output`.
struct EmitRequest {
    std::filesystem::path root;       ///< the `.xl` file being compiled
    std::filesystem::path output;     ///< the final output; `-` streams ir, bc, obj and header
    std::string emit{"exe"};          ///< exe|obj|ir|bc|header|static|shared
    bool incremental{false};          ///< one object per module, reused from `cache_dir`
    std::filesystem::path cache_dir;  ///< empty = `.xlc-cache` next to the output
    bool has_inputs{false};           ///< prebuilt objects join the link and may call back in
};

/// The objects an exe, static or shared output is archived or linked from, by the CLI
/// (src/driver/link.xl). Empty for obj, ir, bc and header, which are written in place.
struct EmitResult {
    std::vector<std::filesystem::path> objects;
    std::vector<std::pair<std::string, std::string>> imports;  ///< `@library` (library, symbol)
    bool temporary{false};  ///< the objects are scratch files, removed after the link
};

/// Code generation for the lowered program `module`. Problems are reported on `diagnostics`;
/// I/O failures throw `XlangError`.
[[nodiscard]] EmitResult emitOutput(Compilation& compilation, hir::Module& module, const BuildConfig& config,
                                    SourceManager& sources, DiagnosticEngine& diagnostics,
                                    const EmitRequest& request);

}  // namespace xlang::cli
