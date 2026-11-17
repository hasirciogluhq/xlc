# xlang language rules — memory

This file is the AI's working memory for this repository. Everything here is read from the
compiler source in `src/` (verified, with the source file named next to each fact), never from
memory alone. When a rule changes the sources change first and this file changes with them.
Spelling of hint text and error codes follows the code so diagnostics quoted here match `xlc`.

Source map (who owns what):
- Lexer: `src/lang/lexer/` (xlang: xlcLex). Parser: `src/lang/parser/parser.xl` (xlang, one file).
- Preprocessor: `src/lang/preprocess/` (`#if`, `#define`). Macros: `src/lang/macro/`.
- Sema: `src/sema/*.xl` (types, resolution, layout, const-eval, enums, instances).
- Lowering: `src/lower/*.xl` (AST -> HIR, conversions, diagnostics, drops, moves, iterators).
- Static analysis (use-after-move): `src/analysis/moves.xl` over the MIR (`src/mir/`).
- HIR flat form: `src/hir/abi.xl` (node kinds, `E_*` constants below). MIR: `src/mir/model.xl`.
- Native codegen: `src/codegen/` (being reorganized 2026-11-17 into target/, obj/, macho/, elf/,
  arm64/encode, arm64/isel, os/, native/; arm64 for macOS, Linux and generic). Its own memory
  file is `src/codegen/codegen.md`; read that for anything codegen.
- Session/config/CLI: `src/session/` (config.xl = targets, `-D`, incremental key), `src/cli.xl` (flags).
- Parameter packs: `src/sema/packs.xl` (instances, use checks) and `src/lower/pack.xl` (lowering).

## The language in one paragraph

xlang is a compiled, memory-safe-by-default systems language with Go-flavored syntax and
Rust-like ownership. It has no garbage collector: values own their memory, moves transfer
ownership (E0780 on use after move), every owned value is dropped by generated glue at the end
of its scope or at a jump out, and `delete p` runs the drop then calls the registered `free`.
Memory is either stack (locals), static (globals) or heap (`new T` through the `alloc` role).
Pointers exist but arithmetic on them is not implicit: an index `p[i]` steps by `sizeof(*p)`.
Structs are plain data (a droppable field is E0721); classes add methods, single inheritance, a
vptr and `drop`; interfaces are two-word fat pointers `{ data, itable }`. The compiler owns
`str`, slices, arrays and pointers; the stdlib builds `String`, `Vec<T>`, `Option`, `Result` on
them. Only a closed, listed set of `__builtin_` names exists.

## Lexical grammar (src/lang/lexer/)

- Identifiers: `[A-Za-z_][A-Za-z0-9_]*`, ASCII only (bytes >= 0x80 are never identifier bytes).
  Comments are `//` to end of line (incl. `///` docs). No `/* */` block comments.
- Keywords (42): fn local return if else while continue break do switch case default defer try
  catch unsafe import from as export declare register global const delete struct class interface
  impl enum type define new virtual override self for except true false.
  `move` is NOT a keyword: it is recognized only directly before `|` (`move |x| ...`), so `a | b`
  and `a || b` stay operators. `Self`-like type names are ordinary identifiers.
- Primitive type tokens: i8 i16 i32 i64 isize u8 u16 u32 u64 usize f32 f64 bool void never str.
- Preprocessor: `#if #elif #else #endif #ifdef #ifndef #error`; must be the first token of its
  line (else error). `#` anywhere else is an error. The `-D`/builtin values (os, arch,
  pointerWidth, endian, debug, optLevel) reach the preprocessor and `define`.
- Numbers (numbers.xl): decimal, `0x`, `0b`, `0o` (`0o644`; a bare leading `0` stays decimal,
  never octal), `_` separators (`1_000`). Floats: `1.5`, `1e9`, `1.5e-3`. Rule: right after a `.`
  a number is a tuple position (`t.0.1`) and takes no fraction/exponent. Literals are u64
  internally; overflow (any hex 64-bit cap, or decimal > 2^64-1) is a lexical error. Float
  overflow is a lexical error.
- Strings (strings.xl): `"text"` with escapes `\n \t \r \0 \\ \" \' \xHH`; may not span a line
  break. At most one error per literal; the token is still emitted.
- Byte literal (literals.xl): `b'x'` — exactly one byte 0..255 (`u8`), escapes as strings; a
  non-ASCII byte must be written `b'\xHH'`.
- Token kinds along with the punctuation: `( ) { } [ ] , ; @ ? ~ : := . .. ..= -> ! + - * / % & |
  ^ << >> = == != < <= > >= && || += -= *= /= %= &= |= ^= <<= >>= ++ --` (tokens.xl lists all
  K_* constants; `++`/`--` are statements only).
- Newlines matter: `newlineBefore` on a token ends an expression chain unless a `(`/`[`/`{` is
  still open; `for i in 0..n` vs C-style `for` is decided by an `Ident` followed by `in` or `,`
  on the next token.
- Spans are 1-based lines and columns counting bytes; every lexer error is E0001.

## Types (src/sema/types.xl, TypeKind order in include/xlang/lang/types.h)

Kinds: void, never, bool, int, float, pointer, fn-ptr, array, enum, struct, class, interface,
str, slice, tuple, named (resolved away). Type grammar (parser.xl @grammar):
`*T`, `[]T`, `[]mut T`, `[T; N]`, `(T,)`, `(A, B)`, `()`, `fn(A) -> R`, `fn(A)`, `Name`,
`a.b.Name`, `Name<T, U>`. `*T` is prefix-only, never a postfix form. `(T)` without a trailing
comma is just `T` parenthesized, not a one-element tuple. `never` is accepted in a type and
rejected with E0765 unless it is a return type.

- Integers i8..i64 (signed) and u8..u64, plus isize/usize whose width comes from the target
  pointer width (`bits == 0`). Floats f32/f64; floats count as signed for the E0601 rules.
  `bool` is 1 byte, `b'x'` is always `u8` (never widened by context).
- `str` is `{ ptr: *u8, len: usize }` (2 words). `[]T`/`[]mut T` are `{ ptr: *T, len: usize }`
  (2 words; `[]mut T` degrades to `[]T` with no code, the two share one layout).
  `interface` is a fat pointer: 2 * pointer width, aligned to the pointer width.
