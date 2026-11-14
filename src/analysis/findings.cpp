#include "analysis/findings.hpp"

#include <algorithm>
#include <string_view>
#include <tuple>

namespace xlang::analysis {

namespace {

[[nodiscard]] auto sortKey(const Diagnostic& d) {
    const Span at = d.labels.empty() ? Span{} : d.labels.front().span;
    return std::tuple{at.file, at.line, at.column, std::string_view(d.code), std::string_view(d.message)};
}

} // namespace

void Findings::reportTo(DiagnosticEngine& diagnostics) {
    std::ranges::stable_sort(items_, {}, [](const Diagnostic& d) { return sortKey(d); });
    const auto [first, last] =
        std::ranges::unique(items_, {}, [](const Diagnostic& d) { return sortKey(d); });
    items_.erase(first, last);
    for (Diagnostic& d : items_) {
        diagnostics.report(std::move(d));
    }
    items_.clear();
}

} // namespace xlang::analysis
