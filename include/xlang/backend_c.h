#pragma once

// The C boundary of the compiler backend: everything the CLI written in xlang (src/main.xl)
// drives from C++. The backend owns the whole C++ pipeline behind one opaque session -- the
// source manager, the diagnostics, the compilation (sema), the lowering to HIR and MIR, the
// analyses and the emission -- so no AST, HIR or MIR type ever crosses the boundary; the CLI
// only decides what happens in which order.
//
// Ownership: a session returned by `xlcSessionNew` is owned by the caller and released with
// `xlcSessionFree`. Every string crosses as a pointer and a length, not NUL-terminated; the
// caller keeps the bytes alive for the duration of the call.
//
// Every function returns 0 on success and 1 on failure; failures are reported on stderr by the
// backend itself, in the same shape the C++ CLI printed them.

#include <stdint.h>

#if defined(_WIN32)
#define XLC_BACKEND_CALL __cdecl
#else
#define XLC_BACKEND_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// One string across the boundary: the bytes are not NUL-terminated.
typedef struct XlcStr {
    const char* bytes;
    uint32_t len;
} XlcStr;

/// A list of strings across the boundary.
typedef struct XlcStrList {
    const XlcStr* items;
    uint32_t count;
} XlcStrList;

/// The command-line configuration, already parsed into values by the CLI. `target`, `opt`,
/// `cpu`, `mangle`, `safety`, `layout_scheme` and `layout_history` may be empty (unset); the
/// backend applies the same defaults and checks the C++ CLI applied.
typedef struct XlcConfig {
    XlcStr target;
    XlcStr opt;
    int32_t debug;
    XlcStr cpu;
    XlcStr mangle;
    XlcStr safety;
    XlcStr layout_scheme;
    XlcStr layout_history;
    XlcStrList defines;  ///< raw `-D` values: `NAME` or `NAME=VALUE`
    XlcStrList undefs;   ///< raw `-U` names
    XlcStrList allow;    ///< warning codes silenced (`--allow`)
    XlcStrList deny;     ///< warning codes turned into errors (`--deny`)
    int32_t warn_all;
    int32_t allow_all;
    uint64_t context_errors;
    uint64_t context_warnings;
    XlcStr log_path;        ///< `--diag-log`; empty = no log
    XlcStr diag_format;     ///< text|json
    XlcStr diag_log_level;  ///< all|warn|error
    int32_t crt;            ///< `--crt`: link the platform CRT and C++ runtime
} XlcConfig;

/// What code generation writes and where. The CLI resolves the output path; linking and
/// archiving are the CLI's work too (src/driver/link.xl), on the objects `XlcEmitResult` names.
typedef struct XlcEmitRequest {
    XlcStr root;       ///< the `.xl` file being compiled
    XlcStr output;     ///< the final output path; `-` streams ir, bc, obj and header to stdout
    XlcStr emit;       ///< exe|obj|ir|bc|header|static|shared
    int32_t incremental;
    XlcStr cache_dir;  ///< empty = `.xlc-cache` next to the output
    int32_t has_inputs; ///< prebuilt objects or libraries join the link (they may call back in)
} XlcEmitRequest;

/// One `@library("lib") declare fn` import of the program: the Windows link generates an
/// import library per DLL from these.
typedef struct XlcImport {
    XlcStr library;
    XlcStr symbol;
} XlcImport;

/// What the LLVM codegen entry (`xlcLlvmEmit`) produced for the CLI to archive or link. Empty
/// for obj, ir, bc and
/// header, which are written in place. The strings live in the session until the next emit or
/// `xlcSessionFree`.
typedef struct XlcEmitResult {
    XlcStrList objects;        ///< the objects to archive or link, in link order
    const XlcImport* imports;  ///< the `@library` imports reachable after pruning
    uint32_t import_count;
    int32_t temporary;         ///< 1: the objects are scratch files the CLI removes after the link
} XlcEmitResult;

/// The resolved target of a configuration (`--target`, else the host).
typedef struct XlcTarget {
    XlcStr triple;
    XlcStr os;    ///< linux|macos|windows
    XlcStr arch;  ///< x86_64|aarch64
} XlcTarget;

/// An opaque backend session.
typedef struct XlcSession XlcSession;

/// Colors the backend's own output (diagnostics, error lines) or not. The CLI decides once
/// (src/cli/console.xl: `--color`, `XLANG_COLOR`, `NO_COLOR`, the terminal) and calls this
/// before anything else prints.
void XLC_BACKEND_CALL xlcSetColor(int32_t on);

/// Prints the long explanation of one diagnostic code. 0 = printed, 1 = unknown code.
int32_t XLC_BACKEND_CALL xlcExplain(XlcStr code);

