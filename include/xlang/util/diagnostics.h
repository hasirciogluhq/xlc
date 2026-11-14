#pragma once

// The diagnostic engine shim: the implementation is the xlang DiagnosticEngine
// (src/util/diagnostics.xl) reached through the C ABI of util_c.h. This side builds the flat
// records, forwards every call, writes the rendered queue of `flush()` to the stream and keeps
// the registry lookups (`--allow` validation, `--explain`) on the C++ side of the bridge.

#include "xlang/util/error.h"
#include "xlang/util/source.h"
#include "xlang/util/util_c.h"

#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace xlang {

enum class Severity { Warning, Error };

/// One marked place of a diagnostic. The first label of a diagnostic is the primary one;
/// further labels show the other side of a conflict.
struct Label {
    Span span;
    std::string text;
};

/// A secondary message at a place, such as the request that caused an instantiation.
struct Note {
    Span span;
    std::string message;
};

struct Diagnostic {
    std::string code;
    Severity severity{Severity::Error};
    std::string message;
    std::vector<Label> labels;
    std::vector<Note> notes;
    std::string detail;  ///< what exactly is wrong
    std::string reason;  ///< why it matters
    std::string fix;     ///< how to fix it
    bool suppressed{false};
};

enum class DiagFormat { Text, Json };
enum class DiagLogLevel { All, Warn, Error };

struct DiagnosticOptions {
    std::vector<std::string> allow;  ///< --allow=CODE / -Wno-CODE
    std::vector<std::string> deny;   ///< --deny=CODE
    bool warn_all{false};            ///< --warn-all
    bool allow_all{false};           ///< --allow-all
    std::size_t context_errors{10};
    std::size_t context_warnings{2};
    std::string log_path;            ///< --diag-log=<file>; empty = no log
    DiagFormat format{DiagFormat::Text};
    DiagLogLevel log_level{DiagLogLevel::All};
    bool color{false};               ///< ANSI colors on the text output; never in `--diag-log` or JSON
};

namespace xl {

/// The wire view of a span; the field order matches `XbSpan`.
[[nodiscard]] inline XbSpan xbSpan(const Span& span) {
    return XbSpan{span.file, span.line, span.column, span.end_line, span.end_column};
}

[[nodiscard]] inline XbStr xbStr(std::string_view text) {
    return XbStr{text.data(), static_cast<std::uint32_t>(text.size())};
}

/// The wire record of one diagnostic; the label and note arrays live in `scratch` and only need
/// to outlive the call that consumes the record.
[[nodiscard]] inline XbDiagnostic xbDiagnostic(const Diagnostic& d, std::vector<XbLabel>& scratch,
                                              std::vector<XbNote>& note_scratch) {
    scratch.clear();
    note_scratch.clear();
    scratch.reserve(d.labels.size());
    note_scratch.reserve(d.notes.size());
    for (const Label& label : d.labels) {
        const XbSpan s = xbSpan(label.span);
        scratch.push_back(XbLabel{s.file, s.line, s.column, s.end_line, s.end_column,
                                  label.text.data(), static_cast<std::uint32_t>(label.text.size())});
    }
    for (const Note& note : d.notes) {
        const XbSpan s = xbSpan(note.span);
        note_scratch.push_back(XbNote{s.file, s.line, s.column, s.file != kNoFile ? 1u : 0u,
                                      note.message.data(),
                                      static_cast<std::uint32_t>(note.message.size())});
    }
    XbDiagnostic out{};
    out.code = xbStr(d.code);
    out.severity = d.severity == Severity::Error ? 1u : 0u;
    out.message = xbStr(d.message);
    out.labels = scratch.data();
    out.label_count = static_cast<std::uint32_t>(scratch.size());
    out.notes = note_scratch.data();
    out.note_count = static_cast<std::uint32_t>(note_scratch.size());
    out.detail = xbStr(d.detail);
    out.reason = xbStr(d.reason);
    out.fix = xbStr(d.fix);
    return out;
}

}  // namespace xl

class DiagnosticEngine {
public:
    DiagnosticEngine(const SourceManager& sources, std::ostream& out)
        : sources_(sources), out_(out), engine_(xl::xlcDiagNew(sources.handle())) {}
    ~DiagnosticEngine() {
        if (engine_ != nullptr) {
            xl::xlcDiagFreeEngine(engine_);
        }
    }
    DiagnosticEngine(const DiagnosticEngine&) = delete;
    DiagnosticEngine& operator=(const DiagnosticEngine&) = delete;

    /// Applies CLI options. Throws XlangError on unknown codes or when an error code is
    /// asked to be allowed (errors cannot be silenced).
    void configure(const DiagnosticOptions& options) {
        for (const std::string& code : options.allow) {
            const xl::XbStr xb = xl::xbStr(code);
            std::uint32_t severity = 0;
            if (xl::xlcDiagFindCode(&xb, &severity) < 0) {
                throw XlangError(std::format("unknown diagnostic code `{}`", code));
            }
            if (severity != 0) {
                throw XlangError(std::format("`{}` is an error and cannot be allowed", code));
            }
        }
        for (const std::string& code : options.deny) {
            const xl::XbStr xb = xl::xbStr(code);
            std::uint32_t severity = 0;
            if (xl::xlcDiagFindCode(&xb, &severity) < 0) {
                throw XlangError(std::format("unknown diagnostic code `{}`", code));
            }
        }
        if (!options.log_path.empty()) {
            // The xlang side truncates and appends; probe the path here so a bad `--diag-log`
            // stays an XlangError instead of a panic.
            std::ofstream probe(options.log_path, std::ios::trunc);
            if (!probe) {
                throw XlangError(std::format("cannot open diagnostic log `{}`", options.log_path));
            }
        }
        std::vector<xl::XbStr> allow;
        allow.reserve(options.allow.size());
        for (const std::string& code : options.allow) {
            allow.push_back(xl::xbStr(code));
        }
        std::vector<xl::XbStr> deny;
        deny.reserve(options.deny.size());
        for (const std::string& code : options.deny) {
            deny.push_back(xl::xbStr(code));
        }
        xl::XbOptions o{};
        o.allow = allow.data();
        o.allow_count = static_cast<std::uint32_t>(allow.size());
        o.deny = deny.data();
        o.deny_count = static_cast<std::uint32_t>(deny.size());
        o.warn_all = options.warn_all ? 1u : 0u;
        o.allow_all = options.allow_all ? 1u : 0u;
        o.context_errors = static_cast<std::uint32_t>(options.context_errors);
        o.context_warnings = static_cast<std::uint32_t>(options.context_warnings);
        o.log_path = xl::xbStr(options.log_path);
        o.format = static_cast<std::uint32_t>(options.format);
        o.log_level = static_cast<std::uint32_t>(options.log_level);
        o.color = options.color ? 1u : 0u;
        xl::xlcDiagConfigure(engine_, &o);
    }

