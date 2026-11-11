#pragma once

#include "xlang/lang/ast.h"
#include "xlang/lang/lexer.h"
#include "xlang/util/diagnostics.h"

#include <map>
#include <string>
#include <vector>

namespace xlang {

struct MacroDef {
    std::string name;
    std::vector<MacroParam> params;
    std::vector<Token> body;
    Span span;
};

/// Macros visible to an expansion. Seed it with exported macros of imported modules; the
/// expander adds the ones defined in the token stream itself.
struct MacroTable {
    std::map<std::string, MacroDef> macros;
};

/// Expands `name!(args)` calls on the token stream (plan 3.7): parameters are substituted as
/// token groups (an `expr` argument is parenthesized, so precedence is preserved), `local` names
/// introduced by a macro body are renamed (hygiene), expansion repeats until no call is left
/// (depth limit 64). `define` bodies are not expanded where they are written, only when used.
/// Calls of unknown macros are left in place for a later module-aware stage.
[[nodiscard]] std::vector<Token> expandMacros(std::vector<Token> tokens, MacroTable& table,
                                              DiagnosticEngine& diagnostics);

inline constexpr int kMacroDepthLimit = 64;

}  // namespace xlang
