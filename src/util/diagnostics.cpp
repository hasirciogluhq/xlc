#include "xlang/util/diagnostics.h"

#include "xlang/util/error.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string_view>
#include <map>
#include <ostream>
#include <tuple>

namespace xlang {

namespace {

constexpr std::size_t kTabWidth = 4;
constexpr std::size_t kMaxLineWidth = 110;
constexpr std::size_t kLeadWidth = 40;

[[nodiscard]] const std::vector<DiagnosticInfo>& registry() {
    static const std::vector<DiagnosticInfo> table = {
        {"E0001", Severity::Error, "lexical error",
         "The source contains a character, literal or escape the lexer cannot read (unknown "
         "character, unterminated string, malformed number, misplaced `#` directive). Fix the "
         "token at the marked position."},
        {"E0002", Severity::Error, "syntax error",
         "The tokens do not form a valid construct at the marked position. The detail line names "
         "what the parser expected. Statements and items end at a line break; an expression may "
         "continue on the next line only inside parentheses/brackets or after a binary operator."},
        {"E0003", Severity::Error, "preprocessor error",
         "A `#error` directive was reached, a conditional-compilation directive is malformed "
         "(missing condition, `#endif`, stray `#else`), or a condition uses an undefined name or "
         "mixes kinds. Use `defined(NAME)` to test for optional definitions."},
        {"E0004", Severity::Error, "macro expansion error",
         "A macro call has the wrong number or kind of arguments, a macro is defined twice, or "
         "expansion is deeper than 64 levels. The labels show the expansion chain."},
        {"W0201", Severity::Warning, "layout of a declared type is unknown, C layout assumed",
         "A `declare`d struct/class/global has no layout record in the linked objects and no entry "
         "in `xl.layout`, so the C layout is assumed. Add `@repr(c)` if the other side is C/C++, "
         "build the other side with xl so a layout record is emitted, or silence it with "
         "`@allow(W0201)` / `--allow=W0201`."},
        {"W0202", Severity::Warning, "declared layout contradicts the recorded layout",
         "The layout written in the `declare` differs from the recorded layout of the same type."},
        {"W0203", Severity::Warning, "layout record uses a different layout scheme",
         "The layout record was produced by another default layout scheme version (for example "
         "v2 vs v3)."},
        {"W0301", Severity::Warning, "declared symbol not resolved in this compilation",
         "The symbol is `declare`d but not defined in this compilation; it will be looked up at "
         "link time."},
        {"E0302", Severity::Error, "type not expressible in C used in `export(c)` or `declare`",
         "`export(c)` and `declare` signatures follow the target's C calling convention and accept "
         "only C-compatible types: integers, `f32/f64`, `bool`, `*T`, `@repr(c)` structs, function "
         "pointers and integer-based enums."},
        {"W0401", Severity::Warning, "same entity imported/exported through two paths",
         "Not an error (it is one entity), but two paths may silently conflict later if one of "
         "them defines its own symbol. Import through a single path or use `import * as ns`."},
        {"E0401", Severity::Error, "different entities bound to the same name",
         "Two imports or exports bind the same name to different entities. Rename one with `as`, "
         "use `except`, or import into a namespace with `import * as ns`."},
        {"E0501", Severity::Error, "no registered `alloc`, `free` or `unwind` function",
         "`new T { }` gets its memory from the function registered with `register(alloc)`, "
         "`delete p` releases it through the function registered with `register(free)`, and "
         "`try` / `catch` runs on the function registered with `register(unwind)`; the needed "
         "one was not found. Register it, or write `new(p) T { }` with memory you already have."},
        {"E0502", Severity::Error, "duplicate `register`",
         "A role (or a global name) was registered twice. At most one registration per role is "
         "allowed per group (a module root is the library group, everything else the program "
         "group); remove the extra registration. A program registration replaces a library "
         "default of the same role."},
        {"E0503", Severity::Error, "`register` role contract not met",
         "The registered symbol does not fit its role: `panic` is `fn(msg: *u8)`, `alloc` is "
         "`fn(size: usize, align: usize) -> *void`, `free` is `fn(p: *void)`, `unwind` is `fn(op: i32, snapshot: *usize, message: *u8)`."},
        {"E0504", Severity::Error, "interface used without `impl`",
         "Interfaces are nominal: a type is usable as an interface only after "
         "`impl Interface for Type`, even when its method names match."},
        {"W0402", Severity::Warning, "local name shadows a registered global",
         "A local definition or import hides a `register`ed global name. The role binding is "
         "unaffected because it is bound by registration, not by name."},
        {"E0601", Severity::Error, "implicit signedness-changing conversion",
         "An implicit conversion changes signedness (`u <-> i`, `u -> f`; floats count as "
         "signed), or a binary operation mixes signedness. Use `as`, or write the expression "
         "inside an `unsafe { }` block."},
        {"E0701", Severity::Error, "constant array index out of bounds",
         "A constant index is outside `[0, N)`. Checked regardless of `--safety` and cannot be "
         "silenced."},
        {"E0702", Severity::Error, "static assertion failed",
         "A `staticAssert` condition evaluated to false at compile time (or was not a bool "
         "constant)."},
        {"E0703", Severity::Error, "type has no known size",
         "`sizeOf`/layout needs a sized type: `void`, opaque `declare` types, types from modules "
         "that are not loaded, and types that contain themselves by value have none."},
        {"E0704", Severity::Error, "cyclic type alias",
         "A `type` alias refers to itself, directly or through other aliases."},
        {"E0705", Severity::Error, "invalid enum or layout attribute",
         "An enum has a non-constant underlying type, a duplicate member, a missing value in a "
         "non-integer enum or a value of the wrong kind; or `@align`/`@layout` has a bad argument."},
        {"E0706", Severity::Error, "unknown type or name",
         "The name is not declared in this module (or is not of the expected kind)."},
        {"E0707", Severity::Error, "not a compile-time constant",
         "A constant expression (array length, enum value, `staticAssert`, `sizeOf` argument) "
         "uses something that cannot be evaluated at compile time."},
        {"E0708", Severity::Error, "type mismatch",
         "A value does not have the type its context needs and no implicit conversion exists "
         "(`as` is required for narrowing, float to integer, pointer casts and so on)."},
        {"E0709", Severity::Error, "invalid operation",
         "An operator, field access, index, call or assignment does not apply to the operand: "
         "unknown field or method, dereferencing a non-pointer, assigning to a non-place, "
         "`&` of a temporary, arithmetic on pointers, and similar."},
        {"E0710", Severity::Error, "wrong number of arguments",
         "A call has a different number of arguments than the callee declares, or a `syscall` "
         "has more arguments than the target allows."},
        {"E0711", Severity::Error, "module or imported name not found",
         "An `import`/`export ... from` names a module that cannot be located, a path that is both "
         "`x.xl` and `x/index.xl`, or a name the module does not export."},
        {"E0712", Severity::Error, "invalid `virtual`/`override` use",
         "A method marked `override` replaces nothing, replaces a method with a different "
         "signature, re-declares an already virtual method without `override`, or hides a "
         "virtual base method with a plain method."},
        {"E0713", Severity::Error, "`impl` does not match its interface",
         "An `impl Interface for Type` lacks a method of the interface, defines a method the "
         "interface does not have, or a method has a different signature."},
        {"E0714", Severity::Error, "code generation failed",
         "The back end could not lower the program: an unresolved external reference, a syscall "
         "form the target does not support, a generated module that failed LLVM verification, or "
         "a module whose back end returned no object without a reason (an internal error that "
         "names the module)."},
        {"E0403", Severity::Error, "ambiguous overloaded call or use",
         "Several overloads of a function match the arguments (or the expected function type) "
         "equally well, so no one is chosen. Give the arguments types that prefer one overload, "
         "or use a typed value instead of an untyped literal."},
        {"E0404", Severity::Error, "no overload matches",
         "No overload of a function takes these arguments (or has the expected function type). "
         "The diagnostic lists the declared signatures. Change the arguments or the call."},
        {"E0405", Severity::Error, "function cannot be overloaded",
         "A name with more than one declaration names a function that cannot be overloaded: `main`, "
         "a `register`ed function, `export(c)` (one C symbol per name) or `declare` (one external "
         "symbol). Keep a single declaration with that name."},
        {"E0715", Severity::Error, "`const` initializer is not a compile-time constant",
         "The value of a `const` (module level or in a block) must be known at compile time: "
         "literals, arithmetic and casts on them, `define` constants, arrays and struct literals "
         "of such values, and `&global`. Calls and reads of other variables or `const`s are not "
         "allowed. A module-level `const` without a type must have a literal value; write "
         "`const NAME: T = ...` otherwise."},
        {"E0716", Severity::Error, "write to a read-only place",
         "A value that cannot change is assigned to: an element or field of a `str` (`s[i] = x`, "
         "`s.len = n`), or the name of a `const`. Copy the bytes into a `[u8; N]` local or a "
         "`String` to get a writable buffer."},
        {"E0717", Severity::Error, "string literal does not fit the array",
         "A string literal initializes a `[u8; N]` but has more than N bytes. Make the array "
         "longer, or use a `str` value instead."},
        {"E0726", Severity::Error, "reserved entry symbol",
         "A symbol named `_start`, `mainCRTStartup` or `_xl_initAll` was requested through `export(c)`, "
         "`@mangle` or `declare`. The compiler generates these symbols to start the program. Rename the "
         "function or global."},
        {"E0727", Severity::Error, "call or value use of an `@init`, `@fini` or `@entry` function",
         "Hook functions are started by the generated code and cannot be called from code or used as a "
         "function value. Move the shared code into a normal function and call or use that."},
        {"E0729", Severity::Error, "invalid `@init` or `@fini` function",
         "`@init` and `@fini` mark a module-level function with no generics, no parameters, no return type "
         "and a body. They are not allowed on methods or on other kinds of declarations."},
        {"E0720", Severity::Error, "move out of a field, element or pointer",
         "A value that needs drop was moved out of a class field, an array element or a dereferenced "
         "pointer. Only locals and parameters can give up ownership. Take its address or copy it "
         "through a `clone()` method."},
        {"E0721", Severity::Error, "value that needs drop in a place that cannot own it",
         "A global, a struct field, a local or a parameter has a type that needs drop, or an array "
         "whose element type needs drop, or a payload enum holds such a value. Structs and globals "
         "are plain data, arrays do not drop their elements, and a payload enum needs every payload "
         "type to be droppable through its variant. Use a class field or a local of a class type."},
        {"E0722", Severity::Error, "invalid `init` or `drop` method",
         "An `init` method takes no return type, and a `drop` method takes only `self`, returns "
         "nothing and cannot be virtual. A class whose `init` needs a base constructor requires that "
         "base to have an `init` without arguments."},
        {"E0723", Severity::Error, "literal for a class or struct with an `init`",
         "A class with an `init` cannot be built with a field literal, because the literal would skip "
         "the constructor. Build it with `T(...)` or allocate it with `new T(...)`."},
        {"E0724", Severity::Error, "wrong or missing type arguments",
         "A generic declaration (`fn`, `struct`, `class`, `interface`, `enum` or `type`) needs one type "
         "argument per type parameter. A non-generic declaration takes none, and a generic function "
         "value needs its type arguments written out."},
        {"E0725", Severity::Error, "generic instantiation too deep",
         "The type arguments of a generic instance nest more than 64 levels. Use a simpler type "
         "argument."},
        {"E0730", Severity::Error, "generic method cannot be virtual",
         "A method with its own type parameters cannot be `virtual` or `override`: a vtable slot "
         "has one fixed signature. Remove the `virtual`/`override` or the type parameters."},
        {"E0731", Severity::Error, "conflicting impls",
         "Two `impl` blocks give the same interface for the same type, after generic arguments are "
         "filled in. Keep one of them."},
        {"E0732", Severity::Error, "impl type parameter not in the target",
         "Every type parameter of a generic `impl` must be a direct type argument of its target, so "
         "that the target type fixes it. Write `impl<T> Iface for Target<T>`."},
        {"E0733", Severity::Error, "generic method outside a class",
         "Generic methods are monomorphized per class, so only methods of a `class` (not of an "
         "`impl` or an `interface`) can have their own type parameters. Move the method into the "
         "class, or make it a generic free function."},
        {"E0734", Severity::Error, "invalid parameter pack",
         "A `..name` parameter takes the arguments after the fixed parameters, so it must be the "
         "last parameter, and a function can have only one. Remove the extra pack or move it to "
         "the end of the parameter list."},
        {"E0735", Severity::Error, "parameter pack used as a value",
         "A parameter pack is not a value. Read its count with `name.len`, iterate it with "
         "`for x in name { }`, or pass it on with `..name` in a call. Nothing else may use it."},
        {"E0736", Severity::Error, "parameter pack not allowed here",
         "Only a free function with a body can take a parameter pack. It cannot be `declare`d, "
         "`export(c)`, `virtual` or `override`, have type parameters, or be a method of a class, "
         "impl or interface."},
        {"E0737", Severity::Error, "spread outside a pack call",
         "`..name` is valid only as an argument of a call, and only when `name` is the parameter "
         "pack of the enclosing function. Pass the pack's elements to a function that takes them, "
         "or write the arguments out."},
        {"E0738", Severity::Error, "parameter pack indexed",
         "A parameter pack has no index operator, because its elements have different types. "
         "Iterate it with `for x in name { }`, or pass it on with `..name`."},
        {"E0739", Severity::Error, "executable without an entry point",
         "`--emit=exe` (and `xl run`) needs exactly one `@entry` function with a body: the compiler "
         "generates the program start code around it. A program without `@entry` cannot be linked as an "
         "executable. Mark `@entry fn main()`, or build the file with `--emit=obj`, `--emit=static` or "
         "`--emit=shared`."},
        {"E0740", Severity::Error, "invalid xlang.toml or xlang.lock",
         "The project manifest or lock file cannot be read or does not follow the format: a TOML syntax "
         "error, an unknown key, a dependency that is not named by a repository path such as "
         "`github.com/user/repo`, or a `[[native]]` entry without `link`. Fix the reported line; delete "
         "xlang.lock and run `xl install` if the lock is damaged."},
        {"E0741", Severity::Error, "package not installed",
         "A package that the build needs is not in `.xlang/cache`, or the cached copy does not match the "
         "commit in xlang.lock. Builds never use the network: run `xl install` once."},
        {"E0742", Severity::Error, "invalid version tag",
         "A dependency version must be `latest` (projects only) or a release tag: `vMAJOR.MINOR.PATCH` or "
         "`MAJOR.MINOR.PATCH`, for example `v1.2.0`. Pre-release (`-rc.1`) and `-test` tags are never "
         "chosen as `latest` but install when named explicitly (`repo@v1.2.0-test`). A package that other "
         "projects depend on must pin exact tags for its own dependencies."},
        {"E0743", Severity::Error, "conflicting package versions",
         "Two packages need different versions of the same repository and the project does not choose. "
         "There is one copy of each package per build. Name the version you want in the `[dependencies]` "
         "table of your own xlang.toml; the project's choice wins."},
        {"E0744", Severity::Error, "package download failed",
         "`git` could not list or download a repository, the repository has no release tag, or a tag "
         "moved to another commit than the one recorded in xlang.lock. Release tags must never move."},
        {"E0745", Severity::Error, "import of an undeclared package",
         "The import path looks like a repository path (`github.com/user/repo/...`) but no file matches "
         "and no `[dependencies]` entry of xlang.toml is a prefix of it. Run `xl install "
         "<repository>` to add it."},
        {"E0746", Severity::Error, "native file missing",
         "A `[[native]]` entry lists a file that does not exist in the package at the installed version. "
         "Fix the `link` list, or commit the file."},
        {"E0747", Severity::Error, "payload enum mixed with constant values",
         "An enum with payload variants (`Add(*Expr, *Expr)`) is a sum type: its tag is numbered by "
         "position. It cannot have an underlying type (`: T`) or `= value` members. Use two enums: "
         "one with constants, one with payloads."},
        {"E0749", Severity::Error, "invalid case of a payload enum switch",
         "A `case` of a `switch` on a payload enum must name variants of that enum: `case E.A(x, y) { }` "
         "binds the payload (one name per value, `_` to ignore, alone in the case), `case E.A, E.B { }` "
         "matches without binding."},
        {"E0750", Severity::Error, "switch does not cover every variant",
         "A `switch` on a payload enum without `default` must have a `case` for every variant. Add the "
         "missing cases or a `default`."},
        {"E0751", Severity::Error, "wrong number of payload values",
         "A payload variant is built like a call, `E.A(x, y)`, with exactly as many values as its "
         "payload has; a variant without data is written `E.B`."},
        {"E0752", Severity::Error, "invalid `@entry` function",
         "`@entry` marks a module-level function with no generics and a body, and a signature of no parameters "
         "or exactly `argc: i32` and `argv: **u8` in that order, returning void or i32. Methods and other "
         "declarations cannot be the entry."},
        {"E0753", Severity::Error, "more than one `@entry` function",
         "A program has exactly one `@entry` function. Remove the second `@entry` attribute, or move the code "
         "into an `@init` function or a normal function that the entry calls."},
        {"E0754", Severity::Error, "`@entry` in a static library or object file",
         "Static libraries and object files have no entry point: the program that links them provides it. "
         "Remove `@entry`, use `@init` for startup work and `@fini` for teardown."},
        {"E0755", Severity::Error, "`@entry` arguments the target cannot pass",
         "The `argc`/`argv` form of `@entry` is available for executables. Shared libraries take "
         "`@entry fn name()` with no parameters. A Windows program is started with a command-line string only, "
         "so the arguments come from the `register(args)` function (`fn(argc: *i32) -> **u8`); the standard "
         "library registers one, and a program built without it registers its own."},
        {"E0756", Severity::Error, "cannot infer the type arguments of a generic payload enum",
         "`Result.Ok(v)` names a variant of every instance of the enum, so the compiler needs the "
         "instance to build. Write the target type where the value is created, as in "
         "`local r: Result<i32, E> = Result.Ok(v)`."},
        {"E0757", Severity::Error, "`case` names a variant of another enum",
         "Every `case` of a `switch` on a payload enum must name a variant of that same enum. Check the "
         "enum in front of the variant name, and write the variants of the enum being switched on."},
        {"E0758", Severity::Error, "payload enum needs drop in a place that cannot own it",
         "An enum with a droppable payload owns that value and drops it with the live variant, so it "
         "is a local or a class field. It cannot be a global or a struct field, because those hold "
         "plain data and drop nothing. Use a local, a class field or a `Vec<T>`."},
        {"E0759", Severity::Error, "array of a payload enum that needs drop",
         "An array of an enum whose payload needs drop cannot own its elements, because arrays do not "
         "drop their elements. Use a `Vec<T>`, whose elements are dropped when it is dropped."},
        {"E0760", Severity::Error, "stack object returned as an interface",
         "An interface value is a fat pointer that borrows its object and owns nothing. Returning one "
         "whose object is a local, a parameter or a temporary of the function leaves it pointing at a "
         "frame that is gone. This is always an error and cannot be allowed. Allocate the object with "
         "`new` and return the pointer: `local p := new CProvider()` then `return p`."},
        {"E0761", Severity::Error, "`?` in a place that cannot return",
         "`e?` returns the failure from the enclosing function. A `defer` body runs while the "
         "block is being left, after the function's result is decided, so it has nothing to return "
         "to and `?` is refused there. Handle the failure in the `defer` with `switch` or `if`."},
        {"E0762", Severity::Error, "`?` on a value that is not a success-or-failure enum",
         "`e?` needs an enum of two variants: a success carrying one value and a failure carrying at "
         "most one (`Ok(T)` / `Err(E)`), or `None` / `Some(T)`. `?` gives the success value, or "
         "returns the failure from the function."},
        {"E0763", Severity::Error, "`?` cannot return its failure from this function",
         "`e?` returns the failure of `e` from the function, so the function must return an enum of "
         "the same shape whose failure carries the same type (there is no conversion between error "
         "types). Match the error type, or handle the failure with a `switch`."},
        {"E0764", Severity::Error, "missing return",
         "Control can reach the end of a function that has a return type: some path has no "
         "`return` (a loop that ends with `break`, an `if` without `else`, ...). A `-> never` "
         "function must not reach its end at all. End every path with `return`, an endless loop "
         "or a call to a `-> never` function. A function without `->` returns `void` and needs no "
         "`return`."},
        {"E0765", Severity::Error, "invalid use of `never`",
         "`never` is the return type of a function that does not return (it exits, traps or loops "
         "forever). It is only written after `->`; a `-> never` function has no `return`, and a "
         "call to it has no value."},
        {"E0766", Severity::Error, "drop through a base pointer would skip a derived drop",
         "`delete p` and `__builtin_drop(p)` with `p: *Base` run the drop of `Base` only when "
         "`Base` has no virtual method, but a class derived from `Base` has its own `drop` or a "
         "field that needs drop, and that part would never run. Give `Base` a virtual method: a "
         "polymorphic class drops through its vtable, so the drop of the object's own class runs. "
         "Or drop through a pointer to the derived class."},
        {"E0770", Severity::Error, "closure parameter type unknown",
         "A closure parameter without a type takes it from the `fn(...)` type the closure is used "
         "for: an argument, a typed `local`, a field or a `return`. Here nothing names one. Write "
         "the type (`|x: i32| x * 2`), or give the closure a `fn(...)` context "
         "(`local f: fn(i32) -> i32 = |x| x * 2`)."},
        {"E0771", Severity::Error, "closure parameter count does not match",
         "The closure is used where a `fn(...)` type is expected, and the type has a different "
         "number of parameters than the closure. Write as many parameters as the type has."},
        {"E0773", Severity::Error, "closure with captures where a function pointer is expected",
         "A closure that uses variables of the enclosing function is a struct of those variables, "
         "and a `fn(...)` pointer has no place to keep them. Take the closure as a generic "
         "parameter (`fn apply<F>(f: F, x: i32)`), or pass the variable as an argument."},
        {"E0774", Severity::Error, "`move` closure captures a value that needs drop",
         "`move` copies the captured variables into the closure's struct, which is plain data and "
         "cannot own a value that needs drop. Borrow the variable (write the closure without "
         "`move`)."},
        {"E0775", Severity::Error, "type cannot be iterated",
         "`for x in value` over a struct or class needs `fn next(self: *T) -> Option<U>` (the "
         "value is its own iterator) or `fn iter(self: *T) -> I` where `I` has `next`. The type "
         "has neither."},
        {"E0776", Severity::Error, "`iter` does not return an iterator",
         "`iter` must return a struct or class that has `next`; a type that only has `iter` gives "
         "the loop nothing to call. Add `next` to the returned type."},
        {"E0777", Severity::Error, "`next` cannot end a loop",
         "`next` returns an enum with two variants, one carrying the item and one without data "
         "(`Some(T)` / `None`); the variant without data ends the loop. A `Result` or any other "
         "type has no end."},
        {"E0778", Severity::Error, "C-convention function that passes a struct used as a value",
         "A call through a `fn(...)` pointer passes a struct as the address of a copy (the xlang "
         "convention). An `export(c)` or `declare` function that takes or returns a struct by "
         "value follows the C convention, so it can only be called by name. Call it directly, or "
         "change it to take and return the struct through a pointer."},
        {"W0770", Severity::Warning, "`next` takes `self` by value",
         "Each call of `next` works on a copy of the iterator, so its position never advances and "
         "the loop does not end. Write `fn next(self: *T)`."},
        {"W0771", Severity::Warning, "`for` uses `iter`, not `next`",
         "The type has both `iter` and `next`. `for` calls `iter` and starts from the beginning "
         "every time; the position `next` keeps on the value itself is not used."},
        {"W0601", Severity::Warning, "implicit signed integer to float conversion",
         "A signed integer is implicitly converted to `f32`/`f64` and precision may be lost. "
         "Write `as f64`, or silence with `@allow(W0601)` / `--allow=W0601`."},
    };
    return table;
}

[[nodiscard]] std::string jsonEscape(std::string_view text) {
    std::string out;
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            } else {
                out += c;
            }
        }
    }
    return out;
}

