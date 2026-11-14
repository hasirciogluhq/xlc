-- The xlang lexer as one build unit.
--
-- `src/lang/lexer/*.xl` is compiled by an installed `xl` into one object, and that object is
-- linked into `xlc-core`. `src/lang/lexer/lexer.cpp` bridges the C ABI on the C++ side.
--
-- `XL_XL` selects the `xl` binary (default: `xl` on PATH). A missing or failing `xl` fails the
-- build instead of linking a stale object.
--
-- Loaded from the root `xmake.lua` with `includes("xmake/lexer.lua")`.

-- The lexer entry file: it imports the rest, so one object carries them all.
local xlang_lexer_entry = "src/lang/lexer/lexer.xl"

rule("xlang.lexer")
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
        -- `xl` emits no depfile, so watch every `.xl` next to the entry: the entry imports its
        -- siblings, and a change to any of them must rebuild the object.
        batchcmds:add_depfiles(sourcefile)
        for _, importfile in ipairs(os.files(path.join(path.directory(sourcefile), "*.xl"))) do
            batchcmds:add_depfiles(importfile)
        end
        batchcmds:set_depmtime(os.mtime(objectfile))
        batchcmds:set_depcache(target:dependfile(objectfile))
    end)

-- Adds the xlang lexer to a target. Call inside a `target(...)` block. The object the rule
-- builds lands on the target's standard object path, so the linker picks it up like any other
-- object of the target.
function add_xlang_lexer()
    add_rules("xlang.lexer")
    add_files(xlang_lexer_entry)
end
