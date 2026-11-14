# Compiler comment tags

This file defines the tags that may appear as a doc comment (`///`) above a declaration in the
compiler source. Each tag is one grep handle, so a reader or a tool finds every place a fact is
recorded without reading the code.

Rule: a tag is written only where it is true and useful. A tag that is only decoration is an
error. When the code under a tag changes, the tag changes in the same edit.

Rule: one tag per line. The tag word starts the line, and a line that continues a tag repeats it.
A long tag is wrapped with the tag on every line, never with a bare `///` continuation, so grep
returns the whole tag and nothing is lost:

    /// @grammar `for init; cond; step { body }` (every part optional)
    /// @grammar `for x in e { body }`, `for x, i in e { body }`
    /// @grammar `for i in lo..hi { body }`, `for i in lo..=hi { body }`

This makes every tag collectable with one grep, with no state and no tool:

    grep -rn "@grammar" src/
    grep -rn "@invariant" src/
    grep -rn "@ownership" src/
    grep -rn "@errno" src/
    grep -rn "@layout" src/
    grep -rn "@abi" src/
    grep -rn "@spec" src/
    grep -rn "@todo" src/
    grep -rn "@cost" src/

The tag word is always lower case ASCII and always followed by one space, so `@todo` and
`@todos` are different tags and a tag is never a prefix of another by accident.
## @grammar

