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

### @grammar (13)

```
include/xlang/lang/ast.h:21:/// @grammar the five prefix operators: `-e`, `!e`, `*e`, `&e`, `~e`
include/xlang/lang/ast.h:24:/// @grammar the infix operators: `a + b`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`,
include/xlang/lang/ast.h:25:/// @grammar `&&`, `||`, `&`, `|`, `^`, `<<`, `>>` (Go precedence, `&&`/`||` lowest above `?`)
include/xlang/lang/lexer.h:12:/// @grammar every lexical token the parser can see; the spellings are fixed by `lexer.cpp` and
include/xlang/lang/lexer.h:13:/// @grammar the language grammar in the parser's `@grammar` comments uses these tokens
src/sema/lower/call/builtins.cpp:7:/// @grammar `syscall(nr, args...)`, returning `i64`: a negative result is `-errno`, so the caller checks the sign rather than a separate error out.
src/sema/lower/call/builtins.cpp:68:/// @grammar `__builtin_atomicLoad(p, order)`, `__builtin_atomicStore(p, v, order)`, `__builtin_atomicXchg(p, v, order)`, `__builtin_atomicCas(p, expected, new, order)`, `__builtin_atomicAdd(p, v, order)`.
src/sema/lower/call/builtins.cpp:69:/// @grammar `__builtin_read(p)`, `__builtin_write(p, v)`, `__builtin_drop(p)`, `__builtin_needsDrop<T>()`, `__builtin_fence(order)`, `__builtin_unwindPads()`, `__builtin_unwindPadCount()`, `__builtin_asm(...)`.
src/sema/lower/call/builtins.cpp:290:/// @grammar `__builtin_asm("template", in(reg) x, out("rax") y, inout(reg) z, clobber("memory"), options(nomem|noreturn))`, with `{k}` placeholders numbered by operand order.
src/sema/lower/expr/expr.cpp:115:/// @grammar a string literal is a `str`, or byte by byte into a `[u8; N]` when the context names one; a literal longer than N is E0717.
src/sema/lower/item/roles.cpp:17:/// @grammar `register(panic)`, `register(alloc)`, `register(free)`, `register(unwind)`, `register(args)`: one symbol per role per group, checked structurally against the shape the role needs.
src/sema/lower/item/roles.cpp:129:/// @grammar `@init fn`, `@fini fn`, `@entry fn`: module-level hooks that are not called by any code and are kept by reachability for that reason.
src/sema/sema/const_eval.cpp:325:/// @grammar `sizeOf(T)`, `alignOf(T)` and `offsetOf(T, field)` are the only calls that are compile-time constants; every other call in a constant position is E0707.
```

### @invariant (17)

```
include/xlang/analysis/analysis.h:14:/// @invariant findings are reported sorted by source position, never in the order the passes or functions ran.
include/xlang/mir/mir.h:27:// @invariant MIR is built only from HIR and never refers back to it; a span on every statement and
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

### @ownership (5)

```
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

Total: 54 tag lines.
<!-- END COLLECTED TAGS -->