- Enums: `enum E: T { A, B = 3, C }`, `i32` implied. Member auto-numbering: written value, then
  one more than the previous (`next++`), so `A, B = 3, C` gives 0, 3, 4. Underlying type must
  hold a constant: integer, float, bool or `*u8` (E0705 otherwise, with the fix text naming the
  rule). Only integer enums auto-number; a non-integer member without a value is E0705. Duplicate
  member names are E0705. Payload enums (sum types) are written `enum E { Empty, Pair(Res, Res) }`
  and lower to a `{ tag, data }` record where `data` is a union of per-variant payload structs.
- Arrays: `[T; N]` with N a non-negative integer constant (E0707 otherwise, everywhere in a type
  tree, checked by src/sema/checks.xl). An array of a length no expression spells keeps it in
  `bits`. Array `.len` is a compile-time constant. `[v; n]` is an array-repeat literal.
- Tuples: `(A, B)`, one element keeps the comma `(T,)`, `()` is the empty tuple. Elements are
  accessed `.0`, `.1`...; out-of-range is E0709.
- Function pointers: `fn(A, B) -> R`; `fn()` means `fn() -> void`; `-> never` is spelled.
- Type equivalence is by spelling (`same(a,b)` compares `text(a) == text(b)`); mangled link names
  are symbol-safe spellings (`P<T>` for `*T`, `A<N>_<T>`, `F...R...E`, `T...E`, `N<len><name>`;

## Source layout of this repo

- The whole compiler is xlang (`src/`, ~118 .xl files); the shipped binary is one xlang program
  (`src/main.xl`, entry `main`). The C++ sema/lowering was deleted in commit 6ed7fa6 (read it
  with `git show 6ed7fa6^:src/sema/lower/<path>`); no C++ pass runs anywhere. Left over:
  headers in `include/` and C++ harnesses in `test/unit/*.cpp`, which no longer build (xmake was
  removed in 215f114). Diagnostic hints that say "call it through the C++ pass" are stale text.
- `xlc` is a package (`xlang.toml`: name xlc, root src, one stdlib dependency pinned to v1.6.2;
  its source is in `.xlang/cache/github.com/hasirciogluhq/xl-std@v1.6.2/src`).
- The only code generator is the native backend in xlang (no LLVM). It worked for macOS arm64
  Mach-O and Linux/generic arm64 ELF (static, ld.lld, own `_start`; the generic OS, no OS word
  in the triple, is Linux). Layout: src/codegen/target (Target), obj, macho, elf,
  arm64/encode, arm64/isel; details in `src/codegen/codegen.md`. conv.xl is still the Apple
  variant (Linux stack-argument packing differs: gap). arm32 parses (linux only), no backend.
- An untyped global `const n = 1` lowers with the context type, else i32 (lowerName).
- Lazy instance methods (C++ lazy_methods, ported): a non-virtual method of a generic class
  instance (not init/drop) is queued in `Lowerer.lazy` (items.xl lowerMethods) and lowered by
  lowerLazyMethods once `HirBuilder.called` (every E_CALL/E_FUNC_REF name) holds its key; the
  program loop repeats while any is lowered. Fixes `Vec<T>.clone` on a `T` without `clone`.
  Improved over C++: the queue drains in the program loop across modules.
- Arena pointers go stale: `runFunction` copies its `SDecl` (`l.fnDecl` pointed into
  `s.decls`, which a generic instance in the body moved; every method call on a generic class
  instance crashed the compiler in endFunction/findAttr). `packInstanceOf` matches by arena
  address, so runFunction asks it before the copy.
- `x.m()` on a concrete type also finds `impl I for T` methods (methods.xl findInherent; key
  `T.I.m`, `MethodRef.cls` = `T.I`). An inherent `impl T` method wins; the name in two interface
  impls is E0403 (the C++ pass took the first impl silently).
- `sizeOf/alignOf/offsetOf` (queries.xl) resolve the operand to a type (exprToType, resolve
  applies the instance env, layoutOf) instead of matching its text: fixes `sizeOf(T)` in
  `Vec<Route>`, imported names (`PrivKey`), `[]u8` (object.xl) and drops the hardcoded
  primitive table (str was 16 on every target). An unknown `offsetOf` field is E0709 (was 0).
  The default layout scheme reorders fields: tests use `@repr(c)` for fixed offsets.
- Overload lists: `Sema.pushOverload` moves a symbol's range to the end of `overloadRefs` when
  another symbol grew in between (was: appended in place, so `f, g, g, f` gave the second `f`
  `g`'s decl; E0404 on stdlib `jsonValid`/`jsonPointerGet`).
- Index typing: `p[i]` steps into a pointer only to an array/slice/str; `**u8`[k] is `*u8`.
- `pointerLiteral` (typing.xl): `nil`, a string literal or a ternary of them takes a pointer
  `want` (`c ? "a" : "b"` as `*u8`); used by effectiveType and the ternary lowering.
- Generic methods: `genericMethodInstance` (generics.xl) infers/reads the method's type
  arguments and calls `instantiateMethod` (owner module, key `Cls.m<Args>`, env stored in
  `methodInstEnvs`); `lowerPendingMethods` (lower/instances.xl) lowers them with the method env
  plus the class instance's env (C++ used only the method's). `genericInstance` now adopts an
  imported generic's parameter types before `unify` (was reading the owner's indices in the
  caller's arena). A function name has type `fn(P) -> R` (nameType), and `f(x)` through a
  function value has its `ret` type (the HIR call.indirect was untyped).
- `declare fn` lowers with HIR `hasBody=0`, `declared=1` and `importLibrary` from
  `@library("name")` (function.xl endFunction); before, it was emitted as an empty defined fn on
  every target and no @library import reached the linker.
- Windows arm64 (codegen agent): `aarch64-pc-windows-msvc` / `aarch64-windows` -> COFF
  (src/codegen/coff), `mainCRTStartup` -> ExitProcess, lld-link with import libs made by
  llvm-dlltool (no Windows SDK), `syscall` is E0714 there. Built and inspected on macOS, never
  run (needs Windows on Arm). Gaps: no .pdata/.xdata, DLL exports, variadic C calls (see
  codegen.md). xlang classes use one vtable layout on every OS (vptr at 0, slots in declaration
  order); foreign C++ objects are mapped as `@repr(c)` struct + vtbl struct of fn pointers.
- Issue routing (user decision, 2026-11-17): xlang is the general tracker (language, `xl`,
  packages, anything unsure); xlc takes only clear compiler bugs (crash, miscompile,
  diagnostic, target/object/link, xlc flags) and compiler features. SUPPORT.md and
  .github/ISSUE_TEMPLATE (config.yml, bug_report.yml, feature_request.yml) hold it.
- e2e `version` reads XLC_VERSION from src/session/session.xl (xmake.lua is gone). Full e2e on
  macOS: 36 passed, 0 failed (2026-11-17).
- arm32 (codegen agent): A32 encoder/isel, ELF32, armv7-unknown-linux-gnueabihf and
  armv7-unknown run in Docker; layout_queries assumes 64-bit sizes (not in the arm32 run).
- Build: `xl build src/main.xl -o <scratch>/xlc` from the repo root (about 3 s; it runs the
  installed `~/.xlang/xlc/v1.0.0-rc.12/xlc` with the std and package roots). Never call the
  installed xlc directly. `build/` and `builtNewXlc`, `src/main` are old binaries.
- Gate (2026-11-17): the self-built xlc compiles and runs `@entry fn main() -> i32 { return 42 }`;
  `hello.xl` with `--import std` (which loads the WHOLE stdlib) fails in lowering. Error groups
  before the pack work: E0500 packs (12) + E0710 `format` arity (3), for-in over `Vec` (13), no
  layout / unknown type `PrivKey`, `Route` (6), `clone`/`alloc`/`free` "no field" on instances (8),
  E0404 `jsonValid`/`jsonPointerGet` (8), generic method `streamVia` (1), E0708 (2).

## Expressions (@grammar in parser.xl, full list)

Precedence, low to high: `cond ? a : b` (right-assoc, lowest) -> `a || b` -> `a && b` ->
`a == b a != b` -> `a < b a <= b a > b a >= b` -> `a + b a - b a | b a ^ b` ->
`a * b a / b a % b a & b a << b a >> b` -> `e as T` (binds tighter than every binary op, so
`a + b as i64` casts `b`) -> unary `-e !e ~e *e &e &mut e` (repeatable: `--x`, `*&p`) ->
postfix: `f(a, b)`, `a[i]`, `a[lo..hi]`, `a[lo..]`, `e.field`, `e<T>(...)`, `e!`, `e?`,
`name!(args...)` (macro, raw tokens), `in(reg) x` / `out("rax") y` / `inout(reg) z` (asm call
arguments), `..pack` as a call argument. `&e` / `&mut e` (mutability of a borrow is
compile-time checked).

Literals: `123`, `1.5`, `"text"`, `b'x'`, `true`, `false`, `self`, `name`, `name::name`
(`Name::Member`), `name<T, U>`, `(a)`, `(a, b)`, `[a, b]`, `[v; n]`, `new T`, `new(p) T`,
`new T { fields }`, `new T(args)`, `new(p) T { fields }`, `Type { field: value, other }`,
`fn(params) -> R { body }` (function expression), closures `||`, `|a, b|`, `|a: T, b| -> R`,
`|x| expr`, `move |x| { }`, `__builtin_asm(...)`.

## Statements (@grammar, parser.xl)

`label: while ...`, `label: do ... while ...`, `label: for ...`; `switch ...`, `if ...`,
`while ...`, `do ... while ...`, `for ...`; `return e`, `return`, `break [label]`,
`continue [label]`, `defer stmt`; `try { } catch e { }` (needs the `unwind` role, else E0501),
`unsafe { }` (accepts signedness conversions: E0601 only outside), `{ ... }`, `local ...`;
simple statements: assignment, compound assignment, `++`, `--`, expression.

- `local name`, `local name: T`, `local name := expr`, `local name = expr`, `local name: T = expr`.
  Tuple destructuring: `local (a, b) = e` (the tuple is held once in a hidden `$tupleN`).
- `target = v`, `target += v` (and every other compound), `target++`, `target--`, `expr`.
  A compound assignment lowers as `target = target op v` (the target is lowered twice, the C++
  pass clones its AST the same way).
- `for init; cond; step { body }` — every part optional; `local` in the initializer behaves as
  in statement position. Declared when `Ident in`/`Ident ,` is NOT the next shape.
- `for x in e { }`, `for x, i in e { }` — iterable: array, slice or `str` (E0709 otherwise;
  ranges are `for i in a..b`). The element is `*T` into the storage (no copy; a write reaches
  the owner) except `str`, which yields `u8` by value. The index is `usize`. The bound is read
  once; an array is indexed through its address, never copied. A `for .. in` over a struct or
  class (the `next`/`iter` protocol) is not lowered in this pass.
- `for i in lo..hi`, `for i in lo..=hi`. `..pack` spreads.
- `switch v { case a, b { } default { } }` on integers, bools, enums and sum types;
  `switch { case cond { } }` (subject-less) chains its cases as if/else-if. `case` takes a
  list; `default` takes none and at most once. Duplicate `case` value = E0709, second `default`
  = E0709, `case` with no value = E0709. A `switch` on a sum type binds the payload per arm
  (`case Two.Pair(a, b) { }`); `_` still holds its droppable value.
- `defer stmt` — runs at block end and at every jump out (`return`, `break` etc.), innermost
  first. Droppable locals are dropped through the defer frames and the function's unwind list, so
  every exit path runs them.
- `try { } catch e { }` — catches the unwinder's value; requires a registered `unwind` role.
- `unsafe { }` — allows signedness conversions that E0601 would flag; inside it, int-to-int
  signedness to the same or wider target converts (still an error when narrowing; that needs
  `as` even inside `unsafe`).

## Declarations and items (@grammar, parser.xl)

Top level: `fn name<T>(params) -> R { body }`, operator names `fn +(...)`, `fn name: sig`,
`global name: T = c`, `const name: T = c`, `struct`, `class`, `interface`, `enum`, `impl`,
`type Name = T`, `define`, `import`, `export name from "m"`, `export *`, a module-level
`name!(...)` call, `@attribute` before any of them, and the modifiers `export` / `export(c)` /
`@library` before the keyword.

Functions: `(a: T, b: U) -> R { body }`, `(self)`, `(self: *T)`, trailing `,` allowed. Operator
names as functions: `+ - * / % & | ^ << >> == != < <= > >= ! ~ ++ -- [ ] [ ]= ( )`. Methods:
`fn self.name(...)` inside a class. `fn name = fnType` declares a function value. A parameter
pack is `..param` and must be the last parameter of a free function only (E0734 one pack, last;
E0736 packs are free functions only, so no `declare`, `export(c)`, `virtual`, `override` or type
params).

- `global name: T = expr` — a global (static) variable; its initializer must be a compile-time
  constant (E0707 at the first node that is not one; no calls, no reads of other globals).
- `const name: T = expr` — a compile-time constant; E0715 for a non-constant initializer; an
  untyped `const` needs a literal value (E0715). A `const` in a body becomes a hidden global
  `fn$NAME$N` bound in the scope; a second definition of one name in one scope is E0401.
- `struct Name { fields }`, `struct Name<T, U> { fields }`, `@repr(c) struct ...`. Fields are
  `field: T` separated by `,` or a line break. A struct holds plain data only: a field whose
  type needs drop is E0721 (structs cannot own; hold a class by pointer instead).
- `class Name [: Base] { members }`, `class Name<T> [: Base] { members }`; single inheritance
  (base fields come first in the layout), `[virtual|override] fn name(...)`, `fn [](...)` etc.
  A class with any virtual method carries a vptr. `drop` is the destructor method. `T(args)`
  constructs through the `init` method (`E_CONSTRUCT`, key `Cls.init`); `new T(args)` allocates
  then runs `init`. A base class's `init` is called by every constructor.
- `interface Name { fn name(params) -> R }`, `interface Name : Base { ... }` — a fat pointer
  `{ data, itable }`; impl tables are built by `impl Iface for T { ... }`.
- `impl T { fn +(...) ... }`, `impl<T> Box<T> { ... }` — inherent impls add methods.
  `impl Iface for T` — an interface impl. The two are called through `methodsNamed`/vtables.
- `enum E: T { A, B = 3, C }`, `enum E { A, B }` (i32 implied); payload variants allowed.
- `type Name = T`, `type Name<T, U> = T` — aliases; cyclic aliases resolve through their name.
- `define name: T = value`, `define name = value` (compiler's `-D` constants), and hygienic
  macros `define name(a: expr, b: stmt) { body }` used as `name!(...)`. A `-D` value wins over
  a `define` (compile-time switches reach code as `define featureX = true`).
- Roles: `register(role) fn ...` makes the function global and exported (one per role per
  group; group 1 stdlib default, group 2 user registration replaces it). Roles and signatures:
  panic `fn(msg: *u8) -> never`, alloc `fn(size: usize, align: usize) -> *void`,
  free `fn(p: *void)` (no return), unwind `fn(op: i32, snapshot: *usize, message: *u8)`,
  args `fn(argc: *i32) -> **u8`. A registration that does not fit is E0503 (text says what the
  role needs); a second registration of a role in one module is E0502 and does not replace the
  first. Every `register(panic|alloc|free|unwind)` in the library group is a default the program
  replaces.
- Hooks: `@init fn` (boot, stdlib group first, then user; import before importer), `@fini fn`,
  `@entry fn` — module-level, not called by code, kept by reachability for that reason.
- `export(c)` / `declare` functions and globals: every signature type must be C-expressible
  (E0302), and the symbol must not be `_start`, `mainCRTStartup`, `_xl_initAll` (E0726).
  `@library("name") declare fn ...` links an OS library; the library group's `@library`
  imports reach the link.
- A `switch` subject can be missing (`switch { case cond ... }`); a `staticAssert(cond, msg?)`
  is a top-level call checked at compile time (E0702).
- Reserved entry symbols `_start`, `mainCRTStartup`, `_xl_initAll` are compiler-owned; user
  symbols with those names are E0726. `@init` runs from `_xl_initAll`, the only caller.

## Modules and imports

`import "x"` resolves `x.xl` or `x/index.xl` next to the importing file. Forms: `import "m"`,
`import "m" as alias`, `import name from "m"`, `import * from "m"`, `import * as ns from "m"`,
`from "m" import name, other as alias`. Re-exports: `export * from "m"`, `export * as ns from "m"`,
`export * from "m" except a, b`, `export { a, b as c } from "m"`.

- `-M NAME=DIR` (`--module-root`) makes `import "NAME"` resolve inside `DIR`; `"NAME/x"`
  names a submodule. Modules found this way form the library group: their role registrations are
  defaults the program replaces, their `@init` runs first.
- `--import SPEC` loads a module although nothing imports it, so its registrations and `@init`
  take part.
- Two exports of one name are an error; the same entity via two paths is warning W0401.
- Packages are git repos of `.xl` source with `xlang.toml` (name, root, dependencies); builds
  never use the network (xlang.lock, xlang.local.toml, .xlang/cache).

## Attributes

`@name`, `@name(a, key = v, ...)`, `@file(name(...))` at the top of a file; one or more
`@attribute` lines before an item, statement or parameter. Attribute argument values: an
identifier, an integer literal or a string. Known ones seen in the compiler: `@repr(c)` (C
layout, declaration order), `@packed` (every field aligned to 1; `@layout(packed)` sets
`packed`), `@align(N)` (power of two, raises the alignment; else E0705), `@layout(scheme = v1)`
(the one scheme; `@layout(shuffle)` + `@layout(seed=...)` randomizes the field order under a
seed — `repr=xl` in `xl.layout`), `@mangle` (E0705 on a function in this pass), `@repr(c)` on
`struct`/`class`, `@library`, `@entry`, `@init`, `@fini`, `@grammar`/`@invariant`/... are
compiler-source doc tags only (COMPILER-TAGS.md), not attributes of the language.

## Type checking and conversions (src/sema/conversion.xl, src/lower/diagnose.xl)

Implicit conversion statuses: C_OK (silent), C_WARN (W0601), C_SIGNEDNESS (E0601),
C_NEEDS_CAST (E0708 "expected X, found Y"), C_INVALID (E0708 "no conversion exists").

- int -> int: same signedness, widening ok (C_OK); narrowing needs `as` (E0708); signedness
  change to same or wider target is E0601 unless inside `unsafe { }` (narrowing across
  signedness needs `as` even inside `unsafe`).
- signed int -> float: allowed with W0601 "precision may be lost". Unsigned -> float: E0601
  (floats count as signed), accepted in `unsafe` with W0601 still issued.
- float -> float: widen ok, narrow needs `as`. float -> int: `as` (E0708).
- bool/pointers/enums/aggregates: only `as` can convert (when `castable` at all); enum converts
  as its underlying type; `*u8` and `*i8` take string literals (the address of their bytes);
  `nil` is any pointer and, untyped, is `*void` nil. `nil` with no binding = `*void` null.
- `[]mut T` -> `[]T` degrades with no code (one layout).
- An interface value accepts a value that implements it (`toInterface`: `{ data, itable }`); an
  interface converts to an interface unchanged.
- Binary ops (`commonBinaryType`): result type = the wider operand (int with int, float with
  float); mixed signedness is E0601 even inside `unsafe` (one side needs `as`); integer with
  float converts the integer to the float type (W0601 when the integer is signed; unsigned is
  E0601). Shifts keep the left operand's type and convert the count to it. Comparisons and
  `&&`/`||` are bool. A literal operand takes the type of the other operand.
- `as` (`checkCast`): int/float/bool scalars to scalars, pointer/fn-ptr to one of those, and
  integer <-> address. Anything else is E0709 "cannot cast X to Y". The `castable` rule calls an
  enum's underlying type first.
- Negative literal of a narrower type keeps only its low bits of that width: `-1 as u8` is 255,
  never a sign-extended 64-bit value (helpers.xl invariant).
- Unsuffixed integer literal: `i32` unless the context asks; widens to `i64` when the value does
  not fit in 32 bits. Unsuffixed float literal: `f64` unless the context asks for `f32`; so
  `local x: f32 = 1.5` narrows at the literal, not at a cast. `b'x'` is always `u8`.
- Define constants: numeric `-D` values are typed like literals (they take the type the context
  expects); a `-D` int in a float position becomes a float constant.
- `--safety=checked` (default): bounds checks on array/slice/`str`/`[v;n]` indexing and repeat;
  `--safety=unchecked` emits the access with no branch. A pointer index is never checked.

## Literals and value rules

- A string literal is a `str`; a context naming `[u8; N]` fills the array byte by byte (a
  literal longer than N is E0717).
- Struct literals: `Type { field: value, other }` — unknown field, duplicate field (E0709
  "initialized twice"), bad type are all checked; the type must be a struct or class (E0709
  otherwise).
- `new T` calls the registered `alloc`; `new(p) T` places into `p` and calls no allocator (only
  the first form's memory is owned by the pointer).
- `delete p` drops the object through its class drop glue first, then calls the registered
  `free`; with no `free` role registered it is E0501 (never a silent leak).

## Ownership, moves and drops (src/lower/own.xl, src/analysis/moves.xl)

The model: every value is owned by exactly one place. Passing a droppable value by value, storing
it into a tuple/struct field, returning it, or assigning it moves it: the source local's hidden
`$liveN` flag is cleared by the HIR `Move` node, so its drop does not run on it later. The dataflow
(over the MIR, one function per pass) tracks each local as owned / moved / maybe-moved; only the
user's own locals and parameters are reported (a finding on a hidden compiler local would be a
compiler bug).

- Use after move = E0780, at the first read after a definite move on every reaching path: "use of
  a moved value `x`" with the fix "pass `&x` to borrow it, move a copy (`x.clone()`), or assign
  `x` a new value before this use". Every read counts: a copy, a move, `&x`, a field read, a
  write into a part of it; an assignment to the whole local owns it again; a `Drop` ends it
  without reading it.
- A field, element or `*p` read by value cannot be moved out: E0720 "cannot move out of a field,
  element or pointer", fix "take its address (`&x.f`) or copy it through a `clone()` method".
  The one exception is the payload of a `case` binding (the owner is consumed as a whole).
- Droppable = needsDrop (memoized, `false` written before recursion so a self-referencing type
  through a pointer terminates): a class with a `drop` method, a class field or base that needs
  drop, a payload enum with such a payload, a tuple with such an element. `void`/ints/floats/
  bools/pointers/arrays of non-droppables/fn-ptrs never need drop.
- Drop glue is made once per type for the whole program (HirBuilder.glues): `Cls$dropGlue(p)`
  runs the user's `drop`, then the droppable fields newest first, then the same up the base chain
  (the layout puts the base's fields first, so pos counts from the root); `E$dropGlue` drops only
  the live variant's payload;
  `$tuple<M>$dropGlue` drops the droppable elements last first.
- A droppable local carries `$liveN` (declared false at the head); its drop is
  `if $liveN { glue(&x); $liveN = false }` registered in the innermost defer frame AND the
  function's unwind list, so a `return`, a `break` or an unwind runs it. A move out is the HIR
  `Move` node which clears the flag where the value is read.
- `target = value` on a droppable type (dropAssign): the new value is computed into `$asgN`
  first (it may read the old one), the old value is dropped (through its flag when the target is
  a tracked local), then stored, and the flag is set again.
- A temporary of a droppable type whose address is taken (a method receiver) is materialized
  into a hidden `$tmpN` with its own flag, dropped when the block ends.
- Polymorphic drops: dropping through a base pointer uses vtable slot 0, which every class of the
  hierarchy fills with its own glue (a virtual destructor). A `@repr(c)` root mirrors a C++
  vtable: no drop slot, the drop is static. `delete p` / `__builtin_drop(p)` through a base
  without a vtable that skips a derived `drop` is E0766; on the exact type it is fine.
- `register(free)` absence makes `delete` E0501 (the hint says the role); `delete` is the only
  way to combine drop + free.
- Out-of-scope alias of this: `let`-like names are `local`; nothing here binds by reference.

## Layout (src/sema/layout_calc.xl, src/codegen/arm64/isel/layout.xl)

- Primitives: bool 1 byte; int N/8; float N/8; pointer and fn-ptr = pointerBits/8 (8 on both
  current targets); interface = 2 * pointer width, aligned to pointer width.
- `str`, `[]T`, `[]mut T`: 2 words, align = word, `{ ptr: word, len: word }` (ptrs at 0, len at
  word; `[]T`'s `ptr` carries the element type so `p[i]` steps by sizeof(T)).
- Tuples: C order, elements in declaration order (no scheme reordering, so `(A,B)` and `(B,A)`
  differ and `.0` always names element 0); `()` size 0.
- Arrays: size = elem size * N, align = elem align.
- Struct/class default scheme v1: fields sorted by alignment, LARGEST first, stable; the size is
  the end of the last field rounded up to the alignment; the aggregate's align is the max field
  align, raised to `@align(N)` (N must be a power of two, else E0705). `@repr(c)` keeps
  declaration order; `@packed` aligns every field to 1 (offsets/sizes computed with that); a
  union puts every field at offset 0.
- Class: a class with a virtual method or one whose base has it starts with the vptr (8 bytes;
  if the base is polymorphic the derived members may reuse its tail padding, Itanium dsize
  rule). The base comes first, its fields inherited (field order in HIR field expressions is the
  index in the sorted order, so `fieldOff` uses that order). An empty `@repr(c)` class with no
  vptr is one byte (C++ empty-class rule); otherwise an empty class is alignUp(0, align).
- A recursive type held by value (a struct containing itself) is E0703 "`X` contains itself by
  value" (fix: use a pointer). `void`/`never` have no size (E0703 "`void` has no size"). An
  opaque `declare` type has no known size (E0703) and can only be used behind a pointer.
- `xl.layout` (LayoutHistory): one record per type
  `type Name size=N align=N scheme=v1 repr=xl|vptr=..|union=..|shuffled=..|seed=..`, then
  `field f offset=N size=N align=N`. `--layout-history=` reads/writes this; missing file = empty
  history. `--dump-layout[=Type]` prints layouts.
- The native backend computes the layout from the flat HIR spellings with the same v1 scheme;
  `@packed`, `@align(N)`, `@layout(shuffle)`, `@layout(seed)` and recorded layouts of
  `declare`d types are NOT in the flat HIR, so codegen lays such an aggregate out as plain v1.

## Indexing, slicing, pointers, arithmetic (src/codegen/arm64/isel/expr.xl, diagnose.xl)

- Only `str`, slices, arrays and pointers can be indexed (E0709 "`X` cannot be indexed"; index of
  a struct/class is `operator[]`, not checked there). An index must be an integer (E0708).
  `*void` cannot be indexed (E0709, fix "cast it to a typed pointer first").
- Slice only `str`, arrays, slices: `a[lo..hi]`, `a[lo..]` (E0709 "cannot be sliced").
- `*p`: operand must be a pointer, not `*void` (E0709; a struct/class operand is `operator*`).
- Field access: `.ptr`/`.len` on `str`/slice; `.len` on an array (a compile-time constant; any
  other name on an array is E0709); tuple elements `.0 .1 ...` (out of range E0709); struct and
  class fields (unknown field E0709); a field of a pointer is read through it. A class with a
  base is not field-checked here (a base may supply it).
- `p[i]` lowers to `p + i * sizeof(*p)` in the selector: a constant index into an array folds
  into the offset; views take `{ptr,len}` out; the bounds check (under checked) compares the
  index against `len`/the array length (`--safety=checked` default: "str/slice/array index out
  of bounds" cold panic). The step is a `log2` shift when the element size is a power of two,
  else a multiply.
- Pointer arithmetic is not implicit: the binary-operator type path converts only numeric
  operands to a common type; a pointer and an integer never share a type silently. Pointer
  movement is written as indexing (`p[i]`), as explicit `as` casts (integer <-> address are
  `castable`), or with `__builtin_asm`. Identical-type operands pass through without a
  common-type conversion.
- Integer overflow is wrapping (the selector works on raw 64-bit registers and normalizes
  width); `bool` is canonical 0/1; a shift count >= the width gives 0 (sign-fill for signed
  `>>`); literal shifts fold (sign-extend for signed). Division/modulo by zero panics under
  `--safety=checked`.
- `-e` on a type narrower than 64 bits keeps only the low bits of that width (negation happens
  in the value's width, so `-1 as u8` is 255, never a sign-extended value); `!e`/`~e` both
  lower to bitwise NOT, and the normalization on a `bool` keeps truth (logical NOT).

## Calling conventions (src/codegen/arm64/isel/conv.xl; target = arm64 Mach-O)

xlang convention (every function that is not `export(c)` or `declare`d):
- integers, bools, pointers, enums: x0..x7 then the stack;
- floats: d0..d7 then the stack;
- aggregates (`str`, slices, arrays, tuples, structs): the ADDRESS of a copy the caller made
  (one x register);
- result: x0 or d0; an aggregate result is written through the address in x8 (sret);
- stack arguments: one 8-byte slot each, in order, from the callee's `[x29+16]`.

C convention (AAPCS64 with Apple changes; `export(c)` and `declare`d):
- an aggregate of at most 16 bytes travels in one or two x registers; a homogeneous float
  aggregate (HFA, up to 4 members of one float type, no vptr) in consecutive d registers; larger
  aggregates as the address of a copy. The result follows the same classes: 0 = x0/x1 (an
  aggregate <= 16 bytes), 1 = d0.. (HFA), 2 = through x8 (sret).
- A narrow integer argument is extended by the caller to 32 bits only, so a C callee normalizes
  its parameters again in the prolog (Apple ABI).
- Not done (a `fail`, i.e. an internal panic): a C-convention aggregate argument that no longer
  fits the registers, and the Apple packing of stack arguments smaller than 8 bytes.
- `syscall` (macOS arm64): number in x16, args in x0.., `svc #0x80`; the kernel reports an error
  with carry set and positive errno in x0, which becomes the `-errno` the language returns. The
  xlang convention limit is six argument registers (x0-x7 minus the number); arm64 and x86-64
  Linux/macOS share that (the number takes the first register); Windows NT has no `syscall`
  instruction (the compiler spills to the stack, no argument limit, number resolved by the
  stdlib).
- Atomics are the ARMv8.1 LSE instructions (ldar/stlr/swpal/ldaddal/casal/dmb); every one is
  sequentially consistent, a weaker MemoryOrder is not distinguished.

## Roles and entry (src/lower/roles.xl, src/cli/entry.xl)

- `register(panic|alloc|free|unwind|args)` — one symbol per role per group, checked
  structurally (E0503; signatures in the Declarations section). Group 1 is the stdlib default; a
  group 2 (user) registration replaces it per role.
- `@entry fn main(argc: i32, argv: **u8) -> i32` (the `@entry` signature must be usable on the
  target). Entry rules per output (`checkEntry`): an exe needs `@entry` (E0739); a static
  library must not have one (E0754, use `@init`/`@fini`); a shared library or a Windows program
  without `register(args)` may not take parameters (E0755).
- `@init` runs before user code, imports before importers; `_xl_initAll` is compiler-owned and
  the only caller. `@fini` runs at teardown.

## Builtins (src/lower/builtins.xl; closed list, E0706 for anything else)

- `syscall(nr, args...)` -> i64; negative result is `-errno`.
- `__builtin_atomicLoad(p, order)`, `atomicStore(p, v, order)`, `atomicXchg(p, v, order)`,
  `atomicCas(p, expected, new, order)`, `atomicAdd(p, v, order)`, `__builtin_fence(order)`:
  pointee must be an integer, `bool` or pointer (E0708); `atomicAdd` needs an integer; loads may
  not be `release`/`acqRel`, stores may not be `acquire`/`acqRel`, a fence cannot be `relaxed`
  (E0708). MemoryOrder members are `MemoryOrder.seqCst` etc. — compile-time enum constants.
- `__builtin_read(p)` -> the pointee's type, copies out, drops nothing (two reads of one pointer
  are two owners of the same bytes; the caller's discipline keeps it sound).
`__builtin_write(p, v)` is a statement, never a value (E0709 otherwise), moves the value through
  the pointer, runs no destructor on the old bytes. `__builtin_drop(p)` runs the pointee's drop
  glue (nothing when it needs none). `__builtin_needsDrop<T>()` -> bool (compile-time; `if` on
  it lowers only the branch that runs).
- `__builtin_unwindPads()` -> `*usize`, `__builtin_unwindPadCount()` -> `usize` (the unwind
  pads, `E_UNWIND_PADS`/`E_UNWIND_PAD_COUNT`; the unwinder writes its snapshot there).
- `__builtin_asm("template", in(reg) x, out("rax") y, inout(reg) z, clobber("memory"),
  options(nomem), options(noreturn))` — `{k}` placeholders numbered by operand order (asm.xl /
  genAsm); operands are scalars (E0708), registers are `reg` or a register name string
  (E0709), options only `nomem`/`noreturn` (E0709).
- `sizeOf(T)`, `alignOf(T)`, `offsetOf(T, field)` are the only compile-time constant calls;
  any other call in a constant position is E0707 (a global initializer may not call).
- `__builtin_write` is handled at statement position; `__builtin_read` needs a typed pointer.

## Diagnostics index (codes seen in this tree)

E0001 lexical error. E0302 `export(c)`/`declare` uses a type C cannot express. E0401 defined
  twice (overload key collision / two `const`s in one scope). E0403/E0404/E0405 overload
  resolution / one-slot rules (generic method cannot be overloaded or virtual). E0501 `try`
  needs the `unwind` role; `delete` needs `free`. E0502 role registered twice. E0503 role
  signature does not fit. E0601 signedness / unsigned-to-float implicit conversion. E0702
  `staticAssert` misuse. E0703 sizeless (`void`), unknown-size opaque, self-containing type.
E0706 unknown type / unknown name / `nil` context. E0707 non-constant array length or global
  initializer (also the E0707 of `sizeOf`-in-constant-position). E0708 expected-vs-found,
  index-is-not-integer, needs `as`. E0709 cast/index/slice/deref/field/struct-literal/jump
  errors (the big one; hint text varies). E0710 wrong number of arguments (builtins). E0715
  `const` not constant / untyped `const` needs a literal. E0720 move out of field/element/
  pointer. E0721 droppable field in a struct (fix: pointer) / droppable tuple in a plain array
  or struct. E0726 reserved symbol. E0730 generic method cannot be virtual.
E0734/E0736 parameter-packs rules. E0739 no `@entry` for an exe. E0754 `@entry` in a static
  library. E0755 `@entry` with args on a shared lib / Windows w/o `register(args)`. E0761 `?`
  inside a `defer`. E0762 `?` on a non-Result/Option. E0763 `?` returns a failure the function
  cannot return (same carried type required). E0765 `return` in a `-> never`; `never` in a non-
  return type. E0766 static drop that skips a derived drop. E0780 use of a moved value. W0401
  same entity via two paths. W0601 precision may be lost (signed int -> float; write `as`).
  `--explain E0501` prints code, fix and example.

## CLI (src/cli.xl)

`xlc [options] main.xl [prebuilt.o ...] [-o out] [--emit=exe|obj|header|static|shared]`.
The first `.xl` is the program root; every other input is a prebuilt object/library for the
linker. Flags: `--check` (type-check only), `--dump-ast`, `--dump-hir`, `--dump-mir`,
`--dump-layout[=Type]`, `--print-cfg`, `--incremental`, `--crt`, `--link-arg=`, `--cache-dir=`,
`-M NAME=DIR` / `--module-root`, `--import=`, `-D NAME=VALUE` / `--define`, `-U` / `--undef`,
`--target=`, `--cpu=`, `--mangle=`, `--safety=checked|unchecked` (default checked), `--layout-
scheme=`, `--layout-history=`, `--layout-record`, `--allow=`, `--deny=`, `--warn-all`,
`--allow-all`, `--context=`, `--context-errors=`, `--context-warnings=`, `--diag-log=`,
`--diag-format=`, `--diag-log-level=`, `--color=`, `--explain=`, `--output=`/`-o`, `--help`,
`--version`. There is no `--backend` flag.
Valid `--emit`: exe|obj|header|static|shared. An exe needs `@entry`; `header` writes the C
header the xlang side generates (header.xl); `--crt` links the CRT and registers `--crt` boot
hooks (main.xl comment). The incremental cache key is `xlc-VERSION|target|os|arch|O|debug|cpu|
|mangle|safety|layoutScheme|emit|D:name=value...` (session/config.xl).

## Parameter packs (src/sema/packs.xl, src/lower/pack.xl; done 2026-11-17)

- `fn f(a: T, ..args)`: a pack function is a template, never lowered itself (items.xl skips it,
  `functionDecls` hides it). A call that no fixed-parameter overload takes picks among the pack
  overloads (fixed params + the trailing argument types, `pickOverload`) and instantiates only
  the chosen one: `instantiatePack` makes the decl with the pack replaced by params `args#0..`
  typed with the argument types (`preType`: int literal i32, float f64, string str), named
  `f<..i32, str>` (`#<declIdx>` appended when two pack overloads collide), queued like a generic
  instance. The body is shared, not copied.
- Inside an instance (`l.packName`, `l.packLocals`, `l.packTypes`, set in `runFunction`):
  `args.len` = element count, an untyped int literal (i32 default, takes the context type);
  `f(x, ..args)` = a synthesized copy of the call (Sema `synthExpr`/`synthExprRefs`, cached per
  function in `spreadFrom/spreadTo`) with `args#i` names; `for a in args` / `for a, i in args` =
  unrolled `do { do { body } while false ... } while false`, `a` bound to the element param itself
  (a move of `a` moves the param). `jumpDepth(l, label, isBreak)`: frames `$item:<label>` take
  `continue`, `break` passes them.
- Sema use checks once per declaration (`checkPackUses`): E0735 value use or member other than
  `len`, E0737 stray `..x`, E0738 index; E0734 one pack, last; E0736 no body / declare /
  export(c) / virtual / override / type params / methods.
- Fixed versus the C++ variadic.cpp: index form `for a, i in args` supported; only the chosen
  pack overload is instantiated (C++ made all); a fixed overload with the right count that does
  not fit no longer blocks the pack (C++ skipped the pack whenever any fixed overload had the
  call's argument count); `break` in a `switch` inside a pack loop leaves the loop; a pack
  argument of unknown type gets its own E0708 (a bare `nil` is `*void`, so `f(nil)` is
  `f<..*void>`); `declare`/`export(c)` pack functions are E0736 again.
- Verified 2026-11-17 (scratch tests, not yet in test/): sum/count/forward/index form/break/
  continue/fixed-overload-wins/fixed-mismatch-falls-to-pack all run correctly; E0735 (value and
  member), E0737, E0738 (also in an uncalled function), E0736 (export(c)) each reported once.
- A synthesized `*XExpr` is invalid after the next synthExpr (Vec may move): copy the record by
  value (`local held: XExpr = *l.s.exprAt(idx)`), as lowerCall and exprType do.

## Lowering/sema gaps (verified in source 2026-11-16, packs closed 2026-11-17)

The C++ pass is gone (6ed7fa6): the xlang lowering is the only one. Each file's header names its
gap; these were re-verified against the code. docs-notes/lower-port.md and lowering-todo.md
are stale (they still describe the C++ pass as production).

- **Generic payload enums** `Option<T>`/`Result<T,E>`: not ported. sum.xl:8-9; the instance choice
  (`sumFor`) is only partly wired through `l.sumWant` (sum.xl:39-45, state.xl:150, function.xl:591).
  propagate.xl:16-18: `?` in a `defer` (E0761), `$ok`/`$try` drop+move, `&&`/`||` rhs, `?:` hoist
  not ported.
- **Iterator protocol** `for x in e` over struct/class (`next`/`iter`, E0775..E0777): not ported
  (iterate.xl:17-18). Only array/slice/`str` lowered (iterate.xl:28-39).
- **Calls** (call.xl): virtual method calls E0709 (call.xl:83-87); generic methods E0709
  (call.xl:184-189); interface method calls lowered but the E0504 "impl exists" check missing
  (interface.xl:9-10); generic interface not supported (interface.xl:10).
- **Generic impls** + per-instance copies (E0732 `instantiateImpls`), interface-method signature
  compare (E0713, only missing/extra methods reported) and duplicate impl check (E0731): not ported
  (impls.xl:10-12).
- **Cross-module base classes**: methods.xl:5 searches only the module's own declarations; a base
  class of another module is a known gap. Inherited field access is not E0709-checked here (a base
  may supply it).
- **Overload** (overload.xl): byte-array param does not fit a string literal (length unknown,
  overload.xl:53-54); interface / class-to-interface conversions not decided (overload.xl:70).
- **E0500 "not lowered yet"**: function.xl statement form, expression form (closures
  `XE_CLOSURE`, `operator()`; packs are done), switch (switch: only int/bool/enum; sum
  handled separately at 1064). `switch` on float/string/pointer not lowered.
- **Closures**: not ported at all (no lower file; `XE_CLOSURE` imported in function.xl:9 but no
  handler -> E0500 on the expr path).
- **Generic methods of instances** (`pendingMethods`) and generic impls: not lowered
  (instances.xl:7).
- **Import/role binding across modules**: state.xl:1-4 — compilation-aware paths (driver import
  bindings, registered names) "filled in when the compilation driver lands". Imported overloads
  reach the scope only through a single-entity `entryOf` fallback (state.xl:369-375), so the
  `overloads` list of an imported name is not filled. E0502 role-twice is per module only
  (roles.xl:1-5, "across modules the registrations meet in the session of the driver"). W0401 not
  reported here.
- **Ownership edges**: E0721 global-needs-drop and E0758 not fully ported; own.xl makes drop glue
  (Cls/E/tuple) but `?`'s `$ok`/`$try` move (propagate.xl:16) is missing. Use-after-move analysis
  lives in `src/analysis/moves.xl` but checkers rc.11/rc.12 do not report E0780 (manual read only).
- **Codegen input**: the flat HIR spells types as text; `@packed`, `@align(N)`,
  `@layout(shuffle)` are not in the flat HIR, so codegen lays such an aggregate out as plain v1.
- **Sema walker gap**: `checkStatements` (checks.xl) visits only `target`, `expr`, `body`,
  `elseBody` of a statement: the `init`/`step` of a C-style `for`, `defer` statements and
  `switch` case values/bodies are not type-checked there (the pack use walker in packs.xl visits
  all slots and can serve as the model).
- **Debt**: function.xl is ~1290 lines (over the size rule; callbacks/cycle-break proposed but not
  done); docs-notes/lowering-todo.md lists some items (impls, interface calls, `?`, drops) that
  later commits (`f8d5834`, `fcce8b5`, `d8733bb`, `ecbaeaa`, `656e874`) already did in part, so the
  todo list is stale relative to the code.

Suggested work order (dependency-first, by hello-gate errors): the iterator protocol over `Vec`
(needs generic `Option<T>` + generic method calls on instances), generic methods (`streamVia`,
`clone` on instances), then the remaining E0404/E0708/no-layout errors, then virtual methods +
vtable, interface E0504, cross-module bases, generic impls (E0732/E0713/E0731), `switch`
widening, closures, ownership edges, cross-module role/import checks, the function.xl split.

## Working here

- Repo rules: English code/comments/docs; user chat Turkish; never `../` (clone xlang-docs into
  the scratchpad if needed; the stdlib is in `.xlang/cache`); edits only with Read/Edit/Write
  (scratch files too); docs first for OS calls; run the tests of what changed, full suites once
  at the end; no commit unless asked.
- Porting rule (user, 2026-11-17): never copy the old C++ as is; review it for bugs, gaps and
  future problems, write the better version, fix the old bugs, and record the fixes here.
- Tests: the C++ harnesses in test/unit no longer build. New tests are xlang programs (CLAUDE.md
  rule); e2e lives in test/e2e (run.sh) and test/native.
- Two agents may work in the tree at once (lead: src/lower, src/sema; codegen agent:
  src/codegen, src/driver/link.xl, target parts of session/config.xl and cli). A build error in
  the other side's file is its work in progress: tell it, do not fix it.
- docs-notes/ (lower-port.md, lowering-todo.md, codegen-port.md) are stale history.
- e2e baseline (HEAD 656e874, 2026-11-17): `bash test/e2e/run.sh <xlc>` passes exit,
  links-only-system-libraries (and packs with the pack work); fails version (reads the deleted
  xmake.lua), write, syscall_registers, c_abi (build), object-aarch64-apple-darwin and
  stdout-header (`sizeOf([]u8)` gives E0706 unknown type `` in lowering). Known, not regressions.
- To test lowering while the codegen agent's tree is mid-change: copy src to a scratch snapshot,
  replace src/codegen, src/driver, src/cli*, src/session/config.xl with `git archive HEAD` ones,
  link `.xlang`, and `xl build` there.
