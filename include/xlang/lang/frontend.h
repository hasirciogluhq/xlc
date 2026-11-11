#pragma once

#include "xlang/lang/ast.h"
#include "xlang/lang/config.h"
#include "xlang/lang/macro.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

namespace xlang {

/// The whole frontend pipeline for one file:
///   lex -> preprocess (`#if`) -> macro expansion -> parse.
/// `macros` may be pre-seeded with exported macros of imported modules; it receives the macros
/// defined by this file.
[[nodiscard]] Program parseSource(const SourceManager& sources, FileId file, const BuildConfig& config,
                                  DiagnosticEngine& diagnostics, MacroTable& macros);

[[nodiscard]] Program parseSource(const SourceManager& sources, FileId file, const BuildConfig& config,
                                  DiagnosticEngine& diagnostics);

}  // namespace xlang
