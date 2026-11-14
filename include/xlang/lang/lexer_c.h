#pragma once

// The C boundary of the lexer, which is written in xlang (src/lang/lexer/lexer.xl) and built into
// the compiler as an object file. This header is the C++ side of that seam: `lex()` in
// src/lang/lexer/lexer.cpp turns the frozen result into `Token` values and reports the diagnostics
// the xlang side collected.
//
// Ownership: everything the xlang side returns lives in one block that `xlcLexFree` releases
// through the `block` field of the result. `text` of each token points into that block; the C++
// side copies what it keeps.

#include <stdint.h>

#if defined(_WIN32)
#define XLC_LEX_CALL __cdecl
#else
#define XLC_LEX_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// One token across the boundary. Mirrors `Token` in lexer.xl field for field; `kind` uses the same
/// numbers as `TokenKind` in lexer.h (see `xlcTokenKindOf`).
typedef struct XlcLexToken {
    int32_t kind;
    /// Pointer to the spelling/decoded value; not NUL-terminated, length in `text_len`.
    const char* text;
    uint32_t text_len;
    uint64_t int_value;
    double float_value;
    uint32_t line;
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
    int32_t newline_before;  ///< 0 or 1
} XlcLexToken;

/// One label of an error: a span and its text. The first label of an error is the primary one.
typedef struct XlcLexLabel {
    const char* text;  ///< not NUL-terminated
    uint32_t text_len;
    uint32_t line;
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
} XlcLexLabel;

/// One lexical error: the code is always E0001, as the C++ lexer reported it. A lexical error
/// carries exactly one label.
typedef struct XlcLexError {
    const char* message;  ///< not NUL-terminated
    uint32_t message_len;
    uint32_t label_count;
    const XlcLexLabel* labels;
} XlcLexError;

/// The frozen outcome of one file: tokens (always ending in kind 0, end of file) and the errors
/// found while scanning it. `block` is the opaque handle `xlcLexFree` releases.
typedef struct XlcLexResult {
    const XlcLexToken* tokens;
    uint32_t token_count;
    const XlcLexError* errors;
    uint32_t error_count;
    void* block;
} XlcLexResult;

/// Tokenizes `text` (see `lexer.xl`). The result is owned by the callee's allocator and must be
/// passed to `xlcLexFree` exactly once. The spans carry lines and columns only; the caller stamps
/// its own file onto them.
XlcLexResult XLC_LEX_CALL xlcLex(const char* text, uint32_t length);

/// Releases a result returned by `xlcLex`. A default-constructed result is a no-op.
void XLC_LEX_CALL xlcLexFree(XlcLexResult result);

#ifdef __cplusplus
}  // extern "C"
#endif
