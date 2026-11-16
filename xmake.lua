-- xlc: the xlang compiler. One executable, `xlc`. The code generator is the native arm64 Mach-O
-- backend written in xlang (src/codegen); there is no LLVM.
--
--   xmake f -m release && xmake        build into build/<plat>/<arch>/release/
--   xmake run xlc-tests                frontend/sema/HIR unit tests
--
-- The version below is the single source of truth: release tags must match it (`vX.Y.Z` or
-- `vX.Y.Z-rc.N`, see .github/workflows/release.yml).

local xlc_version = "1.0.0-rc.12"

set_project("xlc")
set_version(xlc_version)
set_languages("c++23")
set_warnings("all")
add_rules("mode.debug", "mode.release")
add_rules("plugin.compile_commands.autoupdate", {outputdir = "build"})
add_defines("XLANG_VERSION=\"" .. xlc_version .. "\"")

-- The xlang parts of the compiler (the lexer, the preprocessor, the macro expander) are compiled
-- by an installed `xl` into one object, reached from C++ through the C ABIs in
-- `include/xlang/lang/lexer_c.h`, `directives_c.h` and `macro_c.h`. `xmake/xlang.lua` turns the
-- entry file into one object.
includes("xmake/xlang.lua")

-- Every module builds standalone as its own static archive, so a port of one module to xlang
-- replaces exactly one archive. The order is the dependency order.
local xlc_modules = {
    {"xlc-util",     {"src/util/**.cpp"}},
    {"xlc-lang",     {"src/lang/**.cpp"}},
    {"xlc-hir",      {"src/hir/**.cpp"}},
    {"xlc-mir",      {"src/mir/**.cpp"}},
    {"xlc-analysis", {"src/analysis/**.cpp"}},
    {"xlc-backend",  {"src/backend/**.cpp"}},
}

-- Adds every module archive to a target that needs the whole compiler.
function add_xlc_modules()
    for _, module in ipairs(xlc_modules) do
        add_deps(module[1])
    end
    -- The modules are cyclic (the xlang object in `xlc-lang` calls the backend C API, and the
    -- backend calls the pipeline through it): a link group lets the linker rescan.
    local names = {}
    for _, module in ipairs(xlc_modules) do
        table.insert(names, module[1])
    end
    add_linkgroups(names, {group = true})
    add_includedirs("include", "src", {public = true})
end

for _, module in ipairs(xlc_modules) do
    target(module[1])
        set_kind("static")
        set_default(false)
        add_files(module[2])
        add_includedirs("include", "src", {public = true})
    target_end()
end

-- `xlc-lang` also carries the xlang object: the scanner, the directive walk and the macro
-- expander; lexer.cpp, directives_bridge.cpp and macro.cpp only bridge the C ABI.
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
    set_kind("binary")
    set_default(false)
    set_basename("xlc-bootstrap")
    add_files("src/bootstrap.cpp")
    add_xlang_object("src/bootstrap.xl")
    add_xlc_modules()

-- The binary: the CLI is xlang code (`src/main.xl`, `@entry fn main`) and the C++ front half joins
-- it as the static archives of the modules above. `xlc` performs the final link itself in its
-- `--crt` mode through the platform C++ compiler driver. The `xlc` that compiles and links is
-- `XL_XL`: the bootstrap binary built by the `xlc-bootstrap` target above, or `$XL_XL` to
-- bootstrap from another compiler that speaks `--crt`. The stdlib comes from `$XLANG_STD` first
-- of all, else the `xl-std` submodule checkout when it exists (local development), else the
-- newest stdlib the toolchain installed (`~/.xlang/std/<version>`).
target("xlc")
    set_kind("binary")
    set_default(true)
    set_basename("xlc")
    add_deps("xlc-bootstrap")
    add_deps("xlc-backend")
    add_includedirs("include", "src")
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
        -- The C++ runtime of the front-half archives; the driver brings libc itself.
        table.insert(args, "--link-arg=-lc++")
        -- Rebuild only when a source of the binary changed: every `.xl` under `src` or one of the
        -- archives.
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
