#pragma once

#include "xlang/lang/config.h"

#include <filesystem>
#include <string>
#include <vector>

namespace xlang::codegen {

enum class LinkKind { Executable, Shared };

struct LinkRequest {
    std::vector<std::filesystem::path> objects;
    std::filesystem::path output;
    LinkKind kind{LinkKind::Executable};
    const BuildConfig* config{nullptr};
    std::string entry;  ///< entry symbol of executables (`_start`, `mainCRTStartup`)
    /// `@library("x")` imports: (library, symbol). Only Windows uses them: an import library is
    /// generated per DLL (llvm-dlltool) and handed to the linker. Nothing else is ever linked.
    std::vector<std::pair<std::string, std::string>> imports;
};

/// Plan 5.9: picks the target's linker, passes no libc/libSystem/kernel32 and links only the
/// given objects. Throws XlangError with a hint (`--emit=obj`) when no linker is available.
void link(const LinkRequest& request);

/// Plan 5.8: archives objects into a static library (written in-process, no external `ar`).
void archive(const std::vector<std::filesystem::path>& objects, const std::filesystem::path& output,
             const BuildConfig& config);

}  // namespace xlang::codegen