[[nodiscard]] std::size_t digits(std::size_t value) {
    std::size_t n = 1;
    while (value >= 10) {
        value /= 10;
        ++n;
    }
    return n;
}

/// One display column per cell: a code point, or one space of an expanded tab.
struct DisplayLine {
    std::vector<std::string> cells;
    std::vector<std::size_t> byte_to_cell;  ///< size = bytes + 1

    [[nodiscard]] std::size_t cellOf(std::size_t byte_index) const {
        return byte_to_cell[std::min(byte_index, byte_to_cell.size() - 1)];
    }
};

[[nodiscard]] DisplayLine expand(std::string_view line) {
    DisplayLine out;
    out.byte_to_cell.assign(line.size() + 1, 0);
    for (std::size_t i = 0; i < line.size();) {
        const auto lead = static_cast<unsigned char>(line[i]);
        std::size_t len = 1;
        if (lead >= 0xF0) {
            len = 4;
        } else if (lead >= 0xE0) {
            len = 3;
        } else if (lead >= 0xC0) {
            len = 2;
        }
        len = std::min(len, line.size() - i);
        for (std::size_t k = 0; k < len; ++k) {
            out.byte_to_cell[i + k] = out.cells.size();
        }
        if (line[i] == '\t') {
            for (std::size_t k = 0; k < kTabWidth; ++k) {
                out.cells.emplace_back(" ");
            }
        } else {
            out.cells.emplace_back(line.substr(i, len));
        }
        i += len;
    }
    out.byte_to_cell[line.size()] = out.cells.size();
    return out;
}

