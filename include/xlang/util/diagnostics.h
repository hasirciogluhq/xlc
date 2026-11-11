#pragma once

#include "xlang/util/source.h"

#include <cstddef>
#include <fstream>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
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

/// Registry entry: every code the compiler can emit lives in one table.
struct DiagnosticInfo {
    std::string_view code;
    Severity severity;
    std::string_view title;
    std::string_view explanation;
};

[[nodiscard]] const DiagnosticInfo* findDiagnosticInfo(std::string_view code);
[[nodiscard]] const std::vector<DiagnosticInfo>& allDiagnosticInfos();

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

class DiagnosticEngine {
public:
    DiagnosticEngine(const SourceManager& sources, std::ostream& out);

    /// Applies CLI options. Throws XlangError on unknown codes or when an error code is
    /// asked to be allowed (errors cannot be silenced).
    void configure(const DiagnosticOptions& options);

    /// `@allow(CODE...)`: warnings whose primary span lies inside `scope` are silenced.
    void addAllowScope(const Span& scope, const std::vector<std::string>& codes);

    /// Applies registry severity and allow/deny, counts, and queues the diagnostic. Nothing is
    /// printed until `flush()`, which emits the queue ordered by source position (the lexer runs
    /// ahead of the parser, so reporting order alone would not be source order).
    void report(Diagnostic diagnostic);
    void flush();

    /// Trial lowering (overload resolution): while quiet, reports are dropped and do not count.
    /// Calls nest; each `beginQuiet` needs one `endQuiet`.
    void beginQuiet() { ++quiet_; }
    void endQuiet() { --quiet_; }

    // Builders for the common shapes.
    void error(std::string code, const Span& span, std::string message, std::string label = {},
               std::vector<Note> notes = {});
    void warning(std::string code, const Span& span, std::string message, std::string label = {});

    [[nodiscard]] std::size_t errorCount() const { return errors_; }
    [[nodiscard]] std::size_t warningCount() const { return warnings_; }
    [[nodiscard]] bool hasErrors() const { return errors_ != 0; }

    /// The text form of a diagnostic (what `report` prints), per `--context-*` settings.
    [[nodiscard]] std::string renderText(const Diagnostic& diagnostic) const;
    [[nodiscard]] std::string renderJson(const Diagnostic& diagnostic) const;

    /// `--explain CODE`; empty when the code is unknown.
    [[nodiscard]] static std::string explain(std::string_view code);

private:
    struct AllowScope {
        Span scope;
        std::unordered_set<std::string> codes;
    };

    [[nodiscard]] bool isAllowed(const Diagnostic& diagnostic) const;

    const SourceManager& sources_;
    std::ostream& out_;
    DiagnosticOptions options_;
    std::unordered_set<std::string> allow_;
    std::unordered_set<std::string> deny_;
    std::vector<AllowScope> scopes_;
    std::optional<std::ofstream> log_;
    std::vector<Diagnostic> pending_;
    std::size_t errors_{0};
    std::size_t quiet_{0};
    std::size_t warnings_{0};
};

}  // namespace xlang
