#pragma once

#include "xlang/lang/lexer.h"
#include "xlang/lang/types.h"
#include "xlang/util/source.h"

#include <cstdint>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace xlang {

// AST nodes are tagged structs (Kind) with static factory helpers; declarations are a variant
// of structs deriving from `DeclBase`. Types live in `types.h` (`Type` tree).

/// @grammar the five prefix operators: `-e`, `!e`, `*e`, `&e`, `~e`
enum class UnaryOp { Neg, Not, Deref, AddrOf, BitNot };

/// @grammar the infix operators: `a + b`, `-`, `*`, `/`, `%`, `==`, `!=`, `<`, `<=`, `>`, `>=`,
/// @grammar `&&`, `||`, `&`, `|`, `^`, `<<`, `>>` (Go precedence, `&&`/`||` lowest above `?`)
enum class BinaryOp {
    Add, Sub, Mul, Div, Mod, Eq, Ne, Lt, Le, Gt, Ge, And, Or,
    BitAnd, BitOr, BitXor, Shl, Shr
};

[[nodiscard]] const char* unaryOpName(UnaryOp op);
[[nodiscard]] const char* binaryOpName(BinaryOp op);

/// `==`, `!=`, `<`, `<=`, `>`, `>=`: the operators whose user functions borrow their operands.
[[nodiscard]] constexpr bool isComparison(BinaryOp op) {
    return op >= BinaryOp::Eq && op <= BinaryOp::Ge;
}

/// `@name`, `@name(arg, key = value, ...)`.
struct AttributeArg {
    enum class Kind { Ident, Int, String } kind{Kind::Ident};
    std::string key;   ///< empty for positional arguments
    std::string text;  ///< identifier or string value
    std::int64_t number{0};
    Span span;
};

struct Attribute {
    std::string name;
    std::vector<AttributeArg> args;
    Span span;
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;
struct ClosureData;

struct FieldInit {
    std::string name;
    ExprPtr value;
    Span span;
};

struct Expr {
    enum class Kind {
        IntLiteral,
        FloatLiteral,
        BoolLiteral,
        StringLiteral,
        ByteLiteral,   ///< `b')'`: one byte, typed `u8`; int_value holds the byte
        Name,          ///< identifier, including `self`
        Unary,
        Binary,
        Call,          ///< lhs(args...)
        Index,         ///< lhs[rhs]
        Slice,         ///< `base[lo..hi]` / `base[lo..]`: a view of the bytes `lo` up to `hi`
        Field,         ///< lhs.text
        Cast,          ///< lhs as type
        ArrayList,     ///< [args...]
        ArrayRepeat,   ///< [lhs; rhs]
        TupleLiteral,  ///< (args...): a tuple value; `()` has no arguments, `(x,)` one
        StructLiteral, ///< type { fields }
        New,           ///< new T { fields } / new(lhs) T { fields }; lhs = memory pointer (placement); null = the `register(alloc)` function
        Ternary,       ///< lhs ? rhs : args[0]
        Try,           ///< `lhs?`: the value of a success variant, or an early `return` of the failure
        MacroCall,     ///< text!( macro_args... ), arguments kept as raw token lists
        Spread,        ///< `..name` as a call argument: expands a parameter pack (text = pack name)
        AsmOperand,    ///< `in(reg) x` / `out("rax") y` / `inout(reg) z` inside `__builtin_asm(...)`:
                       ///< text = in|out|inout, lhs = register spec, rhs = operand
        TypeOperand,   ///< a slice type written where an argument is expected, `sizeOf([]i32)`: `type`
                       ///< is the whole type (`[]T` cannot be told from an array literal by the name
                       ///< path that carries the other type arguments)
        Closure,       ///< `|a: i32, b| -> i32 { ... }` / `|x| x + 1` / `move |x| ...`: see `closure`
    } kind{Kind::IntLiteral};