struct Mark {
    const Label* label{nullptr};
    std::size_t first_cell{0};  ///< display columns on this line
    std::size_t last_cell{0};   ///< exclusive
};

struct Shown {
    std::string text;
    std::ptrdiff_t shift{0};  ///< display column of a cell in `text` = cell + shift
    std::size_t width{0};
};

[[nodiscard]] Shown show(const DisplayLine& line, std::size_t anchor) {
    Shown out;
    std::size_t begin = 0;
    std::size_t end = line.cells.size();
    bool lead_cut = false;
    bool tail_cut = false;
    if (line.cells.size() > kMaxLineWidth) {
        if (anchor > kLeadWidth) {
            begin = std::min(anchor - kLeadWidth, line.cells.size() - kMaxLineWidth);
        }
        end = begin + kMaxLineWidth;
        lead_cut = begin > 0;
        tail_cut = end < line.cells.size();
    }
    if (lead_cut) {
        out.text += "\xE2\x80\xA6";
    }
    for (std::size_t i = begin; i < end; ++i) {
        out.text += line.cells[i];
    }
    if (tail_cut) {
        out.text += "\xE2\x80\xA6";
    }
    out.shift = static_cast<std::ptrdiff_t>(lead_cut ? 1 : 0) - static_cast<std::ptrdiff_t>(begin);
    out.width = (end - begin) + (lead_cut ? 1 : 0) + (tail_cut ? 1 : 0);
    return out;
}

