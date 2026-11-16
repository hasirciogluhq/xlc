Lowering port todo (shared by simsek and got)

Goal: `xlc hello.xl --import std -o hello` works with the self-built xlc. The old C++ lowering is in git:
`git show 6ed7fa6^:src/sema/lower/<path>`. Mark an item DOING with your name before you start, DONE when it
builds and the stdlib errors of that kind are gone. Edit this file with small Edit hunks only.

Test: build `cd src && ~/.xlang/xlc/v1.0.0-rc.12/xlc main.xl --emit=exe -M std=../../xl-std/src --import std -DfeatureDefaultAllocator=true -o <dir>/xlc`
then `<dir>/xlc hello.xl -M std=../xl-std/src --import std -DfeatureDefaultAllocator=true -o hello`.

Owners: simsek = src/sema, src/cli*, src/session, src/lower/{state,typing,call,methods,function}.xl.
got = src/codegen, src/driver/link.xl, new files under src/lower.

- DONE simsek: imported names, calls, globals, defines (owner-aware lookups)
- DOING simsek: methods of imported classes (lower/methods.xl, typing.xl, call.xl)
- DONE simsek: methods of imported classes; constructor call `Type(args)` (class construct, E_CONSTRUCT, key `Cls.init`)
- TODO got: interface method calls on a value (E0709 "no field alloc/free": 193), "needs an addressable receiver" (38), "no layout for" (8), "unknown type" (14)
- DOING simsek: coercions: str literal to *u8, nil to pointers, define consts i32/i64, array literal elements; sum names (Result, Option) as values
- TODO simsek: E0708/E0601 conversion parity (expected X found Y, signedness)
- TODO simsek: "has no field" (E0709), "unknown type" (E0706), "no layout for" (E0709)
- TODO simsek: generic struct/class and generic methods, E0404/E0403/E0710 call picks
- TODO simsek: MIR stage run on the lowered stdlib, then link and run hello
- DOING got: `?` on Option and Result (lower/propagate.xl written and hooked; non-generic enums only, generic Option/Result wait on sum.xl)
- DOING got: `for x in e`, `for x, i in s` (lower/iterate.xl: array, slice, str done; the next/iter protocol over a struct or class is open, 11 errors)
- TODO got: closures (expr/closure.cpp)
- TODO got: switch forms not lowered (stmt/switch.cpp)
- TODO got: other "not lowered yet" statement and expression forms (grep -n "not lowered yet" src/lower)
- DOING got: lower/builtins.xl (`__builtin_*`, call/builtins.cpp)
- TODO got: impls, vtables, virtual and interface calls (item/impls.cpp, item/vtable.cpp)
- TODO got: drops and moves (own/drop.cpp, own/moves.cpp)
- TODO both: docs (src/README.md, xlang-docs/compiler/cli.md), commit only after telling each other
