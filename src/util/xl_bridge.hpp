// The util bridge: the C ABI of the xlang source manager and diagnostic engine
// (src/util/source.xl, src/util/diagnostics.xl). The C++ side hands flat records over; the xlang
// side copies every string into its own arenas. Handles are plain pointers to the xlang objects;
// every function is one simple translation, no state on this side.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

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
void xlcDiagConfigure(void* engine, const XbOptions* options);
void xlcDiagAddAllowScope(void* engine, const XbSpan* scope, const XbStr* codes,
                          std::uint32_t count);
void xlcDiagReport(void* engine, const XbDiagnostic* diagnostic);
void xlcDiagBeginQuiet(void* engine);
void xlcDiagEndQuiet(void* engine);
XbFlush xlcDiagFlush(void* engine);
std::uint32_t xlcDiagExplain(const XbStr* code, void* out, std::uint32_t cap);
void xlcDiagFreeEngine(void* engine);
}

/// The C++ side of the bridge: one engine plus its source manager, fed with the C++ types and
/// read back as text. Everything is one simple translation per call.
class UtilBridge {
public:
    UtilBridge() = default;
    ~UtilBridge();
    UtilBridge(const UtilBridge&) = delete;
    UtilBridge& operator=(const UtilBridge&) = delete;

    /// Copies `path`/`text` into the xlang source manager and returns the FileId.
    std::uint32_t addFile(std::string_view path, std::string_view text);
    /// Applies the options.
    void configure(const DiagnosticOptions& options);
    /// `@allow(CODE...)`: warnings whose primary span lies inside `scope` are silenced.
    void addAllowScope(const Span& scope, const std::vector<std::string>& codes);
    /// Reports one diagnostic (a copy of every string crosses the bridge).
    void report(const Diagnostic& diagnostic);
    void beginQuiet();
    void endQuiet();
    /// Renders and drops the queue; `text` is valid until the next flush.
    const XbFlush& flush();
    /// `--explain CODE` into a fresh string; empty when the code is unknown.
    std::string explain(std::string_view code);

    [[nodiscard]] std::size_t errorCount() const { return last_.errors; }
    [[nodiscard]] std::size_t warningCount() const { return last_.warnings; }

private:
    void* sources_{nullptr};
    void* engine_{nullptr};
    XbFlush last_{};
};

}  // namespace xlang::xl
