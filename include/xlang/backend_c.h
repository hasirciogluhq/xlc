#pragma once

// The C boundary of the compiler backend: everything the CLI written in xlang (src/main.xl)
// drives from C++. The backend owns the C++ pipeline behind one opaque session -- the source
// manager, the diagnostics, the compilation (sema), the lowering to HIR and MIR, the analyses --
// so no AST, HIR or MIR type ever crosses the boundary; the CLI only decides what happens in
// which order. Code generation (LLVM) is behind include/xlang/codegen/codegen_c.h, on the same
// session; the output step, the link and the header are xlang (src/cli/, src/codegen/).
//
// Ownership: a session returned by `xlcSessionNew` is owned by the caller and released with
// `xlcSessionFree`. Every string crosses as a pointer and a length, not NUL-terminated; the
// caller keeps the bytes alive for the duration of the call, except where a function says that
// the result lives as long as the session.
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

/// A list of indices (into a module's functions, globals or module list).
typedef struct XlcIndexList {
    const uint32_t* items;
    uint32_t count;
} XlcIndexList;

/// A source span, the layout of `xlang::Span` (include/xlang/util/source.h).
typedef struct XlcSpan {
    uint32_t file;
    uint32_t line;
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
} XlcSpan;

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

/// The resolved target of a configuration (`--target`, else the host).
typedef struct XlcTarget {
    XlcStr triple;
    XlcStr os;    ///< linux|macos|windows
    XlcStr arch;  ///< x86_64|aarch64
} XlcTarget;

/// One source module of the compilation, in dependency order (a module comes after the modules
/// it imports). The strings live as long as the session.
typedef struct XlcModule {
    XlcStr key;          ///< the module key ("" for the root module)
    XlcStr text;         ///< the source text of the module's file
    int32_t registered;  ///< 1: the module has `register(...)` items
    XlcIndexList deps;   ///< the modules it imports, as indices into this same list
} XlcModule;

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

/// The optimization level of the session's configuration (`-O`, 0 when unset).
int32_t XLC_BACKEND_CALL xlcSessionOptLevel(XlcSession* session);

/// Lowers the program (once) and returns the `hir::Module` of the session for the xlang side,
/// which reads it through the flat form of src/hir/abi.xl (src/hir/flatten.xl). The pointer
/// lives as long as the session, until `xlcSessionKeep` changes the module. The layout is the
/// C++ one, pinned by the offsets in src/hir/flatten.xl.
const void* XLC_BACKEND_CALL xlcSessionHirModule(XlcSession* session);

/// Keeps only the listed functions and globals of the lowered module (indices into the module
/// as `xlcSessionHirModule` shows it, in module order); every other one is erased. The output
/// step calls it once, after the reachability prune (src/hir/prune.xl), before any code
/// generation.
void XLC_BACKEND_CALL xlcSessionKeep(XlcSession* session, XlcIndexList functions,
                                     XlcIndexList globals);

/// The size in bytes of the aggregate `name` as the semantic phase computed it. 0 = known (the
/// size is written to `size`), 1 = no such aggregate.
int32_t XLC_BACKEND_CALL xlcSessionSizeOf(XlcSession* session, XlcStr name, int64_t* size);

/// The number of source modules in dependency order, and the module at `index` (0 = ok,
/// 1 = bad index).
uint32_t XLC_BACKEND_CALL xlcSessionModuleCount(XlcSession* session);
int32_t XLC_BACKEND_CALL xlcSessionModule(XlcSession* session, uint32_t index, XlcModule* module);

/// The part of the incremental cache key that only the backend knows: the compiler version,
/// the target, the optimization, debug, cpu, mangling, safety and layout settings, `emit`, and
/// every definition. The string is written to `key` and lives until the next call on the session.
void XLC_BACKEND_CALL xlcSessionConfigKey(XlcSession* session, XlcStr emit, XlcStr* key);

/// Reports an error on the session's diagnostics: `code`, the span (NULL = no position),
/// `message` and the `hint` line (empty = none).
void XLC_BACKEND_CALL xlcSessionDiagnose(XlcSession* session, XlcStr code, const XlcSpan* span,
                                         XlcStr message, XlcStr hint);

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
