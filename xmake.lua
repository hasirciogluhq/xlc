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

local xlc_version = "1.0.0-rc.2"

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

add_requires("cli11 2.7.2")

-- Finds llvm-config and adds the static LLVM libraries the compiler uses to `target`.
local function add_llvm(target)
    local candidates = {}
    if os.getenv("LLVM_CONFIG") then
        table.insert(candidates, os.getenv("LLVM_CONFIG"))
    end
    for _, prefix in ipairs({os.getenv("HOMEBREW_PREFIX"), "/opt/homebrew", "/usr/local"}) do
        if prefix then
            table.insert(candidates, path.join(prefix, "opt/llvm/bin/llvm-config"))
        end
    end
    table.insert(candidates, "llvm-config")
    local cfg
    for _, c in ipairs(candidates) do
        if try { function () return os.iorun(c .. " --version") end } then
            cfg = c
            break
        end
    end
    assert(cfg, "llvm-config not found (set LLVM_CONFIG)")
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

-- Everything except `main.cpp`, so the unit tests link the same code.
target("xlc-core")
    set_kind("static")
    set_default(false)
    add_files("src/codegen/**.cpp", "src/driver/**.cpp", "src/hir/**.cpp", "src/lang/**.cpp",
              "src/sema/**.cpp", "src/util/**.cpp", "src/cli/console.cpp", "src/cli/emit.cpp")
    add_includedirs("include", "src", {public = true})
    add_packages("cli11", {public = true})
    on_load(add_llvm)

target("xlc")
    set_kind("binary")
    set_default(true)
    set_basename("xlc")
    add_files("src/cli/main.cpp")
    add_deps("xlc-core")
    -- Released binaries carry their C++ runtime: no libstdc++/libc++ DLL to install next to them.
    if is_plat("windows", "mingw") then
        add_syslinks("ws2_32", "ole32", "uuid", "version", "shell32", "advapi32")
    end
    if is_plat("mingw") then
        add_ldflags("-static", {force = true})
    elseif is_plat("linux") then
        add_ldflags("-static-libstdc++", "-static-libgcc", {force = true})
    end

-- Frontend/sema/HIR unit tests: `xmake build xlc-tests && xmake run xlc-tests`.
target("xlc-tests")
    set_kind("binary")
    set_default(false)
    add_files("test/unit/*.cpp")
    add_deps("xlc-core")
    add_tests("unit")
