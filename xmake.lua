-- xlc: the xlang compiler (C++23, LLVM). One executable, `xlc`.
--
--   xmake f -m release && xmake        build into build/<plat>/<arch>/release/
--   xmake run xlc-tests                frontend/sema/HIR unit tests
--
-- LLVM comes from the system: `$LLVM_CONFIG`, then Homebrew, then `llvm-config` on PATH. It is
-- linked statically (only the components the compiler uses), so the released binary has no LLVM
-- runtime dependency.
--
-- The version below is the single source of truth: release tags must match it (`vX.Y.Z` or
-- `vX.Y.Z-rc.N`, see .github/workflows/release.yml).

local xlc_version = "1.0.0-rc.12"

set_project("xlc")
set_version(xlc_version)
set_languages("c++23")
set_warnings("all")
if is_plat("windows") then
    set_symbols("debug") -- a PDB next to the exe: crash dumps and debuggers show function names
    -- clang-cl produced a stack frame that was not 16-aligned around `__int128` code (see
    -- `ConstValue`); every function realigns its frame instead of trusting the caller.
    add_cxxflags("/clang:-mstackrealign", {tools = "clang_cl"})
end
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})
add_defines("XLANG_VERSION=\"" .. xlc_version .. "\"")

-- Finds llvm-config: `$LLVM_CONFIG`, then Homebrew, then the pinned LLVM 23 install of
-- apt.llvm.org (`/usr/lib/llvm-23`), then `llvm-config` on PATH. Probed by file existence only:
-- this function runs with the xmakefile's restricted environment, which has no `os.which`, no
-- `assert` and no way to run a program. The version is pinned, not discovered: the code needs
-- the LLVM 20+ APIs, and the runners carry an older llvm-dev that must never win. When nothing
-- matches it returns the bare name, so the caller's `llvm-config` invocation fails with the
-- linker's own clear message.
local function find_llvm_config()
    if os.getenv("LLVM_CONFIG") then
        return os.getenv("LLVM_CONFIG")
    end
    local candidates = {}
    for _, prefix in ipairs({os.getenv("HOMEBREW_PREFIX"), "/opt/homebrew", "/usr/local"}) do
        if prefix then
            table.insert(candidates, path.join(prefix, "opt/llvm/bin/llvm-config"))
        end
    end
    table.insert(candidates, "/usr/lib/llvm-23/bin/llvm-config")
    table.insert(candidates, "/usr/bin/llvm-config")
    for _, c in ipairs(candidates) do
        if os.isfile(c) then
            return c
        end
    end
    return "llvm-config"
end

