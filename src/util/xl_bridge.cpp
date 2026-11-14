// The util bridge, C++ side: one simple translation per call between the C++ types
// (include/xlang/util/source.h, include/xlang/util/diagnostics.h) and the flat C ABI of the
// xlang source manager and diagnostic engine (src/util/source.xl, src/util/diagnostics.xl).
#include "xl_bridge.hpp"

namespace xlang::xl {

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
    const XbStr p = xbStr(path);
    const XbStr t = xbStr(text);
    return xlcUtilAddFile(sources_, &p, &t);
}

void UtilBridge::configure(const DiagnosticOptions& options) {
    if (engine_ == nullptr) {
        engine_ = xlcDiagNew(sources_);
    }
    std::vector<XbStr> allow;
    allow.reserve(options.allow.size());
    for (const std::string& code : options.allow) {
        allow.push_back(xbStr(code));
    }
    std::vector<XbStr> deny;
    deny.reserve(options.deny.size());
    for (const std::string& code : options.deny) {
        deny.push_back(xbStr(code));
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
    o.log_path = xbStr(options.log_path);
    o.format = static_cast<std::uint32_t>(options.format);
    o.log_level = static_cast<std::uint32_t>(options.log_level);
    o.color = options.color ? 1u : 0u;
    xlcDiagConfigure(engine_, &o);
}

void UtilBridge::addAllowScope(const Span& scope, const std::vector<std::string>& codes) {
    if (engine_ == nullptr) {
        engine_ = xlcDiagNew(sources_);
    }
    const XbSpan s = xbSpan(scope);
    std::vector<XbStr> list;
    list.reserve(codes.size());
    for (const std::string& code : codes) {
        list.push_back(xbStr(code));
    }
    xlcDiagAddAllowScope(engine_, &s, list.data(), static_cast<std::uint32_t>(list.size()));
}

void UtilBridge::report(const Diagnostic& diagnostic) {
    if (engine_ == nullptr) {
        engine_ = xlcDiagNew(sources_);
    }
    std::vector<XbLabel> labels;
    std::vector<XbNote> notes;
    const XbDiagnostic d = xbDiagnostic(diagnostic, labels, notes);
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
    const XbStr c = xbStr(code);
    std::string out;
    out.resize(4096);
    const std::uint32_t n = xlcDiagExplain(&c, out.data(), static_cast<std::uint32_t>(out.size()));
    out.resize(n);
    return out;
}

}  // namespace xlang::xl
