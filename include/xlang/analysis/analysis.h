#pragma once

#include "xlang/hir/hir.h"
#include "xlang/mir/mir.h"
#include "xlang/util/diagnostics.h"

namespace xlang::analysis {

/// Runs every static analysis of a lowered program and reports what they find.
///
/// The analyses only read: the HIR passes check what the typed tree shows (types, names,
/// signatures), the MIR passes check what needs the control flow (moves, borrows,
/// initialization). Neither changes the tree, the graph or an earlier phase.
/// @invariant findings are reported sorted by source position, never in the order the passes or functions ran.
void analyze(const hir::Module& hir, const mir::Module& mir, DiagnosticEngine& diagnostics);

} // namespace xlang::analysis
