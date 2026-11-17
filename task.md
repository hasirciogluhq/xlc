# Task

Goal: the self-built xlc compiles `hello.xl` with `--import std` and runs it. One module at a
time in dependency order, each finished, tested (test/e2e) and in the xlc binary.

Done:
- Parameter packs (`..args`): src/sema/packs.xl, src/lower/pack.xl, test/e2e/packs.xl.
- Untyped global const typed by context (lowerName).
- Lazy methods of generic class instances, lowered on first call (items.xl lowerLazyMethods,
  builder `called` set); stale `fnDecl` pointer crash fixed (runFunction copies the decl).
  test/e2e/lazy_methods.xl.
- `impl I for T` methods called on the concrete type (`heap.free`): methods.xl findInherent,
  E0403 when two interface impls declare the name. test/e2e/impl_calls.xl.

- Layout queries resolve a type (queries.xl via exprToType + resolve + layoutOf): `sizeOf(T)`
  in instances, imported types, `[]u8`; unknown `offsetOf` field is E0709. test/e2e/layout_queries.xl.

- Overload ranges stay contiguous (sema pushOverload), `p[k]` of `**u8` is `*u8`, a ternary of
  string literals/nil takes a pointer type (pointerLiteral). test/e2e/overloads.xl.
- Generic methods (genericMethodInstance + lowerPendingMethods, env = method's + class
  instance's), function names typed `fn(..) -> R`, calls through fn values typed.
  test/e2e/generic_methods.xl.

- Lowering errors no longer crash the prune (emit.xl lowers and checks errors before the
  prune; lowering is lazy in hirModule). Test still to add: a program with a lowering error
  built as exe prints the diagnostic, exit 1.

Second gate: the self-built xlc builds the `xl` tool (xlang repo, `src/main.xl`, std v1.6.2):
216 errors on 2026-11-17. Groups, with counts and a source sample:
- A. Generic type alias as a return type: `type Done = Result<bool, Problem>`;
  `?` sees an empty return type (E0763 x30), `Result.Ok(true)` E0706 x2.
- B. Namespace import calls: `files.currentDir()` (`import files from ...`), E0706 x76.
- C. Methods of generic enums: `failure.isSome()` on `Option<T>`, E0709 x1 (impl<T> on enums).
- D. Iterator protocol: `for x in Vec<T>` / `*Vec<T>` / `for x, i in`, E0709 x~93.
- E. `switch` on a payload enum value with patterns: `case Result.Ok(v) { }`, E0500 x4.
- F. Closures: `|name: *String| { }`, nested closure args, E0500 x3.
- G. Method overload by argument type: String `equals(*String)` vs `equals(str)`, E0403 x1.
- H. A `str` const where `*u8` is expected: `sysOpenRead(NULL_DEVICE)`, E0708 x2.
- I. Pack args with no type (`println("{}", &absolute)`), E0708 x3: likely follow from A/B
  (the local's type comes from an unlowered call); recheck after them.

Order (each needs the ones above it):
5. A: generic type aliases resolved everywhere a type is read (unlocks `?`, enum ctors).
6. B: namespace import calls and values (largest group, independent).
7. G, H: overload choice for method calls, str const to `*u8` (small, independent).
8. C: generic `impl<T>` blocks on enums/classes and `Option<*T>` returns.
9. D: iterator protocol (`iter()` + `next() -> Option<*T>`), needs C.
10. E: payload-enum `switch` patterns, needs A and C.
11. F: closures (also the 2 E0500 of hello.xl).
12. I: recheck; then rerun both gates (hello.xl, the xl tool).
13. `syscall` on Windows is never rejected (user decision 2026-11-17): the caller owns the
    numbers; only the argument count is checked. Remove the rejection, update docs.
14. Then from memory.md: virtual methods + vtable, interface E0504, cross-module bases,
    `switch` widening, ownership edges, the function.xl split.

Later tracks (user request 2026-11-17; design, not started). Split of duties: xlc only
translates what it is given (files in, files out, no cache directory of its own, no network);
everything about where files live, when to rebuild and what to fetch is `xl`.

L. Own linker (no `cc`, `ld.lld`, `lld-link`, `llvm-dlltool`, `llvm-ar` left).
   - xlc already writes the objects; the link is one more output of the same translation
     (`--emit=exe|shared|static`), so it belongs in xlc: src/link/ next to src/codegen/.
   - Order, simplest first: static archive (`ar` format, the Windows `.lib` variant); ELF static
     executable (Linux arm64/armv7, no dynamic loader); Mach-O executable (dyld load commands,
     chained fixups, `LC_MAIN`, and the ad-hoc code signature arm64 macOS requires before it
     runs a binary); PE/COFF executable with import tables written directly (replaces
     `llvm-dlltool` and import libraries); then shared libraries (ELF `.so`, `.dylib`, `.dll`).
   - Input: our own objects plus prebuilt objects/archives handed on the command line, so the
     reader parses the standard formats, not only what xlc writes.
   - Tests: e2e rows link and run per OS; the linker driver (src/driver/link.xl) is deleted
     at the end.

U. Compiled packages ("units"; the name is open), incremental builds and the cache.
   - A unit is one package compiled once per (compiler version, target, defines, features,
     safety mode, dependency unit hashes). The stdlib is a unit like any other: today every
     build parses and checks all of std again; as a unit it is built once per target and
     reused.
   - One unit file (`.xlu`, a small container with a table of sections), written by
     `xlc --emit=unit` and read with `--unit name=path`:
     - interface: the exported names, types, layouts, enum values, constants, roles,
       `@init` order, so importers type-check without the source;
     - HIR of everything an importer must instantiate or run: generic functions, classes and
       methods (instantiated in the importer, as today), `@inline` bodies, macros, and the
       compile-time functions of track C;
     - native code: one object section for the target with every non-generic function;
     - fingerprints: a hash per item (signature hash and body hash), so a change in a body
       only rebuilds importers when the signature hash changed.
   - Why mixed and not only objects: generic and compile-time code has no machine code until
     the importer's types are known; plain code does, and storing it as an object means the
     importer never compiles it again. Instances made in two units can both exist: the final
     link keeps one by symbol name (COMDAT/weak-style, needs track L).
   - The final step: xlc reads the units, instantiates generics, runs the codegen for the root
     package and links the objects of all units (track L).
   - `xl` side (not in xlc): computes the unit key, keeps the units in its cache
     (`.xlang/cache/units/<key>.xlu`), decides what is stale, builds the dependency order in
     parallel, and passes `--unit` flags; `xl clean` and the uninstall scripts know the
     directory. xlc never looks for a unit by itself.
   - Incremental inside one unit (later): the per-item fingerprints let xlc reuse the native
     code of unchanged functions from the previous unit file handed to it (`--previous`).

C. Compile-time functions and attributes (libraries extend the language without the compiler
   knowing them).
   - A library declares `@attribute fn name(d: *Decl) -> Items` (names open). A user writes
     `@name(args)` on a declaration; the compiler calls the function at compile time and adds
     the items it returns (methods, impls, functions) to the module, then checks them like
     hand-written code. Typical: equality, hashing, formatting, serialization, builders,
     registration tables.
   - Execution: an interpreter over HIR inside xlc, not a native plugin. Portable (the unit
     carries HIR, so it runs on any host for any target), deterministic, and sandboxed: no
     files, network, processes or clock; the only inputs are the declaration and the args.
     A compile-time function may call only other compile-time-able code (no syscall, no
     `declare fn`), checked with a diagnostic.
   - The API it sees: a read-only description of the declaration (name, fields, types,
     attributes, generic parameters) and a builder that returns new items as AST, not text,
     so spans and errors point at the attribute.
   - Stored in the unit as HIR (track U); evaluated during sema of the importer, before
     lowering. Results are cached in the importer's unit by (function body hash, input
     declaration hash).
   - Needs: the HIR interpreter (also gives `const` evaluation of calls), a stable `Decl`
     description in the stdlib (`std/meta`), and track U for sharing across packages.

Order of the tracks: U interface + HIR sections and the stdlib as a unit first (largest build
time gain), then L (needed for the native-code sections and instance dedupe across units), then
C (needs the interpreter and the unit HIR).

Codegen (sub-agents, finished): arm64 macOS/Linux/generic/Windows (COFF, not run yet) and
arm32 (armv7 Linux, A32) done;
gaps (T32, try on arm32, PIC, Linux C stack args) in src/codegen/codegen.md.
Rules: memory.md is the shared memory; port by improving the old C++, never by copying.