void appendBlock(std::string& out, std::string_view tag, const std::string& text) {
    if (text.empty()) {
        return;
    }
    out += tag;
    for (const char c : text) {
        out += c;
        if (c == '\n') {
            out += "        ";
        }
    }
    out += '\n';
}

/// ANSI-colors the plain text `renderText` produces, line by line: the header, the `-->` locations,
/// the gutter, the `<=======` marker and carets (error red, warning yellow), and the `note:`,
/// `detail:`, `reason:`, `fix:` tags. The plain text stays the source of truth.
std::string colorize(const std::string& text, Severity severity) {
    constexpr std::string_view reset = "\x1b[0m";
    constexpr std::string_view blue = "\x1b[1;34m";
    constexpr std::string_view cyan = "\x1b[1;36m";
    constexpr std::string_view green = "\x1b[1;32m";
    constexpr std::string_view bold = "\x1b[1m";
    const std::string_view accent = severity == Severity::Error ? "\x1b[1;31m" : "\x1b[1;33m";

    std::string out;
    std::size_t start = 0;
    bool first = true;
    while (start <= text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string_view line(text.data() + start, end - start);
        start = end + 1;
        if (!first) {
            out += '\n';
        }

        const std::size_t indent = line.find_first_not_of(' ');
        const std::string_view body = indent == std::string_view::npos ? std::string_view{} : line.substr(indent);
        const auto tagged = [&](std::string_view tag, std::string_view color) {
            if (!body.starts_with(tag)) {
                return false;
            }
            out += line.substr(0, indent);
            out += color;
            out += tag;
            out += reset;
            out += body.substr(tag.size());
            return true;
        };

        if (first) {
            first = false;
            const std::size_t close = line.find(']');
            if (close != std::string_view::npos) {
                out += accent;
                out += line.substr(0, close + 1);
                out += reset;
                out += bold;
                out += line.substr(close + 1);
                out += reset;
                continue;
            }
        }
        if (tagged("-->", blue) || tagged(":::", blue) || tagged("note:", cyan) ||
            tagged("detail:", cyan) || tagged("reason:", cyan) || tagged("fix:", green) ||
            tagged("...", blue)) {
            continue;
        }

        // Gutter: optional spaces, optional line number, spaces, then `|`.
        std::size_t i = 0;
        while (i < line.size() && line[i] == ' ') {
            ++i;
        }
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
            ++i;
        }
        while (i < line.size() && line[i] == ' ') {
            ++i;
        }
        if (i < line.size() && line[i] == '|') {
            out += blue;
            out += line.substr(0, i + 1);
            out += reset;
            const std::string_view rest = line.substr(i + 1);
            const std::size_t marker = rest.rfind("<=======");
            const std::size_t caret = rest.find_first_not_of(' ');
            if (marker != std::string_view::npos) {
                out += rest.substr(0, marker);
                out += accent;
                out += rest.substr(marker);
                out += reset;
            } else if (caret != std::string_view::npos && rest[caret] == '^') {
                out += rest.substr(0, caret);
                out += accent;
                out += rest.substr(caret);
                out += reset;
            } else {
                out += rest;
            }
            continue;
        }
        out += line;
    }
    return out;
}

}  // namespace

