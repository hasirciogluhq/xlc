# Lowering port, lane A: statements and items

Scope: the statement forms of the lowering pass (`src/sema/lower/stmt/`, `item/enum.cpp`) ported
to `src/lower/` (xlang). The C++ pass stays the production path (`Compilation::lowerAll`), and the
gate in `test/unit/xl_lower_test.cpp` is its reference, so no C++ lowering code is deleted here.

## Ported and compared (kLaneASamples, 23 samples)

- `local` (typed, inferred, `:=`, no initializer), return (with and without a value).
- `if` / `else` / `else if`, `while`, `do ... while`, a block statement.
- C-style `for`, `for i in lo..hi` and `lo..=hi` (the hidden `$endN` bound is read once).
- `break` and `continue`, with and without a label; the depth counts the loops left.
- Assignment, compound assignment and `++`/`--` (`t op= v` lowers as `t = t op v`).
- `switch` on integers, bools and enums, and the subject-less `switch`.
- `defer` (a guard flag `$dfrN` declared at the function head; the guard runs at the end of the
  block, at each jump out of it, and goes to the unwind list when an unwinder is registered).
- `try { } catch e { }` (the `$snapN` and `$msgN` locals, the leave call at the end of the body).
- `register(role)` for the function role names (panic, alloc, free, args, unwind).
- Plain `enum` items: the underlying type (`i32` by default), member values (written or one more
  than the previous), and `Enum.Member` expressions.

## Not ported (still C++ only)

- `for x in e` and `for x, i in e` (iterate.cpp): the iterator protocol and the index loop.
- `switch` on a sum type (sum/switch.cpp) and sum construction.
- Drops and moves (own/): the samples use non-droppable types only.
- The unwind pads of a function are built only when `register(unwind)` was seen before that
  function ends; the C++ pass knows the unwinder before it lowers any body. A `try` in a function
  that comes before the `unwind` function is therefore not checked the C++ way (no E0501 and no
  pads here, and E0501 when the function with the `try` comes first). Samples put the `unwind`
  function first; the fix is a role pass before the bodies, as in C++ roles.cpp.
- Structural checks of `register(role)` (E0503 and the duplicate-role diagnostics).
- Duplicate `case` values (E0709) and the `case` value constant evaluation.
- The subject-less `switch` splices a longer chain into the enclosing list in C++; here a chain
  with no arm is a block.
- The `?` prelude and the condition prelude of `while`/`if` (lane C).
- Globals beyond the sample: the sample covers one `global` with a constant initializer.

## Known layout gap

The statement walk stays in `function.xl`: the walk and the blocks call each other, and xlang has
no cyclic imports, so only builders that take already lowered parts live in `stmt.xl`.