local function add_llvm(target)
    local cfg = find_llvm_config()
    local function query(args)
        return (os.iorun(cfg .. " " .. args)):trim()
    end
    target:add("sysincludedirs", query("--includedir"))
    target:add("linkdirs", query("--libdir"))
    -- llvm-config spells a library `-lLLVMCore` (Unix) or `LLVMCore.lib` / a full path (MSVC).
    local function add_lib(lib)
        if lib:startswith("-l") then
            target:add("links", lib:sub(3))
        elseif lib:endswith(".lib") then
            if lib:find("[/\\]") then
                target:add("linkdirs", path.directory(lib))
            end
            local name = path.basename(lib)
            -- llvm-config names the zstd *import* library (`zstd.dll.lib`); the release links the
            -- static one so the binary needs no zstd.dll next to it.
            if name == "zstd.dll" or name == "zstd" then
                name = "zstd_static"
            elseif name == "z" then
                name = "zlibstatic" -- conda's zlib: `zlib.lib` is the import library
            elseif name == "xml2" then
                return -- only LLVM's Windows manifest tool needs libxml2; it is not linked
            end
            target:add("links", name)
        elseif #lib > 0 and not lib:startswith("-") then
            target:add("links", lib)
        end
    end
    for _, lib in ipairs(query("--link-static --libs aarch64 x86 passes irreader bitwriter object mc target codegen"):split("%s+")) do
        add_lib(lib)
    end
    -- clang-cl does not link compiler-rt by itself: the 128-bit helpers (`__floattidf`, ...) that
    -- `__int128` code needs live in clang_rt.builtins.
    if is_plat("windows") then
        local dir = try { function () return os.iorun("clang-cl /clang:-print-resource-dir") end }
        if dir then
            local lib = path.join(dir:trim(), "lib", "windows", "clang_rt.builtins-x86_64.lib")
            assert(os.isfile(lib), "clang_rt.builtins not found: " .. lib)
            add_lib(lib)
        end
    end
    -- Homebrew keeps zstd & co. outside the default linker search path.
    for _, dir in ipairs({"/opt/homebrew/lib", "/usr/local/lib"}) do
        if os.isdir(dir) then
            target:add("linkdirs", dir)
        end
    end
    -- Whatever LLVM itself needs from the system (zlib, zstd, curses, ...), as llvm-config knows it.
    -- The released binary must start on a machine that has none of the build machine's packages, so
    -- a library outside the OS itself is linked as a static archive or left out:
    --   z3, xml2  only LLVM parts the compiler never links use them (the Z3 constraint solver of
    --             LLVMSupport, the Windows manifest tool); Homebrew's z3 has no archive at all.
    --   zstd      linked from its archive (`libzstd.a`) where one is installed.
    local skipped = {z3 = true, xml2 = true}
    local static_dirs = {"/opt/homebrew/lib", "/usr/local/lib", "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu"}
    for _, lib in ipairs(query("--link-static --system-libs"):split("%s+")) do
        local name = lib:startswith("-l") and lib:sub(3) or path.basename(lib):gsub("^lib", ""):gsub("[%.%d]*%.dylib$", ""):gsub("%.so.*$", "")
        if not is_plat("windows") and skipped[name] then
            -- not linked
        elseif not is_plat("windows") and name == "zstd" then
            local archive
            for _, dir in ipairs(static_dirs) do
                if os.isfile(path.join(dir, "libzstd.a")) then
                    archive = path.join(dir, "libzstd.a")
                    break
                end
            end
            if archive then
                target:add("ldflags", archive, {force = true})
            else
                target:add("syslinks", "zstd")
            end
        elseif lib:startswith("-l") then
            target:add("syslinks", lib:sub(3))
        else
            add_lib(lib)
        end
    end
end

-- The xlang parts of the compiler (the lexer, the preprocessor, the macro expander) are compiled
-- by an installed `xl` into one object, reached from C++ through the C ABIs in
-- `include/xlang/lang/lexer_c.h`, `preprocess_c.h` and `macro_c.h`. `xmake/xlang.lua` turns the
-- entry file into one object.
includes("xmake/xlang.lua")

-- Every module builds standalone as its own static archive, so a port of one module to xlang
-- replaces exactly one archive. The order is the dependency order.
local xlc_modules = {
    {"xlc-util",     {"src/util/**.cpp"}},
    {"xlc-lang",     {"src/lang/**.cpp"}},
    {"xlc-sema",     {"src/sema/**.cpp"}},
    {"xlc-hir",      {"src/hir/**.cpp"}},
    {"xlc-mir",      {"src/mir/**.cpp"}},
    {"xlc-codegen",  {"src/codegen/**.cpp"}},
    {"xlc-analysis", {"src/analysis/**.cpp"}},
    {"xlc-cli",      {"src/cli/console.cpp", "src/cli/emit.cpp", "src/driver/**.cpp"}},
    {"xlc-backend",  {"src/backend/**.cpp"}},
}