const DiagnosticInfo* findDiagnosticInfo(std::string_view code) {
    for (const DiagnosticInfo& info : registry()) {
        if (info.code == code) {
            return &info;
        }
    }
    return nullptr;
}

const std::vector<DiagnosticInfo>& allDiagnosticInfos() {
    return registry();
}

DiagnosticEngine::DiagnosticEngine(const SourceManager& sources, std::ostream& out)
    : sources_(sources), out_(out) {}

void DiagnosticEngine::configure(const DiagnosticOptions& options) {
    options_ = options;
    allow_.clear();
    deny_.clear();
    for (const std::string& code : options.allow) {
        const DiagnosticInfo* info = findDiagnosticInfo(code);
        if (info == nullptr) {
            throw XlangError(std::format("unknown diagnostic code `{}`", code));
        }
        if (info->severity == Severity::Error) {
            throw XlangError(std::format("`{}` is an error and cannot be allowed", code));
        }
        allow_.insert(code);
    }
    for (const std::string& code : options.deny) {
        if (findDiagnosticInfo(code) == nullptr) {
            throw XlangError(std::format("unknown diagnostic code `{}`", code));
        }
        deny_.insert(code);
    }
    if (!options.log_path.empty()) {
        log_.emplace(options.log_path, std::ios::trunc);
        if (!*log_) {
            throw XlangError(std::format("cannot open diagnostic log `{}`", options.log_path));
        }
    }
}

