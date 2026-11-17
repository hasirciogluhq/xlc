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

Codegen (sub-agents, finished): arm64 macOS/Linux/generic/Windows (COFF, not run yet) and
arm32 (armv7 Linux, A32) done;
gaps (T32, try on arm32, PIC, Linux C stack args) in src/codegen/codegen.md.
Rules: memory.md is the shared memory; port by improving the old C++, never by copying.
