// Plan 5.8 / 5.9: linker driver and in-process archiver.

#include "xlang/codegen/link.h"

#include "xlang/util/error.h"

#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/Error.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <map>
#include <set>

namespace xlang::codegen {

namespace {

[[nodiscard]] std::filesystem::path findTool(const std::vector<std::string>& names) {
    std::vector<std::filesystem::path> dirs;
    if (const char* path = std::getenv("PATH")) {
        const std::string p = path;
        // `;` between entries on Windows, `:` everywhere else (a Windows drive letter `C:` is why).
#if defined(_WIN32)
        constexpr char kSeparator = ';';
#else
        constexpr char kSeparator = ':';
#endif
        std::size_t start = 0;
        while (start <= p.size()) {
            const std::size_t end = p.find(kSeparator, start);
            dirs.emplace_back(p.substr(start, end == std::string::npos ? std::string::npos : end - start));
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
#if defined(_WIN32)
    // The LLVM installer's default directory, then the current drive's root as a last resort.
    if (const char* program_files = std::getenv("ProgramFiles")) {
        dirs.emplace_back(std::filesystem::path(program_files) / "LLVM" / "bin");
    }
    dirs.emplace_back("C:\\Program Files\\LLVM\\bin");
#else
    for (const char* extra : {"/opt/homebrew/opt/llvm/bin", "/usr/local/opt/llvm/bin", "/opt/homebrew/bin",
                              "/usr/lib/llvm/bin", "/usr/bin"}) {
        dirs.emplace_back(extra);
    }
#endif
    std::error_code ec;
    for (const std::string& name : names) {
#if defined(_WIN32)
        // A Win32 program is `name.exe`; `lld-link` is `lld-link.exe` on disk.
        const std::filesystem::path spelled = name.find('.') == std::string::npos ? name + ".exe" : name;
#else
        const std::filesystem::path spelled = name;
#endif
        for (const std::filesystem::path& d : dirs) {
            const std::filesystem::path candidate = d / spelled;
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }
    }
    return {};
}

#if !defined(_WIN32)
/// The macOS SDK root (`xcrun --show-sdk-path`), where ld64 finds `usr/lib/libSystem.tbd`. Empty when
/// xcrun is missing; the linker then reports the undefined library itself. Only macOS has the SDK,
/// so the call is not compiled on Windows or Linux (`popen` would need `_popen` there).
[[nodiscard]] std::string macosSdkRoot() {
    std::string out;
    FILE* pipe = popen("xcrun --show-sdk-path 2>/dev/null", "r");
    if (pipe == nullptr) {
        return out;
    }
    std::array<char, 512> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), pipe) != nullptr) {
        out += buf.data();
    }
    pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) {
        out.pop_back();
    }
    return out;
}
#endif

/// Quotes one command-line argument for `std::system`.
///   POSIX: single quotes, an embedded `'` written as `'\''`.
///   Windows: `std::system` runs through `cmd.exe`, where single quotes are *not* quoting. Double
///            quotes are, and a literal `"` becomes `\"` (the `cmd.exe` / MSVC runtime rules,
///            https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args).
[[nodiscard]] std::string quote(const std::string& s) {
#if defined(_WIN32)
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"') {
            out += "\\\"";
        } else {
            out += c;
        }
    }
    return out + "\"";
#else
    std::string out = "'";
    for (const char c : s) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out += c;
        }
    }
    return out + "'";
#endif
}

/// Runs a command line built from `quote`d words through the shell and returns its status.
/// Windows: `std::system` is `cmd.exe /c <command>`, and cmd strips the first and the last `"` of a
/// command that starts with a quote and holds more than two of them, which breaks every path in it
/// ("The filename, directory name, or volume label syntax is incorrect"). One more pair around the whole
/// command is the documented fix (`cmd /?`, the `/c` rules).
[[nodiscard]] int runShell(const std::string& command) {
#if defined(_WIN32)
    return std::system(("\"" + command + "\"").c_str());
#else
    return std::system(command.c_str());
#endif
}

}  // namespace

