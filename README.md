# xlc

`xlc` is the compiler of the [xlang](https://github.com/hasirciogluhq/xlang) language. It is one
command, like `clang++`: it reads source, writes an object, library, executable or C header, and
exits. It does not run programs, manage packages or know a standard library; tools built on top of
it (the `xl` CLI) do that and call `xlc`.

`xlc` is written in xlang and has its own native code generator (`src/codegen/`, no LLVM).

| `--target` | OS | Object | Status |
| --- | --- | --- | --- |
| `aarch64-apple-darwin` (default on a Mac) | macOS | Mach-O | built and run |
| `aarch64-unknown-linux-gnu`, `aarch64-unknown` | Linux | ELF64 | built and run (static, `ld.lld`) |
| `armv7-unknown-linux-gnueabihf`, `armv7-unknown` | Linux | ELF32 | built and run (static, `ld.lld`) |
| `aarch64-pc-windows-msvc`, `aarch64-windows` | Windows | COFF | built and linked (`lld-link`), not run yet |

A triple without an OS (`aarch64-unknown`) is Linux. `arm64` and `aarch64` name the same CPU.
The facts behind each target and its known gaps are in
[src/codegen/codegen.md](src/codegen/codegen.md).

```
xlc [options] main.xl [prebuilt.o ...] [-o out] [--emit=exe|obj|header|static|shared]
```

| Task | Command |
| --- | --- |
| Executable (default) | `xlc main.xl -o main` |
| Object | `xlc main.xl --emit=obj` |
| Static / shared library, C header | `xlc lib.xl --emit=static\|shared\|header` |
| Type-check only | `xlc main.xl --check` |
| Dumps | `--dump-ast`, `--dump-hir`, `--dump-mir`, `--dump-layout[=Type]`, `--print-cfg` |
| Explain a diagnostic | `xlc --explain E0501` |

The first `.xl` input is the program root. Every other input is a prebuilt object or library that
is passed to the linker.

## Modules

`import "x"` resolves `x.xl` or `x/index.xl` next to the importing file. Two options connect
libraries, so `xlc` never has to know where one lives:

- `-M NAME=DIR` / `--module-root NAME=DIR`: `import "NAME"` and `import "NAME/sub"` resolve inside
  `DIR`. Modules found this way form the *library group*: their `register(role)` names are
  defaults that the program replaces, and their `@init` functions run first.
- `--import SPEC`: load module `SPEC` although nothing imports it, so its registrations and
  `@init` functions take part in the program.

Compile-time switches are plain definitions: `-DfeatureSched=true`, `-DNAME=VALUE`, `-UNAME`.

```
xlc app.xl -M std=$HOME/.xlang/std --import std -DfeatureDefaultAllocator=true -o app
```

## Operator overloading

An operator is a function named `operator<symbol>`. It is written as `fn <symbol>(...)` in an
inherent `impl T { }` block, or at module level, or in a class body. The parser makes each one a
module-level function, so operators overload by parameter types, can be generic (`impl<T> Pair<T>`)
and are exported like any function.

```
struct Vec2 { x: f32, y: f32 }

impl Vec2 {
    fn +(self: Vec2, other: Vec2) -> Vec2 { return Vec2 { x: self.x + other.x, y: self.y + other.y } }
    fn ==(self: *Vec2, other: *Vec2) -> bool { return self.x == other.x && self.y == other.y }
    fn +=(self: *Vec2, other: Vec2) { self.x += other.x; self.y += other.y }
    fn *(self: Vec2, k: f32) -> Vec2 { return Vec2 { x: self.x * k, y: self.y * k } }
    fn *(k: f32, self: Vec2) -> Vec2 { return self * k }
    fn -(self: Vec2) -> Vec2 { return Vec2 { x: -self.x, y: -self.y } }   // unary: one operand
}
```

In an inherent `impl`, `self: T` names the type of the first operand (`self` alone means `self: T`
by value); the block holds operators only. The operand types decide: `a + b` is the call
`operator+(a, b)` when an operand is a struct or a class, and nothing changes for numbers.

| Source | Calls | When not defined |
| --- | --- | --- |
| `a + b` `- * / % & \| ^ << >>` | `operator+(a, b)` ... | error `E0709` |
| `a == b` | `operator==(&a, &b)` | error `E0709` |
| `a != b` | `operator!=(&a, &b)` | `!(a == b)` |
| `a < b` | `operator<(&a, &b)` | error `E0709` |
| `a > b` `a <= b` `a >= b` | `operator>` `operator<=` `operator>=` | from `<`: `b < a`, `!(b < a)`, `!(a < b)` |
| `-a` `!a` `~a` | `operator-(a)` `operator!(a)` `operator~(a)` | error `E0709` |
| `*a` | `*operator*(a)`: the function returns a pointer, `*a` is the place it addresses | error `E0709` |
| `a[i]` | `operator[](a, i)` | error `E0708` |
| `a[i] = v` | `operator[]=(&a, i, v)` | not assignable (`a[i]` is a value) |
| `a += b` `-= *= /= %= &= \|= ^= <<= >>=` | `operator+=(&a, b)` ... | `a = a + b` |
| `a++` `a--` | `operator++(&a)` `operator--(&a)` | `operator+=(&a, 1)`, then `a = a + 1` |
| `a(x, y)` | `operator()(&a, x, y)` | error `E0709` |

- A comparison (`==` `!=` `<` `<=` `>` `>=`) borrows both operands: every operand parameter is a
  pointer (`fn ==(a: *T, b: *T) -> bool`, or `fn ==(self, other: *T)` in a class), and a
  by-value one is error `E0779`. Comparing never moves or drops a value, so a generic `sort` or
  `contains` compares elements that need drop in place. A temporary operand (`make() == a`) lives
  in a hidden local until the end of the block.
- The operand that is a place is passed by address when the function's first parameter is a
  pointer to the type and none takes it by value (`fn [](self: *Grid, i: usize)`). `+=`, `++`,
  `[]=` and `()` always pass the address, so `self` is `*T` there.
- Lookup: the name in scope, then the module of every struct or class operand, so an operator next
  to its type needs no import. A function that fits the operand count and types is preferred.
- A class with a method of that name uses it, with the class as `self`: `fn +(self, k: i32) -> i32`.
- `&&` and `||` are not overloadable (they do not always evaluate the right side). `=` is not an
  operator.
- An operand lowered only to learn its type is lowered again for the call; a `?` inside it is not
  run twice.

## Closures

```
local add = |a: i32, b: i32| -> i32 { return a + b }
local base = 10
local addBase = |x: i32| x + base            // captures `base`
local twice = apply(|x| x * 2, 4)            // `x: i32` from apply's `fn(i32) -> i32`
local copy = move |x: i32| x + base          // copies `base` instead of borrowing it
```

`|p: T, q| body` (`||` for none); the body is one expression or a block. The return type is
written (`-> R`, then a block), or comes from the `fn(...)` type the closure is used for, or from
the first `return` of the body. A parameter type may be left out only where the context names a
`fn(...)` type: an argument, a typed `local`, a field, a `return`. `local d = |x| x * 2` has no
context and is `E0770`; write `|x: i32|`.

What a closure is depends on what it uses from the function around it:

- **Nothing**: it is a function and its value a `fn(A, B) -> R` pointer. It goes anywhere a function
  pointer goes.
- **Variables of the enclosing function**: it is a struct `__closureN` with one field per captured
  variable, and its function takes a pointer to that struct first. A call `f(x)` is
  `operator()(&f, x)`, so it is called like a function, and a generic parameter takes it
  (`fn apply<F>(f: F, x: i32) -> i32 { return f(x) }`). A `fn(...)` pointer cannot hold it
  (`E0773`), because it has no place for the variables.
- By default a captured variable is **borrowed**: the field is a pointer to it, the closure sees
  later changes and its assignments change the variable. `move` copies the value at the point
  where the closure is written (`E0774` for a type that needs drop). A borrowing closure must not
  outlive the function that made it; the compiler does not check that.
- A closure inside a closure captures through the outer one. A name the closure declares itself
  (parameter, `local`, loop variable) shadows the outer variable and is never captured.
- `move` is a keyword only when a `|` follows; `a || b` and `a | b` stay operators.

The closure's function is lowered where the closure is written, inside the body of the function
around it, so the next expression knows its return type.

Diagnostics: `E0770` parameter type unknown, `E0771` wrong parameter count for the `fn(...)` type,
`E0773` captures in a `fn(...)` position, `E0774` `move` of a type that needs drop.

## Methods of structs and iterators

`impl T { fn name(self: *T, ...) }` also holds the methods of a struct (not only operators). Each
becomes the function `T.name`, and `value.name(...)` on a struct calls it: the address of `value`
is passed when `self` is `*T`, the value when it is `T`. `impl<T> Box<T> { }` declares the type
parameter; the parameters are written on the `impl`.

`for x in value` over a struct or class uses two methods the compiler finds by name; it knows no
library type, so a stdlib `Vec` is iterable by having them:

| The type has | Behavior |
| --- | --- |
| `next` only | it is its own iterator; a second `for` over the same variable goes on where the first stopped |
| `iter` and `next` (on what `iter` returns) | every `for` calls `iter` and starts from the beginning |
| both `iter` and `next` on the same type | `iter` wins |
| `iter` only | error `E0776`: nothing to call in the loop |
| neither | error `E0775` |

```
impl<T> Vec<T> { fn iter(self: *Vec<T>) -> VecIter<T> { ... } }
impl<T> VecIter<T> { fn next(self: *VecIter<T>) -> Option<T> { ... } }

for x in v { }          // v.iter(), then iter.next() until it returns None
```

`next` returns an enum with two variants: one carrying the item and one without data (`Some(T)` /
`None`, in either order). The variant without data ends the loop and `x` is the item itself
(`for x, i in v` adds the index). `E0777` when `next` returns anything else (a `Result` has no end).
`x` is the item by value, unlike an array or slice, where it is a pointer to the element.
`for x in p` over a pointer to such a type iterates the pointee. A temporary (`for x in make()`)
lives until the loop ends.

Warnings: `W0770` `next` takes `self` by value (every call works on a copy, the loop never ends);
`W0771` the type has `iter` and `next` and they differ in type: `for` uses `iter`, so the position
`next` keeps is not used.

## Build

The repository is an [xl](https://github.com/hasirciogluhq/xlang) package (`xlang.toml`) and needs
only `xl`, a released `xlc` and the stdlib it pins (`xl lc install`, `xl std install`, `xl pkg
install`) on a Mac with Apple silicon, and the system `cc` for the final link. `ld.lld` and
Docker add the Linux runs to the tests, `lld-link` and `llvm-dlltool` the Windows links.

```
xl build src/main.xl -o build/xlc         # the compiler
test/native/run.sh                        # the arm64 encoders against the system assembler
test/e2e/run.sh build/xlc                 # programs built, linked and run per target
```

## Releases

Releases are built by GitHub Actions from a version tag. The tag must equal `XLC_VERSION` in
`src/session/session.xl`.

| Tag | Release |
| --- | --- |
| `vX.Y.Z` | stable, marked *latest* |
| `vX.Y.Z-rc.N` | preview, marked *pre-release* |

Assets are `xlc_<version>_<os>_<arch>.tar.gz` (`.zip` on Windows) and `checksums.txt` (SHA-256).

To cut a release: set `XLC_VERSION`, commit, then `git tag vX.Y.Z[-rc.N] && git push
origin vX.Y.Z[-rc.N]`.

## Contributing

Where to report a problem is in [SUPPORT.md](SUPPORT.md): compiler bugs here, the language, the
`xl` tool and everything unsure in [xlang](https://github.com/hasirciogluhq/xlang). See also
[CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) and the
[Code of Conduct](CODE_OF_CONDUCT.md).

## License

Licensed under the [Apache License, Version 2.0](LICENSE).

## Layout

```
src/lang/        lexer, preprocessor, macros, parser, AST, types, config (all xlang)
src/sema/        modules, name tables, constant evaluation, layout, generic instances, packs
src/lower/       the lowering pass to the flat HIR: program.xl is its entry, state.xl the scopes and
                 the Lowerer, builder.xl the HIR arenas, items.xl the module items (globals, enums,
                 functions, methods), roles.xl the role registrations (before the bodies),
                 typing.xl the per-expression types, function.xl the expression walk and the
                 statement walk (loops, jumps, assignments, switch, defer, try), stmt.xl the switch
                 if-chain builder, queries.xl the compile-time queries (layout, enum values,
                 operator spellings, array length, constant initializers), diagnose.xl the
                 conversion, cast, index, slice, field, literal and C ABI checks, call.xl the calls
                 (functions, overloads, methods, function values), overload.xl the overload keys
                 and the choice among candidates, methods.xl the class declarations and the method
                 lookup through base classes and impls, generics.xl generic calls and generic
                 methods, instances.xl the generic instances and lazy instance methods, pack.xl
                 parameter packs (`..args`), inherent.xl struct `impl` calls
src/hir/         the flat HIR (abi.xl), its dump (dump.xl) and the reachability prune (prune.xl)
src/mir/         HIR -> MIR (control-flow graph per function): builder/, model.xl, dump.xl
src/analysis/    static analyses over MIR (moves.xl, findings.xl): read only, findings sorted by
                 position
src/codegen/     the code generator (details in codegen.md): target/ (triples), native/ (picks the
                 selector and writer per target), arm64/ and arm32/ (encode/ the instruction
                 encoders, isel/ the instruction selectors), common/ (layout and types shared by
                 the selectors), obj/ (the format-neutral object), macho/, elf/, coff/ (the
                 object writers), header.xl (the C header, `--emit=header`)
src/driver/      the linker driver (`link.xl`): `cc` on macOS, `ld.lld` on Linux, `lld-link` on
                 Windows
src/util/        source files and diagnostics
src/session/     the compilation session: module loading, the multi-module lowering driver
src/main.xl      the CLI entry
src/cli.xl       command line parser and the run it drives
src/cli/         the output step: `emit.xl` (the orchestration), `entry.xl` (entry-point rules),
                 `program.xl` (prune and link imports), `incremental.xl` (per-module objects and
                 cache), `path.xl` (paths), `console.xl` (terminal color)
test/native/     the arm64 encoders checked word by word against the system assembler
test/e2e/        the binary on the host OS: starts, links only the OS, builds and runs programs
```
