#pragma once

#include "xlang/lang/config.h"

#include <filesystem>
#include <vector>

namespace xlang::codegen {

/// Plan 5.8: archives objects into a static library, written in-process with LLVM's archive
/// writer (no external `ar`): the BSD format for macOS, the GNU format elsewhere. Linking is the
/// CLI's work (src/driver/link.xl). Throws XlangError when an object cannot be read or the
/// archive cannot be written.
void archive(const std::vector<std::filesystem::path>& objects, const std::filesystem::path& output,
             const TargetInfo& target);

}  // namespace xlang::codegen
