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

## Build

Requirements: [xmake](https://xmake.io), a C++23 compiler, and LLVM 20 or newer with `llvm-config`
(static libraries; `$LLVM_CONFIG` overrides the search).

```
xmake f -P . -m release && xmake -P .     # build/<plat>/<arch>/release/xlc
xmake build -P . xlc-tests && xmake run -P . xlc-tests
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
test/unit/       frontend/sema/HIR unit tests
```
