#pragma once

#include "xlang/util/diagnostics.h"

#include <vector>

namespace xlang::analysis {

/// The diagnostics of every analysis, held until all of them ran.
/// @invariant `reportTo` emits them sorted by the position of their first label, without duplicates, whatever order the passes and functions ran in.
class Findings {
public:
    void add(Diagnostic d) { items_.push_back(std::move(d)); }
    void reportTo(DiagnosticEngine& diagnostics);

private:
    std::vector<Diagnostic> items_;
};

} // namespace xlang::analysis