/// Prints the version string, as `--version` shows it.
void XLC_BACKEND_CALL xlcVersion(void);

/// Prints every resolved definition (`--print-cfg`). 0 = printed, 1 = bad configuration.
int32_t XLC_BACKEND_CALL xlcPrintCfg(const XlcConfig* config);

/// Parses one file and prints the AST summary (`--dump-ast`). 0 = printed, 1 = failed.
int32_t XLC_BACKEND_CALL xlcDumpAst(const XlcConfig* config, XlcStr path);

/// Opens a session with the given configuration. Returns NULL on a bad configuration (reported).
XlcSession* XLC_BACKEND_CALL xlcSessionNew(const XlcConfig* config);

/// Releases a session. NULL is a no-op.
void XLC_BACKEND_CALL xlcSessionFree(XlcSession* session);

/// Makes `import "NAME"` resolve inside `DIR` (`--module-root NAME=DIR`). Call before `loadRoot`.
int32_t XLC_BACKEND_CALL xlcSessionAddModuleRoot(XlcSession* session, XlcStr spec);

/// Loads module `spec` although nothing imports it (`--import`). Call before `loadRoot`.
int32_t XLC_BACKEND_CALL xlcSessionAddImplicitImport(XlcSession* session, XlcStr spec);

/// Loads the root module and everything it imports.
int32_t XLC_BACKEND_CALL xlcSessionLoadRoot(XlcSession* session, XlcStr path);

/// Resolves the imports and analyzes every module (`finish` + `analyzeAll`).
int32_t XLC_BACKEND_CALL xlcSessionAnalyze(XlcSession* session);

/// Lowers the program (once) without printing anything (`--check`).
int32_t XLC_BACKEND_CALL xlcSessionLower(XlcSession* session);

/// Lowers the program (once) and prints the typed HIR (`--dump-hir`).
int32_t XLC_BACKEND_CALL xlcSessionDumpHir(XlcSession* session);

/// Lowers the program (once) and prints the control-flow graph (`--dump-mir`).
int32_t XLC_BACKEND_CALL xlcSessionDumpMir(XlcSession* session);

/// Prints the layout of every aggregate, or of the one type named (`--dump-layout`). With
/// `record` set, the layouts also go into the history file.
int32_t XLC_BACKEND_CALL xlcSessionDumpLayout(XlcSession* session, XlcStr type_name,
                                              int32_t record);

/// The resolved target of the session's configuration. The strings live as long as the session.
void XLC_BACKEND_CALL xlcSessionTarget(XlcSession* session, XlcTarget* target);

/// Lowers the program (once) and returns the `hir::Module` of the session for a backend written
/// in xlang (the native arm64/Mach-O path). The pointer lives as long as the session; the layout
/// is the C++ one, mirrored field for field by src/hir/abi.xl.
const void* XLC_BACKEND_CALL xlcSessionHirModule(XlcSession* session);

/// The LLVM codegen step behind the C API: the only codegen entry the xlang CLI calls, and
/// only when the backend dispatch picks LLVM (`--backend=llvm`, or native on a target it does
/// not cover). Lowers the program (once) and generates code (`--emit`). obj, ir, bc and header
/// are written to `request->output`; for exe, static and shared `result` names the objects to
/// archive or link. The request's `root` must be the path given to `loadRoot`. Code generation
/// errors are diagnostics: check `xlcSessionHasErrors` before using `result`.
int32_t XLC_BACKEND_CALL xlcLlvmEmit(XlcSession* session, const XlcEmitRequest* request,
                                     XlcEmitResult* result);

/// Writes `objects` into the static library `output`, in-process (LLVM's archive writer, no
/// external `ar`): the BSD format for `os` macos, the GNU format otherwise. 0 = written.
int32_t XLC_BACKEND_CALL xlcArchive(XlcStrList objects, XlcStr output, XlcStr os);

/// True (1) when any error was reported so far, else 0.
int32_t XLC_BACKEND_CALL xlcSessionHasErrors(XlcSession* session);

/// The number of errors and warnings reported so far.
int64_t XLC_BACKEND_CALL xlcSessionErrorCount(XlcSession* session);
int64_t XLC_BACKEND_CALL xlcSessionWarningCount(XlcSession* session);

/// Flushes the diagnostics and prints the `N error(s), M warning(s)` summary on stderr.
void XLC_BACKEND_CALL xlcSessionPrintSummary(XlcSession* session);

/// Flushes the diagnostics without the summary.
void XLC_BACKEND_CALL xlcSessionFlush(XlcSession* session);

#ifdef __cplusplus
}  // extern "C"
#endif
