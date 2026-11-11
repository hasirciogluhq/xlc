#pragma once

#include "xlang/sema/sema.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// Internal helpers shared by the parts of `Sema` (`src/sema/sema/`). The parts define the members
// declared in `xlang/sema/sema.h`; this header only carries the free helpers and includes.

namespace xlang::sema_detail {

/// `Int128` as decimal text, without a locale and without going through `double`.
[[nodiscard]] inline std::string int128ToString(Int128 v) {
    if (v == 0) {
        return "0";
    }
    const bool negative = v < 0;
    UInt128 u =
        negative ? static_cast<UInt128>(-(v + 1)) + 1 : static_cast<UInt128>(v);
    std::string out;
    while (u != 0) {
        out += static_cast<char>('0' + static_cast<int>(u % 10));
        u /= 10;
    }
    if (negative) {
        out += '-';
    }
    std::reverse(out.begin(), out.end());
    return out;
}

/// `a.b.c` from a path, the spelling used in diagnostics for a qualified name.
[[nodiscard]] inline std::string joinDotted(const std::vector<std::string>& path) {
    std::string out;
    for (const std::string& p : path) {
        out += out.empty() ? p : "." + p;
    }
    return out;
}

/// `value` rounded up to a multiple of `align` (a no-op for `align <= 1`).
[[nodiscard]] inline std::uint64_t alignUp(std::uint64_t value, std::uint64_t align) {
    return align <= 1 ? value : (value + align - 1) / align * align;
}

[[nodiscard]] inline const Attribute* findAttr(const std::vector<Attribute>& attrs,
                                               std::string_view name) {
    for (const Attribute& a : attrs) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

/// `@name(c)` / `@name(x = c)`: is `text` one of the bare identifier arguments?
[[nodiscard]] inline bool attrHasIdent(const Attribute& a, std::string_view text) {
    return std::ranges::any_of(a.args, [&](const AttributeArg& arg) {
        return arg.key.empty() && arg.kind == AttributeArg::Kind::Ident && arg.text == text;
    });
}

[[nodiscard]] inline const AttributeArg* attrKey(const Attribute& a, std::string_view key) {
    for (const AttributeArg& arg : a.args) {
        if (arg.key == key) {
            return &arg;
        }
    }
    return nullptr;
}

/// Wraps `value` to the range of an integer type of `bits` bits.
[[nodiscard]] inline Int128 wrapInt(Int128 value, unsigned bits, bool is_signed) {
    if (bits >= 64) {
        const auto u = static_cast<std::uint64_t>(value);
        return is_signed ? static_cast<Int128>(static_cast<std::int64_t>(u))
                         : static_cast<Int128>(u);
    }
    const std::uint64_t mask = (std::uint64_t{1} << bits) - 1;
    const std::uint64_t u = static_cast<std::uint64_t>(value) & mask;
    if (is_signed && (u >> (bits - 1)) != 0) {
        return static_cast<Int128>(u) - (static_cast<Int128>(1) << bits);
    }
    return static_cast<Int128>(u);
}

/// Nesting depth of a type: `i32` is 0, `Vec<Vec<i32>>` and `*[*i32; 2]` are deeper. Caps the
/// recursion of generic instantiation (E0725).
[[nodiscard]] inline int typeNesting(const Type& t) {
    int inner = 0;
    if (t.elem) {
        inner = std::max(inner, typeNesting(*t.elem));
    }
    if (t.ret) {
        inner = std::max(inner, typeNesting(*t.ret));
    }
    for (const Type& p : t.params) {
        inner = std::max(inner, typeNesting(p));
    }
    for (const Type& a : t.args) {
        inner = std::max(inner, typeNesting(a));
    }
    const bool has_children = t.elem || t.ret || !t.params.empty() || !t.args.empty();
    return has_children ? inner + 1 : 0;
}

}  // namespace xlang::sema_detail