The syntax the parser function accepts, written in the language's own syntax, short, one form or
a family of forms per tag. Goes only on the parser function that accepts the form: the parser is
the one place that decides what is accepted. Never on a helper, on sema, on HIR or on codegen.

    /// @grammar `for i in lo..hi { body }`, `for x, i in s { body }`
    Stmt Parser::parseFor() {

When a form changes, its tag changes in the same edit. A new form without a tag is an incomplete
change.

## @comment

One line of plain explanation at a site where the code alone does not say what or why. It names
at the start what is being explained: a function name, a rule, a field, a stage (`codegen`,
`sema`, `lower`). It is the tag for a remark that is not a grammar form, not an invariant, not an
ownership fact and not a citation, so it does not invent a category that does not exist.

    /// @comment `parseCondition` sets `no_struct_` around the expression: in `if a == b {`
    /// @comment the `{` opens the block, never a struct literal.

Rules: one sentence, no history, no restatement of the signature. A @comment that is still needed
after the code is read once belongs in the declaration's doc comment instead.

## @invariant

A fact the code relies on and does not re-check, stated right above the code that keeps it. Says
what must stay true, and what breaks when it stops being true.

    /// @invariant `pos_` is always at or before `tokens_.size() - 1`, so `cur()` never reads past
    /// the `End` token that the constructor appends.
    const Token& cur() const { return tokens_[pos_]; }

## @ownership

Who owns a heap value, who frees it, and who must not. Used where C++ ownership is not obvious
from a `unique_ptr` or a `shared_ptr`: raw pointers that point into another node, references
that outlive a call, handles that the caller keeps.

    /// @ownership `lhs` and `rhs` are moved into the node; the caller keeps nothing.
    ExprPtr Parser::makeBinary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {

## @errno

The OS error that a call returns and what it means here. Used at a syscall, a library call or an
`errno` check, with the number and the behavior the code expects, never a guess.

    /// @errno `write` returns `-EPIPE` (32 on Linux) when the reader is gone; the caller treats
    /// it as a closed stream, not as a failure.

Only Linux, macOS and Windows values are recorded, each with its source.

## @layout

A struct or union layout that matters: offsets, padding, alignment, field order and why. Used
where a layout is part of a contract, not where `@repr(c)` already says it.

    /// @layout `Span` is 5 words: `file` (4 bytes), `line`, `column`, `end_line`, `end_column`.
    /// It is copied by value everywhere, so no field may ever become a heap pointer.

## @abi

A calling convention, a register assignment or a symbol name that something outside the compiler
depends on: an entry point, a `register(role)` name, an exported symbol, a `--target` quirk.

    /// @abi The entry symbol is `_start` on ELF and `mainCRTStartup` on PE; a user symbol with
    /// either name is `E0726`.

## @spec

A citation for a rule the code implements: an RFC, a man page, a vendor document, a language
specification page. The URL, the section, and the one sentence being implemented.

    /// @spec https://www.rfc-editor.org/rfc/rfc3986#section-3.3 for the path grammar used below.

## @todo

A known gap with a reason and a boundary, not a wish. Names what is missing, why it is acceptable
now, and what would make it wrong later. Anything that is a real limitation of the language also
goes into xlang-docs/known-limitations.md.

    /// @todo Only the little-endian targets are handled; a big-endian target would need the
    /// byte swap in `emitConstant`, so the target list is checked in `config.cpp`.

## @cost

A measured or clearly reasoned cost at a hot point: an allocation per call, a copy, a quadratic
loop. Says the size and what would fix it, so it is visible without a profiler.

    /// @cost One `std::string` per token: acceptable because a token is 24 bytes and a file has
    /// a few thousand of them; a substring view would remove it if it ever shows up.

## Adding a tag

A new tag is added to this file first, with its rule, its example and the reason it is a tag and
not a plain comment. Then the sites are tagged in the same change. Two tags never mean the same
thing.

## Collected view

Everything between the two markers is generated by `scripts/tags.sh sync` from the source, so it
is never edited by hand. The pre-commit hook runs the sync and stages this file when it changed,
which keeps the view in the same commit as the tag it came from.

<!-- BEGIN COLLECTED TAGS -->
<!-- written by scripts/tags.sh sync; every line below is a tag found in src/ and
     include/, grouped by tag and then by file. Do not edit inside the markers. -->

### @grammar (119)

```
include/xlang/lang/ast.h:21:/// @grammar the five prefix operators: `-e`, `!e`, `*e`, `&e`, `~e`
include/xlang/lang/ast.h:24:/// @grammar the infix operators: `a + b`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`,
include/xlang/lang/ast.h:25:/// @grammar `&&`, `||`, `&`, `|`, `^`, `<<`, `>>` (Go precedence, `&&`/`||` lowest above `?`)
include/xlang/lang/lexer.h:12:/// @grammar every lexical token the parser can see; the spellings are fixed by `lexer.cpp` and
include/xlang/lang/lexer.h:13:/// @grammar the language grammar in the parser's `@grammar` comments uses these tokens
src/lang/lexer/lexer.cpp:176:    /// @grammar the lexical forms are dispatched by first character: `b'x'` byte literal,
src/lang/lexer/lexer.cpp:177:    /// @grammar identifier or keyword, number, string, `#directive`, then punctuation
src/lang/lexer/lexer.cpp:345:    /// @grammar `[A-Za-z_][A-Za-z0-9_]*`, a keyword or a primitive type name
src/lang/lexer/lexer.cpp:361:    /// @grammar `#if`, `#elif`, `#else`, `#endif`, `#ifdef`, `#ifndef`, `#error`, first token of
src/lang/lexer/lexer.cpp:362:    /// @grammar its line only; `#` anywhere else is an error
src/lang/lexer/lexer.cpp:392:    /// @grammar `b'x'`, `b'\n'`, `b'\xHH'`, `b'\u{...}'`: exactly one byte, value 0..255
src/lang/lexer/lexer.cpp:453:    /// @grammar `"text"` with the escapes `\n \t \r \0 \\ \" \xHH \u{...}`; may not span a
src/lang/lexer/lexer.cpp:454:    /// @grammar line break
src/lang/lexer/lexer.cpp:514:    /// @grammar `123`, `0x1f`, `0b1010`, `0o17`, `1_000`, `1.5`, `1e9`, `1.5e-3`; after a `.` it is
src/lang/lexer/lexer.cpp:515:    /// @grammar a tuple position, so it takes no fraction and no exponent there
src/lang/lexer/lexer.cpp:648:/// @grammar the whole token stream of one file, always ending with `TokenKind::End`
src/lang/macro/macro.cpp:375:/// @grammar `name!(args...)`: each argument is matched against the parameter kinds `expr`,
src/lang/macro/macro.cpp:376:/// @grammar `type`, `ident`, `block`, `items`, and the body is substituted hygienically
src/lang/parser/attributes.cpp:7:/// @grammar `@name`, `@name(a, key = v, ...)`, `@file(name(...))` at the top of a file
src/lang/parser/attributes.cpp:37:/// @grammar the value of an attribute argument: an identifier, an integer literal or a string
src/lang/parser/attributes.cpp:65:/// @grammar one or more `@attribute` lines before an item, a statement or a parameter
src/lang/parser/expr/binary.cpp:7:/// @grammar `cond ? a : b` is right associative and the lowest precedence level
src/lang/parser/expr/binary.cpp:37:/// @grammar `a || b`
src/lang/parser/expr/binary.cpp:47:/// @grammar `a && b`
src/lang/parser/expr/binary.cpp:57:/// @grammar `a == b`, `a != b`
src/lang/parser/expr/binary.cpp:67:/// @grammar `a < b`, `a <= b`, `a > b`, `a >= b`
src/lang/parser/expr/binary.cpp:92:/// @grammar `a + b`, `a - b`, `a | b`, `a ^ b`
src/lang/parser/expr/binary.cpp:119:/// @grammar `a * b`, `a / b`, `a % b`, `a & b`, `a << b`, `a >> b`
src/lang/parser/expr/binary.cpp:151:/// @grammar `e as T` binds tighter than every binary operator, so `a + b as i64` casts `b`
src/lang/parser/expr/binary.cpp:152:/// @grammar `[cast_type_]` tells `parseType` that a `<` after a name may be a comparison
src/lang/parser/expr/binary.cpp:153:/// @grammar (`x as Idx < n`) rather than type arguments; `castTypeArgsAhead` decides
src/lang/parser/expr/binary.cpp:170:/// @grammar `-e`, `!e`, `~e`, `*e`, `&e`, `&mut e`, repeated (`--x`, `*&p`)
src/lang/parser/expr/postfix.cpp:33:/// @grammar the brace that may open the body of a struct literal: `{` is a literal only when it
src/lang/parser/expr/postfix.cpp:34:/// @grammar is followed by `}` or by `name :`
src/lang/parser/expr/postfix.cpp:44:/// @grammar `Type { field: value, other }`, `Type { }`
src/lang/parser/expr/postfix.cpp:74:/// @grammar `name!(raw tokens...)`, the arguments are not parsed
src/lang/parser/expr/postfix.cpp:214:/// @grammar `f(a, b)`, `a[i]`, `a[lo..hi]`, `a[lo..]`, `e.field`, `e<T>(...)`, `e!`, `e?`
src/lang/parser/expr/postfix.cpp:215:/// @grammar `name!(args...)`, `in(reg) x` / `out("rax") y` / `inout(reg) z` as a call argument
src/lang/parser/expr/postfix.cpp:216:/// @grammar `..pack` as a call argument, chained (`a.b(c)[d]?.e`)
src/lang/parser/expr/postfix.cpp:217:/// @grammar the loop keeps applying suffixes while `continues()` holds, so a newline ends the
src/lang/parser/expr/postfix.cpp:218:/// @grammar chain unless a `(`/`[`/`{` is still open
src/lang/parser/expr/primary.cpp:10:/// @grammar `fn(params) -> R { body }` as an expression
src/lang/parser/expr/primary.cpp:33:/// @grammar `||`, `|a, b|`, `|a: T, b| -> R { body }`, `|x| expr`, `move |x| expr`
src/lang/parser/expr/primary.cpp:34:/// @grammar `move` is a keyword only directly in front of `|`, so `a | b` and `a || b` stay
src/lang/parser/expr/primary.cpp:35:/// @grammar operators
src/lang/parser/expr/primary.cpp:95:/// @grammar `123`, `1.5`, `"text"`, `b'x'`, `true`, `false`, `self`, `name`, `name::name`
src/lang/parser/expr/primary.cpp:96:/// @grammar `name<T, U>`, `(a)`, `(a, b)`, `[a, b]`, `[v; n]`, `new T`, `new(p) T`
src/lang/parser/expr/primary.cpp:97:/// @grammar `__builtin_asm(...)`
src/lang/parser/expr/primary.cpp:98:/// @grammar a name is a primary; the suffixes (`call`, `index`, `field`, `?`) are applied by
src/lang/parser/expr/primary.cpp:99:/// @grammar `parsePostfix`, so `a.b(c)` is not a single primary form
src/lang/parser/expr/primary.cpp:204:/// @grammar `[]T` and `[]mut T` as the element type of a slice, versus `[]` the empty array
src/lang/parser/expr/primary.cpp:205:/// @grammar literal: only a type token directly after `]` on the same line means a slice type
src/lang/parser/expr/primary.cpp:224:/// @grammar `[a, b]`, `[v; n]`, `[a, b,]`, `[]`
src/lang/parser/expr/primary.cpp:266:/// @grammar `new T { fields }`, `new T(args)`, `new(p) T { fields }`, `new(p) T(args)`
src/lang/parser/item/aggregate.cpp:7:/// @grammar `field: T`, `@attribute field: T`, member separated by `,` or a line break
src/lang/parser/item/aggregate.cpp:29:/// @grammar `struct Name { fields }`, `struct Name<T, U> { fields }`, `@repr(c) struct ...`
src/lang/parser/item/aggregate.cpp:68:/// @grammar `class Name [: Base] { members }`, `class Name<T> [: Base] { members }`
src/lang/parser/item/aggregate.cpp:112:/// @grammar `[virtual|override] fn name(params) -> R { body }`, `fn +(...)`, `fn [](...)`
src/lang/parser/item/aggregate.cpp:142:/// @grammar `interface Name { fn name(params) -> R }`, `interface Name : Base { ... }`
src/lang/parser/item/aggregate.cpp:181:/// @grammar `impl T { fn +(...) ... }`, `impl<T> Box<T> { ... }`, `impl Iface for T { ... }`
src/lang/parser/item/enum.cpp:7:/// @grammar `enum Name: T { A, B }`, `enum Name<T> { A(T), B, C(A, B) }`
src/lang/parser/item/enum.cpp:150:/// @grammar `type Name = T`, `type Name<T> = T`
src/lang/parser/item/enum.cpp:166:/// @grammar `define name(a: expr, b: stmt) { body }`, used as `name!(...)`
src/lang/parser/item/fn.cpp:9:/// @grammar the parameters and the body of a function: `(a: T, b: U) -> R { body }`
src/lang/parser/item/fn.cpp:10:/// @grammar `(self)`, `(self: *T)`, a trailing `,` before `)` allowed
src/lang/parser/item/fn.cpp:71:/// @grammar `+ - * / % & | ^ << >> == != < <= > >= ! ~ ++ -- [ ] [ ]= ( )`, as a function name
src/lang/parser/item/fn.cpp:127:/// @grammar `fn name(params) -> R { body }`, `fn name(params)`, `fn self.name(...)` in a member
src/lang/parser/item/fn.cpp:154:/// @grammar `fn name = fnType`
src/lang/parser/item/fn.cpp:172:/// @grammar `global name: T = expr`
src/lang/parser/item/fn.cpp:190:/// @grammar `const name: T = expr`
src/lang/parser/item/item.cpp:26:/// @grammar `fn name<T>(params) -> R { body }`, `fn +(...)` and the other operator names
src/lang/parser/item/item.cpp:27:/// @grammar `fn name: sig`, `global name: T = c`, `const name: T = c`
src/lang/parser/item/item.cpp:28:/// @grammar `struct`, `class`, `interface`, `enum`, `impl`, `type Name = T`, `define`
src/lang/parser/item/item.cpp:29:/// @grammar `import`, `export name from "m"`, `export *`, a module-level `name!(...)` call
src/lang/parser/item/item.cpp:30:/// @grammar `@attribute` before any of them, `export` / `export(c)` / `@library` modifiers
src/lang/parser/item/item.cpp:115:/// @grammar `export`, `export(c)`, `@library` and the other item modifiers before the keyword
src/lang/parser/item/module.cpp:47:/// @grammar `import "m"`, `import "m" as alias`, `import name from "m"`, `import * from "m"`
src/lang/parser/item/module.cpp:48:/// @grammar `from "m" import name, other as alias`
src/lang/parser/item/module.cpp:87:/// @grammar `export * from "m"`, `export { a, b as c } from "m"`
src/lang/parser/item/module.cpp:142:/// @grammar a module-level `name!(...)` call, or a plain call, as an item
src/lang/parser/stmt/block.cpp:7:/// @grammar `{ stmt; stmt }`
src/lang/parser/stmt/block.cpp:59:/// @grammar `@attribute` applied to a statement, before any statement form
src/lang/parser/stmt/block.cpp:71:/// @grammar `label: while ...`, `label: do ... while ...`, `label: for ...`
src/lang/parser/stmt/block.cpp:72:/// @grammar `switch ...`, `if ...`, `while ...`, `do ... while ...`, `for ...`
src/lang/parser/stmt/block.cpp:73:/// @grammar `return e`, `return`, `break [label]`, `continue [label]`, `defer stmt`
src/lang/parser/stmt/block.cpp:74:/// @grammar `try { } catch e { }`, `unsafe { }`, `{ ... }`, `local ...`
src/lang/parser/stmt/block.cpp:75:/// @grammar the simple statement: assignment, compound assignment, `++`, `--`, expression
src/lang/parser/stmt/block.cpp:199:/// @grammar `local name`, `local name: T`, `local name := expr`
src/lang/parser/stmt/block.cpp:200:/// @grammar `local name = expr`, `local name: T = expr`
src/lang/parser/stmt/block.cpp:262:/// @grammar `target = v`, `target += v` (and the other compound assignments)
src/lang/parser/stmt/block.cpp:263:/// @grammar `target++`, `target--`, `expr`
src/lang/parser/stmt/control.cpp:7:/// @grammar `for init; cond; step { body }` (every part optional)
src/lang/parser/stmt/control.cpp:8:/// @grammar `for x in e { body }`, `for x, i in e { body }`
src/lang/parser/stmt/control.cpp:9:/// @grammar `for i in lo..hi { body }`, `for i in lo..=hi { body }`
src/lang/parser/stmt/control.cpp:10:/// @grammar `for` header vs `for x in` is decided by `Ident` followed by `in` or `,` on the next
src/lang/parser/stmt/control.cpp:11:/// @grammar token, so `for i in 0..n` and `for x, i in s` never reach the C-style form
src/lang/parser/stmt/control.cpp:76:/// @grammar `for init; cond; step { body }` and the range/foreach forms share one rule so a
src/lang/parser/stmt/control.cpp:77:/// @grammar `local` in the initializer behaves exactly as in statement position
src/lang/parser/stmt/control.cpp:89:/// @grammar `switch v { case a, b { ... } default { ... } }`, `switch { case cond { } ... }`
src/lang/parser/stmt/control.cpp:90:/// @grammar `case a, b` takes a list; `default` takes none, at most one arm
src/lang/parser/types.cpp:7:/// @grammar `<T, U>` after a `fn`, `struct`, `class` or `interface` name, `<>` empty
src/lang/parser/types.cpp:32:/// @grammar `<A, B>` after a type name or an identifier in an expression, `>>` closing two levels
src/lang/parser/types.cpp:167:/// @grammar the type after `->`, which may be `never` (the one place it may be written)
src/lang/parser/types.cpp:176:/// @grammar `*T`, `[]T`, `[]mut T`, `[T; N]`, `(T,)`, `(A, B)`, `()`
src/lang/parser/types.cpp:177:/// @grammar `fn(A) -> R`, `fn(A)`, `Name`, `a.b.Name`, `Name<T, U>`
src/lang/parser/types.cpp:178:/// @grammar `*T` is written with a star, never a postfix form: the type grammar is prefix only
src/lang/parser/types.cpp:179:/// @grammar `(T)` without the trailing comma is `T` in parentheses, not a one-element tuple
src/lang/parser/types.cpp:180:/// @grammar `[T; N]` keeps N as an expression, parsed with `no_struct_` cleared
src/lang/parser/types.cpp:181:/// @grammar `never` is accepted here and rejected later with E0765 unless it is a return type
src/lang/preprocess/preprocess.cpp:545:/// @grammar `#if`, `#elif`, `#else`, `#endif`, `#ifdef`, `#ifndef`, `#error`, resolved against the
src/lang/preprocess/preprocess.cpp:546:/// @grammar build definitions (`-DNAME=VALUE`, `-UNAME`) before the parser sees the tokens
src/sema/lower/call/builtins.cpp:7:/// @grammar `syscall(nr, args...)`, returning `i64`: a negative result is `-errno`, so the caller checks the sign rather than a separate error out.
src/sema/lower/call/builtins.cpp:68:/// @grammar `__builtin_atomicLoad(p, order)`, `__builtin_atomicStore(p, v, order)`, `__builtin_atomicXchg(p, v, order)`, `__builtin_atomicCas(p, expected, new, order)`, `__builtin_atomicAdd(p, v, order)`.
src/sema/lower/call/builtins.cpp:69:/// @grammar `__builtin_read(p)`, `__builtin_write(p, v)`, `__builtin_drop(p)`, `__builtin_needsDrop<T>()`, `__builtin_fence(order)`, `__builtin_unwindPads()`, `__builtin_unwindPadCount()`, `__builtin_asm(...)`.
src/sema/lower/call/builtins.cpp:290:/// @grammar `__builtin_asm("template", in(reg) x, out("rax") y, inout(reg) z, clobber("memory"), options(nomem|noreturn))`, with `{k}` placeholders numbered by operand order.
src/sema/lower/expr/expr.cpp:115:/// @grammar a string literal is a `str`, or byte by byte into a `[u8; N]` when the context names one; a literal longer than N is E0717.
src/sema/lower/item/roles.cpp:17:/// @grammar `register(panic)`, `register(alloc)`, `register(free)`, `register(unwind)`, `register(args)`: one symbol per role per group, checked structurally against the shape the role needs.
src/sema/lower/item/roles.cpp:129:/// @grammar `@init fn`, `@fini fn`, `@entry fn`: module-level hooks that are not called by any code and are kept by reachability for that reason.
src/sema/sema/const_eval.cpp:325:/// @grammar `sizeOf(T)`, `alignOf(T)` and `offsetOf(T, field)` are the only calls that are compile-time constants; every other call in a constant position is E0707.
```

### @invariant (42)

```
include/xlang/analysis/analysis.h:14:/// @invariant findings are reported sorted by source position, never in the order the passes or functions ran.
include/xlang/mir/mir.h:27:// @invariant MIR is built only from HIR and never refers back to it; a span on every statement and
src/analysis/findings.hpp:10:/// @invariant `reportTo` emits them sorted by the position of their first label, without duplicates, whatever order the passes and functions ran in.
src/lang/parser/expr/binary.cpp:8:/// @invariant `open_ternaries_` holds the token index of every `?` whose `:` is still owed, so
src/lang/parser/expr/binary.cpp:9:/// @invariant `ternaryColonAhead` can tell a ternary `?` from a propagating one
src/lang/parser/expr/binary.cpp:154:/// @invariant `cast_type_` stays true for the whole target type and is cleared right after
src/lang/parser/expr/binary.cpp:171:/// @invariant `prefix_operand_` and `prefix_nl_base_` are raised around the operand, so a
src/lang/parser/expr/binary.cpp:172:/// @invariant newline in front of a binary `&`/`*` ends the operand, not the next argument
src/lang/parser/expr/postfix.cpp:35:/// @invariant this is the only place that answers the struct-literal-or-block question; every
src/lang/parser/expr/postfix.cpp:36:/// @invariant caller must consult it rather than re-implementing the two-token peek
src/lang/parser/expr/postfix.cpp:219:/// @invariant `no_struct_` is cleared inside `(` and `[` and restored on exit, so `{` in an
src/lang/parser/expr/postfix.cpp:220:/// @invariant argument list is a struct literal even inside an `if` condition
src/lang/parser/expr/primary.cpp:36:/// @invariant a parameter type may be omitted only where the context names a `fn(...)` type; the
src/lang/parser/expr/primary.cpp:37:/// @invariant parser accepts the omission and lowering rejects an unknown one with E0770
src/lang/parser/expr/primary.cpp:206:/// @invariant no expression continues with a type name after `]`, which is what makes this peek
src/lang/parser/expr/primary.cpp:207:/// @invariant unambiguous
src/lang/parser/expr/primary.cpp:225:/// @invariant `no_struct_` is cleared inside the brackets and restored on exit, so an element may
src/lang/parser/expr/primary.cpp:226:/// @invariant be a struct literal
src/lang/parser/stmt/control.cpp:12:/// @invariant `no_struct_` and `for_header_` are put back on every return path, including the
src/lang/parser/stmt/control.cpp:13:/// @invariant two early returns of the range forms; the `for` header is not a struct context
src/lang/parser/stmt/control.cpp:91:/// @invariant `nl_depth_` is reset to 0 inside the body and restored through `restore` on every
src/lang/parser/stmt/control.cpp:92:/// @invariant exit, including the end-of-file error, so a raised depth never leaks past it
src/lang/parser/types.cpp:33:/// @invariant inside `<...>` a nested `<` always opens type arguments, also in the type of a
src/lang/parser/types.cpp:34:/// @invariant cast, so `cast_type_` is cleared here and restored on exit
src/lang/parser/types.cpp:182:/// @invariant `nl_depth_` is raised around every bracket the rule consumes and lowered on every
src/lang/parser/types.cpp:183:/// @invariant exit path, so a newline inside a type does not end the enclosing statement
src/mir/dataflow/forward.hpp:24:/// @invariant the worklist is FIFO from block 0, so the result and the step count depend only on the body, never on hashing or addresses.
src/sema/lower/call/builtins.cpp:70:/// @invariant the `__builtin_` set is closed: a name not listed here is not a builtin, and the compiler never learns a new one from the standard library.
src/sema/lower/call/builtins.cpp:71:/// @invariant `__builtin_read` copies the value out of the pointer and drops nothing, so two reads of one pointer yield two owners of the same bytes and only the caller's discipline keeps that sound.
src/sema/lower/call/builtins.cpp:72:/// @invariant `__builtin_write` is a statement, never a value, and moves the value through the pointer without running a destructor on the old bytes.
src/sema/lower/call/overload.cpp:9:/// @invariant the overload key is the qualified name plus the parameter types, so two overloads of one name never share a symbol.
src/sema/lower/expr/closure.cpp:175:/// @invariant a closure that captures nothing is a plain `fn(A) -> R` pointer and captures anything only as a struct, so a `fn(...)` position holding a capturing closure is E0773.
src/sema/lower/expr/expr.cpp:88:/// @invariant an unsuffixed integer literal is `i32` unless the context asks for another type, and it widens to `i64` only when the value does not fit in 32 bits.
src/sema/lower/expr/expr.cpp:101:/// @invariant an unsuffixed float literal is `f64` unless the context asks for `f32`, so `local x: f32 = 1.5` narrows at the literal, not at a cast.
src/sema/lower/expr/expr.cpp:118:/// @invariant `b'x'` is always `u8`, never widened by the context, because the byte is the whole value.
src/sema/lower/expr/index.cpp:7:/// @invariant a bounds check exists only under `--safety=checked`; an unchecked build emits the access with no branch.
src/sema/lower/helpers.cpp:21:/// @invariant a negative literal of a type narrower than 64 bits keeps only its low bits of that width, which is why `-1 as u8` is 255 and not a sign-extended 64-bit value.
src/sema/lower/item/roles.cpp:18:/// @invariant group 1 is the stdlib default and a group 2 (user) registration replaces it per role, so user code can override the allocator without changing the stdlib.
src/sema/lower/item/roles.cpp:130:/// @invariant `@init` runs for the stdlib group before user code and for an import before its importer; `_xl_initAll` is the only caller and it is compiler-owned.
src/sema/lower/own/drop.cpp:58:/// @invariant `needsDrop` is memoized with a `false` written before the recursion, so a type that owns itself through a pointer terminates instead of recursing forever.
src/sema/lower/own/drop.cpp:122:/// @invariant a class dropped through a pointer to its base loses the derived destructor unless the base has a virtual drop slot; this returns the name of the lost one and is empty when nothing is lost.
src/sema/lower/stmt/try_catch.cpp:10:/// @invariant `try` requires the `unwind` role: with no registered unwinder the program cannot catch and the form is E0501.
```

### @ownership (7)

```
src/lang/parser/expr/primary.cpp:11:/// @ownership the produced `FnDecl` is moved into `lambdas_` and spliced onto `program.items` by
src/lang/parser/expr/primary.cpp:12:/// @ownership `run()`; the expression only keeps the generated name
src/sema/lower/expr/closure.cpp:110:/// @ownership a closure without `move` borrows: the field holds a pointer to the variable, so the closure sees later writes and a write inside changes the variable.
src/sema/lower/expr/literal.cpp:191:    /// @ownership `new T` calls the registered `alloc`; `new(p) T` places into `p` and calls no
src/sema/lower/expr/literal.cpp:192:    /// @ownership allocator, so the memory of only the first form is owned by the pointer.
src/sema/lower/own/moves.cpp:24:/// @ownership the drop glue takes the address of the place and leaves the place unmodified; the flag that decides whether to run it is cleared by the caller, never here.
src/sema/lower/stmt/statement.cpp:84:/// @ownership `delete` drops the object through its class drop glue first, then calls the registered `free`; with no `free` role it is E0501, never a silent leak.
```

### @layout (1)

```
src/sema/sema/layout.cpp:48:/// @layout `interface` is a fat pointer: 2 * pointer width, aligned to the pointer width.
```

### @abi (14)

```
src/codegen/llvm/c_abi/aapcs64.cpp:64:/// @abi AAPCS64 6.8: HFA/HVA aggregates take up to four SIMD registers, other aggregates up to
src/codegen/llvm/c_abi/aapcs64.cpp:65:/// @abi 16 bytes go in x0-x7, the rest on the stack.
src/codegen/llvm/c_abi/classify.cpp:7:/// @abi the C convention of a target follows the architecture first, the OS second: every arm64
src/codegen/llvm/c_abi/classify.cpp:8:/// @abi target uses AAPCS64, x86-64 uses Win64 on Windows and SysV elsewhere.
src/codegen/llvm/c_abi/sysv_x86_64.cpp:84:/// @abi System V AMD64 3.2.3: INTEGER and SSE eightbytes go in registers, MEMORY on the stack
src/codegen/llvm/c_abi/sysv_x86_64.cpp:85:/// @abi (6 integer and 8 SSE register slots); an aggregate over 16 bytes is always MEMORY.
src/codegen/llvm/c_abi/win_x64.cpp:32:/// @abi Microsoft x64: an aggregate of 1, 2, 4 or 8 bytes is passed as an integer of that size,
src/codegen/llvm/c_abi/win_x64.cpp:33:/// @abi every other aggregate goes by hidden reference; four register slots, then the stack.
src/codegen/llvm/entry.cpp:34:/// @abi the entry symbol is `_start` on ELF and `mainCRTStartup` on PE; a user symbol with either
src/codegen/llvm/entry.cpp:35:/// @abi name is E0726, so this lookup is the only way `@entry` becomes the program entry.
src/sema/lower/lower.hpp:128:/// @abi the syscall number goes in one register and the arguments in the next ones: x0-x7 on
src/sema/lower/lower.hpp:129:/// @abi arm64 and rdi/rsi/rdx/r10/r8/r9 on x86-64 Linux and macOS, which is why the limit is six.
src/sema/lower/lower.hpp:130:/// @abi Windows NT has no `syscall` instruction: the compiler spills to the stack, so there is no
src/sema/lower/lower.hpp:131:/// @abi argument limit there and the number is resolved by the stdlib instead.
```

### @spec (3)

```
src/codegen/llvm/c_abi/aapcs64.cpp:66:/// @spec https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
src/codegen/llvm/c_abi/sysv_x86_64.cpp:86:/// @spec https://gitlab.com/x86-psABIs/x86-64-ABI
src/codegen/llvm/c_abi/win_x64.cpp:34:/// @spec https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention
```

### @todo (1)

```
include/xlang/mir/mir.h:29:// @todo MIR does not yet carry what only code generation needs (the `asm` template and clobbers,
```

### @cost (1)

```
src/mir/dataflow/forward.hpp:25:/// @cost each block is processed once per change of its entry fact: at most (lattice height) times.
```

Total: 188 tag lines.
<!-- END COLLECTED TAGS -->