    Span span;
    std::uint64_t int_value{0};
    double float_value{0.0};
    bool bool_value{false};
    /// Name: identifier. Field: member name. StringLiteral: decoded value. MacroCall: macro name.
    std::string text;
    UnaryOp unary_op{UnaryOp::Neg};
    BinaryOp binary_op{BinaryOp::Add};
    ExprPtr lhs;
    ExprPtr rhs;
    std::vector<ExprPtr> args;
    std::vector<FieldInit> fields;
    /// Cast: target type. StructLiteral/New: the constructed type.
    Type type;
    /// New: written as `new T(args)` (constructor arguments in `args`, possibly none).
    bool has_call_args{false};
    /// Name/Call/StructLiteral: explicit generic arguments (`max<i64>(a, b)`, `Pair<A, B> { }`).
    std::vector<Type> generic_args;
    std::vector<std::vector<Token>> macro_args;
    /// Closure: the parameters and the body. Shared by value semantics: a copy of the expression
    /// (`copyExpr`) gets a copy of this too.
    std::shared_ptr<ClosureData> closure;
};

struct Block;
struct Stmt;

/// One arm of a `switch`: `case a, b { ... }` or `default { ... }`.
struct SwitchCase {
    Span span;
    bool is_default{false};
    std::vector<ExprPtr> values;
    std::unique_ptr<Block> body;
};

struct Stmt {
    enum class Kind {
        Local,   ///< local name [: type] [= expr]; a tuple target `local (a, b) = expr` fills `names`
        Delete,  ///< delete ptr: runs the drop glue, then frees through the `register(free)` function (expr = pointer)
        Const,   ///< const name [: type] = expr (read-only; the value is a compile-time constant)
        Assign,  ///< target = expr; compound (`+=` ...) when `has_compound_op`
        Return,  ///< return [expr]
        Expr,    ///< expr
        If,      ///< if expr { then } [else { else_body }]
        While,   ///< [label:] while expr { body }
        DoWhile, ///< [label:] do { body } while expr
        For,     ///< [label:] for [init]; [cond]; [step] { body }
        ForRange,///< [label:] for name in expr .. rhs { body }   (rhs exclusive; `..=` inclusive)
        ForPack, ///< [label:] for name in pack { body }   (expr = the pack's name; unrolled per element)
        ForEach, ///< [label:] for name[, index] in iterable { body }   (arrays, slices, `str`, `Vec<T>`)
        Switch,  ///< switch expr { case a, b { } ... default { } }
        Defer,   ///< defer stmt (runs when the enclosing block is left)
        Try,     ///< try { body } catch [name] { else_body }  (name = the message binding, empty = none)
        Continue,  ///< continue [label] (jumps to the loop's next iteration)
        Break,   ///< break [label]
        Unsafe,  ///< unsafe { body }
        Block,   ///< { body }
    } kind{Kind::Expr};

