# Native backend handoff

Goal (from the owner): xlc has no LLVM and no C++ code generator. The only code generator is the
native arm64 Mach-O backend written in xlang (`src/codegen/`), macOS on Apple silicon only. No C++
may be written (deleting C++ is allowed, and only after its xlang replacement exists). Use the `xl`
binary (compiler rc.12 in `~/.xlang/xlc/`) to build; the built `xlc` of this tree is not the tool.
Commit at every milestone, no attribution trailers. Chat is Turkish, repo content English.

## Done (committed)

- LLVM removed: `src/codegen/llvm/`, `src/codegen/*.cpp`, `src/backend/codegen.cpp`, the C++
  archiver, LLVM unit tests, LLVM rules in `xmake.lua`. The old generator is readable with
  `git show 7a9813b^:src/codegen/llvm/<file>` (functions.cpp, expressions.cpp, memory.cpp,
  scalar.cpp, unwind.cpp, atomics_asm.cpp, entry.cpp, declarations.cpp, c_abi/*.cpp): it is the
  reference for semantics.
- CLI: `--backend`, `--emit=ir|bc` removed. `src/cli/emit.xl` calls
  `emitNativeObj(&flat.m, hasOnly, only, &output)` (src/codegen/native/index.xl), which calls
  `select(m, &o, hasOnly, only)` (NOT YET REWRITTEN: `src/codegen/isel/index.xl` still has the old
  two-argument naive selector and must be replaced). `src/driver/link.xl` links with `cc` and
  archives with `libtool -static`.
- `arm64/index.xl`, `arm64/float.xl`, `arm64/atomic.xl`: encoders, verified word by word against
  clang by `test/native/run.sh` (add a line to `encode.s` and `encode.xl` for every new encoder).
- `isel/layout.xl`: type classes (`kind`), size/align, struct layout scheme v1, tuple/str/slice.
  Gap: `@packed`, `@align`, shuffle, recorded layouts are not in the flat HIR (record in
  xlang-docs/known-limitations.md, not done yet).
- `isel/regs.xl`: frame layout and local homes (callee-saved registers by use count).
- `isel/gen.xl`: `Gen` state, symbols (`Syms`), relocations, labels, temp register pools, spill
  area, frame bump area, local access, `panicCall`/`coldPanic`, `Place`, `CallHook`.
- `isel/mem.xl`: zero and copy. `isel/emit.xl`: loads/stores/constants. `macho/index.xl`: sections,
  `__DATA` segments, symbol names always get one `_` prefix.
- `isel/expr.xl`: genScalar, genFloat, genPlace, genInto, genCond, slices, indexing (type-checks, never run).
- `hir/abi.xl`, `hir/flatten.xl`: `XlcHirGlobal.declared` added (offset 243, unverified).

## Not done

1. `isel/call.xl`: install `g.callHook`. Calls (direct, virtual: vptr slot `index*8`, itable,
   indirect), `syscall` (x16 number, `svc #0x80`, carry means `-errno`), `new`/`Construct`
   (`alloc` role, nil check), atomics (LSE: `ldaddal`, `swpal`, `casal`, `ldar`, `stlr`, `dmb ish`),
   `asm` (needs a small arm64 assembler for the stdlib subset: mov, ldr, ldp, stp, br, yield...).
   Convention: x0-x7/d0-d7, sret in x8, aggregates by address of a caller copy, stack arguments
   Apple-packed, spill live temps around calls (`spillLive`/`reloadLive`), C ABI (AAPCS64 HFA,
   <=16 bytes in x0/x1, bigger indirect) for `export(c)` and `declare`d functions.
2. `isel/stmt.xl`: Local, Assign (register locals vs places), Return (sret, C results), Expr, If,
   While (continue goes to the step; labeled break/continue via `depth`), Block, Try (snapshot:
   word 0 catch address via `adr`, 1 sp, 2 fp, 3.. x19-x28, d8-d15 from byte 104; call the
   `unwind` role with op 1; see `~/.xlang/std/v1.6.2/runtime/unwind.xl`).
3. `isel/func.xl`: prolog `stp x29,x30,[sp,#-16]!; mov x29,sp; sub sp` (2-word patched pair),
   saves at `sp+0`, params to homes, epilog `restore; mov sp,x29; ldp; ret`, cold panic blocks
   after the body, landing pads and the `xl$unwindPads` / `xl$unwindPadCount` data, spill/temp
   high-water patch.
4. `isel/data.xl`: globals (`__data`/`__const`, constant initializers with pointer relocations),
   vtables (`vtable.<class>`, repr(c) adds 2 words before the slots), itables
   (`itable.<type>|<iface>`), symbol table assembly (defined before undefined), boot: `main` stub,
   `_xl_initAll`/`_xl_finiAll` calling `@init`/`@fini`, `only` module mode.
5. Replace `isel/index.xl` by `select(m, o, hasOnly, only)`; build `xlc` with xmake
   (`XL_XL=~/.xlang/xlc/v1.0.0-rc.12/xlc xmake`); unit/e2e tests; port e2e tests that used `ir`.
6. Docs: `src/README.md`, xlang-docs (cli.md, install.md, known-limitations.md), delete remaining
   C++ only when its xlang replacement exists.

## Traps found

- `local from`, `local fn`, `end` are keywords in xlang.
- No sed/awk/python for editing files (project rule); use Read/Edit/Write.
- The two-argument `select` call in `native/index.xl` fails `xl check` until step 5 is done.