void DiagnosticEngine::addAllowScope(const Span& scope, const std::vector<std::string>& codes) {
    AllowScope entry;
    entry.scope = scope;
    entry.codes.insert(codes.begin(), codes.end());
    scopes_.push_back(std::move(entry));
}

bool DiagnosticEngine::isAllowed(const Diagnostic& diagnostic) const {
    if (diagnostic.severity != Severity::Warning || options_.warn_all) {
        return false;
    }
    if (options_.allow_all || allow_.contains(diagnostic.code)) {
        return true;
    }
    if (diagnostic.labels.empty()) {
        return false;
    }
    const Span& primary = diagnostic.labels.front().span;
    return std::ranges::any_of(scopes_, [&](const AllowScope& scope) {
        return scope.codes.contains(diagnostic.code) && scope.scope.contains(primary);
    });
}

void DiagnosticEngine::report(Diagnostic diagnostic) {
    if (quiet_ > 0) {
        return;
    }
    if (const DiagnosticInfo* info = findDiagnosticInfo(diagnostic.code)) {
        diagnostic.severity = info->severity;
    }
    diagnostic.suppressed = isAllowed(diagnostic);
    if (!diagnostic.suppressed && diagnostic.severity == Severity::Warning &&
        deny_.contains(diagnostic.code)) {
        diagnostic.severity = Severity::Error;
    }

    if (!diagnostic.suppressed) {
        if (diagnostic.severity == Severity::Error) {
            ++errors_;
        } else {
            ++warnings_;
        }
    }
    pending_.push_back(std::move(diagnostic));
}

