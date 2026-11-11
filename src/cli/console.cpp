// Terminal queries (see `cli/console.h`).

#include "cli/console.h"

#if defined(_WIN32)

#include <windows.h>

// There is no Win32 call that maps a C `FILE`/file descriptor to a console handle, so the standard
// handle is asked for by name and `_isatty` answers "attached to a console".
//   https://learn.microsoft.com/en-us/windows/console/getstdhandle
//   https://learn.microsoft.com/en-us/windows/console/setconsolemode
#include <io.h>

#include <mutex>
#include <unordered_map>

#else

#include <unistd.h>

#endif

namespace xlang::host {

#if defined(_WIN32)

namespace {

[[nodiscard]] HANDLE consoleHandle(int fd) {
    switch (fd) {
    case kStdoutFd:
        return ::GetStdHandle(STD_OUTPUT_HANDLE);
    case kStderrFd:
        return ::GetStdHandle(STD_ERROR_HANDLE);
    default:
        return INVALID_HANDLE_VALUE;
    }
}

} // namespace

bool isTerminal(int fd) {
    return ::_isatty(fd) != 0;
}

bool colorCapable(int fd) {
    if (!isTerminal(fd)) {
        return false; // redirected to a file or a pipe: no terminal to color
    }
    static std::mutex guard;
    static std::unordered_map<int, bool> decided; // the mode is set once per standard stream
    const std::lock_guard lock(guard);
    if (const auto it = decided.find(fd); it != decided.end()) {
        return it->second;
    }
    bool capable = false;
    const HANDLE handle = consoleHandle(fd);
    DWORD mode = 0;
    if (handle != INVALID_HANDLE_VALUE && ::GetConsoleMode(handle, &mode) != 0) {
        // Without ENABLE_VIRTUAL_TERMINAL_PROCESSING the console prints `ESC[31m` literally. The
        // consoles that refuse it are pre-1511 Windows 10, where the palette stays off.
        capable = (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0 ||
                  ::SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }
    decided.emplace(fd, capable);
    return capable;
}

#else

bool isTerminal(int fd) {
    return ::isatty(fd) != 0;
}

bool colorCapable(int fd) {
    return isTerminal(fd); // a terminal understands ANSI escapes; a file or pipe is never colored
}

#endif

} // namespace xlang::host
