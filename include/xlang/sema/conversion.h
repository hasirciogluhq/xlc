#pragma once

#include "xlang/lang/types.h"

#include <string>

namespace xlang {

enum class ConversionStatus {
    Ok,         ///< allowed without a cast, silently
    Warn,       ///< allowed, W0601 (signed integer -> float)
    Signedness, ///< E0601: signedness changes; `as`, or inside `unsafe { }` (same or wider target)
    NeedsCast,  ///< not implicit: write `as`
    Invalid,    ///< no conversion exists
};

struct ConversionResult {
    ConversionStatus status{ConversionStatus::Invalid};
    /// With `unsafe` the signedness change was accepted (W0601 may still apply for `ui -> f`).
    bool used_unsafe{false};
    std::string note;
};

/// Implicit conversion rules (plan 2.2, eg/02 + eg/02.1). `pointer_bits` resolves isize/usize.
[[nodiscard]] ConversionResult implicitConversion(const Type& from, const Type& to, bool in_unsafe,
                                                  unsigned pointer_bits = 64);

struct CommonTypeResult {
    ConversionStatus status{ConversionStatus::Invalid};
    Type type;
    bool warn{false};  ///< an operand was a signed integer widened to float (W0601)
};

/// Result type of an arithmetic/comparison operation on `a` and `b`. Mixed signedness is
/// E0601 (`ConversionStatus::Signedness`) even inside `unsafe`: one side needs `as`.
[[nodiscard]] CommonTypeResult commonBinaryType(const Type& a, const Type& b, unsigned pointer_bits = 64);

}  // namespace xlang
