#pragma once

#include "cli/console.h"

#include <cstdlib>
#include <string>
#include <string_view>

namespace xlang::cli {

/// ANSI styles for CLI output. With `on` unset every member returns an empty string, so call sites
/// never branch on color.
struct Palette {
    bool on;
    const char* reset() const { return on ? "\x1b[0m" : ""; }
    const char* bold() const { return on ? "\x1b[1m" : ""; }
    const char* dim() const { return on ? "\x1b[2m" : ""; }
    const char* red() const { return on ? "\x1b[31m" : ""; }
    const char* green() const { return on ? "\x1b[32m" : ""; }
    const char* yellow() const { return on ? "\x1b[33m" : ""; }
    const char* blue() const { return on ? "\x1b[34m" : ""; }
    const char* cyan() const { return on ? "\x1b[36m" : ""; }
    const char* boldRed() const { return on ? "\x1b[1;31m" : ""; }
    const char* boldGreen() const { return on ? "\x1b[1;32m" : ""; }
    const char* boldYellow() const { return on ? "\x1b[1;33m" : ""; }
    const char* boldBlue() const { return on ? "\x1b[1;34m" : ""; }
    const char* boldCyan() const { return on ? "\x1b[1;36m" : ""; }
    const char* inverseGreen() const { return on ? "\x1b[1;30;42m" : ""; }
    const char* inverseRed() const { return on ? "\x1b[1;37;41m" : ""; }
    const char* inverseCyan() const { return on ? "\x1b[1;30;46m" : ""; }
};

/// `--color=always|never`, set once by the command line; empty means "not given".
inline std::string& colorMode() {
    static std::string mode;
    return mode;
}

/// Whether output to `fd` is colored. `--color` (or, without it, `XLANG_COLOR=always|never`)
/// decides outright; otherwise a terminal is
/// colored unless `NO_COLOR` is set to a non-empty value.
///
/// "A terminal is colored" is `host::colorCapable`: `isatty` on POSIX, and on Windows also that
/// virtual terminal processing could be switched on for the console (see `cli/console.h`). A
/// Windows console that cannot render escapes gets no `ESC` bytes at all.
inline bool useColor(int fd) {
    if (const char* forced = colorMode().empty() ? std::getenv("XLANG_COLOR") : colorMode().c_str()) {
        const std::string_view value(forced);
        if (value == "always") {
            return true;
        }
        if (value == "never") {
            return false;
        }
    }
    const char* no_color = std::getenv("NO_COLOR");
    return host::colorCapable(fd) && (no_color == nullptr || *no_color == '\0');
}

} // namespace xlang::cli
