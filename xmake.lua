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

local xlc_version = "1.0.0-rc.1"

set_project("xlc")
set_version(xlc_version)
set_languages("c++23")
set_warnings("all")
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
    for _, lib in ipairs(query("--link-static --libs aarch64 x86 passes irreader bitwriter object mc target codegen"):split("%s+")) do
        target:add("links", (lib:gsub("^%-l", "")))
    end
    -- Homebrew keeps zstd & co. outside the default linker search path.
    for _, dir in ipairs({"/opt/homebrew/lib", "/usr/local/lib"}) do
        if os.isdir(dir) then
            target:add("linkdirs", dir)
        end
    end
    -- Whatever LLVM itself needs from the system (zlib, zstd, curses, ...), as llvm-config knows it.
    for _, lib in ipairs(query("--link-static --system-libs"):split("%s+")) do
        if lib:startswith("-l") then
            target:add("syslinks", lib:sub(3))
        elseif #lib > 0 and not lib:startswith("-") then
            target:add("links", lib)
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
