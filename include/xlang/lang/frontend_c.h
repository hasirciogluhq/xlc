#pragma once

// The C boundary of the frontend pipeline, which is written in xlang
// (src/lang/frontend/frontend.xl) and built into the compiler as an object file: one call runs
// lex, preprocess, macro expansion and parse over one file and returns the flat AST the parser
// bridge reads. The source manager and the diagnostic engine cross as the opaque handles of
// source.h and diagnostics.h; the flat records are the ones of lexer_c.h, macro_c.h and
// parser_c.h.

#include "xlang/lang/directives_c.h"
#include "xlang/lang/lexer_c.h"
#include "xlang/lang/macro_c.h"
#include "xlang/lang/parser_c.h"

#include <stdint.h>

#if defined(_WIN32)
#define XLC_FRONTEND_CALL __cdecl
#else
#define XLC_FRONTEND_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Runs the whole frontend of one file: the text of `file` in the xlang source manager `sources`
/// is lexed, preprocessed with `defines`, expanded with the seeded `macros`, and parsed; the
/// stage errors go to the xlang diagnostic engine `engine` (E0001, E0003, E0004), the syntax
/// errors stay in the result for the bridge (E0002). The result is owned by the callee's
/// allocator and must be passed to `xlcParseFree` exactly once.
XlcParseResult XLC_FRONTEND_CALL xlcFrontendParse(void* sources, uint32_t file,
                                                  const XlcDefine* defines, uint32_t define_count,
                                                  const XlcMacroDef* macros, uint32_t macro_count,
                                                  void* engine);

#ifdef __cplusplus
}  // extern "C"
#endif