-- Adds every module archive to a target that needs the whole compiler.
function add_xlc_modules()
    for _, module in ipairs(xlc_modules) do
        add_deps(module[1])
    end
    -- The modules are cyclic (the xlang object in `xlc-lang` calls the backend C API, and the
    -- backend calls the pipeline through it), and the ELF linker scans archives once; a link
    -- group lets it rescan. Only the GNU-style linkers implement the group (`--start-group`, in
    -- xmake's gcc/clang tool modules); the MSVC `link.exe`/`lld-link` has none, so on Windows the
    -- group would silently drop every archive from the link line. Guard to the linkers that have
    -- it and keep plain `add_deps` propagation elsewhere.
    if is_plat("linux") or is_plat("macosx") then
        local names = {}
        for _, module in ipairs(xlc_modules) do
            table.insert(names, module[1])
        end
        add_linkgroups(names, {group = true})
    end
    add_includedirs("include", "src", {public = true})
end

for _, module in ipairs(xlc_modules) do
    target(module[1])
        set_kind("static")
        set_default(false)
        add_files(module[2])
        add_includedirs("include", "src", {public = true})
        on_load(add_llvm)
    target_end()
end

-- `xlc-lang` also carries the xlang object: the scanner, the directive walk and the macro
-- expander; lexer.cpp, preprocess.cpp and macro.cpp only bridge the C ABI.
target("xlc-lang")
    add_xlang_object("src/lang/compiler.xl")
target_end()

-- The bootstrap rung of the ladder: the released `xlc` that builds this repository can neither
-- link `--crt` nor emit an `@entry` function to an object file (E0754), so the compiler binary
-- of this tree is built in two steps. The bootstrap is a plain C++ link (xmake's own linker):
-- the CLI entry src/bootstrap.xl (`export(c) fn main`, the same `run` as src/main.xl), the
-- constructor shim src/bootstrap.cpp and the module archives. It ships in no release; the
-- `xlc` target below builds the real binary from src/main.xl with `--crt` through it.
target("xlc-bootstrap")
    -- The released xlc for Windows has no lowering for `try` (E0714 on windows/x86_64), and
    -- every stdlib-importing program reaches it, so no bootstrap object can be built there:
    -- the rung needs a compiler release with Windows unwinding. The Windows CI builds the C++
    -- archives and the unit tests only.
    if is_plat("windows") then
        set_enabled(false)
    end
    set_kind("binary")
    set_default(false)
    set_basename("xlc-bootstrap")
    add_files("src/bootstrap.cpp")
    add_xlang_object("src/bootstrap.xl")
    add_xlc_modules()
    on_load(add_llvm)
    if is_plat("linux") then
        -- Released binaries carry their C++ runtime: no libstdc++ DSO to install next to them.
        add_ldflags("-static-libstdc++", "-static-libgcc", {force = true})
    end

-- The binary: the CLI is xlang code (`src/main.xl`, `@entry fn main`) and the C++ backend joins
-- it as the static archives of the modules above. `xlc` performs the final link itself in its
-- `--crt` mode: the boot hooks become image constructors/destructors and the platform C++
-- compiler driver brings the CRT startup files, libc, the C++ runtime and the
-- static-initializer runners. The `xlc` that compiles and links is `XL_XL`: the bootstrap
-- binary built by the `xlc-bootstrap` target above, or `$XL_XL` to bootstrap from another
-- compiler that speaks `--crt`. The stdlib comes from `$XLANG_STD` first of all (on CI the
-- toolchain action sets it), else the `xl-std` submodule checkout when it exists (local
-- development), else the newest stdlib the toolchain installed (`~/.xlang/std/<version>`).
target("xlc")
    -- See `xlc-bootstrap` above: without a bootstrap there is no compiler to build this with.
    if is_plat("windows") then
        set_enabled(false)
    end
    set_kind("binary")
    set_default(true)
    set_basename("xlc")
    add_deps("xlc-bootstrap")
    add_deps("xlc-backend")
    add_includedirs("include", "src")
    on_load(function (target)
        -- The LLVM archives for the `--crt` link are resolved here, not at the xmakefile scope:
        -- only target scripts can run programs (`os.iorun`). The same components `add_llvm`
        -- links, as full archive paths so the order the driver sees them in does not matter.
        local cfg = find_llvm_config()
        local function query(args)
            return (os.iorun(cfg .. " " .. args)):trim()
        end
        local libdir = query("--libdir")
        local llvmargs = {}
        for _, lib in ipairs(query("--link-static --libs aarch64 x86 passes irreader bitwriter object mc target codegen"):split("%s+")) do
            if lib:startswith("-l") then
                table.insert(llvmargs, path.join(libdir, "lib" .. lib:sub(3) .. ".a"))
            end
        end
        -- Whatever LLVM itself needs from the system (zlib, zstd, curses, ...), as llvm-config
        -- knows it. z3 and xml2 are left out: only LLVM parts the compiler never links use them,
        -- and Homebrew's z3 has no archive at all. zstd is linked from its archive where one is
        -- installed.
        local skipped = {z3 = true, xml2 = true}
        local static_dirs = {"/opt/homebrew/lib", "/usr/local/lib", "/usr/lib/x86_64-linux-gnu", "/usr/lib/aarch64-linux-gnu"}
        for _, lib in ipairs(query("--link-static --system-libs"):split("%s+")) do
            local name = lib:startswith("-l") and lib:sub(3) or path.basename(lib):gsub("^lib", ""):gsub("[%.%d]*%.dylib$", ""):gsub("%.so.*$", "")
            if skipped[name] then
                -- not linked
            elseif name == "zstd" then
                local archive
                for _, dir in ipairs(static_dirs) do
                    if os.isfile(path.join(dir, "libzstd.a")) then
                        archive = path.join(dir, "libzstd.a")
                        break
                    end
                end
                table.insert(llvmargs, archive or "-lzstd")
            else
                table.insert(llvmargs, lib)
            end
        end
        target:data_set("llvm_link_args", llvmargs)
    end)
    on_build(function (target, opt)
        import("core.project.depend")
        local xlcbin = os.getenv("XL_XL") or target:dep("xlc-bootstrap"):targetfile()
        local out = target:targetfile()
        local archives = {}
        for _, module in ipairs(xlc_modules) do
            table.insert(archives, path.join(target:targetdir(), "lib" .. module[1] .. ".a"))
        end
        local std = os.getenv("XLANG_STD")
        if not std and os.isdir(path.join(os.projectdir(), "..", "xl-std", "src")) then
            std = path.join(os.projectdir(), "..", "xl-std", "src")
        end
        if not std then
            local installed = os.dirs(path.join(os.getenv("HOME") or "~", ".xlang", "std", "*")) or {}
            table.sort(installed)
            std = installed[#installed]
        end
        assert(std, "no stdlib found: check out the xl-std submodule or install a release of xl")
        local args = {"src/main.xl", "--emit=exe", "--crt", "-M", "std=" .. std, "--import", "std",
                      "--define", "featureDefaultAllocator=true", "-o", out}
        for _, a in ipairs(archives) do
            table.insert(args, "--link-arg=" .. a)
        end
        for _, a in ipairs(target:data("llvm_link_args")) do
            table.insert(args, "--link-arg=" .. a)
        end
        -- The C++ runtime of the backend archives; the driver brings libc itself.
        if is_plat("macosx") then
            table.insert(args, "--link-arg=-lc++")
        elseif is_plat("linux") then
            table.insert(args, "--link-arg=-lstdc++")
        end
        -- Rebuild only when a source of the binary changed: every `.xl` under `src` (the CLI and
        -- the ported compiler parts the compiler.xl object carries) or one of the archives.
        local inputs = {}
        for _, pattern in ipairs({"src/*.xl", "src/**/*.xl"}) do
            for _, f in ipairs(os.files(pattern)) do
                table.insert(inputs, f)
            end
        end
        for _, a in ipairs(archives) do
            table.insert(inputs, a)
        end
        depend.on_changed(function ()
            os.vrunv(xlcbin, args)
        end, {files = inputs, dependfile = target:dependfile(out)})
    end)
    on_link(function (target, opt)
        -- The link already happened in `on_build`, through `xlc --crt`.
    end)
    on_clean(function (target, opt)
        os.tryrm(target:targetfile())
    end)

-- Frontend/sema/HIR unit tests: `xmake build xlc-tests && xmake run xlc-tests`.
target("xlc-tests")
    set_kind("binary")
    set_default(false)
    add_files("test/unit/*.cpp")
    add_deps("xlc-backend")
    add_xlc_modules()
    add_tests("unit")