void DiagnosticEngine::flush() {
    std::vector<Diagnostic> queue = std::move(pending_);
    pending_.clear();
    const auto key = [](const Diagnostic& d) {
        if (d.labels.empty() || !d.labels.front().span.valid()) {
            return std::tuple<FileId, std::uint32_t, std::uint32_t>(kNoFile, 0, 0);
        }
        const Span& s = d.labels.front().span;
        return std::tuple<FileId, std::uint32_t, std::uint32_t>(s.file, s.line, s.column);
    };
    std::ranges::stable_sort(queue, [&](const Diagnostic& a, const Diagnostic& b) {
        return key(a) < key(b);
    });

    for (const Diagnostic& diagnostic : queue) {
        const std::string text = options_.format == DiagFormat::Json ? renderJson(diagnostic)
                                                                     : renderText(diagnostic);
        if (!diagnostic.suppressed) {
            const bool colored = options_.color && options_.format == DiagFormat::Text;
            out_ << (colored ? colorize(text, diagnostic.severity) : text) << "\n";
        }
        if (!log_) {
            continue;
        }
        // `--diag-log-level`: all/warn keep warnings (suppressed included) and errors.
        if (options_.log_level == DiagLogLevel::Error && diagnostic.severity != Severity::Error) {
            continue;
        }
        *log_ << text << "\n";
    }
    if (log_) {
        log_->flush();
    }
    out_.flush();
}

void DiagnosticEngine::error(std::string code, const Span& span, std::string message,
                             std::string label, std::vector<Note> notes) {
    Diagnostic d;
    d.code = std::move(code);
    d.severity = Severity::Error;
    d.message = std::move(message);
    if (span.valid()) {
        d.labels.push_back({span, std::move(label)});
    }
    d.notes = std::move(notes);
    report(std::move(d));
}

void DiagnosticEngine::warning(std::string code, const Span& span, std::string message,
                               std::string label) {
    Diagnostic d;
    d.code = std::move(code);
    d.severity = Severity::Warning;
    d.message = std::move(message);
    if (span.valid()) {
        d.labels.push_back({span, std::move(label)});
    }
    report(std::move(d));
}

