# xlc

`xlc` is the compiler of the [xlang](https://github.com/hasirciogluhq/xlang) language. It is one
command, like `clang++`: it reads source, writes an object, library, executable, LLVM IR or C
header, and exits. It does not run programs, manage packages or know a standard library; tools
built on top of it (the `xlang` CLI) do that and call `xlc`.

```
xlc [options] main.xl [prebuilt.o ...] [-o out] [--emit=exe|obj|ir|bc|header|static|shared]
```

| Task | Command |
| --- | --- |
| Executable (default) | `xlc main.xl -o main` |
| Object / IR / bitcode | `xlc main.xl --emit=obj\|ir\|bc` |
| Static / shared library, C header | `xlc lib.xl --emit=static\|shared\|header` |
| Type-check only | `xlc main.xl --check` |
| Cross-compile | `xlc main.xl --target=x86_64-linux-gnu` |
| Dumps | `--dump-ast`, `--dump-hir`, `--dump-layout[=Type]`, `--print-cfg` |
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
    fn ==(self: Vec2, other: Vec2) -> bool { return self.x == other.x && self.y == other.y }
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
| `a == b` | `operator==(a, b)` | error `E0709` |
| `a != b` | `operator!=(a, b)` | `!(a == b)` |
| `a < b` | `operator<(a, b)` | error `E0709` |
| `a > b` `a <= b` `a >= b` | `operator>` `operator<=` `operator>=` | from `<`: `b < a`, `!(b < a)`, `!(a < b)` |
| `-a` `!a` `~a` | `operator-(a)` `operator!(a)` `operator~(a)` | error `E0709` |
| `*a` | `*operator*(a)`: the function returns a pointer, `*a` is the place it addresses | error `E0709` |
| `a[i]` | `operator[](a, i)` | error `E0708` |
| `a[i] = v` | `operator[]=(&a, i, v)` | not assignable (`a[i]` is a value) |
| `a += b` `-= *= /= %= &= \|= ^= <<= >>=` | `operator+=(&a, b)` ... | `a = a + b` |
| `a++` `a--` | `operator++(&a)` `operator--(&a)` | `operator+=(&a, 1)`, then `a = a + 1` |
| `a(x, y)` | `operator()(&a, x, y)` | error `E0709` |

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

## Build

Requirements: [xmake](https://xmake.io), a C++23 compiler, and LLVM 20 or newer with `llvm-config`
(static libraries; `$LLVM_CONFIG` overrides the search).

```
xmake f -P . -m release && xmake -P .     # build/<plat>/<arch>/release/xlc
xmake build -P . xlc-tests && xmake run -P . xlc-tests
test/e2e/run.sh build/<plat>/<arch>/release/xlc
```

Run xmake with `-P .` when this repository is checked out as a submodule: xmake otherwise picks
the outermost `xmake.lua`.

## Releases

Releases are built by GitHub Actions for macOS (arm64, x64), Linux (arm64, x64) and Windows (x64,
arm64) from a version tag. The tag must equal `xlc_version` in `xmake.lua`.

| Tag | Release |
| --- | --- |
| `vX.Y.Z` | stable, marked *latest* |
| `vX.Y.Z-rc.N` | preview, marked *pre-release* |

Assets are `xlc_<version>_<os>_<arch>.tar.gz` (`.zip` on Windows) and `checksums.txt` (SHA-256).

To cut a release: set `xlc_version` in `xmake.lua`, commit, then `git tag vX.Y.Z[-rc.N] && git push
origin vX.Y.Z[-rc.N]`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) and the [Code of Conduct](CODE_OF_CONDUCT.md).

## License

Licensed under the [Apache License, Version 2.0](LICENSE).

## Layout

```
include/xlang/   public headers (lang, sema, hir, codegen, util)
src/lang/        lexer, preprocessor, macros, parser, AST, types, build configuration
src/sema/        modules, name tables, constant evaluation, layout, lowering to HIR
src/hir/         HIR dump and reachability pruning
src/codegen/     HIR -> LLVM IR, optimization, object writers, C header
src/driver/      linker driver and archiver
src/util/        source files and diagnostics
src/cli/         command line (`main.cpp`), output selection (`emit.cpp`), terminal color
test/unit/       frontend/sema/HIR unit tests (platform independent)
test/e2e/        the binary on the host OS: starts, links only the OS, builds and runs programs (platform related)
```
