#include "xlang/analysis/analysis.h"

#include "analysis/findings.hpp"
#include "analysis/mir/moves.hpp"

#include <array>

namespace xlang::analysis {

namespace {

using HirPass = void (*)(const hir::Function&, Findings&);
using MirPass = void (*)(const xlang::mir::Body&, Findings&);

// The passes in the order they run. A new analysis is one folder entry (`analysis/hir/<name>` or
// `analysis/mir/<name>`) and one line here; nothing else calls a pass.
constexpr std::array<HirPass, 0> kHirPasses{};
constexpr std::array kMirPasses{&mir::checkMoves};

} // namespace

void analyze(const hir::Module& hir, const xlang::mir::Module& mir, DiagnosticEngine& diagnostics) {
    Findings findings;
    for (const HirPass pass : kHirPasses) {
        for (const hir::Function& f : hir.functions) {
            pass(f, findings);
        }
    }
    for (const MirPass pass : kMirPasses) {
        for (const xlang::mir::Body& b : mir.bodies) {
            pass(b, findings);
        }
    }
    findings.reportTo(diagnostics);
}

} // namespace xlang::analysis
