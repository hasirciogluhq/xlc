#pragma once

// The wire format of the util bridge: the flat records the C++ side and the xlang side
// (src/util/source.xl, src/util/diagnostics.xl) exchange, and the C ABI entry points both
// include. The xlang side copies every string into its own arenas; handles are plain pointers
// to the xlang objects. The C++ DiagnosticEngine and SourceManager (diagnostics.h, source.h)
// are thin shims over these entry points; UtilBridge (src/util/xl_bridge.hpp) is the test
// wrapper over the same calls.

#include <cstddef>
#include <cstdint>

namespace xlang::xl {

struct XbStr {
    const void* ptr;
    std::uint32_t len;
};

struct XbLabel {
    std::uint32_t file;
    std::uint32_t line;
    std::uint32_t column;
    std::uint32_t end_line;
    std::uint32_t end_column;
    const void* text;
    std::uint32_t text_len;
};

struct XbNote {
    std::uint32_t file;
    std::uint32_t line;
    std::uint32_t column;
    std::uint32_t has_span;
    const void* message;
    std::uint32_t msg_len;
};

struct XbDiagnostic {
    XbStr code;
    std::uint32_t severity;
    XbStr message;
    const XbLabel* labels;
    std::uint32_t label_count;
    const XbNote* notes;
    std::uint32_t note_count;
    XbStr detail;
    XbStr reason;
    XbStr fix;
};

struct XbOptions {
    const XbStr* allow;
    std::uint32_t allow_count;
    const XbStr* deny;
    std::uint32_t deny_count;
    std::uint32_t warn_all;
    std::uint32_t allow_all;
    std::uint32_t context_errors;
    std::uint32_t context_warnings;
    XbStr log_path;
    std::uint32_t format;
    std::uint32_t log_level;
    std::uint32_t color;
};

/// The rendered queue of a flush: the text (valid until the next flush) and the counts.
struct XbFlush {
    const void* text;
    std::uint32_t text_len;
    std::uint32_t errors;
    std::uint32_t warnings;
};

struct XbSpan {
    std::uint32_t file;
    std::uint32_t line;
    std::uint32_t column;
    std::uint32_t end_line;
    std::uint32_t end_column;
};

extern "C" {
void* xlcUtilSourcesNew();
std::uint32_t xlcUtilAddFile(void* sources, const XbStr* path, const XbStr* text);
void xlcUtilSourcesFree(void* sources);
void* xlcDiagNew(void* sources);
/// Fills `severity` (1 error, 0 warning) and returns the registry index, or -1 when unknown.
std::int32_t xlcDiagFindCode(const XbStr* code, std::uint32_t* severity);
void xlcDiagConfigure(void* engine, const XbOptions* options);
void xlcDiagAddAllowScope(void* engine, const XbSpan* scope, const XbStr* codes,
                          std::uint32_t count);
void xlcDiagReport(void* engine, const XbDiagnostic* diagnostic);
void xlcDiagBeginQuiet(void* engine);
void xlcDiagEndQuiet(void* engine);
XbFlush xlcDiagFlush(void* engine);
/// The live counts: errors and warnings that survived allow/deny and quiet.
void xlcDiagCounts(void* engine, std::uint32_t* errors, std::uint32_t* warnings);
/// The text (`json == 0`) or JSON form of one diagnostic into `out` (capacity `cap`); returns
/// the byte count without a NUL.
std::uint32_t xlcDiagRender(void* engine, const XbDiagnostic* diagnostic, std::uint32_t json,
                            void* out, std::uint32_t cap);
std::uint32_t xlcDiagExplain(const XbStr* code, void* out, std::uint32_t cap);
void xlcDiagFreeEngine(void* engine);
}

}  // namespace xlang::xl