std::string DiagnosticEngine::renderText(const Diagnostic& diagnostic) const {
    std::string out;
    out += std::format("{}[{}]{}: {}\n", diagnostic.severity == Severity::Error ? "error" : "warning",
                       diagnostic.code, diagnostic.suppressed ? " (suppressed)" : "",
                       diagnostic.message);

    const std::size_t context =
        diagnostic.severity == Severity::Error ? options_.context_errors : options_.context_warnings;

    // Group labels by file, primary file first.
    std::vector<FileId> file_order;
    std::map<FileId, std::vector<const Label*>> by_file;
    for (const Label& label : diagnostic.labels) {
        if (!label.span.valid()) {
            continue;
        }
        if (!by_file.contains(label.span.file)) {
            file_order.push_back(label.span.file);
        }
        by_file[label.span.file].push_back(&label);
    }

    // Gutter width: largest line number that can be displayed.
    std::size_t max_line = 1;
    for (const auto& [file, labels] : by_file) {
        const std::size_t count = sources_.file(file).lineCount();
        for (const Label* label : labels) {
            max_line = std::max<std::size_t>(max_line,
                                             std::min<std::size_t>(count, label->span.end_line + context));
        }
    }
    const std::size_t width = digits(max_line);
    const std::string blank_gutter(width, ' ');

    bool first_group = true;
    for (const FileId file : file_order) {
        const SourceFile& source = sources_.file(file);
        const std::vector<const Label*>& labels = by_file[file];
        const Span& head = labels.front()->span;

        out += std::format("{}{} {}:{}:{}\n", blank_gutter, first_group ? "-->" : ":::",
                           source.path().string(), head.line, head.column);
        first_group = false;
        out += std::format("{} |\n", blank_gutter);

        // Marks per line (start line and, for multi-line spans, end line).
        std::map<std::size_t, std::vector<Mark>> marks;
        std::map<std::size_t, DisplayLine> displays;
        const auto display = [&](std::size_t line) -> const DisplayLine& {
            auto it = displays.find(line);
            if (it == displays.end()) {
                it = displays.emplace(line, expand(source.line(line))).first;
            }
            return it->second;
        };
        for (const Label* label : labels) {
            const Span& s = label->span;
            const DisplayLine& start_line = display(s.line);
            Mark start;
            start.label = label;
            start.first_cell = start_line.cellOf(s.column - 1);
            if (s.end_line == s.line) {
                start.last_cell = std::max(start.first_cell + 1, start_line.cellOf(s.end_column - 1));
            } else {
                start.last_cell = std::max(start.first_cell + 1, start_line.cells.size());
            }
            marks[s.line].push_back(start);
            if (s.end_line != s.line) {
                const DisplayLine& end_line = display(s.end_line);
                Mark end;
                end.label = label;
                end.first_cell = end_line.cellOf(s.end_column > 1 ? s.end_column - 2 : 0);
                end.last_cell = end.first_cell + 1;
                marks[s.end_line].push_back(end);
            }
        }

        // Context ranges, merged when they touch or overlap.
        std::vector<std::pair<std::size_t, std::size_t>> ranges;
        for (const auto& [line, line_marks] : marks) {
            (void)line_marks;
            const std::size_t lo = line > context ? line - context : 1;
            const std::size_t hi = std::min<std::size_t>(source.lineCount(), line + context);
            if (!ranges.empty() && lo <= ranges.back().second + 1) {
                ranges.back().second = std::max(ranges.back().second, hi);
            } else {
                ranges.emplace_back(lo, hi);
            }
        }

        // Render every line first to align the `<=======` column across the file block.
        struct Row {
            std::size_t number;
            Shown shown;
            const std::vector<Mark>* line_marks;
        };
        std::vector<std::vector<Row>> blocks;
        std::size_t marker_column = 0;
        for (const auto& [lo, hi] : ranges) {
            std::vector<Row> rows;
            for (std::size_t line = lo; line <= hi; ++line) {
                Row row;
                row.number = line;
                const auto found = marks.find(line);
                row.line_marks = found != marks.end() ? &found->second : nullptr;
                const DisplayLine& dl = display(line);
                row.shown = show(dl, row.line_marks ? row.line_marks->front().first_cell : 0);
                if (row.line_marks != nullptr) {
                    marker_column = std::max(marker_column, row.shown.width);
                }
                rows.push_back(std::move(row));
            }
            blocks.push_back(std::move(rows));
        }

        bool first_block = true;
        for (const std::vector<Row>& rows : blocks) {
            if (!first_block) {
                out += std::format("{}...\n", blank_gutter);
            }
            first_block = false;
            for (const Row& row : rows) {
                std::string line_out =
                    std::format("{:>{}} | {}", row.number, width, row.shown.text);
                if (row.line_marks != nullptr) {
                    line_out.append(marker_column - row.shown.width + 4, ' ');
                    line_out += "<=======";
                    const std::string& text = row.line_marks->front().label->text;
                    if (!text.empty()) {
                        line_out += " " + text;
                    }
                }
                out += line_out;
                out += '\n';
                if (row.line_marks != nullptr && row.line_marks->size() > 1) {
                    for (const Mark& mark : *row.line_marks) {
                        const std::ptrdiff_t start =
                            static_cast<std::ptrdiff_t>(mark.first_cell) + row.shown.shift;
                        std::string carets = std::format("{} | ", blank_gutter);
                        carets.append(static_cast<std::size_t>(std::max<std::ptrdiff_t>(0, start)), ' ');
                        carets.append(std::max<std::size_t>(1, mark.last_cell - mark.first_cell), '^');
                        if (!mark.label->text.empty()) {
                            carets += " " + mark.label->text;
                        }
                        out += carets;
                        out += '\n';
                    }
                }
            }
        }
        out += std::format("{} |\n", blank_gutter);
    }

    for (const Note& note : diagnostic.notes) {
        out += std::format("note: {}\n", note.message);
        if (note.span.valid()) {
            out += std::format("  --> {}:{}:{}\n", sources_.file(note.span.file).path().string(),
                               note.span.line, note.span.column);
        }
    }
    appendBlock(out, "detail: ", diagnostic.detail);
    appendBlock(out, "reason: ", diagnostic.reason);
    appendBlock(out, "fix:    ", diagnostic.fix);
    if (!out.empty() && out.back() == '\n') {
        out.pop_back();
    }
    return out;
}

std::string DiagnosticEngine::renderJson(const Diagnostic& diagnostic) const {
    std::string out = "{";
    out += std::format("\"code\":\"{}\",\"severity\":\"{}\",\"suppressed\":{},\"message\":\"{}\"",
                       jsonEscape(diagnostic.code),
                       diagnostic.severity == Severity::Error ? "error" : "warning",
                       diagnostic.suppressed ? "true" : "false", jsonEscape(diagnostic.message));
    out += ",\"labels\":[";
    bool first = true;
    for (const Label& label : diagnostic.labels) {
        if (!label.span.valid()) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        first = false;
        out += std::format(
            "{{\"file\":\"{}\",\"line\":{},\"column\":{},\"endLine\":{},\"endColumn\":{},"
            "\"text\":\"{}\"}}",
            jsonEscape(sources_.file(label.span.file).path().string()), label.span.line,
            label.span.column, label.span.end_line, label.span.end_column, jsonEscape(label.text));
    }
    out += "]";
    out += ",\"notes\":[";
    for (std::size_t i = 0; i < diagnostic.notes.size(); ++i) {
        const Note& note = diagnostic.notes[i];
        out += std::format("{}{{\"file\":\"{}\",\"line\":{},\"column\":{},\"message\":\"{}\"}}",
                           i == 0 ? "" : ",",
                           jsonEscape(sources_.file(note.span.file).path().string()), note.span.line,
                           note.span.column, jsonEscape(note.message));
    }
    out += "]";
    out += std::format(",\"detail\":\"{}\",\"reason\":\"{}\",\"fix\":\"{}\"}}",
                       jsonEscape(diagnostic.detail), jsonEscape(diagnostic.reason),
                       jsonEscape(diagnostic.fix));
    return out;
}

std::string DiagnosticEngine::explain(std::string_view code) {
    const DiagnosticInfo* info = findDiagnosticInfo(code);
    if (info == nullptr) {
        return {};
    }
    return std::format("{} ({}): {}\n\n{}\n", info->code,
                       info->severity == Severity::Error ? "error" : "warning", info->title,
                       info->explanation);
}

}  // namespace xlang
