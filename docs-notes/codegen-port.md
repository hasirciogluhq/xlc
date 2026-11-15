# Codegen port: what moved, what stayed, known gaps

Scope: the output step (`src/cli/emit.cpp` and its callers) and the header writer moved to xlang;
LLVM code generation and the archive writer stayed C++ behind `include/xlang/codegen/codegen_c.h`.

## Why the generator still reads the C++ module

The LLVM generator (`src/codegen/llvm/*.cpp`) reads `hir::Module` directly. The flat form of
`src/hir/abi.xl` cannot rebuild it: types are source spellings, and array lengths, generic
arguments and the resolved `Type` tree are not in the flat form. So the C API takes the session
handle, and the generator reads the same C++ module the session owns. The xlang side reads the
flat form for everything that needs names and flags only: the prune, the entry rules, the header,
the incremental module table and the `@library` imports.

## Gaps found in the flat form

- Function and global `module`, function `declared` and `importLibrary`, aggregate `reprC`, global
  `symbol` were missing. They were added to `abi.xl` and `flatten.xl` at offsets taken from the
  C++ layout of `hir::Function` (432 bytes), `hir::Global` (296) and `hir::Aggregate` (152).
- Types are spellings. The header parses them (`src/codegen/header.xl`). This is exact for HIR:
  array lengths are normalized to literals before codegen, and a qualified name's first component
  is the C++ `path.front()`.
- The header decides between an enum, a struct and an interface by looking the name up in the
  module tables. A name that is neither an enum nor an aggregate is an interface when listed as
  one (C `void`), and otherwise a struct, as the C++ generator would print it.
- A struct size comes from `Sema::layoutOfName` (`xlcSessionSizeOf`), not from `layoutOf(Type)`.
  Both give the same value for a struct or class of the root module.

## Behavior kept on purpose

- The native backend (`--backend=native`) still does not prune, as before.
- The incremental cache key keeps its format (`xlc-VERSION|target|...|emit|D:...`); the backend
  builds it (`xlcSessionConfigKey`) because the defines are C++ values.

## Broken or unverified

- `test/unit/c_abi_test.cpp` and `test/unit/lower_test.cpp` include `xlang/hir/prune.h`, which is
  gone (the C++ prune was replaced by `src/hir/prune.xl`). The unit test build fails until they
  use the xlang prune. Not touched, per the order.
- Nothing above was built or run end to end. Only the raw compiler's `--check` ran over the xlang
  files, and `clang++ -fsyntax-only` over the three changed backend C++ files.
- The `Vec` iteration and the `format` specs (`{:016x}`) of the incremental object names have not
  been exercised.
