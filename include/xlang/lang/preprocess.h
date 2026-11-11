#pragma once

#include "xlang/lang/config.h"
#include "xlang/lang/lexer.h"
#include "xlang/util/diagnostics.h"

#include <vector>

namespace xlang {

/// Conditional compilation over the token stream (plan 3.1-3.3, 3.5).
///
/// `#if/#elif/#else/#endif/#ifdef/#ifndef/#error` are removed; tokens of inactive regions are
/// dropped (they were only lexed). A body on the same line as the condition is the rest of that
/// line and has no `#endif`; a body starting on a later line needs `#endif`.
/// `define NAME = literal` lines seen in active code are visible to later conditions unless
/// `-D` already defines NAME (CLI wins).
[[nodiscard]] std::vector<Token> preprocess(std::vector<Token> tokens, const BuildConfig& config,
                                            DiagnosticEngine& diagnostics);

}  // namespace xlang
