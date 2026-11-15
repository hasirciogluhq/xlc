#pragma once

#include "xlang/lang/ast.h"
#include "xlang/lang/config.h"
#include "xlang/lang/macro.h"
#include "xlang/lang/parser_c.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

namespace xlang {

/// The frozen flat AST of one file, exactly as the xlang frontend returns it (no `Program` tree is
/// rebuilt). The xlang sema and lowering read it in place; the caller releases it once with
/// `xlcParseFree`. Syntax errors stay in the result: this does not report them.
[[nodiscard]] XlcParseResult parseSourceFlat(const SourceManager& sources, FileId file,
                                             const BuildConfig& config,
                                             DiagnosticEngine& diagnostics);

/// The whole frontend pipeline for one file:
///   lex -> preprocess (`#if`) -> macro expansion -> parse.
/// `macros` may be pre-seeded with exported macros of imported modules; it receives the macros
/// defined by this file.
[[nodiscard]] Program parseSource(const SourceManager& sources, FileId file, const BuildConfig& config,
                                  DiagnosticEngine& diagnostics, MacroTable& macros);

[[nodiscard]] Program parseSource(const SourceManager& sources, FileId file, const BuildConfig& config,
                                  DiagnosticEngine& diagnostics);

}  // namespace xlang
