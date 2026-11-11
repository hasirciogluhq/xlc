#pragma once

// Terminal queries: is stdout/stderr a console, and does it understand ANSI escapes?
//
// POSIX answers both with `isatty`. Windows has two separate facts: a stream can be attached to the
// console (`_isatty`) and the console can still refuse escape sequences until virtual terminal
// processing is switched on for its handle
// (https://learn.microsoft.com/en-us/windows/console/setconsolemode). The palette is shared by the
// CLI, so the answers live here instead of on each call site.

namespace xlang::host {

/// File descriptors the CLI colors; the same values as `STDOUT_FILENO`/`STDERR_FILENO` everywhere.
inline constexpr int kStdoutFd = 1;
inline constexpr int kStderrFd = 2;

/// Whether `fd` is a terminal. On Windows an attached console counts; the color decision is
/// `colorCapable`.
[[nodiscard]] bool isTerminal(int fd);

/// Whether `fd` can take ANSI escape sequences right now.
///   Windows: `GetConsoleMode`, then `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING`
///            (https://learn.microsoft.com/en-us/windows/console/setconsolemode) once for that
///            handle; false when the console rejects it (a redirected stream, or Windows 10 before
///            1511).
///   POSIX:   `isatty`; a terminal understands escapes.
[[nodiscard]] bool colorCapable(int fd);

} // namespace xlang::host
