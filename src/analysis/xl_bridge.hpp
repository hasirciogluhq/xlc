// Temporary C bridge between the C++ compiler and the analyses written in xlang
// (src/analysis/*.xl). It flattens each mir::Body into the arenas of abi.xl and calls the xlang
// entry points exported by analysis.xl. Remove it when the caller of `analysis::analyze` is
// itself xlang.
#pragma once

#include "xlang/hir/hir.h"
#include "xlang/mir/mir.h"
#include "xlang/util/diagnostics.h"

namespace xlang::analysis::xl {

/// Runs every analysis over `mir` and reports the findings to `diagnostics`.
void run(const mir::Module& mir, DiagnosticEngine& diagnostics);

} // namespace xlang::analysis::xl
