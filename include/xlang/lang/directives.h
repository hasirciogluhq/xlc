#pragma once

#include "xlang/lang/config.h"
#include "xlang/lang/lexer.h"

#include <vector>

namespace xlang {

// The directive walk (`#if/#elif/#else/#endif/#ifdef/#ifndef/#error`, plan 3.1-3.3, 3.5) lives
// in src/lang/preprocess/*.xl and runs inside the frontend pipeline
// (src/lang/frontend/frontend.xl); there is no C++ entry point. `define NAME = literal` lines
// seen in active code are visible to later conditions unless `-D` already defines NAME (CLI
// wins).

}  // namespace xlang
