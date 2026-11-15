#include "xlang/analysis/analysis.h"

#include "analysis/xl_bridge.hpp"

namespace xlang::analysis {

// The analyses are written in xlang (src/analysis/analysis.xl); see xl_bridge.hpp.
void analyze(const hir::Module&, const xlang::mir::Module& mir, DiagnosticEngine& diagnostics) {
    xl::run(mir, diagnostics);
}

} // namespace xlang::analysis
