#pragma once

// The C boundary of the preprocessor, which is written in xlang (src/lang/preprocess/) and built
// into the compiler as an object file. This header is the C++ side of that seam: `preprocess()` in
// src/lang/preprocess/preprocess.cpp turns the token stream and the resolved definitions into the
// flat arrays below, calls `xlcPreprocess`, and turns the frozen result back into `Token` values.
//
// Ownership: everything the xlang side returns lives in one block that `xlcPreprocessFree`
// releases through the `block` field of the result. The token texts of the result point into that
// block; the C++ side copies what it keeps. The token texts and definition names passed in must
// stay alive for the duration of the call only.

#include "xlang/lang/lexer_c.h"

#include <stdint.h>

#if defined(_WIN32)
#define XLC_PREPROCESS_CALL __cdecl
#else
#define XLC_PREPROCESS_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// One definition crossing the boundary: a builtin, a `-D NAME=VALUE` or a file `define`. The
/// value kinds are the `V_BOOL`/`V_INT`/`V_STRING` constants of the xlang side (0, 1, 2).
typedef struct XlcDefine {
    const char* name;  ///< not NUL-terminated
    uint32_t name_len;
    int32_t kind;              ///< 0 = bool, 1 = int, 2 = string
    int64_t int_value;         ///< the value when `kind` is 1; 0 or 1 when `kind` is 0
    const char* string_value;  ///< the bytes when `kind` is 2; not NUL-terminated
    uint32_t string_len;
} XlcDefine;

/// One preprocessing error: the code is always E0003, as the C++ preprocessor reported it. Same
/// shape as `XlcLexError`.
typedef XlcLexError XlcPreprocessError;

/// The frozen outcome of one file: the tokens that survive preprocessing (always ending in kind 0,
/// end of file) and the errors found on the way. `block` is the opaque handle `xlcPreprocessFree`
/// releases.
typedef struct XlcPreprocessResult {
    const XlcLexToken* tokens;
    uint32_t token_count;
    const XlcLexError* errors;
    uint32_t error_count;
    void* block;
} XlcPreprocessResult;

/// Resolves the directives of `token_count` tokens against `define_count` definitions (see
/// preprocess.h for the semantics). The result is owned by the callee's allocator and must be
/// passed to `xlcPreprocessFree` exactly once. The spans carry lines and columns only; the caller
/// stamps its own file onto them.
XlcPreprocessResult XLC_PREPROCESS_CALL xlcPreprocess(const XlcLexToken* tokens,
                                                      uint32_t token_count,
                                                      const XlcDefine* defines,
                                                      uint32_t define_count);

/// Releases a result returned by `xlcPreprocess`. A default-constructed result is a no-op.
void XLC_PREPROCESS_CALL xlcPreprocessFree(XlcPreprocessResult result);

#ifdef __cplusplus
}  // extern "C"
#endif
