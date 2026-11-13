#pragma once

// The recursive-descent parser shared by the files of src/lang/parser/: the class declaration with
// its state and the small token-cursor helpers. Definitions live by area:
//   parser.cpp          run(), state snapshots, the public entry points
//   cursor.cpp          token access, errors, recovery
//   attributes.cpp      `@name(...)` attributes
//   types.cpp           types, generic parameters/arguments and their lookahead
//   expr/binary.cpp     ternary, binary operators, `as`, prefix operators
//   expr/postfix.cpp    paths, field initializers, macro and call arguments, postfix operators
//   expr/primary.cpp    primary expressions, fn literals, closures, array literals, `new`
//   stmt/block.cpp      blocks, statements, locals, assignments
//   stmt/control.cpp    `for`, `switch`, `if`
//   item/item.cpp       item dispatch, modifiers
//   item/fn.cpp         `fn`, operator names, globals and constants
//   item/aggregate.cpp  `struct`, `class`, `interface`, `impl`
//   item/enum.cpp       `enum`, `type`, `define`
//   item/module.cpp     `import`, `export`, module-level calls

#include "xlang/lang/parser.h"

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace xlang::parse {

/// Thrown after a syntax error has been reported; caught at statement/item level, which
/// resynchronizes and keeps parsing.
struct ParseAbort {};

[[nodiscard]] inline ExprPtr makeExpr(Expr::Kind kind, const Span& span) {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->span = span;
    return e;
}

class Parser {

  public:
    Parser(std::vector<Token> tokens, FileId file, DiagnosticEngine& diagnostics)
        : tokens_(std::move(tokens)), file_(file), diag_(diagnostics) {
        if (tokens_.empty() || tokens_.back().kind != TokenKind::End) {
            Token end;
            end.kind = TokenKind::End;
            end.newline_before = true;
            if (!tokens_.empty()) {
                end.span = tokens_.back().span;
            } else {
                end.span.file = file;
            }
            tokens_.push_back(std::move(end));
        }
    }

    Program run();

  private:
    /// The context flags that expression and statement parsing set and put back. A `ParseAbort`
    /// skips the code that puts them back, so recovery restores the snapshot taken where it
    /// resumes: otherwise a raised `nl_depth_` would let the next statements run on past their
    /// line breaks.
    struct State {
        int nl_depth{0};
        bool prefix_operand{false};
        int prefix_nl_base{0};
        bool no_struct{false};
        bool for_header{false};
        bool cast_type{false};
        std::size_t open_ternaries{0};
    };

    [[nodiscard]] State state() const;

    void restore(const State& s);

    // ------------------------------------------------------------------------------------
    // Token access

    [[nodiscard]] const Token& cur() const { return tokens_[pos_]; }
    [[nodiscard]] const Token& peek(std::size_t ahead) const;
    [[nodiscard]] TokenKind peekKind(std::size_t ahead) const { return peek(ahead).kind; }
    [[nodiscard]] bool at(TokenKind kind) const { return cur().kind == kind; }
    [[nodiscard]] const Token& prev() const { return tokens_[pos_ == 0 ? 0 : pos_ - 1]; }

    const Token& advance();

    bool match(TokenKind kind);

    /// The current token may continue the expression/statement being parsed: it is on the same
    /// line, or we are inside `(...)`/`[...]` where line breaks do not end anything. While a prefix
    /// `*` or `&` operand is being parsed, the operand's own `(...)` is not counted, so a newline
    /// in front of a binary `&`/`*` ends the operand instead of turning the next argument into its
    /// right side.
    [[nodiscard]] bool continues() const;

    [[nodiscard]] static bool adjacent(const Token& a, const Token& b);

    [[nodiscard]] Span spanFrom(const Span& start) const { return Span::merge(start, prev().span); }

    // ------------------------------------------------------------------------------------
    // Errors

    [[noreturn]] void failAt(const Token& token,
                             std::string message,
                             std::string label,
                             std::string detail = {},
                             std::string fix = {});

    [[noreturn]] void expected(std::string_view what);

    const Token& expect(TokenKind kind, std::string_view what);

    std::string expectIdent(std::string_view what = "an identifier");

    /// A member name: an identifier, or a word keyword where only a name can stand (after `fn` in a
    /// class, interface or impl body, and after `.`), so `fn delete(...)` and `c.delete(url)` work.
    [[nodiscard]] bool atMemberName() const;

    void softError(const Span& span, std::string message, std::string label);

    // ------------------------------------------------------------------------------------
    // Recovery

    [[nodiscard]] static bool startsItem(TokenKind kind);

    void syncItem(std::size_t start);

    void syncStatement(std::size_t start);

    void rejectDirective();

    // ------------------------------------------------------------------------------------
    // Attributes

    Attribute parseAttribute();

    void parseAttributeValue(AttributeArg& arg);

    std::vector<Attribute> parseAttributes();

    void registerAllowScopes(const std::vector<Attribute>& attrs, const Span& scope);

    // ------------------------------------------------------------------------------------
    // Types

    /// `<T, U>` after a `fn`, `struct` or `class` name.
    std::vector<std::string> parseGenericParams();

    /// Type parameters on a declaration that cannot have them (E0724); they are consumed.
    void rejectTypeParams();

    /// `<A, B>` after a type name or an identifier in an expression.
    std::vector<Type> parseTypeArgs();

    /// Consumes one `>`. A `>>` (or `>=`, `>>=`) token gives up its first character, so nested
    /// type arguments `Vec<Vec<i32>>` close one level at a time.
    void closeAngle();

    /// Index of the `>` that closes the angle brackets opened by the `<` at the cursor, when
    /// everything in between is made of type tokens: names, primitives, `*`, `.`, `,`, array and
    /// slice brackets with an integer length, tuple parentheses and `fn(...) -> R`. Nothing is
    /// returned when another token comes first, or when a `)`/`]`/`>` has no opener inside.
    [[nodiscard]] std::optional<std::size_t> typeArgsClose() const;

    /// Whether the `<` at the cursor opens type arguments of an identifier in an expression: they
    /// hold only type tokens (`typeArgsClose`) and the `>` is followed by what no comparison's right
    /// operand starts with: `(`, `{`, `)`, `]`, `,`, `;` or the end of the line. Otherwise the `<`
    /// is a comparison.
    [[nodiscard]] bool genericArgsAhead() const;

    /// Whether the `<` after the type name of `x as T` opens type arguments (`x as Box<i32>`) rather
    /// than comparing the cast value (`x as Idx < n`): they hold only type tokens (`typeArgsClose`)
    /// and the `>` is not followed, on its line, by a token that starts an operand (a name, a
    /// literal, `(`, `[`, `self`, `!`, `~`, a closure), which only a comparison `a < b > c` has.
    [[nodiscard]] bool castTypeArgsAhead() const;

    Type parseType();

    Type parseReturnType();

    // ------------------------------------------------------------------------------------
    // Expressions

    ExprPtr parseExpr() { return parseTernary(); }

    /// `cond ? a : b` (right associative, lowest precedence).
    ExprPtr parseTernary();

    ExprPtr makeBinary(BinaryOp op, ExprPtr lhs, ExprPtr rhs);

    ExprPtr parseOr();

    ExprPtr parseAnd();

    ExprPtr parseEquality();

    ExprPtr parseComparison();

    ExprPtr parseAdditive();

    ExprPtr parseMultiplicative();

    ExprPtr parseCast();

    ExprPtr parseUnary();

    /// Dotted name of a `Name`/`Field` chain, empty when `e` is anything else.
    [[nodiscard]] static std::string pathOf(const Expr& e);

    [[nodiscard]] static std::vector<std::string> splitPath(const std::string& path);

    /// At `{`: does what follows look like a struct literal body (`}` or `name :`)?
    [[nodiscard]] bool looksLikeStructLiteral() const;

    std::vector<FieldInit> parseFieldInits();

    /// Raw token lists of a macro argument list; consumes the `(` ... `)`.
    std::vector<std::vector<Token>> parseMacroArgs();

    /// A call argument. Inline-asm operands are written `in(reg) x`, `out("rax") y`,
    /// `inout(reg) z`: a register spec in parentheses followed by the operand expression.
    ExprPtr parseCallArg();

    /// A `?` that propagates (`f()?`) rather than starts a ternary. It must touch the operand
    /// before it. Then it propagates when what follows cannot be the first token of a ternary's
    /// value: the end of the line or of the file, a closing or separating token (`;`, `)`, `]`,
    /// `}`, `,`, `.`, `{`, `:`, another `?`), or an operator that only works between two operands
    /// (`f()? + 1`, `a? == b?`, `as`). Otherwise (`-`, `*`, `&`, a name, a literal follow) it is a
    /// ternary only when its `:` follows (`ternaryColonAhead`): `c? -1 : 1` is a ternary,
    /// `f()? - 1` and `f()? * 2` propagate.
    [[nodiscard]] bool tryOperatorAhead() const;

    /// Net count of brackets (`(`, `[`, `{`) opened and not closed by the tokens `from` .. `to - 1`.
    [[nodiscard]] int openBrackets(std::size_t from, std::size_t to) const;

    /// Whether the `?` at the cursor has a ternary `:` of its own after it. The scan stays at the
    /// `?`'s bracket level and stops where the expression ends there: a line break outside
    /// brackets, a `,` or `;`, a closing bracket of an outer level, a block `{` in a condition, or
    /// the end of the file. Each `?` met on the way takes the next `:`. The `:` left over belong
    /// to the ternaries whose then-branch encloses the cursor at this same level
    /// (`open_ternaries_`); one more than those is this `?`'s own.
    [[nodiscard]] bool ternaryColonAhead() const;

    ExprPtr parsePostfix(ExprPtr expr);

    /// `fn(params) -> R { body }` as an expression: the body becomes a module-level function with a
    /// generated name, and the expression is a reference to it. A literal captures nothing; an
    /// outer local used inside it is an unknown name.
    ExprPtr parseFnLiteral();

    /// `[move] |a: T, b| [-> R] { body }` or `[move] |x| expr`; `||` is the empty parameter list.
    /// Types may be left out where the context names a `fn(...)` type (checked by the lowering).
    /// `move` is a keyword only in front of `|`.
    ExprPtr parseClosure();

    ExprPtr parsePrimary();

    /// `[]` directly followed, on the same line, by something that starts an element type
    /// (`[]i32`, `[]mut u8`, `[]Point`, `[][]u8`, `[]*T`, `[]fn(i32)`) is the slice type, never the
    /// empty array literal: no expression continues with a type name after `]`.
    [[nodiscard]] bool atSliceType() const;

    ExprPtr parseArrayLiteral();

    ExprPtr parseNew();

    // ------------------------------------------------------------------------------------
    // Statements

    Block parseBlock();

    std::unique_ptr<Block> parseBlockPtr() { return std::make_unique<Block>(parseBlock()); }

    /// After a statement that did not end in a block: a line break or `}` must follow.
    void endOfStatement();

    Stmt parseStatement();

    Stmt parseStatementBody();

    /// `target = v`, `target op= v`, `target++`, `target--` or an expression statement.
    /// The part of a `local` statement after the keyword: `name [: T | := expr] [= expr]`. Shared
    /// by statement position and the `for` initializer so both spellings of the declaration behave
    /// alike. On return the cursor is after the initializer (or after the type), never at `;`.
    /// `local x: T := expr` is refused because `:=` already means "infer the type".
    void parseLocalTail(Stmt& stmt);

    Stmt parseSimpleStatement();

    /// `for init; cond; step { body }` (every part optional) or `for i in lo..hi { body }`.
    Stmt parseFor();

    Stmt parseForClause();

    /// `switch v { case a, b { ... } default { ... } }`
    Stmt parseSwitch();

    /// `if`/`while` condition: struct literals are not allowed (the `{` starts the block).
    ExprPtr parseCondition();

    Stmt parseIf();

    // ------------------------------------------------------------------------------------
    // Declarations

    struct DeclStart {
        std::vector<Attribute> attributes;
        Modifiers modifiers;
        Span start;
    };

    void finishDecl(DeclBase& base, const DeclStart& s, std::string name, const Span& name_span);

    /// Items that end without a closing brace must be followed by a line break.
    void endOfItemLine();

    void parseItem(Program& program);

    void addItem(Program& program, DeclStart& s, Decl decl);

    void parseModifiers(Modifiers& mods);

    /// Prefix applicability (soft errors: parsing continues).
    void checkModifiers(const Item& item);

    // --- fn ----------------------------------------------------------------------------------

    /// The parameter list, the optional return type and the body of a function (after its name):
    /// shared by named functions and function literals. `needs_body` makes a missing body an error.
    void parseFnTail(FnDecl& fn, const std::string& name, bool member, bool needs_body);

    /// The operator a function name spells after `fn` (`fn +(...)`, `fn []=(...)`, `fn ()(...)`):
    /// the token(s) are consumed and the function is named `operator<symbol>`. nullopt (nothing
    /// consumed) when the next token is no operator. The table is `operatorFunctionName`.
    std::optional<std::string> parseOperatorName();

    FnDecl parseFn(const DeclStart& s, bool member);

    // --- global ------------------------------------------------------------------------------

    /// `fn name = target` or `fn name = module.target` at module level.
    FnAliasDecl parseFnAlias(const DeclStart& s);

    GlobalDecl parseGlobal(const DeclStart& s);

    /// `const NAME [: T] = expr` at module level. The type is optional.
    GlobalDecl parseConstItem(const DeclStart& s);

    // --- struct / class / interface / impl ---------------------------------------------------

    FieldDecl parseField(std::vector<Attribute> attrs);

    /// Separator between members: `,`, or a line break, or the closing `}`.
    void memberSeparator();

    StructDecl parseStruct(const DeclStart& s);

    ClassDecl parseClass(const DeclStart& s);

    /// `[virtual|override] fn ...` inside class/interface/impl bodies.
    FnDecl parseMethod(std::vector<Attribute> attrs, bool inherent = false);

    InterfaceDecl parseInterface(const DeclStart& s);

    /// `impl Iface for T { ... }` becomes an `ImplDecl`. The inherent form `impl T { fn +(self: T,
    /// o: T) -> T { } fn len(self: *T) -> usize { } }` holds operators and methods of a struct; each
    /// becomes a module-level function named `operator<symbol>` or `T.name` (`self` is its first
    /// parameter, `self` alone meaning `self: T`), so it overloads like any function and nothing
    /// after the parser knows about the block. Those functions are added to `program` and the
    /// result is empty.
    std::optional<ImplDecl> parseImpl(Program& program, DeclStart& s);

    // --- enum / type / define ----------------------------------------------------------------

    EnumDecl parseEnum(const DeclStart& s);

    /// A value enum is one item. A payload enum (`Add(A, B)` members) becomes five kinds of
    /// ordinary items: `Name$Tag` (the tag enum), `Name$Vi` (one struct per variant with data),
    /// `Name$Data` (their union) and `Name` (`{ tag, data }` plus the variant list). The union and
    /// the enum carry the enum's type parameters; the tag enum takes none (it is `i32` either
    /// way), and the payload structs take none of their own either, because their fields are the
    /// enum's payload types and they exist only as parts of an enum instance.
    void addEnum(Program& program, DeclStart& s, EnumDecl decl);

    TypeAliasDecl parseTypeAlias(const DeclStart& s);

    Decl parseDefine(const DeclStart& s);

    // --- import / export ---------------------------------------------------------------------

    std::vector<ImportName> parseNameList();

    std::string parseModuleString(Span& span);

    ImportDecl parseImport(const DeclStart& s);

    void parseExportFrom(Program& program, DeclStart& s);

    // --- module-level calls ------------------------------------------------------------------

    Decl parseTopLevelCall(const DeclStart& s);

    std::vector<Token> tokens_;
    FileId file_;
    DiagnosticEngine& diag_;
    std::size_t pos_{0};
    int nl_depth_{0};
    /// A prefix `*`/`&` operand is being parsed: `continues()` ignores the parentheses opened
    /// inside it, so a newline in front of a binary `&`/`*` ends the operand at its line.
    bool prefix_operand_{false};
    int prefix_nl_base_{0};
    bool no_struct_{false};
    bool for_header_{false}; ///< parsing the init/step statements of a `for` header
    bool cast_type_{false};  ///< parsing the type after `as` (see `castTypeArgsAhead`)
    /// Token index of the `?` of every ternary whose then-branch is being parsed, innermost last
    /// (see `ternaryColonAhead`).
    std::vector<std::size_t> open_ternaries_;
    std::vector<Item>
        lambdas_; // function literals lifted out of expressions, appended to the program at the end
    int lambda_count_{0};
};

} // namespace xlang::parse