void link(const LinkRequest& r) {
    const TargetInfo& t = r.config->target;
    std::vector<std::string> args;
    std::vector<std::filesystem::path> temporaries;  ///< generated import libraries, removed after linking
    std::filesystem::path tool;
    const bool shared = r.kind == LinkKind::Shared;
    if (t.os == "macos") {
        tool = findTool({"ld64.lld"});
        if (tool.empty()) {
            throw XlangError("no Mach-O linker found (`ld64.lld`); install LLVM's lld, or stop at `--emit=obj`");
        }
        const std::string arch = t.arch == "aarch64" ? "arm64" : "x86_64";
        args = {"-arch", arch, "-platform_version", "macos", "11.0", "11.0", "-o", r.output.string()};
        if (shared) {
            args.push_back("-dylib");
        } else {
            args.push_back("-e");
            args.push_back("_" + (r.entry.empty() ? std::string("start") : r.entry));
            args.push_back("-no_uuid");
            args.push_back("-dead_strip");
        }
    } else if (t.os == "linux") {
        tool = findTool({"ld.lld", "ld"});
        if (tool.empty()) {
            throw XlangError("no ELF linker found (`ld.lld` or `ld`); install one, or stop at `--emit=obj`");
        }
        args = {"-o", r.output.string(), "-nostdlib", "-static"};
        if (shared) {
            args = {"-o", r.output.string(), "-nostdlib", "-shared"};
        } else {
            args.push_back("-e");
            args.push_back(r.entry.empty() ? "_start" : r.entry);
            args.push_back("--gc-sections");
        }
    } else if (t.os == "windows") {
        tool = findTool({"lld-link"});
        if (tool.empty()) {
            throw XlangError("no COFF linker found (`lld-link`); install LLVM's lld, or stop at `--emit=obj`");
        }
        args = {"/nodefaultlib", "/out:" + r.output.string(), "/subsystem:console"};
        // Native OS libraries (crypt32, ws2_32, ...) imported with `@library`: one import library per
        // DLL, generated from the symbols the program declares (no Windows SDK needed).
        std::map<std::string, std::set<std::string>> byLibrary;
        for (const auto& [lib, sym] : r.imports) {
            byLibrary[lib].insert(sym);
        }
        if (!shared) {
            byLibrary["ntdll"].insert("NtTerminateProcess");  // the entry stub's exit
        }
        if (!byLibrary.empty()) {
            const std::filesystem::path dlltool = findTool({"llvm-dlltool"});
            if (dlltool.empty()) {
                throw XlangError("no import-library tool found (`llvm-dlltool`); install LLVM, or stop at `--emit=obj`");
            }
            const std::filesystem::path dir = r.output.parent_path().empty() ? std::filesystem::path(".")
                                                                           : r.output.parent_path();
            const std::string machine = t.arch == "aarch64" ? "arm64" : "i386:x86-64";
            for (const auto& [lib, syms] : byLibrary) {
                std::string dll = lib;
                if (dll.size() < 4 || dll.substr(dll.size() - 4) != ".dll") {
                    dll += ".dll";
                }
                const std::filesystem::path def = dir / (lib + ".xl-import.def");
                const std::filesystem::path implib = dir / (lib + ".xl-import.lib");
                std::string text = "LIBRARY " + dll + "\nEXPORTS\n";
                for (const std::string& s : syms) {
                    text += "    " + s + "\n";
                }
                {
                    std::ofstream f(def, std::ios::binary);
                    f << text;
                }
                const std::string cmd = quote(dlltool.string()) + " -m " + quote(machine) + " -d " +
                                        quote(def.string()) + " -l " + quote(implib.string()) + " 2>&1";
                const int rc = runShell(cmd);
                std::filesystem::remove(def);
                if (rc != 0) {
                    throw XlangError("cannot create the import library for `" + dll + "`");
                }
                args.push_back(implib.string());
                temporaries.push_back(implib);
            }
        }
        if (shared) {
            args.push_back("/dll");
        } else {
            args.push_back("/entry:" + (r.entry.empty() ? std::string("mainCRTStartup") : r.entry));
        }
    } else {
        throw XlangError("cannot link for target OS `" + t.os + "`");
    }
    for (const std::filesystem::path& o : r.objects) {
        args.push_back(o.string());
    }
    // Plan 24: the OS libraries the program imports with `@library`, after pruning, so only what is
    // reachable is linked. They come after the objects so that static archives resolve.
    if (t.os == "macos" || t.os == "linux") {
        std::set<std::string> libs;
        for (const auto& [lib, sym] : r.imports) {
            libs.insert(lib);
        }
        if (t.os == "macos" && !libs.empty()) {
#if !defined(_WIN32)
            const std::string sdk = macosSdkRoot();
            if (!sdk.empty()) {
                args.push_back("-syslibroot");
                args.push_back(sdk);
            }
#endif
        }
        for (const std::string& lib : libs) {
            args.push_back("-l" + lib);
        }
    }
    std::string command = quote(tool.string());
    for (const std::string& a : args) {
        command += " " + quote(a);
    }
    command += " 2>&1";
    const int status = runShell(command);
    for (const std::filesystem::path& tmp : temporaries) {
        std::filesystem::remove(tmp);
    }
    if (status != 0) {
        throw XlangError(std::format("linker failed: {}", command));
    }
}

void archive(const std::vector<std::filesystem::path>& objects, const std::filesystem::path& output,
             const BuildConfig& config) {
    std::vector<llvm::NewArchiveMember> members;
    for (const std::filesystem::path& o : objects) {
        auto m = llvm::NewArchiveMember::getFile(o.string(), true);
        if (!m) {
            throw XlangError("cannot read `" + o.string() + "`: " + llvm::toString(m.takeError()));
        }
        members.push_back(std::move(*m));
    }
    const auto kind = config.target.os == "macos" ? llvm::object::Archive::K_DARWIN : llvm::object::Archive::K_GNU;
    if (llvm::Error e = llvm::writeArchive(output.string(), members, llvm::SymtabWritingMode::NormalSymtab, kind, true,
                                           false)) {
        throw XlangError("cannot write archive: " + llvm::toString(std::move(e)));
    }
}

}  // namespace xlang::codegen
