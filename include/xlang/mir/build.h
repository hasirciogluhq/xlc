#pragma once

#include "xlang/hir/hir.h"
#include "xlang/mir/mir.h"

namespace xlang::mir {

/// Builds the MIR of every HIR function that has a body, in HIR order.
[[nodiscard]] Module build(const hir::Module& module);

} // namespace xlang::mir
