#include "analysis/xl_bridge.hpp"

// The xlang side: src/analysis/analysis.xl.
extern "C" void xlcAnalysisRun(void* module, void* engine);

namespace xlang::analysis::xl {

void run(const mir::Module& mir, DiagnosticEngine& diagnostics) {
    xlcAnalysisRun(mir.handle(), diagnostics.handle());
}

} // namespace xlang::analysis::xl
