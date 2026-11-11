#pragma once

#include "xlang/hir/hir.h"

#include <cstddef>

namespace xlang::hir {

/// Removes every function and global that no root reaches (plan 24, reachability pruning).
///
/// Roots are the functions and globals the program needs whether or not its own code calls them:
///   - `@entry`, every `@init` and every `@fini` function, and every `register(role)` function;
///     the hooks the generated code calls by name (`panic`, `alloc`, `free`, `unwind`, the init and
///     fini lists) are roots too, because codegen looks them up by name;
///   - every vtable slot and every interface method of an `impl` (codegen writes those tables for all
///     classes and impls, so the functions they name must exist);
///   - with `keepExports` (libraries and objects), every `export` / `export(c)` function and global.
/// Reachability then follows direct calls (`Call`, `CallVirtual`), function values (`FuncRef`),
/// constructors (`Construct`), globals (`GlobalRef`) and the cleanup bodies (`unwind_body`) of every reached
/// function. Functions that stay unreached are removed from `module.functions`, so their `@library`
/// imports (`collectImports`) are not linked either.
///
/// Returns the number of functions removed.
std::size_t pruneUnreachable(Module& module, bool keepExports);

}  // namespace xlang::hir