    /// `@allow(CODE...)`: warnings whose primary span lies inside `scope` are silenced.
    void addAllowScope(const Span& scope, const std::vector<std::string>& codes) {
        std::vector<xl::XbStr> list;
        list.reserve(codes.size());
        for (const std::string& code : codes) {
            list.push_back(xl::xbStr(code));
        }
        const xl::XbSpan s = xl::xbSpan(scope);
        xl::xlcDiagAddAllowScope(engine_, &s, list.data(),
                                 static_cast<std::uint32_t>(list.size()));
    }

    /// Applies registry severity and allow/deny, counts, and queues the diagnostic. Nothing is
    /// printed until `flush()`, which emits the queue ordered by source position (the lexer runs
    /// ahead of the parser, so reporting order alone would not be source order).
    void report(Diagnostic diagnostic) {
        std::vector<xl::XbLabel> scratch;
        std::vector<xl::XbNote> note_scratch;
        const xl::XbDiagnostic xb = xl::xbDiagnostic(diagnostic, scratch, note_scratch);
        xl::xlcDiagReport(engine_, &xb);
    }

    void flush() {
        const xl::XbFlush f = xl::xlcDiagFlush(engine_);
        out_.write(static_cast<const char*>(f.text), static_cast<std::streamsize>(f.text_len));
    }

    /// Trial lowering (overload resolution): while quiet, reports are dropped and do not count.
    /// Calls nest; each `beginQuiet` needs one `endQuiet`.
    void beginQuiet() { xl::xlcDiagBeginQuiet(engine_); }
    void endQuiet() { xl::xlcDiagEndQuiet(engine_); }

    // Builders for the common shapes.
    void error(std::string code, const Span& span, std::string message, std::string label = {},
               std::vector<Note> notes = {}) {
        Diagnostic d;
        d.code = std::move(code);
        d.severity = Severity::Error;
        d.message = std::move(message);
        if (span.valid()) {
            d.labels.push_back({span, std::move(label)});
        }
        d.notes = std::move(notes);
        report(std::move(d));
    }

    void warning(std::string code, const Span& span, std::string message, std::string label = {}) {
        Diagnostic d;
        d.code = std::move(code);
        d.severity = Severity::Warning;
        d.message = std::move(message);
        if (span.valid()) {
            d.labels.push_back({span, std::move(label)});
        }
        report(std::move(d));
    }

    [[nodiscard]] std::size_t errorCount() const {
        std::uint32_t errors = 0;
        std::uint32_t warnings = 0;
        xl::xlcDiagCounts(engine_, &errors, &warnings);
        return errors;
    }

    [[nodiscard]] std::size_t warningCount() const {
        std::uint32_t errors = 0;
        std::uint32_t warnings = 0;
        xl::xlcDiagCounts(engine_, &errors, &warnings);
        return warnings;
    }

    [[nodiscard]] bool hasErrors() const { return errorCount() != 0; }

    /// The text form of a diagnostic (what `report` prints), per `--context-*` settings.
    [[nodiscard]] std::string renderText(const Diagnostic& diagnostic) const {
        return render(diagnostic, 0);
    }

    [[nodiscard]] std::string renderJson(const Diagnostic& diagnostic) const {
        return render(diagnostic, 1);
    }

    /// `--explain CODE`; empty when the code is unknown.
    [[nodiscard]] static std::string explain(std::string_view code) {
        std::string out(1 << 16, '\0');
        const xl::XbStr xb = xl::xbStr(code);
        std::uint32_t need =
            xlcDiagExplain(&xb, out.data(), static_cast<std::uint32_t>(out.size()));
        if (need == 0) {
            return {};
        }
        if (need > out.size()) {
            out.resize(need);
            need = xlcDiagExplain(&xb, out.data(), static_cast<std::uint32_t>(out.size()));
        }
        out.resize(need);
        return out;
    }

    [[nodiscard]] std::string render(const Diagnostic& diagnostic, std::uint32_t json) const {
        std::vector<xl::XbLabel> scratch;
        std::vector<xl::XbNote> note_scratch;
        const xl::XbDiagnostic xb = xl::xbDiagnostic(diagnostic, scratch, note_scratch);
        std::string out(1 << 16, '\0');
        const std::uint32_t need =
            xlcDiagRender(engine_, &xb, json, out.data(), static_cast<std::uint32_t>(out.size()));
        out.resize(need);
        return out;
    }

private:
    const SourceManager& sources_;
    std::ostream& out_;
    void* engine_{nullptr};
};

}  // namespace xlang
