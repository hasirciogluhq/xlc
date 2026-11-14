// The util bridge, C++ side: one simple translation per call between the C++ types
// (include/xlang/util/source.h, include/xlang/util/diagnostics.h) and the flat C ABI of the
// xlang source manager and diagnostic engine (src/util/source.xl, src/util/diagnostics.xl).
#include "xl_bridge.hpp"

#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

namespace xlang::xl {

namespace {

XbStr str(std::string_view text) {
    return XbStr{text.data(), static_cast<std::uint32_t>(text.size())};
}

}  // namespace

UtilBridge::~UtilBridge() {
    if (engine_ != nullptr) {
        xlcDiagFreeEngine(engine_);
    }
    if (sources_ != nullptr) {
        xlcUtilSourcesFree(sources_);
    }
}

std::uint32_t UtilBridge::addFile(std::string_view path, std::string_view text) {
    if (sources_ == nullptr) {
        sources_ = xlcUtilSourcesNew();
    }
    const XbStr p = str(path);
    const XbStr t = str(text);
    return xlcUtilAddFile(sources_, &p, &t);
}

void UtilBridge::configure(const DiagnosticOptions& options) {
    if (engine_ == nullptr) {
        engine_ = xlcDiagNew(sources_);
    }
    std::vector<XbStr> allow;
    allow.reserve(options.allow.size());
    for (const std::string& code : options.allow) {
        allow.push_back(str(code));
    }
    std::vector<XbStr> deny;
    deny.reserve(options.deny.size());
    for (const std::string& code : options.deny) {
        deny.push_back(str(code));
    }
    XbOptions o{};
    o.allow = allow.data();
    o.allow_count = static_cast<std::uint32_t>(allow.size());
    o.deny = deny.data();
    o.deny_count = static_cast<std::uint32_t>(deny.size());
    o.warn_all = options.warn_all ? 1u : 0u;
    o.allow_all = options.allow_all ? 1u : 0u;
    o.context_errors = static_cast<std::uint32_t>(options.context_errors);
    o.context_warnings = static_cast<std::uint32_t>(options.context_warnings);
    o.log_path = str(options.log_path);
    o.format = static_cast<std::uint32_t>(options.format);
    o.log_level = static_cast<std::uint32_t>(options.log_level);
    o.color = options.color ? 1u : 0u;
    xlcDiagConfigure(engine_, &o);
}

void UtilBridge::addAllowScope(const Span& scope, const std::vector<std::string>& codes) {
    if (engine_ == nullptr) {
        engine_ = xlcDiagNew(sources_);
    }
    XbSpan s{};
    s.file = scope.file;
    s.line = scope.line;
    s.column = scope.column;
    s.end_line = scope.end_line;
    s.end_column = scope.end_column;
    std::vector<XbStr> list;
    list.reserve(codes.size());
    for (const std::string& code : codes) {
        list.push_back(str(code));
    }
    xlcDiagAddAllowScope(engine_, &s, list.data(), static_cast<std::uint32_t>(list.size()));
}

void UtilBridge::report(const Diagnostic& diagnostic) {
    if (engine_ == nullptr) {
        engine_ = xlcDiagNew(sources_);
    }
    std::vector<XbLabel> labels;
    labels.reserve(diagnostic.labels.size());
    for (const Label& label : diagnostic.labels) {
        XbLabel l{};
        l.file = label.span.file;
        l.line = label.span.line;
        l.column = label.span.column;
        l.end_line = label.span.end_line;
        l.end_column = label.span.end_column;
        l.text = label.text.data();
        l.text_len = static_cast<std::uint32_t>(label.text.size());
        labels.push_back(l);
    }
    std::vector<XbNote> notes;
    notes.reserve(diagnostic.notes.size());
    for (const Note& note : diagnostic.notes) {
        XbNote n{};
        n.file = note.span.file;
        n.line = note.span.line;
        n.column = note.span.column;
        n.has_span = note.span.valid() ? 1u : 0u;
        n.message = note.message.data();
        n.msg_len = static_cast<std::uint32_t>(note.message.size());
        notes.push_back(n);
    }
    XbDiagnostic d{};
    d.code = str(diagnostic.code);
    d.severity = diagnostic.severity == Severity::Error ? 1u : 0u;
    d.message = str(diagnostic.message);
    d.labels = labels.data();
    d.label_count = static_cast<std::uint32_t>(labels.size());
    d.notes = notes.data();
    d.note_count = static_cast<std::uint32_t>(notes.size());
    d.detail = str(diagnostic.detail);
    d.reason = str(diagnostic.reason);
    d.fix = str(diagnostic.fix);
    xlcDiagReport(engine_, &d);
}

void UtilBridge::beginQuiet() {
    if (engine_ != nullptr) {
        xlcDiagBeginQuiet(engine_);
    }
}

void UtilBridge::endQuiet() {
    if (engine_ != nullptr) {
        xlcDiagEndQuiet(engine_);
    }
}

const XbFlush& UtilBridge::flush() {
    if (engine_ == nullptr) {
        engine_ = xlcDiagNew(sources_);
    }
    last_ = xlcDiagFlush(engine_);
    return last_;
}

std::string UtilBridge::explain(std::string_view code) {
    const XbStr c = str(code);
    std::string out;
    out.resize(4096);
    const std::uint32_t n = xlcDiagExplain(&c, out.data(), static_cast<std::uint32_t>(out.size()));
    out.resize(n);
    return out;
}

}  // namespace xlang::xl
