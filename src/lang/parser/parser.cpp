// Parser: the public entry point. The parser itself is written in xlang
// (src/lang/parser/parser.xl); src/lang/parser/bridge.cpp reads its flat result and rebuilds the
// C++ Program this compiler works with.

#include "xlang/lang/parser.h"

#include <utility>
#include <vector>

namespace xlang {

Program parseTokens(std::vector<Token> tokens, FileId file, DiagnosticEngine& diagnostics) {
    return parseTokensFlat(std::move(tokens), file, diagnostics);
}

Program parseFile(const SourceManager& sources, FileId file, DiagnosticEngine& diagnostics) {
    return parseTokens(lex(sources, file, diagnostics), file, diagnostics);
}

}  // namespace xlang
