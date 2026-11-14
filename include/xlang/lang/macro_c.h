#pragma once

// The C boundary of the macro expander, which is written in xlang (src/lang/macro/) and built
// into the compiler as an object file. This header is the C++ side of that seam: `expandMacros()`
// in src/lang/macro/macro.cpp turns the token stream and the seed macros into the flat arrays
// below, calls `xlcExpandMacros`, and turns the frozen result back into `Token` values.
//
// Ownership: everything the xlang side returns lives in one block that `xlcExpandMacrosFree`
// releases through the `block` field of the result. The token texts of the result point into that
// block and into the caller's arrays; the C++ side copies what it keeps. The arrays passed in
// must stay alive for the duration of the call only.

#include "xlang/lang/lexer_c.h"

#include <stdint.h>

#if defined(_WIN32)
#define XLC_MACRO_CALL __cdecl
#else
#define XLC_MACRO_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// One macro parameter kind: `expr`, `type`, `ident`, `block`, `items` (0..4).
enum {
    XLC_MACRO_PARAM_EXPR = 0,
    XLC_MACRO_PARAM_TYPE = 1,
    XLC_MACRO_PARAM_IDENT = 2,
    XLC_MACRO_PARAM_BLOCK = 3,
    XLC_MACRO_PARAM_ITEMS = 4,
};

/// One macro parameter; field for field the `CMacroParam` of the xlang side.
typedef struct XlcMacroParam {
    int32_t kind;
    const char* name;  ///< not NUL-terminated
    uint32_t name_len;
} XlcMacroParam;

/// One macro definition; field for field the `CMacro` of the xlang side. `body` holds the raw
/// tokens between the outer braces of `define name(params) { ... }`.
typedef struct XlcMacroDef {
    const char* name;  ///< not NUL-terminated
    uint32_t name_len;
    const XlcMacroParam* params;
    uint32_t param_count;
    const XlcLexToken* body;
    uint32_t body_len;
    uint32_t line;      ///< the span of the name token; zeros for a seed macro
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
} XlcMacroDef;

/// The frozen outcome of one expansion: the expanded tokens (always ending in kind 0, end of
/// file), the errors found on the way, and the macros collected from the token stream itself,
/// which the caller writes back into its table. `block` is the opaque handle
/// `xlcExpandMacrosFree` releases.
typedef struct XlcMacroResult {
    const XlcLexToken* tokens;
    uint32_t token_count;
    const XlcLexError* errors;
    uint32_t error_count;
    const XlcMacroDef* defs;
    uint32_t def_count;
    void* block;
} XlcMacroResult;

/// Expands the macro calls of `token_count` tokens against `macro_count` seed macros (see
/// macro.h for the semantics). The result is owned by the callee's allocator and must be passed
/// to `xlcExpandMacrosFree` exactly once. The spans carry lines and columns only; the caller
/// stamps its own file onto them.
XlcMacroResult XLC_MACRO_CALL xlcExpandMacros(const XlcLexToken* tokens, uint32_t token_count,
                                              const XlcMacroDef* macros, uint32_t macro_count);

/// Releases a result returned by `xlcExpandMacros`. A default-constructed result is a no-op.
void XLC_MACRO_CALL xlcExpandMacrosFree(XlcMacroResult result);

#ifdef __cplusplus
}  // extern "C"
#endif
