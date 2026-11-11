#include "xlang/lang/frontend.h"

#include "xlang/lang/lexer.h"
#include "xlang/lang/parser.h"
#include "xlang/lang/preprocess.h"

namespace xlang {

Program parseSource(const SourceManager& sources, FileId file, const BuildConfig& config,
                    DiagnosticEngine& diagnostics, MacroTable& macros) {
    std::vector<Token> tokens = lex(sources, file, diagnostics);
    tokens = preprocess(std::move(tokens), config, diagnostics);
    tokens = expandMacros(std::move(tokens), macros, diagnostics);
    return parseTokens(std::move(tokens), file, diagnostics);
}

Program parseSource(const SourceManager& sources, FileId file, const BuildConfig& config,
                    DiagnosticEngine& diagnostics) {
    MacroTable macros;
    return parseSource(sources, file, config, diagnostics, macros);
}

}  // namespace xlang
