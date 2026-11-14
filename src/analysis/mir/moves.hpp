#pragma once

#include "analysis/findings.hpp"

#include "xlang/mir/mir.h"

namespace xlang::analysis::mir {

/// E0780: a local read, borrowed or moved again after it was moved on some path to that point.
void checkMoves(const xlang::mir::Body& body, Findings& findings);

} // namespace xlang::analysis::mir
