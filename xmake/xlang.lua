-- The xlang parts of the compiler (the lexer, the preprocessor, the macro expander) as one
-- build unit, plus any other xlang entry (the CLI `src/main.xl`).
--
-- Each entry file is compiled by the installed `xl` into one object. `src/lang/compiler.xl`
-- imports `src/lang/lexer/*.xl` and `src/lang/preprocess/*.xl`, so one object carries them all;
-- `src/lang/lexer/lexer.cpp`, `src/lang/preprocess/preprocess.cpp` and
-- `src/lang/macro/macro.cpp` bridge the C ABI on the C++ side
-- (include/xlang/lang/lexer_c.h, preprocess_c.h, macro_c.h).
--
-- `XL_XL` selects the `xl` binary (default: `xl` on PATH). A missing or failing `xl` fails the
-- build instead of linking a stale object.
--
-- Loaded from the root `xmake.lua` with `includes("xmake/xlang.lua")`.

-- The stdlib directory for a raw `xlc` compile (`-M std=<dir>`), computed inside the build
-- script: the script sandbox does not see the xmakefile's globals. `$XLANG_STD`, else the
-- `xl-std` submodule next to this repository, else the newest stdlib under `~/.xlang/std`. The
-- `xl` driver needs none of this: it resolves `std` from `xlang.toml` and `xlang.lock`.
local function script_std_dir()
    if os.getenv("XLANG_STD") then
        return os.getenv("XLANG_STD")
    end
    local submodule = path.normalize(path.join(os.projectdir(), "..", "xl-std", "src"))
    if os.isdir(submodule) then
        return submodule
    end
    local newest
    for _, dir in ipairs(os.dirs(path.join(os.getenv("HOME") or "~", ".xlang", "std", "*"))) do
        newest = dir
    end
    if newest then
        return newest
    end
    return nil
end

rule("xlang.object")
    set_extensions(".xl")
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        local objectfile = target:objectfile(sourcefile)
        local xl = os.getenv("XL_XL") or "xl"
        -- `xl` is the driver (`xl build <file> ...`); a raw `xlc` binary takes the file directly.
        -- The self-hosted `xlc` has no `build` subcommand, so pick the form by the binary name.
        local argv
        if path.filename(xl) == "xl" then
            argv = {"build", path(sourcefile), "--emit=obj", "-o", path(objectfile)}
        else
            -- A raw `xlc` resolves no packages: point it at the stdlib and set the feature flag
            -- the stdlib requires.
            local std = script_std_dir()
            if std then
                argv = {path(sourcefile), "--emit=obj", "-o", path(objectfile),
                        "-M", "std=" .. std, "--define", "featureDefaultAllocator=true"}
            else
                argv = {path(sourcefile), "--emit=obj", "-o", path(objectfile)}
            end
        end
        batchcmds:show_progress(opt.progress, "${color.build.object}compiling.xlang %s", sourcefile)
        batchcmds:mkdir(path.directory(objectfile))
        batchcmds:vrunv(xl, argv)
        -- The object is not a toolchain product, so the linker does not know it: register it on
        -- the target's object list, the way the built-in `utils.bin2obj` rule does.
        table.insert(target:objectfiles(), objectfile)
        -- `xl` emits no depfile, so watch every `.xl` under `src`: an entry imports the lexer,
        -- the preprocessor and the macro expander trees, and a change to any of them must
        -- rebuild the object. `**` does not match files directly in `src`, so add them too.
        batchcmds:add_depfiles(sourcefile)
        for _, pattern in ipairs({"src/*.xl", "src/**/*.xl"}) do
            for _, importfile in ipairs(os.files(pattern)) do
                batchcmds:add_depfiles(importfile)
            end
        end
        batchcmds:set_depmtime(os.mtime(objectfile))
        batchcmds:set_depcache(target:dependfile(objectfile))
    end)

-- Adds one xlang entry file to a target. Call inside a `target(...)` block. The object the rule
-- builds lands on the target's standard object path, so the linker picks it up like any other
-- object of the target.
function add_xlang_object(entry)
    add_rules("xlang.object")
    add_files(entry or "src/lang/compiler.xl")
end
