#pragma once

#include "xlang/lang/ast.h"
#include "xlang/lang/lexer.h"

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

/// Macros visible to an expansion. Seed it with exported macros of imported modules; the
/// module's own `define` macros are visible to later uses in the same file inside the pipeline
/// itself and are collected from the AST by `Compilation::collectMacros` for the dependents.

inline constexpr int kMacroDepthLimit = 64;

}  // namespace xlang
