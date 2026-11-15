#include "xlang/lang/config.h"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace xlang {

namespace {

[[nodiscard]] bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

}  // namespace

std::optional<TargetInfo> parseTargetTriple(std::string_view triple) {
    TargetInfo info;
    info.triple = std::string(triple);
    std::string lower(triple);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return std::tolower(c); });
    const std::string_view arch_part = std::string_view(lower).substr(0, lower.find('-'));

    if (arch_part == "x86_64" || arch_part == "amd64" || arch_part == "x64") {
        info.arch = "x86_64";
    } else if (arch_part == "aarch64" || arch_part == "arm64") {
        info.arch = "aarch64";
    } else {
        return std::nullopt;
    }
    if (contains(lower, "darwin") || contains(lower, "macos") || contains(lower, "apple")) {
        info.os = "macos";
    } else if (contains(lower, "linux")) {
        info.os = "linux";
    } else if (contains(lower, "windows") || contains(lower, "win32") || contains(lower, "mingw")) {
        info.os = "windows";
    } else {
        return std::nullopt;
    }
    info.pointer_width = 64;
    info.endian = "little";
    return info;
}

TargetInfo hostTarget() {
    TargetInfo info;
#if defined(__aarch64__) || defined(__arm64__)
    info.arch = "aarch64";
#else
    info.arch = "x86_64";
#endif
#if defined(__APPLE__)
    info.os = "macos";
    info.triple = info.arch + "-apple-darwin";
#elif defined(_WIN32)
    info.os = "windows";
    info.triple = info.arch + "-pc-windows-msvc";
#else
    info.os = "linux";
    info.triple = info.arch + "-unknown-linux-gnu";
#endif
    return info;
}

ConfigValue ConfigValue::makeBool(bool v) {
    ConfigValue c;
    c.kind = Kind::Bool;
    c.bool_value = v;
    return c;
}

ConfigValue ConfigValue::makeInt(std::int64_t v) {
    ConfigValue c;
    c.kind = Kind::Int;
    c.int_value = v;
    return c;
}

ConfigValue ConfigValue::makeString(std::string v) {
    ConfigValue c;
    c.kind = Kind::String;
    c.string_value = std::move(v);
    return c;
}

ConfigValue ConfigValue::parse(std::string_view text) {
    if (text.empty() || text == "true") {
        return makeBool(true);
    }
    if (text == "false") {
        return makeBool(false);
    }
    if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
        return makeString(std::string(text.substr(1, text.size() - 2)));
    }
    std::string_view digits = text;
    bool negative = false;
    if (digits.starts_with('-')) {
        negative = true;
        digits.remove_prefix(1);
    }
    int base = 10;
    if (digits.starts_with("0x") || digits.starts_with("0X")) {
        base = 16;
        digits.remove_prefix(2);
    }
    std::int64_t value = 0;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
    if (!digits.empty() && ec == std::errc() && ptr == digits.data() + digits.size()) {
        return makeInt(negative ? -value : value);
    }
    return makeString(std::string(text));
}

std::string ConfigValue::toString() const {
    switch (kind) {
    case Kind::Bool: return bool_value ? "true" : "false";
    case Kind::Int: return std::to_string(int_value);
    case Kind::String: return "\"" + string_value + "\"";
    }
    return {};
}

std::optional<ConfigValue> BuildConfig::lookup(std::string_view name) const {
    const std::string key(name);
    if (const auto it = defines.find(key); it != defines.end()) {
        return it->second;
    }
    if (name == "os") {
        return ConfigValue::makeString(target.os);
    }
    if (name == "arch") {
        return ConfigValue::makeString(target.arch);
    }
    if (name == "pointerWidth") {
        return ConfigValue::makeInt(target.pointer_width);
    }
    if (name == "endian") {
        return ConfigValue::makeString(target.endian);
    }
    if (name == "debug") {
        return ConfigValue::makeBool(debug);
    }
    if (name == "optLevel") {
        return ConfigValue::makeInt(opt_level);
    }
    return std::nullopt;
}

std::map<std::string, ConfigValue> BuildConfig::resolved() const {
    std::map<std::string, ConfigValue> out;
    for (const char* name : {"os", "arch", "pointerWidth", "endian", "debug", "optLevel"}) {
        if (auto v = lookup(name)) {
            out.emplace(name, std::move(*v));
        }
    }
    for (const auto& [name, value] : defines) {
        out[name] = value;
    }
    return out;
}

}  // namespace xlang
