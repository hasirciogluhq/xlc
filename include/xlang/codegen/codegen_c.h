#pragma once

// The C boundary of the LLVM code generator: the xlang output step (src/cli/emit.xl, through
// src/codegen/llvm.xl) generates an artifact from the session's HIR, optimizes it and writes it,
// or archives objects. Generation stays in C++ because it is LLVM; everything that decides what
// is generated, where it goes and in what order is xlang.
//
// Ownership: an artifact returned by `xlcCodegenGenerate` is owned by the caller and released
// with `xlcArtifactFree`. The artifact borrows nothing from the session after it is returned.
// Every function returns 0 on success and 1 on failure, except where a return type says
// otherwise; failures are reported on stderr by the backend (or as session diagnostics).

#include "xlang/backend_c.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The code generator's result: an LLVM module and the target machine it was built for.
typedef struct XlcArtifact XlcArtifact;

/// What to generate. `emit` decides the entry code (exe, shared, static or obj); `has_only`
/// restricts the output to one module of the program (`only`, its key), the rest become external
/// declarations, which is how the incremental build writes one object per module.
typedef struct XlcCodegenRequest {
    XlcStr name;        ///< the LLVM module name (the root file stem)
    XlcStr emit;        ///< exe|shared|static|obj|ir|bc
    int32_t has_only;   ///< 1: only the module named by `only` is defined
    XlcStr only;        ///< the module key (may be "" for the root module)
} XlcCodegenRequest;

/// Generates code for the session's lowered and pruned program. NULL on failure; the failure is
/// a session diagnostic, or an error line on stderr.
XlcArtifact* XLC_BACKEND_CALL xlcCodegenGenerate(XlcSession* session,
                                                 const XlcCodegenRequest* request);

/// Optimizes the artifact with the pipeline for `opt_level` (0 = none).
void XLC_BACKEND_CALL xlcArtifactOptimize(XlcArtifact* artifact, int32_t opt_level);

/// Writes the artifact: textual LLVM IR (`.ll`), bitcode (`.bc`) or an object file. `-` as the
/// path streams to stdout for IR, bitcode and objects, as the LLVM writers do.
int32_t XLC_BACKEND_CALL xlcArtifactWriteIR(XlcArtifact* artifact, XlcStr path);
int32_t XLC_BACKEND_CALL xlcArtifactWriteBitcode(XlcArtifact* artifact, XlcStr path);
int32_t XLC_BACKEND_CALL xlcArtifactWriteObject(XlcArtifact* artifact, XlcStr path);

/// Releases an artifact. NULL is a no-op.
void XLC_BACKEND_CALL xlcArtifactFree(XlcArtifact* artifact);

/// Writes `objects` into the static library `output`, in-process (LLVM's archive writer, no
/// external `ar`): the BSD format for `os` macos, the GNU format otherwise. 0 = written.
int32_t XLC_BACKEND_CALL xlcArchive(XlcStrList objects, XlcStr output, XlcStr os);

#ifdef __cplusplus
}  // extern "C"
#endif
