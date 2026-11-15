#include "sema/xl_bridge.hpp"

#include "xlang/lang/parser_c.h"

#include <string>
#include <utility>

namespace xlang {

/// The frozen parse, then the session that reads it. Destruction order matters: the session holds
/// pointers into the parse, so it is released first.
struct XlLowered::Impl {
    XlcParseResult parse{};
    void* session = nullptr;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl() {
        if (session != nullptr) {
            xlcLowerFree(session);
        }
        xlcParseFree(parse);
    }
};

XlLowered::XlLowered(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

XlLowered::XlLowered(XlLowered&&) noexcept = default;
XlLowered& XlLowered::operator=(XlLowered&&) noexcept = default;
XlLowered::~XlLowered() = default;

XlLowered XlLowered::lower(const SourceManager& sources, FileId file, const BuildConfig& config,
                           DiagnosticEngine& diagnostics) {
    auto impl = std::make_unique<Impl>();
    impl->parse = parseSourceFlat(sources, file, config, diagnostics);
    impl->session = xlcLowerRun(&impl->parse, diagnostics.handle());
    return XlLowered(std::move(impl));
}

std::string XlLowered::dump() const {
    const XlLowerText text = xlcLowerDump(impl_->session);
    std::string out(reinterpret_cast<const char*>(text.bytes), text.len);
    xlcLowerDumpFree(text);
    return out;
}

} // namespace xlang
