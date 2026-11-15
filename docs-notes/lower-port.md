# Lowering port: what is ported, what is not

Scope: the xlang lowering pass in `src/lower/` (xlang) against the C++ pass in `src/sema/lower/`
(C++, about 9100 lines). The C++ pass stays the production path: the driver calls
`Compilation::lowerAll`, and the xlang pass runs only in the unit gate
(`test/unit/xl_lower_test.cpp`). No C++ lowering code is deleted by this work.

The gate compares the `--dump-hir` text and the diagnostics (codes, spans and text) of the two
passes for each sample. A sample is equal when both agree. The gate count is in the test output
("N of N samples equal").

## Layout of src/lower

- program.xl: the entry the C++ side calls (`xlcLowerRun`, `xlcLowerDump`, `xlcLowerFree`).
- state.xl: scopes, the `Lowerer` fields (the function under construction, the unsafe depth, the
  loop and defer frames).
- builder.xl: the HIR arenas and the builders that append records. Each expression record takes
  the span set by the expression walk (`HirBuilder.span`).
- items.xl: the module items: globals (with the constant check), enums, aggregates, functions,
  methods; `run` checks the role registrations first.
- roles.xl: the role registrations (E0503 and E0502 within one module), before any body.
- typing.xl: the type of an expression, field and call results, array lengths, side effects.
- function.xl: the expression walk and the statement walk. Its walks call each other, so they
  stay in one file (xlang has no cyclic imports).
- stmt.xl: the if-chain builder of a `switch` (borrows its lists).
- queries.xl: compile-time queries that lower nothing: layout builtins, enum member values,
  operator spellings, `.len` of an array, the constant check of an initializer (`isConstValue`).
- diagnose.xl: conversion, cast, index, slice, field, literal and struct literal checks, and the
  C ABI test `cCompatible`.
- call.xl, overload.xl, methods.xl: calls, overloads and method lookup.
- instances.xl: stubs for generic instances and impls (not ported).

## Ported and compared

Expressions:
- Implicit conversions (W0601, E0601, E0708), casts (E0709), dereference, indexing and slicing.
- Field access: `str` and slice (`ptr`, `len`), tuple elements (type and position), struct and
  class fields, array `.len` (a constant; E0709 when the base has side effects or a field other
  than `len` is named on an array).
- Struct literals and `new T { ... }` (E0709 for an unknown field, a duplicate, a bad type).
- Names: `nil` with no binding is a `*void` nil.
- `Enum.Member` values.
- Calls: functions with one declaration, overloads (cost rules, E0403, E0404), methods of a class
  and its base classes, calls through function values, `syscall`, `sizeOf`/`alignOf`/`offsetOf`.
- `unsafe { }` blocks: a signedness change converts inside them (E0601 outside).

Statements:
- Tuple destructuring `local (a, b) = e` (the tuple held once in `$tupleN`; E0708 for a value that
  is not a tuple or a count mismatch).
- `const NAME = e` and `const NAME: T = e` in a body: a hidden global `fn$NAME$N` bound in the
  scope; E0715 for a non-constant value; E0401 for a second definition in one scope.
- `local` (typed, inferred, `:=`, no initializer), return, if / else / else if, while,
  do-while, blocks, C-style `for`, `for i in lo..hi` and `..=`, break and continue (labels too),
  assignments and compound assignments, `++` and `--`.
- `switch` on integers, bools and enums, and the subject-less `switch`. A duplicate `case` value
  is E0709, a second `default` is E0709, a `case` with no value is E0709.
- `defer` (guard flag, run at block end and at jumps out), `try { } catch e { }` (needs the
  `unwind` role, E0501).

Items:
- Plain enums (underlying type, member values).
- Globals with a type or with an initializer alone (`const N = 4`): the initializer is coerced to
  the declared type; a global or `const` initializer must be a compile-time constant (E0707 for a
  global, E0715 for a `const`, at the first node that is not one); an untyped `const` needs a
  literal (E0715).
- `export(c)` and `declare` functions and globals: the types must be C-expressible (E0302).
- Reserved entry symbols `_start`, `mainCRTStartup`, `_xl_initAll` (E0726).
- Role registrations: each role's signature (E0503) and a role registered twice in one module
  (E0502). The pass runs before the bodies, so the `unwind` function is known to every `try`.

