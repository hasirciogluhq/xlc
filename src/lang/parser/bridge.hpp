#pragma once

// Internal seam of the xlang parser bridge: the flat result of the xlang parser (or of the
// frontend pipeline, which ends in the same parse) rebuilt into the C++ Program tree.

#include "xlang/lang/ast.h"
#include "xlang/lang/parser_c.h"
#include "xlang/util/diagnostics.h"

namespace xlang {

/// Rebuilds the `Program` tree from the frozen arena result of `xlcParse` and reports the
/// recorded syntax errors as E0002. The result stays owned by the caller (`xlcParseFree`).
Program readFlatProgram(const XlcParseResult& result, FileId file, DiagnosticEngine& diagnostics);

}  // namespace xlang
