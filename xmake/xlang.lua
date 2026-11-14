-- The xlang parts of the compiler (the lexer, the preprocessor) as one build unit.
--
-- `src/lang/compiler.xl` is the entry: it imports `src/lang/lexer/*.xl` and
-- `src/lang/preprocess/*.xl`, so one object carries them all. The installed `xl` builds the
-- object, and `src/lang/lexer/lexer.cpp` and `src/lang/preprocess/preprocess.cpp` bridge the C
-- ABI on the C++ side (include/xlang/lang/lexer_c.h, preprocess_c.h).
--
-- `XL_XL` selects the `xl` binary (default: `xl` on PATH). A missing or failing `xl` fails the
-- build instead of linking a stale object.
--
-- Loaded from the root `xmake.lua` with `includes("xmake/xlang.lua")`.

-- The entry file: it imports the rest, so one object carries them all.
local xlang_entry = "src/lang/compiler.xl"

rule("xlang.object")
    set_extensions(".xl")
    on_buildcmd_file(function (target, batchcmds, sourcefile, opt)
        local objectfile = target:objectfile(sourcefile)
        local xl = os.getenv("XL_XL") or "xl"
        batchcmds:show_progress(opt.progress, "${color.build.object}compiling.xlang %s", sourcefile)
        batchcmds:mkdir(path.directory(objectfile))
        batchcmds:vrunv(xl, {"build", path(sourcefile), "--emit=obj", "-o", path(objectfile)})
        -- The object is not a toolchain product, so the linker does not know it: register it on
        -- the target's object list, the way the built-in `utils.bin2obj` rule does.
        table.insert(target:objectfiles(), objectfile)
        -- `xl` emits no depfile, so watch every `.xl` under `src/lang`: the entry imports the
        -- lexer and the preprocessor trees, and a change to any of them must rebuild the object.
        batchcmds:add_depfiles(sourcefile)
        for _, importfile in ipairs(os.files("src/lang/**/*.xl")) do
            batchcmds:add_depfiles(importfile)
        end
        batchcmds:set_depmtime(os.mtime(objectfile))
        batchcmds:set_depcache(target:dependfile(objectfile))
    end)

-- Adds the xlang object to a target. Call inside a `target(...)` block. The object the rule
-- builds lands on the target's standard object path, so the linker picks it up like any other
-- object of the target.
function add_xlang_object()
    add_rules("xlang.object")
    add_files(xlang_entry)
end
