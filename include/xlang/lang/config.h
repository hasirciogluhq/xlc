#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xlang {

/// Compile-time target; `os`, `arch`, `pointerWidth` and `endian` in `#if` come from here.
struct TargetInfo {
    std::string triple;
    std::string os;      ///< "linux" | "macos" | "windows"
    std::string arch;    ///< "x86_64" | "aarch64"
    unsigned pointer_width{64};
    std::string endian{"little"};
};

/// Parses a target triple (`aarch64-apple-darwin`, `x86_64-unknown-linux-gnu`, ...).
[[nodiscard]] std::optional<TargetInfo> parseTargetTriple(std::string_view triple);
[[nodiscard]] TargetInfo hostTarget();

/// Value of a `-D` definition / builtin / `define` constant used in `#if` and constant expressions.
struct ConfigValue {
    enum class Kind { Bool, Int, String } kind{Kind::Bool};
    bool bool_value{false};
    std::int64_t int_value{0};
    std::string string_value;

    [[nodiscard]] static ConfigValue makeBool(bool v);
    [[nodiscard]] static ConfigValue makeInt(std::int64_t v);
    [[nodiscard]] static ConfigValue makeString(std::string v);
    /// `-D NAME` (true), `-D NAME=3`, `-D NAME=0x10`, `-D NAME=true`, `-D NAME="x"` / `-D NAME=x`.
    [[nodiscard]] static ConfigValue parse(std::string_view text);
    [[nodiscard]] std::string toString() const;
};

struct BuildConfig {
    TargetInfo target{hostTarget()};
    int opt_level{0};
    bool debug{false};
    std::string cpu{"generic"};

    std::map<std::string, ConfigValue> defines;  ///< -D

    std::string mangle{"xl"};      ///< --mangle=xl|none
    std::string safety{"checked"}; ///< --safety=checked|unchecked
    std::string layout_scheme;     ///< --layout-scheme (empty = newest)
    std::string layout_history;    ///< --layout-history (empty = xl.layout next to the input)

    /// `--crt`: link the platform C runtime and C++ runtime instead of the freestanding entry.
    /// The `@entry` function stays a plain `main` the CRT calls, the boot hooks register as image
    /// constructors/destructors, and the linker step goes through the C++ compiler driver, which
    /// brings the CRT startup files and the static-initializer runners a freestanding link lacks.
    /// This is how a program that links C++ code (the `xlc` binary itself) is linked.
    bool crt_link{false};

    /// Builtins (`os`, `arch`, `pointerWidth`, `endian`, `debug`, `optLevel`) and user definitions.
    /// `-D` overrides a builtin of the same name; `-U` (handled by the driver) removes user
    /// definitions only.
    [[nodiscard]] std::optional<ConfigValue> lookup(std::string_view name) const;
    [[nodiscard]] bool isDefined(std::string_view name) const { return lookup(name).has_value(); }

    /// Every resolved definition, sorted by name (`--print-cfg`).
    [[nodiscard]] std::map<std::string, ConfigValue> resolved() const;
};

}  // namespace xlang
