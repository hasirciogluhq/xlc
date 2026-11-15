# Lowering port, lane C: what moved, what stayed, known gaps

Scope: the expression forms of the lowering pass (`src/sema/lower/expr/`, `convert.cpp`) and their
diagnostics, ported to `src/lower/` (xlang). The C++ pass stays the production path: the driver
calls `Compilation::lowerAll`, and the xlang pass runs only in the unit gate
(`test/unit/xl_lower_test.cpp`), so no C++ lowering code is deleted by this lane.

## Ported and compared

- Implicit conversions (`coerceTo`, the C++ `coerce`): W0601, E0601, E0708 for a value that needs
  `as` or has no conversion. Interface targets and `[]mut T` to `[]T` are left as they are.
- Casts (`as`): E0709 when no explicit conversion exists (`castable`).
- Dereference: E0709 for a non-pointer and for `*void`.
- Indexing: E0708 for a non-integer index, E0709 for a base that is not indexable and for `*void`.
- Slicing: the bounds are converted to `usize`; the result type follows the C++ rules (`str` for
  `str` and byte arrays, `[]T` otherwise, writable when the base is a place); E0709 for a base that
  cannot be sliced. The dump node carries `rhs` = upper bound and one argument = lower bound.
- Field access: E0709 for `str`/slice fields other than `ptr` and `len`, for a tuple index out of
  range, for a type with no fields, and for an unknown struct or class field.
- Struct literals and `new T { ... }`: E0709 for a type that is not a struct or class, an unknown
  field, and a field given twice; field values are converted to the field type.

The gate compares 58 samples: 39 clean samples (dump and diagnostics; 6 of them new) and 19
samples the C++ pass rejects or warns on (the diagnostics must match; the warning sample also
matches the dump). Every sample is equal.

## Not ported (still C++ only)

- `?` (propagate): needs the statement prelude (hoisted locals and a return branch), the sum
  payload read and the sum construction. Depends on lane A's statement work.
- Closures: capture analysis, `move` closures, generic `F` instances.
- Interface boxing (`ToInterface`, E0504, E0709 for a temporary, E0760): needs the impl table,
  which belongs to lane B (`impl`).
- Destructuring, address-of of a temporary (`&make()`, needs the prelude), `new` target checks,
  `E0723` (class with `init` built by a literal), the user operators `operator[]`, `operator*`.
- Array `.len` (the constant and the side-effect check E0709).
- Field lookup in a class with a base class: inherited fields are not found, so such a field is not
  checked (no diagnostic is reported).
- `unsafe` blocks are not lowered; the conversion check runs as if outside `unsafe`.
- Tuple element types: a tuple field read has no type in the xlang typing yet (the dump of a
  tuple element differs from the C++ dump), so no clean sample reads one.
- E0764 (a function that can reach its end) is not part of the xlang pass.