Gate: 125 samples (cases in the clean, remaining and diagnostic tables of xl_lower_test.cpp). All
125 are equal.

## Not ported (still C++ only)

Each item names the reason it is not in the xlang pass yet.

- `for x in e` and `for x, i in e` (stmt/iterate.cpp): needs the iterator protocol.
- Sum types: `switch` on a sum type, sum construction, sum-variant calls (sum/*). Also needed by
  `?`.
- `?` (expr/propagate.cpp) and the condition prelude of `while` and `if`: need the statement
  prelude and the sum payload.
- Drops and moves (own/*: drop.cpp, moves.cpp, construct.cpp): the drop calls on temporaries,
  `&temporary`, `E0721` for a global that needs drop, `E0758`. The samples use non-droppable types.
- Closures (expr/closure.cpp): capture analysis, `move` closures.
- Interfaces: boxing (`ToInterface`, E0504, E0760), interface method calls, the impl table
  (item/impls.cpp).
- Virtual methods and vtables (item/vtable.cpp), constructors `T(args)`, and E0723.
- Generic functions, methods and types: the instances are not named or lowered (instantiation is
  in sema, the lowering of an instance is not in the xlang pass).
- Operator functions (expr/operators.cpp): `operator[]`, `operator*`, `operator()`.
- Packs (`for` over packs, pack calls).
- `__builtin_` forms (call/builtins.cpp), the atomics, `__builtin_read` and `__builtin_write`.
- `new` target checks, E0764 (a function that can reach its end), and the prelude form of the
  ternary (stmt/ternary.cpp: a branch that hoists `?` or `&temp()`; the plain form is ported).
- E0778 (a `fn(...)` value of a function that passes a struct in the C convention) and E0705 for
  `@mangle` on a function (item/c_abi.cpp). The symbol duplicate check (E0401 on link names) is
  not in the xlang pass either.
- Class base classes: fields of a base class are not found, so a field of an inherited class is
  not checked (E0709 is not reported for it); cross-module base classes are not found; a class
  held by value in a struct is reported E0703 by the xlang layout (the C++ pass accepts it).
- The overloaded class method path: a class with two methods of one name makes the xlang pass
  panic while the declarations are lowered ("Vec: index 2 out of range"); the cause is not found.
- Array `.len` of an array whose length no source expression spells (the type of an array
  literal) gives no value: the xlang pass drops the expression silently, where the C++ pass does
  the same. Such a `.len` is not lowered.

## Known gaps and debt

- function.xl is about 1030 lines and holds one mutually recursive walk (expressions call
  statements and back). It is over the size rule. A split needs a break of the cycle (for example
  a table of callbacks, or the statement walk taking the expression walk as a parameter); this is
  not done.
- Duplicate `case` values and the `default` count are checked in the order of the source; the C++
  pass checks the values of one case before it lowers the whole condition. The diagnostic order
  matches for one error per switch; two errors in one `case` can come out in another order.
- E0502 (a role registered twice) is checked within one module only. The C++ pass also checks
  across modules, through the shared session of the driver, which the xlang pass does not have.
- Use after move (E0780, src/analysis/moves.xl): a droppable local (a `Vec`, a `String`, a class
  with drop) passed by value, assigned, or returned is moved; a later read of it is E0780. A field,
  element or `*p` moved by value is E0720 (classes.md, Drop and moves). The lowering sources were
  read for it. Each by-value `Vec` or `String`
  parameter was checked against its callers. Two were real and are fixed: `switchChain` read
  `conds` after it had been passed by value, and `overloadKey` read `params` after it had been
  passed by value on one path. Both now borrow. The compilers in use (rc.11 and rc.12) do not
  report a use after move, so the check is by reading only.
- The HIR spans: each expression record takes the span of the expression it lowers. Records made
  by a conversion after the expression take the span of their caller.

## How to extend

- Write the gate samples first, in a commented block of xl_lower_test.cpp (the form and the
  diagnostic it must give).
- Port the form in the file that owns its area; keep a pure move in its own commit.
- Run the gate until every sample is equal, then commit with a short English message.
