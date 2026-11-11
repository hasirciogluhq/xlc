#pragma once

#include <stdexcept>
#include <string>
#include <utility>

namespace xlang {

/// Driver-level failure (bad CLI input, unreadable file). Source problems go through
/// `DiagnosticEngine`, not exceptions.
class XlangError : public std::runtime_error {
public:
    explicit XlangError(std::string message) : std::runtime_error(std::move(message)) {}
};

}  // namespace xlang
