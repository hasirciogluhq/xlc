#pragma once

#include "xlang/lang/ast.h"
#include "xlang/lang/lexer.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

#include <vector>

namespace xlang {

/// Lexes and parses one source file. Syntax errors are reported to `diagnostics` (E0001/E0002)
/// and parsing recovers at the next statement/item, so one run reports many problems. The
/// returned program contains everything that parsed; check `diagnostics.hasErrors()`.
[[nodiscard]] Program parseFile(const SourceManager& sources, FileId file,
                                DiagnosticEngine& diagnostics);

/// Parses an already lexed token stream (must end with `TokenKind::End`).
[[nodiscard]] Program parseTokens(std::vector<Token> tokens, FileId file,
                                  DiagnosticEngine& diagnostics);

}  // namespace xlang
