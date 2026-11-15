// Temporary C bridge between the C++ compiler and the analyses written in xlang
// (src/analysis/*.xl). The analyses read the xlang MIR module directly (src/mir/model.xl) and
// report to the C++ DiagnosticEngine. Remove it when the caller of `analysis::analyze` is itself
// xlang.
#pragma once

#include "xlang/mir/build.h"
#include "xlang/util/diagnostics.h"

namespace xlang::analysis::xl {

/// Runs every analysis over `mir` and reports the findings to `diagnostics`.
void run(const mir::Module& mir, DiagnosticEngine& diagnostics);

} // namespace xlang::analysis::xl
