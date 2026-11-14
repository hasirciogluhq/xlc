// The util bridge, C++ side: one simple translation per call between the C++ types
// (include/xlang/util/source.h, include/xlang/util/diagnostics.h) and the flat C ABI of the
// xlang source manager and diagnostic engine (src/util/source.xl, src/util/diagnostics.xl).
// The wire records and the entry points live in util_c.h; the C++ DiagnosticEngine itself is a
// shim over the same entry points. This wrapper exists for the differential unit tests.
#pragma once

#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"
#include "xlang/util/util_c.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xlang::xl {

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