    Span span;
    std::vector<Attribute> attributes;
    /// Local/ForRange: the variable. Break/Continue: the target label (empty = innermost loop).
    /// Local: for the tuple form `local (a, b) = expr` this is empty and `names` holds the targets.
    /// ForEach: the element name (`names[0]` is the index when `for x, i in ...` is written).
    std::string name;
    /// Local with a tuple target: one name per element, in order. Empty for a plain `local`.
    /// ForEach with an index: `names[0]` holds the index name (one element).
    std::vector<std::string> names;
    /// While/DoWhile/For/ForRange: the label written before the loop (empty = none).
    std::string label;
    bool has_type{false};
    Type type;
    ExprPtr target;
    ExprPtr expr;      ///< Assign value / conditions / ForRange lower bound / Switch scrutinee / ForEach iterable
    ExprPtr expr2;     ///< ForRange: exclusive upper bound
    bool has_compound_op{false};
    BinaryOp compound_op{BinaryOp::Add};
    /// The compound assignment is `x++` / `x--` (the value is the literal 1): a user type may
    /// define `operator++` / `operator--` for it.
    bool is_incdec{false};
    /// ForRange: the range is inclusive (`in a..=b` instead of `in a..b`).
    bool range_inclusive{false};
    std::unique_ptr<Block> body;       ///< If: then block; loops/Unsafe/Block: the block
    std::unique_ptr<Block> else_body;  ///< If: else block (an `else if` is a block holding one If)
    bool else_is_if{false};
    std::unique_ptr<Stmt> init;        ///< For: initializer (Local or simple statement), may be null
    std::unique_ptr<Stmt> step;        ///< For: step (simple statement), may be null; Defer: the statement
    std::vector<SwitchCase> cases;     ///< Switch
};

struct Block {
    Span span;
    std::vector<Stmt> statements;
};

// ---------------------------------------------------------------------------------------------
// Declarations

struct DeclBase {
    std::string name;
    std::vector<Attribute> attributes;
    Span span;       ///< the whole declaration, modifiers included
    Span name_span;
};

struct Modifiers {
    bool exported{false};         ///< `export`
    bool export_c{false};         ///< `export(c)` (implies exported)
    bool declared{false};         ///< `declare`
    bool registered{false};       ///< `register` / `register(role)`
    std::string register_role;    ///< empty for a plain `register`
};

struct Param {
    std::string name;
    Type type;  ///< unused for `self` and for a pack
    bool is_self{false};
    bool is_pack{false};  ///< `..args`: the untyped trailing parameter pack (see xlang-docs/language/variadic.md)
    /// `self: T` written in an inherent `impl`; the parser turns it into a plain parameter named
    /// `self`, so later phases never see it.
    bool typed_self{false};
    /// False for a closure parameter written without a type (`|x| x + 1`): the type comes from the
    /// context the closure is used in.
    bool has_type{true};
    Span span;
};

/// A closure literal, `|a: i32, b| -> i32 { return a + b }` or `|x| x * 2`. Parameter types and the
/// return type may be left out when the context names a `fn(...)` type. The lowering turns it
/// into a function (and, when it captures variables, a struct that holds them).
struct ClosureData {
    std::vector<Param> params;  ///< `has_type` is false for a parameter written without a type
    bool has_return_type{false};
    Type return_type;
    bool is_move{false};        ///< `move |x| ...`: captured variables are copied, not borrowed
    bool expr_body{false};      ///< `|x| x * 2`: `body` is one Expr statement that is the value
    Block body;
};

struct FieldDecl {
    std::string name;
    Type type;
    std::vector<Attribute> attributes;
    Span span;
};

struct FnDecl : DeclBase {
    std::vector<std::string> generic_params;  ///< `fn f<T, U>`: type parameters (empty = not generic)
    std::vector<Param> params;
    bool has_return_type{false};
    Type return_type;  ///< void when `->` is absent
    bool has_body{false};
    Block body;
    bool is_virtual{false};
    bool is_override{false};
};

struct GlobalDecl : DeclBase {
    Type type;
    bool has_type{true};  ///< false for `const NAME = expr` (the type comes from the value)
    bool is_const{false}; ///< `const`: read-only, compile-time initializer, no storage to write
    ExprPtr init;  ///< null = zero-initialized / external
};

/// One variant of a payload enum (`Add(*Expr, *Expr)`), kept on the struct the enum lowers to.
struct SumVariant {
    std::string name;
    std::vector<Type> payload;  ///< payload types in order; empty = a variant without data
};

struct StructDecl : DeclBase {
    std::vector<std::string> generic_params;  ///< `struct Pair<A, B>`
    std::vector<FieldDecl> fields;
    bool opaque{false};  ///< `declare struct S` without a body
    /// Set only on the compiler-made `Name$Data` of a payload enum: all fields share offset 0.
    bool is_union{false};
    /// Set only on the compiler-made `Name$Variant` of a payload enum: a struct of `_0`, `_1`, ...
    /// that holds one variant's payload. It is never dropped on its own; the enum's drop glue
    /// drops the live variant, so a droppable payload field is allowed here.
    bool is_payload{false};
    /// Non-empty on the struct a payload enum lowers to (`{ tag: Name$Tag, data: Name$Data }`).
    std::vector<SumVariant> variants;
};

struct ClassDecl : DeclBase {
    std::vector<std::string> generic_params;  ///< `class Vec<T>`
    bool has_base{false};
    std::vector<std::string> base;  ///< qualified base class name
    std::vector<FieldDecl> fields;
    std::vector<FnDecl> methods;
    bool opaque{false};
};

struct InterfaceDecl : DeclBase {
    std::vector<std::string> generic_params;  ///< `interface Iter<T>`
    std::vector<FnDecl> methods;  ///< signatures (no body)
    bool opaque{false};
};

struct ImplDecl : DeclBase {
    std::vector<std::string> generic_params;  ///< `impl<T> Iter<T> for Box<T>`
    std::vector<std::string> interface_path;
    std::vector<Type> interface_args;  ///< the `<...>` after the interface name
    Type target;
    std::vector<FnDecl> methods;
};

struct EnumMember {
    std::string name;
    ExprPtr value;  ///< null = auto-numbered
    bool has_payload{false};  ///< `Add(A, B)`: a payload variant (the enum becomes a sum type)
    std::vector<Type> payload;
    std::vector<Attribute> attributes;
    Span span;
};

struct EnumDecl : DeclBase {
    std::vector<std::string> generic_params;  ///< `enum Result<T, E>`
    bool has_underlying{false};
    Type underlying;  ///< `i32` when no `: T` was written
    std::vector<EnumMember> members;
};

struct TypeAliasDecl : DeclBase {
    std::vector<std::string> generic_params;  ///< `type Pair2<T> = Pair<T, T>`
    Type target;
};

struct DefineConstDecl : DeclBase {
    ExprPtr value;
};

struct MacroParam {
    enum class Kind { Expr, Type, Ident, Block, Items } kind{Kind::Expr};
    std::string name;
    Span span;
};

struct DefineMacroDecl : DeclBase {
    std::vector<MacroParam> params;
    std::vector<Token> body;  ///< raw token tree between the outer braces
};

struct ImportName {
    std::string name;
    std::string alias;  ///< empty = same as `name`
    Span span;
};

/// All import forms land here:
///   Module : import "m" [as a]
///   Star   : import * from "m"
///   StarAs : import * as a from "m"
///   Names  : from "m" import x [as y], ...   and   import x [as y], ... from "m"
struct ImportDecl : DeclBase {
    enum class Form { Module, Star, StarAs, Names } form{Form::Module};
    std::string module;
    Span module_span;
    std::string alias;
    std::vector<ImportName> names;
};

///   Star   : export * from "m" [except a, b]
///   StarAs : export * as ns from "m"
///   Names  : export { a, b as c } from "m"
struct ExportFromDecl : DeclBase {
    enum class Form { Star, StarAs, Names } form{Form::Star};
    std::string module;
    Span module_span;
    std::string alias;
    std::vector<ImportName> names;
    std::vector<ImportName> except;
};

/// `name!(args...)` at module level (generates declarations once expanded).
struct MacroCallDecl : DeclBase {
    std::vector<std::vector<Token>> args;
};

/// A call expression at module level, e.g. `staticAssert(sizeOf(T) == 12, "msg")`.
struct TopLevelCallDecl : DeclBase {
    ExprPtr call;
};

/// `fn short = target`: a module-level name for a function (or a qualified `module.name` path).
struct FnAliasDecl : DeclBase {
    std::vector<std::string> target;  ///< `name` or `module.name`
    Span target_span;
};

using Decl = std::variant<FnDecl, GlobalDecl, StructDecl, ClassDecl, InterfaceDecl, ImplDecl,
                          EnumDecl, TypeAliasDecl, DefineConstDecl, DefineMacroDecl, ImportDecl,
                          ExportFromDecl, MacroCallDecl, TopLevelCallDecl, FnAliasDecl>;

struct Item {
    Modifiers modifiers;
    Decl decl;
};

[[nodiscard]] const DeclBase& declBase(const Item& item);
[[nodiscard]] const char* declKindName(const Decl& decl);

/// Generic instantiation: type parameter name -> the concrete type it stands for.
using TypeEnv = std::map<std::string, Type>;

/// `type` with every type parameter in `env` replaced (also inside pointers, arrays, function
/// types and generic arguments).
[[nodiscard]] Type substituteType(const Type& type, const TypeEnv& env);

/// A deep copy of a generic `fn`, `struct`, `class` or `interface` with its type parameters replaced
/// by `env`, named `name`. The copy has no type parameters of its own.
[[nodiscard]] Item instantiateItem(const Item& generic, const TypeEnv& env, const std::string& name);

/// A deep copy of a generic method (or `fn`) with its type parameters replaced by `env`. The copy
/// keeps its name and has no type parameters of its own.
[[nodiscard]] FnDecl instantiateFn(const FnDecl& generic, const TypeEnv& env);

/// A deep copy of a block with no type parameters replaced (used to unroll a pack loop per element).
[[nodiscard]] Block copyBlock(const Block& block);

/// A deep copy of an expression with no type parameters replaced (operator and compound-assignment
/// desugaring lowers an operand more than once).
[[nodiscard]] ExprPtr copyExpr(const Expr& expr);

/// Calls `on_stmt` for every statement and `on_expr` for every expression of `block`, parents
/// before children, descending into the bodies of closures. Either callback may be empty, and
/// may rewrite the node it is given (its children are visited afterwards).
void walkAst(Block& block,
             const std::function<void(Stmt&)>& on_stmt,
             const std::function<void(Expr&)>& on_expr);

struct Program {
    FileId file{kNoFile};
    /// `@file(...)` attributes at the top of the file.
    std::vector<Attribute> file_attributes;
    std::vector<Item> items;
};

/// `summary`: one line per item. Otherwise the full tree.
void dumpProgram(std::ostream& out, const Program& program, bool summary);

}  // namespace xlang
