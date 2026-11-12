#include "xlang/lang/parser.h"

#include <algorithm>
#include <array>
#include <format>
#include <ranges>
#include <string_view>
#include <utility>

namespace xlang {

namespace {

/// Thrown after a syntax error has been reported; caught at statement/item level, which
/// resynchronizes and keeps parsing.
struct ParseAbort {};

[[nodiscard]] ExprPtr makeExpr(Expr::Kind kind, const Span& span) {
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

    Program run() {
        Program program;
        program.file = file_;

        // `@file(...)` attributes at the very top.
        while (at(TokenKind::At) && peekKind(1) == TokenKind::Ident && peek(1).text == "file") {
            try {
                program.file_attributes.push_back(parseAttribute());
            } catch (const ParseAbort&) {
                syncItem(pos_);
            }
        }

        while (!at(TokenKind::End)) {
            const std::size_t start = pos_;
            try {
                parseItem(program);
            } catch (const ParseAbort&) {
                syncItem(start);
            }
        }
        for (auto& lifted : lambdas_) {
            program.items.push_back(std::move(lifted));
        }
        lambdas_.clear();
        return program;
    }

  private:
    // ------------------------------------------------------------------------------------
    // Token access

    [[nodiscard]] const Token& cur() const { return tokens_[pos_]; }
    [[nodiscard]] const Token& peek(std::size_t ahead) const {
        return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)];
    }
    [[nodiscard]] TokenKind peekKind(std::size_t ahead) const { return peek(ahead).kind; }
    [[nodiscard]] bool at(TokenKind kind) const { return cur().kind == kind; }
    [[nodiscard]] const Token& prev() const { return tokens_[pos_ == 0 ? 0 : pos_ - 1]; }

    const Token& advance() {
        const Token& t = tokens_[pos_];
        if (pos_ + 1 < tokens_.size()) {
            ++pos_;
        }
        return t;
    }

    bool match(TokenKind kind) {
        if (at(kind)) {
            advance();
            return true;
        }
        return false;
    }

    /// The current token may continue the expression/statement being parsed: it is on the same
    /// line, or we are inside `(...)`/`[...]` where line breaks do not end anything. While a prefix
    /// `*` or `&` operand is being parsed, the operand's own `(...)` is not counted, so a newline
    /// in front of a binary `&`/`*` ends the operand instead of turning the next argument into its
    /// right side.
    [[nodiscard]] bool continues() const {
        if (cur().newline_before && prefix_operand_) {
            return nl_depth_ > prefix_nl_base_;
        }
        return !cur().newline_before || nl_depth_ > 0;
    }

    [[nodiscard]] static bool adjacent(const Token& a, const Token& b) {
        return a.span.end_line == b.span.line && a.span.end_column == b.span.column;
    }

    [[nodiscard]] Span spanFrom(const Span& start) const { return Span::merge(start, prev().span); }

    // ------------------------------------------------------------------------------------
    // Errors

    [[noreturn]] void failAt(const Token& token,
                             std::string message,
                             std::string label,
                             std::string detail = {},
                             std::string fix = {}) {
        Diagnostic d;
        d.code = "E0002";
        d.severity = Severity::Error;
        d.message = std::move(message);
        d.labels.push_back({token.span, std::move(label)});
        d.detail = std::move(detail);
        d.fix = std::move(fix);
        diag_.report(std::move(d));
        throw ParseAbort{};
    }

    [[noreturn]] void expected(std::string_view what) {
        failAt(cur(),
               std::format("expected {}, found {}", what, describeToken(cur())),
               std::format("expected {} here", what));
    }

    const Token& expect(TokenKind kind, std::string_view what) {
        if (!at(kind)) {
            expected(what);
        }
        return advance();
    }

    std::string expectIdent(std::string_view what = "an identifier") {
        if (!at(TokenKind::Ident)) {
            expected(what);
        }
        return advance().text;
    }

    /// A member name: an identifier, or a word keyword where only a name can stand (after `fn` in a
    /// class, interface or impl body, and after `.`), so `fn delete(...)` and `c.delete(url)` work.
    [[nodiscard]] bool atMemberName() const {
        const std::string& t = cur().text;
        return at(TokenKind::Ident) ||
               (!t.empty() && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z')) &&
                !at(TokenKind::Int) && !at(TokenKind::Float));
    }

    void softError(const Span& span, std::string message, std::string label) {
        diag_.error("E0002", span, std::move(message), std::move(label));
    }

    // ------------------------------------------------------------------------------------
    // Recovery

    [[nodiscard]] static bool startsItem(TokenKind kind) {
        switch (kind) {
        case TokenKind::At:
        case TokenKind::Export:
        case TokenKind::Declare:
        case TokenKind::Register:
        case TokenKind::Fn:
        case TokenKind::Global:
        case TokenKind::Const:
        case TokenKind::Struct:
        case TokenKind::Class:
        case TokenKind::Interface:
        case TokenKind::Impl:
        case TokenKind::Enum:
        case TokenKind::Type:
        case TokenKind::Define:
        case TokenKind::Import:
        case TokenKind::From:
            return true;
        default:
            return false;
        }
    }

    void syncItem(std::size_t start) {
        if (pos_ == start && !at(TokenKind::End)) {
            advance();
        }
        // Brace depth at the error position, counted from the start of the failed item, so the
        // rest of its body is skipped instead of being re-parsed as top-level code.
        int depth = 0;
        for (std::size_t i = start; i < pos_; ++i) {
            if (tokens_[i].kind == TokenKind::LBrace) {
                ++depth;
            } else if (tokens_[i].kind == TokenKind::RBrace && depth > 0) {
                --depth;
            }
        }
        while (!at(TokenKind::End)) {
            if (depth == 0 && cur().newline_before && startsItem(cur().kind)) {
                return;
            }
            if (at(TokenKind::LBrace)) {
                ++depth;
            } else if (at(TokenKind::RBrace) && depth > 0) {
                --depth;
            }
            advance();
        }
    }

    void syncStatement(std::size_t start) {
        if (pos_ == start && !at(TokenKind::End) && !at(TokenKind::RBrace)) {
            advance();
        }
        int depth = 0;
        while (!at(TokenKind::End)) {
            if (depth == 0 && (cur().newline_before || at(TokenKind::RBrace))) {
                return;
            }
            if (at(TokenKind::LBrace)) {
                ++depth;
            } else if (at(TokenKind::RBrace)) {
                --depth;
            }
            advance();
        }
    }

    void rejectDirective() {
        if (cur().kind >= TokenKind::DirIf && cur().kind <= TokenKind::DirError) {
            failAt(
                cur(),
                std::format("`{}` is not supported yet", tokenKindName(cur().kind)),
                "conditional-compilation directive",
                "The lexer recognizes `#if`/`#elif`/`#else`/`#endif`/`#ifdef`/`#ifndef`/`#error`, "
                "but the preprocessor that evaluates them is not implemented yet.",
                "remove the directive for now");
        }
    }

    // ------------------------------------------------------------------------------------
    // Attributes

    Attribute parseAttribute() {
        Attribute attr;
        const Token& at_token = expect(TokenKind::At, "`@`");
        attr.span = at_token.span;
        attr.name = expectIdent("an attribute name");
        if (at(TokenKind::LParen) && continues() && adjacent(prev(), cur())) {
            advance();
            ++nl_depth_;
            while (!at(TokenKind::RParen)) {
                AttributeArg arg;
                arg.span = cur().span;
                if (at(TokenKind::Ident) && peekKind(1) == TokenKind::Assign) {
                    arg.key = advance().text;
                    advance(); // '='
                    arg.span = cur().span;
                }
                parseAttributeValue(arg);
                attr.args.push_back(std::move(arg));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
            --nl_depth_;
            expect(TokenKind::RParen, "`)` closing the attribute");
        }
        attr.span = spanFrom(attr.span);
        return attr;
    }

    void parseAttributeValue(AttributeArg& arg) {
        switch (cur().kind) {
        case TokenKind::Ident:
        case TokenKind::PrimType:
        case TokenKind::True:
        case TokenKind::False:
            arg.kind = AttributeArg::Kind::Ident;
            arg.text = advance().text;
            return;
        case TokenKind::Int:
            arg.kind = AttributeArg::Kind::Int;
            arg.number = static_cast<std::int64_t>(advance().int_value);
            return;
        case TokenKind::Minus:
            advance();
            arg.kind = AttributeArg::Kind::Int;
            arg.number = -static_cast<std::int64_t>(expect(TokenKind::Int, "an integer").int_value);
            return;
        case TokenKind::String:
            arg.kind = AttributeArg::Kind::String;
            arg.text = advance().text;
            return;
        default:
            expected("an attribute argument (identifier, integer or string)");
        }
    }

    std::vector<Attribute> parseAttributes() {
        std::vector<Attribute> attrs;
        while (at(TokenKind::At)) {
            attrs.push_back(parseAttribute());
        }
        return attrs;
    }

    void registerAllowScopes(const std::vector<Attribute>& attrs, const Span& scope) {
        for (const Attribute& attr : attrs) {
            if (attr.name != "allow") {
                continue;
            }
            std::vector<std::string> codes;
            for (const AttributeArg& arg : attr.args) {
                if (arg.key.empty() && arg.kind == AttributeArg::Kind::Ident) {
                    codes.push_back(arg.text);
                }
            }
            diag_.addAllowScope(scope, codes);
        }
    }

    // ------------------------------------------------------------------------------------
    // Types

    /// `<T, U>` after a `fn`, `struct` or `class` name.
    std::vector<std::string> parseGenericParams() {
        std::vector<std::string> names;
        expect(TokenKind::Lt, "`<` starting the type parameters");
        while (!at(TokenKind::Gt) && !at(TokenKind::End)) {
            names.push_back(expectIdent("a type parameter name"));
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        closeAngle();
        return names;
    }

    /// Type parameters on a declaration that cannot have them (E0724); they are consumed.
    void rejectTypeParams() {
        if (at(TokenKind::Lt)) {
            diag_.error("E0724",
                        cur().span,
                        "type parameters are only allowed on `fn`, `struct` and `class`",
                        "remove the `<...>`");
            (void)parseGenericParams();
        }
    }

    /// `<A, B>` after a type name or an identifier in an expression.
    std::vector<Type> parseTypeArgs() {
        std::vector<Type> args;
        expect(TokenKind::Lt, "`<` starting the type arguments");
        while (!at(TokenKind::Gt) && !at(TokenKind::Shr) && !at(TokenKind::End)) {
            args.push_back(parseType());
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        closeAngle();
        return args;
    }

    /// Consumes one `>`. A `>>` (or `>=`, `>>=`) token gives up its first character, so nested
    /// type arguments `Vec<Vec<i32>>` close one level at a time.
    void closeAngle() {
        Token& t = tokens_[pos_];
        switch (t.kind) {
        case TokenKind::Gt:
            advance();
            return;
        case TokenKind::Shr:
            t.kind = TokenKind::Gt;
            t.text = ">";
            return;
        case TokenKind::Ge:
            t.kind = TokenKind::Assign;
            t.text = "=";
            return;
        case TokenKind::ShrAssign:
            t.kind = TokenKind::Ge;
            t.text = ">=";
            return;
        default:
            expected("`>` closing the type arguments");
        }
    }

    /// Whether the `<` at the cursor opens type arguments of an identifier in an expression: they
    /// must close with `>` directly followed by `(` or `{`, and contain only type-like tokens.
    /// Otherwise the `<` is a comparison.
    [[nodiscard]] bool genericArgsAhead() const {
        int depth = 0;
        for (std::size_t i = pos_; i < tokens_.size(); ++i) {
            switch (tokens_[i].kind) {
            case TokenKind::Lt:
                ++depth;
                break;
            case TokenKind::Gt:
                --depth;
                break;
            case TokenKind::Shr:
                depth -= 2;
                break;
            case TokenKind::Ident:
            case TokenKind::PrimType:
            case TokenKind::Star:
            case TokenKind::Comma:
            case TokenKind::Dot:
            case TokenKind::LBracket:
            case TokenKind::RBracket:
            case TokenKind::Int:
            case TokenKind::Semicolon:
                break;
            default:
                return false;
            }
            if (depth < 0) {
                return false;
            }
            if (depth == 0) {
                // A `>` that ends a comparison needs a right operand, so after it these tokens
                // can only close type arguments of a generic function value or call.
                const bool ends_here = i + 1 >= tokens_.size() || tokens_[i + 1].newline_before;
                const TokenKind next =
                    i + 1 < tokens_.size() ? tokens_[i + 1].kind : TokenKind::End;
                return ends_here || next == TokenKind::LParen || next == TokenKind::LBrace ||
                       next == TokenKind::RParen || next == TokenKind::RBracket ||
                       next == TokenKind::Comma || next == TokenKind::Semicolon;
            }
        }
        return false;
    }

    Type parseType() {
        const Span start = cur().span;
        Type type;
        switch (cur().kind) {
        case TokenKind::Star: {
            advance();
            type = Type::makePointer(parseType());
            break;
        }
        case TokenKind::PrimType: {
            type = *primitiveFromName(advance().text);
            break;
        }
        case TokenKind::Fn: {
            advance();
            expect(TokenKind::LParen, "`(` after `fn` in a function pointer type");
            ++nl_depth_;
            std::vector<Type> params;
            while (!at(TokenKind::RParen)) {
                params.push_back(parseType());
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
            --nl_depth_;
            expect(TokenKind::RParen, "`)` closing the parameter types");
            Type ret = Type::makeVoid();
            if (match(TokenKind::Arrow)) {
                ret = parseType();
            }
            type = Type::makeFnPtr(std::move(params), std::move(ret));
            break;
        }
        case TokenKind::LBracket: {
            advance();
            ++nl_depth_;
            // `[]T` / `[]mut T` is a slice type; `[T; N]` needs the semicolon and length below.
            // `mut` is not a keyword: it is an identifier, so it is matched by its text.
            if (match(TokenKind::RBracket)) {
                --nl_depth_;
                const bool is_mut = at(TokenKind::Ident) && cur().text == "mut";
                if (is_mut) {
                    advance();
                }
                type = Type::makeSlice(parseType(), is_mut);
                break;
            }
            Type element = parseType();
            expect(TokenKind::Semicolon, "`;` between the element type and the length");
            const bool saved_no_struct = no_struct_;
            no_struct_ = false;
            ExprPtr length = parseExpr();
            no_struct_ = saved_no_struct;
            --nl_depth_;
            expect(TokenKind::RBracket, "`]` closing the array type");
            type =
                Type::makeArray(std::move(element), std::shared_ptr<const Expr>(std::move(length)));
            break;
        }
        case TokenKind::LParen: {
            // Tuple type: `()` is the empty tuple, `(A,)` a one-element tuple, `(A, B)` a product.
            advance();
            ++nl_depth_;
            std::vector<Type> elements;
            bool trailing_comma = false;
            while (!at(TokenKind::RParen)) {
                elements.push_back(parseType());
                if (!match(TokenKind::Comma)) {
                    break;
                }
                trailing_comma = at(TokenKind::RParen);
            }
            --nl_depth_;
            expect(TokenKind::RParen, "`)` closing the tuple type");
            if (elements.size() == 1 && !trailing_comma) {
                // `(T)` is not a tuple: it is the type `T` in parentheses.
                type = std::move(elements.front());
            } else {
                type = Type::makeTuple(std::move(elements));
            }
            break;
        }
        case TokenKind::Ident: {
            std::vector<std::string> path;
            path.push_back(advance().text);
            while (at(TokenKind::Dot) && peekKind(1) == TokenKind::Ident) {
                advance();
                path.push_back(advance().text);
            }
            type = Type::makeNamed(std::move(path));
            if (at(TokenKind::Lt)) {
                type.args = parseTypeArgs();
            }
            break;
        }
        default:
            expected("a type");
        }
        type.span = spanFrom(start);
        return type;
    }

    // ------------------------------------------------------------------------------------
    // Expressions

    ExprPtr parseExpr() { return parseTernary(); }

    /// `cond ? a : b` (right associative, lowest precedence).
    ExprPtr parseTernary() {
        ExprPtr cond = parseOr();
        if (at(TokenKind::Question) && continues()) {
            advance();
            ExprPtr then_value = parseTernary();
            expect(TokenKind::Colon, "`:` between the two branches of `?`");
            ExprPtr else_value = parseTernary();
            ExprPtr e = makeExpr(Expr::Kind::Ternary, Span::merge(cond->span, else_value->span));
            e->lhs = std::move(cond);
            e->rhs = std::move(then_value);
            e->args.push_back(std::move(else_value));
            return e;
        }
        return cond;
    }

    ExprPtr makeBinary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
        const Span span = Span::merge(lhs->span, rhs->span);
        ExprPtr e = makeExpr(Expr::Kind::Binary, span);
        e->binary_op = op;
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }

    ExprPtr parseOr() {
        ExprPtr lhs = parseAnd();
        while (at(TokenKind::OrOr) && continues()) {
            advance();
            lhs = makeBinary(BinaryOp::Or, std::move(lhs), parseAnd());
        }
        return lhs;
    }

    ExprPtr parseAnd() {
        ExprPtr lhs = parseEquality();
        while (at(TokenKind::AndAnd) && continues()) {
            advance();
            lhs = makeBinary(BinaryOp::And, std::move(lhs), parseEquality());
        }
        return lhs;
    }

    ExprPtr parseEquality() {
        ExprPtr lhs = parseComparison();
        while ((at(TokenKind::EqEq) || at(TokenKind::NotEq)) && continues()) {
            const BinaryOp op = advance().kind == TokenKind::EqEq ? BinaryOp::Eq : BinaryOp::Ne;
            lhs = makeBinary(op, std::move(lhs), parseComparison());
        }
        return lhs;
    }

    ExprPtr parseComparison() {
        ExprPtr lhs = parseAdditive();
        while ((at(TokenKind::Lt) || at(TokenKind::Le) || at(TokenKind::Gt) || at(TokenKind::Ge)) &&
               continues()) {
            BinaryOp op = BinaryOp::Lt;
            switch (advance().kind) {
            case TokenKind::Lt:
                op = BinaryOp::Lt;
                break;
            case TokenKind::Le:
                op = BinaryOp::Le;
                break;
            case TokenKind::Gt:
                op = BinaryOp::Gt;
                break;
            default:
                op = BinaryOp::Ge;
                break;
            }
            lhs = makeBinary(op, std::move(lhs), parseAdditive());
        }
        return lhs;
    }

    ExprPtr parseAdditive() {
        ExprPtr lhs = parseMultiplicative();
        // Go-style levels: `+ - | ^` here, `* / % << >> &` in parseMultiplicative.
        while ((at(TokenKind::Plus) || at(TokenKind::Minus) || at(TokenKind::Pipe) ||
                at(TokenKind::Caret)) &&
               continues()) {
            BinaryOp op = BinaryOp::Add;
            switch (advance().kind) {
            case TokenKind::Plus:
                op = BinaryOp::Add;
                break;
            case TokenKind::Minus:
                op = BinaryOp::Sub;
                break;
            case TokenKind::Pipe:
                op = BinaryOp::BitOr;
                break;
            default:
                op = BinaryOp::BitXor;
                break;
            }
            lhs = makeBinary(op, std::move(lhs), parseMultiplicative());
        }
        return lhs;
    }

    ExprPtr parseMultiplicative() {
        ExprPtr lhs = parseCast();
        while ((at(TokenKind::Star) || at(TokenKind::Slash) || at(TokenKind::Percent) ||
                at(TokenKind::Shl) || at(TokenKind::Shr) || at(TokenKind::Amp)) &&
               continues()) {
            BinaryOp op = BinaryOp::Mul;
            switch (advance().kind) {
            case TokenKind::Star:
                op = BinaryOp::Mul;
                break;
            case TokenKind::Slash:
                op = BinaryOp::Div;
                break;
            case TokenKind::Shl:
                op = BinaryOp::Shl;
                break;
            case TokenKind::Shr:
                op = BinaryOp::Shr;
                break;
            case TokenKind::Amp:
                op = BinaryOp::BitAnd;
                break;
            default:
                op = BinaryOp::Mod;
                break;
            }
            lhs = makeBinary(op, std::move(lhs), parseCast());
        }
        return lhs;
    }

    ExprPtr parseCast() {
        ExprPtr value = parseUnary();
        while (at(TokenKind::As) && continues()) {
            advance();
            Type target = parseType();
            ExprPtr e = makeExpr(Expr::Kind::Cast, Span::merge(value->span, prev().span));
            e->lhs = std::move(value);
            e->type = std::move(target);
            value = std::move(e);
        }
        return value;
    }

    ExprPtr parseUnary() {
        const Span start = cur().span;
        std::optional<UnaryOp> op;
        switch (cur().kind) {
        case TokenKind::Minus:
            op = UnaryOp::Neg;
            break;
        case TokenKind::Bang:
            op = UnaryOp::Not;
            break;
        case TokenKind::Tilde:
            op = UnaryOp::BitNot;
            break;
        case TokenKind::Star:
            op = UnaryOp::Deref;
            break;
        case TokenKind::Amp:
            op = UnaryOp::AddrOf;
            break;
        case TokenKind::AndAnd: {
            // `&&x` in prefix position is two address-of operators.
            advance();
            ExprPtr inner = parseUnary();
            ExprPtr mid = makeExpr(Expr::Kind::Unary, Span::merge(start, inner->span));
            mid->unary_op = UnaryOp::AddrOf;
            mid->lhs = std::move(inner);
            ExprPtr outer = makeExpr(Expr::Kind::Unary, mid->span);
            outer->unary_op = UnaryOp::AddrOf;
            outer->lhs = std::move(mid);
            return outer;
        }
        default:
            break;
        }
        if (op) {
            advance();
            // `*` and `&` are binary as well, so the operand of a prefix `*`/`&` stops at the end
            // of its line: in `T {\n &A()\n &B() }` the second `&` starts the next argument, not
            // the right side of a `&` term.
            const bool saved_operand = prefix_operand_;
            const int saved_base = prefix_nl_base_;
            prefix_operand_ = true;
            prefix_nl_base_ = nl_depth_;
            ExprPtr operand = parseUnary();
            prefix_operand_ = saved_operand;
            prefix_nl_base_ = saved_base;
            ExprPtr e = makeExpr(Expr::Kind::Unary, Span::merge(start, operand->span));
            e->unary_op = *op;
            e->lhs = std::move(operand);
            return e;
        }
        return parsePostfix(parsePrimary());
    }

    /// Dotted name of a `Name`/`Field` chain, empty when `e` is anything else.
    [[nodiscard]] static std::string pathOf(const Expr& e) {
        if (e.kind == Expr::Kind::Name) {
            return e.text;
        }
        if (e.kind == Expr::Kind::Field && e.lhs) {
            std::string base = pathOf(*e.lhs);
            return base.empty() ? base : base + "." + e.text;
        }
        return {};
    }

    [[nodiscard]] static std::vector<std::string> splitPath(const std::string& path) {
        std::vector<std::string> out;
        std::size_t start = 0;
        while (true) {
            const std::size_t dot = path.find('.', start);
            out.push_back(path.substr(start, dot == std::string::npos ? dot : dot - start));
            if (dot == std::string::npos) {
                break;
            }
            start = dot + 1;
        }
        return out;
    }

    /// At `{`: does what follows look like a struct literal body (`}` or `name :`)?
    [[nodiscard]] bool looksLikeStructLiteral() const {
        if (peekKind(1) == TokenKind::RBrace) {
            return true;
        }
        return peekKind(1) == TokenKind::Ident && peekKind(2) == TokenKind::Colon;
    }

    std::vector<FieldInit> parseFieldInits() {
        expect(TokenKind::LBrace, "`{`");
        const int saved_depth = nl_depth_;
        const bool saved_no_struct = no_struct_;
        nl_depth_ = 0;
        no_struct_ = false;
        std::vector<FieldInit> fields;
        while (!at(TokenKind::RBrace)) {
            FieldInit init;
            init.span = cur().span;
            init.name = expectIdent("a field name");
            expect(TokenKind::Colon, "`:` after the field name");
            init.value = parseExpr();
            init.span = spanFrom(init.span);
            fields.push_back(std::move(init));
            if (match(TokenKind::Comma)) {
                continue;
            }
            if (!at(TokenKind::RBrace) && !cur().newline_before) {
                expected("`,`, a line break or `}`");
            }
        }
        nl_depth_ = saved_depth;
        no_struct_ = saved_no_struct;
        expect(TokenKind::RBrace, "`}`");
        return fields;
    }

    /// Raw token lists of a macro argument list; consumes the `(` ... `)`.
    std::vector<std::vector<Token>> parseMacroArgs() {
        expect(TokenKind::LParen, "`(`");
        std::vector<std::vector<Token>> args;
        std::vector<Token> current;
        int depth = 0;
        bool any = false;
        while (true) {
            if (at(TokenKind::End)) {
                expected("`)` closing the macro call");
            }
            const TokenKind kind = cur().kind;
            if (depth == 0 && kind == TokenKind::RParen) {
                break;
            }
            if (depth == 0 && kind == TokenKind::Comma) {
                args.push_back(std::move(current));
                current.clear();
                advance();
                continue;
            }
            if (kind == TokenKind::LParen || kind == TokenKind::LBracket ||
                kind == TokenKind::LBrace) {
                ++depth;
            } else if (kind == TokenKind::RParen || kind == TokenKind::RBracket ||
                       kind == TokenKind::RBrace) {
                --depth;
            }
            any = true;
            current.push_back(advance());
        }
        if (any || !args.empty()) {
            args.push_back(std::move(current));
        }
        advance(); // ')'
        return args;
    }

    /// A call argument. Inline-asm operands are written `in(reg) x`, `out("rax") y`,
    /// `inout(reg) z`: a register spec in parentheses followed by the operand expression.
    ExprPtr parseCallArg() {
        if (at(TokenKind::DotDot)) { // `..args`: spreads a parameter pack into the call
            ExprPtr spread = makeExpr(Expr::Kind::Spread, cur().span);
            advance();
            spread->text = expectIdent("a parameter pack name after `..`");
            spread->span = spanFrom(spread->span);
            return spread;
        }
        ExprPtr arg = parseExpr();
        const bool operand_head =
            arg->kind == Expr::Kind::Call && arg->lhs->kind == Expr::Kind::Name &&
            (arg->lhs->text == "in" || arg->lhs->text == "out" || arg->lhs->text == "inout") &&
            arg->args.size() == 1;
        if (!operand_head || at(TokenKind::Comma) || at(TokenKind::RParen)) {
            return arg;
        }
        ExprPtr operand = makeExpr(Expr::Kind::AsmOperand, arg->span);
        operand->text = arg->lhs->text;
        operand->lhs = std::move(arg->args.front());
        operand->rhs = parseExpr();
        operand->span = spanFrom(operand->span);
        return operand;
    }

    /// A `?` that propagates (`f()?`) rather than starts a ternary: it touches the operand before
    /// it, and what follows cannot be the first token of a ternary's value: the end of the line or
    /// of the file, a closing or separating token (`;`, `)`, `]`, `}`, `,`, `.`, `{`, another
    /// `?`), or an operator that only works between two operands (`f()? + 1`, `a? == b?`, `as`).
    /// `-`, `*`, `&`, `!` and `~` start an expression, so `c ? -1 : 1` stays a ternary.
    [[nodiscard]] bool tryOperatorAhead() const {
        if (!at(TokenKind::Question) || !continues() || !adjacent(prev(), cur())) {
            return false;
        }
        using enum TokenKind;
        const Token& next = peek(1);
        return next.newline_before ||
               std::ranges::contains(
                   std::array{Semicolon, RParen, RBracket, RBrace, LBrace, Comma, Colon, Dot, Question,
                              End,
                              Plus, Slash, Percent, Pipe, Caret, Shl, Shr, EqEq, NotEq, Lt, Le, Gt,
                              Ge, AndAnd, OrOr, As, Assign, DotDot, DotDotEq},
                   next.kind);
    }

    ExprPtr parsePostfix(ExprPtr expr) {
        while (true) {
            if (at(TokenKind::LParen) && continues()) {
                advance();
                ++nl_depth_;
                const bool saved_no_struct = no_struct_;
                no_struct_ = false;
                ExprPtr call = makeExpr(Expr::Kind::Call, expr->span);
                while (!at(TokenKind::RParen)) {
                    call->args.push_back(parseCallArg());
                    if (!match(TokenKind::Comma)) {
                        break;
                    }
                }
                no_struct_ = saved_no_struct;
                --nl_depth_;
                expect(TokenKind::RParen, "`)` closing the argument list");
                call->span = spanFrom(expr->span);
                call->lhs = std::move(expr);
                expr = std::move(call);
            } else if (tryOperatorAhead()) {
                // `e?`: written against the operand and followed by something that ends it, which
                // no ternary (`c ? a : b`, an operand always follows its `?`) can be.
                advance();
                ExprPtr propagate = makeExpr(Expr::Kind::Try, expr->span);
                propagate->span = spanFrom(expr->span);
                propagate->lhs = std::move(expr);
                expr = std::move(propagate);
            } else if (at(TokenKind::LBracket) && continues()) {
                advance();
                ++nl_depth_;
                const bool saved_no_struct = no_struct_;
                no_struct_ = false;
                // `base[lo..hi]` and `base[lo..]` are slices; `..=` is not a slice, because a view
                // has no defined inclusive end (`base[lo..=hi]` would be off by one).
                // Called right after `..`: reads the optional `hi`, closes the bracket and wraps `expr`.
                const auto finishSlice = [&](ExprPtr lo) {
                    ExprPtr slice = makeExpr(Expr::Kind::Slice, expr->span);
                    if (lo) {
                        slice->args.push_back(std::move(lo));
                    }
                    if (!at(TokenKind::RBracket)) {
                        slice->rhs = parseExpr(); // `hi`; absent when the bracket closes here
                    }
                    no_struct_ = saved_no_struct;
                    --nl_depth_;
                    expect(TokenKind::RBracket, "`]` closing the slice");
                    slice->span = spanFrom(expr->span);
                    slice->lhs = std::move(expr);
                    expr = std::move(slice);
                };
                if (at(TokenKind::DotDot)) {
                    advance();
                    finishSlice(nullptr);
                } else {
                    ExprPtr first = parseExpr();
                    if (at(TokenKind::DotDot)) {
                        advance();
                        finishSlice(std::move(first));
                    } else {
                        ExprPtr index = makeExpr(Expr::Kind::Index, expr->span);
                        index->rhs = std::move(first);
                        no_struct_ = saved_no_struct;
                        --nl_depth_;
                        expect(TokenKind::RBracket, "`]` closing the index");
                        index->span = spanFrom(expr->span);
                        index->lhs = std::move(expr);
                        expr = std::move(index);
                    }
                }
            } else if (at(TokenKind::Dot) && continues()) {
                advance();
                if (at(TokenKind::Int)) {
                    // `.0`, `.1`, ... in a tuple: the element at that position. The index travels
                    // in `int_value`; `text` stays empty, so nothing mistakes it for a field name.
                    ExprPtr elem = makeExpr(Expr::Kind::Field, expr->span);
                    elem->int_value = advance().int_value;
                    elem->span = spanFrom(expr->span);
                    elem->lhs = std::move(expr);
                    expr = std::move(elem);
                    continue;
                }
                if (!atMemberName()) {
                    expected("a member name after `.`");
                }
                ExprPtr field = makeExpr(Expr::Kind::Field, expr->span);
                field->text = advance().text;
                if (at(TokenKind::Lt) && genericArgsAhead()) {
                    field->generic_args = parseTypeArgs(); // `obj.m<T>(...)`
                }
                field->span = spanFrom(expr->span);
                field->lhs = std::move(expr);
                expr = std::move(field);
            } else if (at(TokenKind::Bang) && peekKind(1) == TokenKind::LParen &&
                       adjacent(prev(), cur()) && adjacent(cur(), peek(1)) &&
                       !pathOf(*expr).empty()) {
                advance(); // '!'
                ExprPtr call = makeExpr(Expr::Kind::MacroCall, expr->span);
                call->text = pathOf(*expr);
                call->macro_args = parseMacroArgs();
                call->span = spanFrom(expr->span);
                expr = std::move(call);
            } else if (at(TokenKind::LBrace) && !no_struct_ && continues() &&
                       !pathOf(*expr).empty() && looksLikeStructLiteral()) {
                ExprPtr lit = makeExpr(Expr::Kind::StructLiteral, expr->span);
                Type type = Type::makeNamed(splitPath(pathOf(*expr)));
                type.args = expr->generic_args;
                type.span = expr->span;
                lit->type = std::move(type);
                lit->fields = parseFieldInits();
                lit->span = spanFrom(expr->span);
                expr = std::move(lit);
            } else {
                break;
            }
        }
        return expr;
    }

    /// `fn(params) -> R { body }` as an expression: the body becomes a module-level function with a
    /// generated name, and the expression is a reference to it. A literal captures nothing; an
    /// outer local used inside it is an unknown name.
    ExprPtr parseFnLiteral() {
        const Span start = cur().span;
        expect(TokenKind::Fn, "`fn`");
        FnDecl fn;
        const std::string name = "__lambda" + std::to_string(lambda_count_++);
        parseFnTail(fn, name, false, true);
        fn.name = name;
        fn.name_span = start;
        fn.span = spanFrom(start);
        Item item;
        item.decl = std::move(fn);
        lambdas_.push_back(std::move(item));
        ExprPtr e = makeExpr(Expr::Kind::Name, start);
        e->text = name;
        return e;
    }

    /// `[move] |a: T, b| [-> R] { body }` or `[move] |x| expr`; `||` is the empty parameter list.
    /// Types may be left out where the context names a `fn(...)` type (checked by the lowering).
    /// `move` is a keyword only in front of `|`.
    ExprPtr parseClosure() {
        const Span start = cur().span;
        ExprPtr e = makeExpr(Expr::Kind::Closure, start);
        auto data = std::make_shared<ClosureData>();
        if (at(TokenKind::Ident) && cur().text == "move") {
            advance();
            data->is_move = true;
        }
        if (match(TokenKind::OrOr)) {
            // no parameters
        } else {
            expect(TokenKind::Pipe, "`|` starting the closure parameters");
            while (!at(TokenKind::Pipe)) {
                Param param;
                param.span = cur().span;
                param.name = expectIdent("a closure parameter name");
                if (match(TokenKind::Colon)) {
                    param.type = parseType();
                } else {
                    param.has_type = false;
                }
                param.span = spanFrom(param.span);
                data->params.push_back(std::move(param));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
            expect(TokenKind::Pipe, "`|` closing the closure parameters");
        }
        if (match(TokenKind::Arrow)) {
            data->has_return_type = true;
            data->return_type = parseType();
            if (!at(TokenKind::LBrace)) {
                expected("`{` with the closure body (a closure with `->` has a block body)");
            }
        }
        if (at(TokenKind::LBrace)) {
            data->body = parseBlock();
        } else {
            // `|x| x * 2`: the value is the whole expression after the bars.
            Stmt value;
            value.kind = Stmt::Kind::Expr;
            value.span = cur().span;
            const bool saved_no_struct = no_struct_;
            no_struct_ = false;
            value.expr = parseExpr();
            no_struct_ = saved_no_struct;
            value.span = spanFrom(value.span);
            data->body.span = value.span;
            data->body.statements.push_back(std::move(value));
            data->expr_body = true;
        }
        e->closure = std::move(data);
        e->span = spanFrom(start);
        return e;
    }

    ExprPtr parsePrimary() {
        const Token& tok = cur();
        const Span start = tok.span;
        switch (tok.kind) {
        case TokenKind::Pipe:
        case TokenKind::OrOr:
            return parseClosure();
        case TokenKind::Int: {
            ExprPtr e = makeExpr(Expr::Kind::IntLiteral, start);
            e->int_value = tok.int_value;
            advance();
            return e;
        }
        case TokenKind::Float: {
            ExprPtr e = makeExpr(Expr::Kind::FloatLiteral, start);
            e->float_value = tok.float_value;
            advance();
            return e;
        }
        case TokenKind::String: {
            ExprPtr e = makeExpr(Expr::Kind::StringLiteral, start);
            e->text = tok.text;
            advance();
            return e;
        }
        case TokenKind::ByteLit: {
            ExprPtr e = makeExpr(Expr::Kind::ByteLiteral, start);
            e->int_value = tok.int_value;
            advance();
            return e;
        }
        case TokenKind::True:
        case TokenKind::False: {
            ExprPtr e = makeExpr(Expr::Kind::BoolLiteral, start);
            e->bool_value = tok.kind == TokenKind::True;
            advance();
            return e;
        }
        case TokenKind::Ident: {
            if (tok.text == "move" &&
                (peekKind(1) == TokenKind::Pipe || peekKind(1) == TokenKind::OrOr)) {
                return parseClosure();
            }
            ExprPtr e = makeExpr(Expr::Kind::Name, start);
            e->text = tok.text;
            advance();
            if (at(TokenKind::Lt) && genericArgsAhead()) {
                e->generic_args = parseTypeArgs();
            }
            return e;
        }
        case TokenKind::SelfKw: {
            ExprPtr e = makeExpr(Expr::Kind::Name, start);
            e->text = "self";
            advance();
            return e;
        }
        case TokenKind::PrimType: {
            // A type used as an argument, e.g. `sizeOf(i32)`.
            ExprPtr e = makeExpr(Expr::Kind::Name, start);
            e->text = tok.text;
            advance();
            return e;
        }
        case TokenKind::LParen: {
            // A tuple value: `()` (empty), `(x,)` (one element) or `(a, b, ...)`, with an
            // optional trailing comma. `(x)` stays a parenthesized expression.
            advance();
            ++nl_depth_;
            const bool saved_no_struct = no_struct_;
            no_struct_ = false;
            std::vector<ExprPtr> elements;
            bool trailing_comma = false;
            while (!at(TokenKind::RParen)) {
                elements.push_back(parseExpr());
                if (!match(TokenKind::Comma)) {
                    break;
                }
                trailing_comma = at(TokenKind::RParen);
            }
            no_struct_ = saved_no_struct;
            --nl_depth_;
            expect(TokenKind::RParen, "`)`");
            if (elements.size() == 1 && !trailing_comma) {
                ExprPtr inner = std::move(elements.front());
                inner->span = spanFrom(start);
                return inner;
            }
            ExprPtr tuple = makeExpr(Expr::Kind::TupleLiteral, start);
            tuple->args = std::move(elements);
            tuple->span = spanFrom(start);
            return tuple;
        }
        case TokenKind::Fn:
            return parseFnLiteral();
        case TokenKind::LBracket:
            return parseArrayLiteral();
        case TokenKind::New:
            return parseNew();
        default:
            expected("an expression");
        }
    }

    /// `[]` directly followed, on the same line, by something that starts an element type
    /// (`[]i32`, `[]mut u8`, `[]Point`, `[][]u8`, `[]*T`, `[]fn(i32)`) is the slice type, never the
    /// empty array literal: no expression continues with a type name after `]`.
    [[nodiscard]] bool atSliceType() const {
        if (peekKind(1) != TokenKind::RBracket || peek(2).newline_before) {
            return false;
        }
        switch (peekKind(2)) {
        case TokenKind::PrimType:
        case TokenKind::Ident:
        case TokenKind::Star:
        case TokenKind::LBracket:
        case TokenKind::Fn:
            return true;
        default:
            return false;
        }
    }

    ExprPtr parseArrayLiteral() {
        const Span start = cur().span;
        if (atSliceType()) {
            ExprPtr operand = makeExpr(Expr::Kind::TypeOperand, start);
            operand->type = parseType();
            operand->span = spanFrom(start);
            return operand;
        }
        advance(); // '['
        ++nl_depth_;
        const bool saved_no_struct = no_struct_;
        no_struct_ = false;
        ExprPtr result;
        if (at(TokenKind::RBracket)) {
            result = makeExpr(Expr::Kind::ArrayList, start);
        } else {
            ExprPtr first = parseExpr();
            if (match(TokenKind::Semicolon)) {
                result = makeExpr(Expr::Kind::ArrayRepeat, start);
                result->lhs = std::move(first);
                result->rhs = parseExpr();
            } else {
                result = makeExpr(Expr::Kind::ArrayList, start);
                result->args.push_back(std::move(first));
                while (match(TokenKind::Comma)) {
                    if (at(TokenKind::RBracket)) {
                        break;
                    }
                    result->args.push_back(parseExpr());
                }
            }
        }
        no_struct_ = saved_no_struct;
        --nl_depth_;
        expect(TokenKind::RBracket, "`]` closing the array literal");
        result->span = spanFrom(start);
        return result;
    }

    ExprPtr parseNew() {
        const Span start = cur().span;
        advance(); // 'new'
        ExprPtr e = makeExpr(Expr::Kind::New, start);
        if (at(TokenKind::LParen)) {
            advance();
            ++nl_depth_;
            const bool saved_no_struct = no_struct_;
            no_struct_ = false;
            e->lhs = parseExpr();
            no_struct_ = saved_no_struct;
            --nl_depth_;
            expect(TokenKind::RParen, "`)` closing the memory argument");
        }
        e->type = parseType();
        if (at(TokenKind::LParen)) {
            // `new T(args)`: the class constructor (its `init`) runs on the new object.
            advance();
            ++nl_depth_;
            const bool saved_no_struct = no_struct_;
            no_struct_ = false;
            while (!at(TokenKind::RParen)) {
                e->args.push_back(parseCallArg());
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
            no_struct_ = saved_no_struct;
            --nl_depth_;
            expect(TokenKind::RParen, "`)` closing the constructor arguments");
            e->has_call_args = true;
        } else if (at(TokenKind::LBrace)) {
            e->fields = parseFieldInits();
        } else {
            expected("`(` with the constructor arguments or `{` with the field initializers after "
                     "the type of `new`");
        }
        e->span = spanFrom(start);
        return e;
    }

    // ------------------------------------------------------------------------------------
    // Statements

    Block parseBlock() {
        Block block;
        block.span = cur().span;
        expect(TokenKind::LBrace, "`{`");
        const int saved_depth = nl_depth_;
        const bool saved_no_struct = no_struct_;
        nl_depth_ = 0;
        no_struct_ = false;
        while (!at(TokenKind::RBrace)) {
            if (at(TokenKind::End)) {
                nl_depth_ = saved_depth;
                no_struct_ = saved_no_struct;
                failAt(cur(),
                       "unexpected end of file in a block",
                       "the block opened here is never closed",
                       {},
                       "add a closing `}`");
            }
            const std::size_t start = pos_;
            try {
                block.statements.push_back(parseStatement());
            } catch (const ParseAbort&) {
                syncStatement(start);
            }
        }
        nl_depth_ = saved_depth;
        no_struct_ = saved_no_struct;
        advance(); // '}'
        block.span = spanFrom(block.span);
        return block;
    }

    std::unique_ptr<Block> parseBlockPtr() { return std::make_unique<Block>(parseBlock()); }

    /// After a statement that did not end in a block: a line break or `}` must follow.
    void endOfStatement() {
        if (at(TokenKind::End) || at(TokenKind::RBrace) || cur().newline_before) {
            return;
        }
        if (for_header_ && (at(TokenKind::Semicolon) || at(TokenKind::LBrace))) {
            return; // `for init; cond; step {`
        }
        failAt(cur(),
               std::format("unexpected {} after the end of a statement", describeToken(cur())),
               "a statement ends at a line break",
               "Statements are separated by line breaks; there is no `;` terminator.",
               "put this on a new line");
    }

    Stmt parseStatement() {
        rejectDirective();
        std::vector<Attribute> attrs = parseAttributes();
        const Span start = attrs.empty() ? cur().span : attrs.front().span;
        Stmt stmt = parseStatementBody();
        stmt.attributes = std::move(attrs);
        stmt.span = spanFrom(start);
        registerAllowScopes(stmt.attributes, stmt.span);
        return stmt;
    }

    Stmt parseStatementBody() {
        Stmt stmt;
        // `label: while|do|for ...`
        if (at(TokenKind::Ident) && peekKind(1) == TokenKind::Colon &&
            (peekKind(2) == TokenKind::While || peekKind(2) == TokenKind::Do ||
             peekKind(2) == TokenKind::For)) {
            std::string label = advance().text;
            advance(); // ':'
            Stmt loop = parseStatementBody();
            loop.label = std::move(label);
            return loop;
        }
        switch (cur().kind) {
        case TokenKind::Local: {
            advance();
            parseLocalTail(stmt);
            endOfStatement();
            return stmt;
        }
        case TokenKind::Delete: {
            advance();
            stmt.kind = Stmt::Kind::Delete;
            stmt.expr = parseExpr();
            endOfStatement();
            return stmt;
        }
        case TokenKind::Const: {
            advance();
            stmt.kind = Stmt::Kind::Const;
            stmt.name = expectIdent("a constant name");
            if (match(TokenKind::Colon)) {
                stmt.has_type = true;
                stmt.type = parseType();
            }
            expect(TokenKind::Assign, "`=` and the value of the `const`");
            stmt.expr = parseExpr();
            endOfStatement();
            return stmt;
        }
        case TokenKind::Return: {
            advance();
            stmt.kind = Stmt::Kind::Return;
            if (!at(TokenKind::End) && !at(TokenKind::RBrace) && !cur().newline_before) {
                stmt.expr = parseExpr();
            }
            endOfStatement();
            return stmt;
        }
        case TokenKind::If:
            return parseIf();
        case TokenKind::While: {
            advance();
            stmt.kind = Stmt::Kind::While;
            stmt.expr = parseCondition();
            stmt.body = parseBlockPtr();
            return stmt;
        }
        case TokenKind::Do: {
            advance();
            stmt.kind = Stmt::Kind::DoWhile;
            stmt.body = parseBlockPtr();
            if (!at(TokenKind::While)) {
                expected("`while` and the condition after the `do` block");
            }
            advance();
            stmt.expr = parseExpr();
            endOfStatement();
            return stmt;
        }
        case TokenKind::For:
            return parseFor();
        case TokenKind::Switch:
            return parseSwitch();
        case TokenKind::Defer: {
            advance();
            stmt.kind = Stmt::Kind::Defer;
            stmt.step = std::make_unique<Stmt>(parseStatement());
            return stmt;
        }
        case TokenKind::Try: {
            advance();
            stmt.kind = Stmt::Kind::Try;
            stmt.body = parseBlockPtr();
            expect(TokenKind::Catch, "`catch` after the `try` block");
            if (at(TokenKind::Ident)) {
                stmt.name = advance().text;
            }
            stmt.else_body = parseBlockPtr();
            return stmt;
        }
        case TokenKind::Continue:
        case TokenKind::Break: {
            stmt.kind =
                advance().kind == TokenKind::Break ? Stmt::Kind::Break : Stmt::Kind::Continue;
            if (at(TokenKind::Ident) && !cur().newline_before) {
                stmt.name = advance().text;
            }
            endOfStatement();
            return stmt;
        }
        case TokenKind::Unsafe: {
            advance();
            stmt.kind = Stmt::Kind::Unsafe;
            stmt.body = parseBlockPtr();
            return stmt;
        }
        case TokenKind::LBrace: {
            stmt.kind = Stmt::Kind::Block;
            stmt.body = parseBlockPtr();
            return stmt;
        }
        default:
            break;
        }

        return parseSimpleStatement();
    }

    /// `target = v`, `target op= v`, `target++`, `target--` or an expression statement.
    /// The part of a `local` statement after the keyword: `name [: T | := expr] [= expr]`. Shared
    /// by statement position and the `for` initializer so both spellings of the declaration behave
    /// alike. On return the cursor is after the initializer (or after the type), never at `;`.
    /// `local x: T := expr` is refused because `:=` already means "infer the type".
    void parseLocalTail(Stmt& stmt) {
        stmt.kind = Stmt::Kind::Local;
        if (at(TokenKind::LParen)) {
            // Tuple target: `local (a, b) = expr` takes the tuple apart element by element.
            advance();
            ++nl_depth_;
            while (!at(TokenKind::RParen)) {
                stmt.names.push_back(expectIdent("a variable name inside the tuple target"));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
            --nl_depth_;
            expect(TokenKind::RParen, "`)` closing the tuple target");
            if (stmt.names.empty()) {
                expected("at least one name inside `( )`");
            }
            if (at(TokenKind::Colon) || at(TokenKind::ColonAssign)) {
                failAt(cur(),
                       "a tuple target cannot take a type or `:=`",
                       "each element takes its type from the tuple",
                       "The tuple's element types already fix the variables' types.",
                       "Write `local (a, b) = expr`.");
            }
            if (!match(TokenKind::Assign)) {
                expected("`=` and the tuple value to take apart");
            }
            stmt.expr = parseExpr();
            return;
        }
        stmt.name = expectIdent("a variable name");
        if (match(TokenKind::Colon)) {
            stmt.has_type = true;
            stmt.type = parseType();
            if (at(TokenKind::ColonAssign)) {
                failAt(cur(),
                       "a type and `:=` cannot be combined in one `local`",
                       "`:=` already infers the type from the initializer",
                       "`local x: T := v` writes the type twice.",
                       "Write `local x: T = v` or `local x := v`.");
            }
        } else if (match(TokenKind::ColonAssign)) {
            stmt.has_type = false;
            if (at(TokenKind::Semicolon) || at(TokenKind::RBrace) || at(TokenKind::End) ||
                cur().newline_before) {
                failAt(cur(),
                       "expected an initializer after `:=`",
                       "`:=` needs a value to infer the type from",
                       "Without a value the type cannot be inferred.",
                       "Write `local x := expr`, or `local x: T` to declare without a value.");
            }
            stmt.expr = parseExpr();
            return;
        }
        if (match(TokenKind::Assign)) {
            stmt.expr = parseExpr();
        } else if (!stmt.has_type) {
            expected("`:` with a type, `:=` with an inferred type, or `=` with an initializer");
        }
    }

    Stmt parseSimpleStatement() {
        Stmt stmt;
        stmt.span = cur().span;
        ExprPtr lhs = parseExpr();
        std::optional<BinaryOp> compound;
        bool incdec = false;
        if (continues()) {
            switch (cur().kind) {
            case TokenKind::PlusAssign:
                compound = BinaryOp::Add;
                break;
            case TokenKind::MinusAssign:
                compound = BinaryOp::Sub;
                break;
            case TokenKind::StarAssign:
                compound = BinaryOp::Mul;
                break;
            case TokenKind::SlashAssign:
                compound = BinaryOp::Div;
                break;
            case TokenKind::PercentAssign:
                compound = BinaryOp::Mod;
                break;
            case TokenKind::AmpAssign:
                compound = BinaryOp::BitAnd;
                break;
            case TokenKind::PipeAssign:
                compound = BinaryOp::BitOr;
                break;
            case TokenKind::CaretAssign:
                compound = BinaryOp::BitXor;
                break;
            case TokenKind::ShlAssign:
                compound = BinaryOp::Shl;
                break;
            case TokenKind::ShrAssign:
                compound = BinaryOp::Shr;
                break;
            case TokenKind::PlusPlus:
                compound = BinaryOp::Add;
                incdec = true;
                break;
            case TokenKind::MinusMinus:
                compound = BinaryOp::Sub;
                incdec = true;
                break;
            default:
                break;
            }
        }
        if ((at(TokenKind::Assign) && continues()) || compound) {
            switch (lhs->kind) {
            case Expr::Kind::Name:
            case Expr::Kind::Field:
            case Expr::Kind::Index:
                break;
            case Expr::Kind::Unary:
                if (lhs->unary_op == UnaryOp::Deref) {
                    break;
                }
                [[fallthrough]];
            default:
                failAt(cur(),
                       "invalid assignment target",
                       "cannot assign to this expression",
                       "Only a variable, a field, an index or a dereference can be assigned to.");
            }
            advance();
            stmt.kind = Stmt::Kind::Assign;
            stmt.target = std::move(lhs);
            if (incdec) {
                stmt.is_incdec = true;
                stmt.has_compound_op = true;
                stmt.compound_op = *compound;
                stmt.expr = makeExpr(Expr::Kind::IntLiteral, prev().span);
                stmt.expr->int_value = 1;
            } else {
                stmt.has_compound_op = compound.has_value();
                stmt.compound_op = compound.value_or(BinaryOp::Add);
                stmt.expr = parseExpr();
            }
            endOfStatement();
            return stmt;
        }
        if (at(TokenKind::ColonAssign)) {
            failAt(cur(),
                   "`:=` declares a new variable and cannot assign to an existing one",
                   "`:=` is only used in a `local` declaration",
                   "An assignment to an existing variable is written with `=`.",
                   "Write `local name := expr` to declare, or `name = expr` to assign.");
        }
        stmt.kind = Stmt::Kind::Expr;
        stmt.expr = std::move(lhs);
        endOfStatement();
        return stmt;
    }

    /// `for init; cond; step { body }` (every part optional) or `for i in lo..hi { body }`.
    Stmt parseFor() {
        Stmt stmt;
        advance(); // 'for'
        const bool saved_no_struct = no_struct_;
        const bool saved_header = for_header_;
        no_struct_ = true;
        if (at(TokenKind::Ident) &&
            (peekKind(1) == TokenKind::Ident || peekKind(1) == TokenKind::Comma) &&
            (peek(1).text == "in" || peekKind(1) == TokenKind::Comma)) {
            // `for x in expr { }`, `for x, i in expr { }` or `for i in lo..hi { }`.
            stmt.name = advance().text;
            const bool has_index = match(TokenKind::Comma);
            if (has_index) {
                if (!at(TokenKind::Ident)) {
                    expected("the index name after `,`");
                }
                stmt.names.push_back(advance().text);
            }
            if (!(at(TokenKind::Ident) && cur().text == "in")) {
                expected("`in` after the loop variable");
            }
            advance(); // `in`
            stmt.expr = parseExpr();
            if (at(TokenKind::LBrace)) { // iterates a collection or a parameter pack
                stmt.kind = Stmt::Kind::ForEach;
                no_struct_ = saved_no_struct;
                stmt.body = parseBlockPtr();
                return stmt;
            }
            if (has_index) {
                expected(
                    "`{` after the iterable; `for x, i in ...` needs a collection, not a range");
            }
            stmt.kind = Stmt::Kind::ForRange;
            if (!match(TokenKind::DotDot)) {
                expect(TokenKind::DotDotEq, "`..` or `..=` between the bounds of the range");
                stmt.range_inclusive = true;
            }
            stmt.expr2 = parseExpr();
            no_struct_ = saved_no_struct;
            stmt.body = parseBlockPtr();
            return stmt;
        }
        stmt.kind = Stmt::Kind::For;
        for_header_ = true;
        if (!at(TokenKind::Semicolon)) {
            stmt.init = std::make_unique<Stmt>(parseForClause());
        }
        expect(TokenKind::Semicolon, "`;` after the `for` initializer");
        if (!at(TokenKind::Semicolon)) {
            stmt.expr = parseExpr();
        }
        expect(TokenKind::Semicolon, "`;` after the `for` condition");
        if (!at(TokenKind::LBrace)) {
            stmt.step = std::make_unique<Stmt>(parseSimpleStatement());
        }
        for_header_ = saved_header;
        no_struct_ = saved_no_struct;
        stmt.body = parseBlockPtr();
        return stmt;
    }

    Stmt parseForClause() {
        if (!at(TokenKind::Local)) {
            return parseSimpleStatement();
        }
        Stmt stmt;
        stmt.span = cur().span;
        advance();
        parseLocalTail(stmt);
        return stmt;
    }

    /// `switch v { case a, b { ... } default { ... } }`
    Stmt parseSwitch() {
        Stmt stmt;
        stmt.kind = Stmt::Kind::Switch;
        advance(); // 'switch'
        // `switch { case a < b { } ... }` has no subject: each `case` lists conditions instead.
        if (!at(TokenKind::LBrace)) {
            stmt.expr = parseCondition();
        }
        expect(TokenKind::LBrace, "`{` opening the `switch` body");
        const int saved_depth = nl_depth_;
        const bool saved_no_struct = no_struct_;
        nl_depth_ = 0;
        const auto restore = [&] {
            nl_depth_ = saved_depth;
            no_struct_ = saved_no_struct;
        };
        while (!at(TokenKind::RBrace)) {
            if (at(TokenKind::End)) {
                restore();
                failAt(cur(),
                       "unexpected end of file in a `switch`",
                       "the `switch` opened here is never closed");
            }
            SwitchCase arm;
            arm.span = cur().span;
            if (match(TokenKind::Default)) {
                arm.is_default = true;
            } else if (match(TokenKind::Case)) {
                no_struct_ = true;
                do {
                    arm.values.push_back(parseExpr());
                } while (match(TokenKind::Comma));
                no_struct_ = false;
            } else {
                restore();
                expected("`case` or `default` inside a `switch`");
            }
            arm.body = parseBlockPtr();
            arm.span = spanFrom(arm.span);
            stmt.cases.push_back(std::move(arm));
        }
        restore();
        advance(); // '}'
        return stmt;
    }

    /// `if`/`while` condition: struct literals are not allowed (the `{` starts the block).
    ExprPtr parseCondition() {
        const bool saved = no_struct_;
        no_struct_ = true;
        ExprPtr cond = parseExpr();
        no_struct_ = saved;
        return cond;
    }

    Stmt parseIf() {
        Stmt stmt;
        stmt.kind = Stmt::Kind::If;
        const Span start = cur().span;
        advance(); // 'if'
        stmt.expr = parseCondition();
        stmt.body = parseBlockPtr();
        if (at(TokenKind::Else)) {
            advance();
            if (at(TokenKind::If)) {
                Stmt nested = parseIf();
                auto wrapper = std::make_unique<Block>();
                wrapper->span = nested.span;
                wrapper->statements.push_back(std::move(nested));
                stmt.else_body = std::move(wrapper);
                stmt.else_is_if = true;
            } else {
                stmt.else_body = parseBlockPtr();
            }
        }
        stmt.span = spanFrom(start);
        return stmt;
    }

    // ------------------------------------------------------------------------------------
    // Declarations

    struct DeclStart {
        std::vector<Attribute> attributes;
        Modifiers modifiers;
        Span start;
    };

    void finishDecl(DeclBase& base, const DeclStart& s, std::string name, const Span& name_span) {
        base.name = std::move(name);
        base.attributes = s.attributes;
        base.name_span = name_span;
        base.span = spanFrom(s.start);
    }

    /// Items that end without a closing brace must be followed by a line break.
    void endOfItemLine() {
        if (at(TokenKind::End) || at(TokenKind::RBrace) || cur().newline_before) {
            return;
        }
        failAt(cur(),
               std::format("unexpected {} after the end of a declaration", describeToken(cur())),
               "a declaration ends at a line break",
               {},
               "put this on a new line");
    }

    void parseItem(Program& program) {
        rejectDirective();
        DeclStart s;
        s.start = cur().span;
        s.attributes = parseAttributes();
        if (!s.attributes.empty()) {
            s.start = s.attributes.front().span;
        }
        rejectDirective();

        // Re-exports: `export * ...` / `export { ... } from ...`.
        if (at(TokenKind::Export) &&
            (peekKind(1) == TokenKind::Star || peekKind(1) == TokenKind::LBrace)) {
            parseExportFrom(program, s);
            return;
        }

        parseModifiers(s.modifiers);

        const Token& kw = cur();
        switch (kw.kind) {
        case TokenKind::Fn:
            if (peek(1).kind == TokenKind::Ident && peek(2).kind == TokenKind::Assign) {
                addItem(program, s, parseFnAlias(s));
            } else {
                addItem(program, s, parseFn(s, /*member=*/false));
            }
            return;
        case TokenKind::Global:
            addItem(program, s, parseGlobal(s));
            return;
        case TokenKind::Const:
            addItem(program, s, parseConstItem(s));
            return;
        case TokenKind::Struct:
            addItem(program, s, parseStruct(s));
            return;
        case TokenKind::Class:
            addItem(program, s, parseClass(s));
            return;
        case TokenKind::Interface:
            addItem(program, s, parseInterface(s));
            return;
        case TokenKind::Impl:
            if (auto impl = parseImpl(program, s)) {
                addItem(program, s, std::move(*impl));
            }
            return;
        case TokenKind::Enum:
            addEnum(program, s, parseEnum(s));
            return;
        case TokenKind::Type:
            addItem(program, s, parseTypeAlias(s));
            return;
        case TokenKind::Define:
            addItem(program, s, parseDefine(s));
            return;
        case TokenKind::Import:
        case TokenKind::From:
            addItem(program, s, parseImport(s));
            return;
        case TokenKind::Ident:
            addItem(program, s, parseTopLevelCall(s));
            return;
        default:
            failAt(
                kw,
                std::format("expected a declaration, found {}", describeToken(kw)),
                "expected a declaration here",
                {},
                "declarations start with fn, global, struct, class, interface, impl, enum, type, "
                "define, import, from or export");
        }
    }

    void addItem(Program& program, DeclStart& s, Decl decl) {
        Item item;
        item.modifiers = s.modifiers;
        item.decl = std::move(decl);
        registerAllowScopes(s.attributes, declBase(item).span);
        checkModifiers(item);
        program.items.push_back(std::move(item));
    }

    void parseModifiers(Modifiers& mods) {
        while (true) {
            if (at(TokenKind::Export)) {
                if (mods.exported) {
                    failAt(cur(), "duplicate `export`", "already exported");
                }
                advance();
                mods.exported = true;
                if (at(TokenKind::LParen) && peekKind(1) == TokenKind::Ident &&
                    peekKind(2) == TokenKind::RParen) {
                    advance();
                    const Token& abi = advance();
                    advance();
                    if (abi.text != "c") {
                        failAt(abi,
                               std::format("unknown export ABI `{}`", abi.text),
                               "only `export(c)` exists");
                    }
                    mods.export_c = true;
                }
            } else if (at(TokenKind::Declare)) {
                if (mods.declared) {
                    failAt(cur(), "duplicate `declare`", "already declared");
                }
                advance();
                mods.declared = true;
            } else if (at(TokenKind::Register)) {
                if (mods.registered) {
                    failAt(cur(), "duplicate `register`", "already registered");
                }
                advance();
                mods.registered = true;
                if (at(TokenKind::LParen) && peekKind(1) == TokenKind::Ident &&
                    peekKind(2) == TokenKind::RParen) {
                    advance();
                    mods.register_role = advance().text;
                    advance();
                }
            } else {
                return;
            }
        }
    }

    /// Prefix applicability (soft errors: parsing continues).
    void checkModifiers(const Item& item) {
        const Modifiers& m = item.modifiers;
        const DeclBase& base = declBase(item);
        const std::string kind = declKindName(item.decl);
        const auto reject = [&](std::string_view prefix) {
            softError(base.span,
                      std::format("`{}` cannot be applied to {} `{}`", prefix, kind, base.name),
                      std::format("`{}` is not valid on {}", prefix, kind));
        };
        const bool is_import = std::holds_alternative<ImportDecl>(item.decl);
        const bool is_call = std::holds_alternative<TopLevelCallDecl>(item.decl) ||
                             std::holds_alternative<MacroCallDecl>(item.decl);
        const bool is_impl = std::holds_alternative<ImplDecl>(item.decl);
        const bool is_define = std::holds_alternative<DefineConstDecl>(item.decl) ||
                               std::holds_alternative<DefineMacroDecl>(item.decl);
        const bool is_enum = std::holds_alternative<EnumDecl>(item.decl);
        if (m.exported && (is_import || is_call || is_impl)) {
            reject("export");
        }
        if (m.declared && (is_import || is_call || is_impl || is_define)) {
            reject("declare");
        }
        if (m.declared && is_enum) {
            reject("declare");
        }
        if (m.registered && (is_import || is_call || is_impl || is_define)) {
            reject("register");
        }
        if (m.export_c && !(std::holds_alternative<FnDecl>(item.decl) ||
                            std::holds_alternative<GlobalDecl>(item.decl))) {
            reject("export(c)");
        }
    }

    // --- fn ----------------------------------------------------------------------------------

    /// The parameter list, the optional return type and the body of a function (after its name):
    /// shared by named functions and function literals. `needs_body` makes a missing body an error.
    void parseFnTail(FnDecl& fn, const std::string& name, bool member, bool needs_body) {
        expect(TokenKind::LParen, "`(` starting the parameter list");
        ++nl_depth_;
        while (!at(TokenKind::RParen)) {
            Param param;
            param.span = cur().span;
            if (at(TokenKind::SelfKw)) {
                advance();
                param.name = "self";
                param.is_self = true;
                if (match(TokenKind::Colon)) { // `self: T` (inherent `impl` only, see parseMethod)
                    param.type = parseType();
                    param.typed_self = true;
                }
            } else if (at(TokenKind::DotDot)) { // `..args`: the untyped trailing pack
                advance();
                param.name = expectIdent("a parameter pack name after `..`");
                param.is_pack = true;
            } else {
                param.name = expectIdent("a parameter name");
                expect(TokenKind::Colon, "`:` and the parameter type");
                param.type = parseType();
            }
            param.span = spanFrom(param.span);
            fn.params.push_back(std::move(param));
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        --nl_depth_;
        expect(TokenKind::RParen, "`)` closing the parameter list");

        if (match(TokenKind::Arrow)) {
            fn.has_return_type = true;
            fn.return_type = parseType();
        } else {
            fn.return_type = Type::makeVoid();
        }

        if (at(TokenKind::LBrace)) {
            fn.has_body = true;
            fn.body = parseBlock();
        } else if (needs_body) {
            // A function that needs a body has none here.
            failAt(cur(),
                   std::format("expected `{{` starting the body of `{}`, found {}",
                               name,
                               describeToken(cur())),
                   "function body expected here",
                   {},
                   "write a body, or `declare fn` for a function defined elsewhere");
        } else {
            endOfItemLine();
        }
        (void)member;
    }

    /// The operator a function name spells after `fn` (`fn +(...)`, `fn []=(...)`, `fn ()(...)`):
    /// the token(s) are consumed and the function is named `operator<symbol>`. nullopt (nothing
    /// consumed) when the next token is no operator. The table is `operatorFunctionName`.
    std::optional<std::string> parseOperatorName() {
        const auto simple = [&](std::string_view symbol) {
            advance();
            return std::string("operator") + std::string(symbol);
        };
        switch (cur().kind) {
        case TokenKind::Plus: return simple("+");
        case TokenKind::Minus: return simple("-");
        case TokenKind::Star: return simple("*");
        case TokenKind::Slash: return simple("/");
        case TokenKind::Percent: return simple("%");
        case TokenKind::Amp: return simple("&");
        case TokenKind::Pipe: return simple("|");
        case TokenKind::Caret: return simple("^");
        case TokenKind::Shl: return simple("<<");
        case TokenKind::Shr: return simple(">>");
        case TokenKind::EqEq: return simple("==");
        case TokenKind::NotEq: return simple("!=");
        case TokenKind::Lt: return simple("<");
        case TokenKind::Le: return simple("<=");
        case TokenKind::Gt: return simple(">");
        case TokenKind::Ge: return simple(">=");
        case TokenKind::Bang: return simple("!");
        case TokenKind::Tilde: return simple("~");
        case TokenKind::PlusAssign: return simple("+=");
        case TokenKind::MinusAssign: return simple("-=");
        case TokenKind::StarAssign: return simple("*=");
        case TokenKind::SlashAssign: return simple("/=");
        case TokenKind::PercentAssign: return simple("%=");
        case TokenKind::AmpAssign: return simple("&=");
        case TokenKind::PipeAssign: return simple("|=");
        case TokenKind::CaretAssign: return simple("^=");
        case TokenKind::ShlAssign: return simple("<<=");
        case TokenKind::ShrAssign: return simple(">>=");
        case TokenKind::PlusPlus: return simple("++");
        case TokenKind::MinusMinus: return simple("--");
        case TokenKind::LBracket:
            if (peekKind(1) != TokenKind::RBracket) {
                return std::nullopt;
            }
            advance();
            advance();
            return match(TokenKind::Assign) ? "operator[]=" : "operator[]";
        case TokenKind::LParen:
            if (peekKind(1) != TokenKind::RParen || peekKind(2) != TokenKind::LParen) {
                return std::nullopt;
            }
            advance();
            advance();
            return "operator()";
        default:
            return std::nullopt;
        }
    }

    FnDecl parseFn(const DeclStart& s, bool member) {
        FnDecl fn;
        Span fn_start = cur().span;
        expect(TokenKind::Fn, "`fn`");
        const Token& name_tok = cur();
        const Span name_span = name_tok.span;
        std::string name;
        if (auto op = parseOperatorName()) {
            name = std::move(*op);
        } else if (member ? atMemberName() : at(TokenKind::Ident)) {
            name = advance().text;
        } else {
            expected("a function name");
        }
        if (at(TokenKind::Lt)) {
            fn.generic_params = parseGenericParams();
        }
        parseFnTail(fn, name, member, !member && !s.modifiers.declared);
        finishDecl(fn, s, name, name_span);
        if (member) {
            fn.span = spanFrom(fn_start);
        }
        return fn;
    }

    // --- global ------------------------------------------------------------------------------

    /// `fn name = target` or `fn name = module.target` at module level.
    FnAliasDecl parseFnAlias(const DeclStart& s) {
        FnAliasDecl a;
        advance(); // 'fn'
        const Token& name_tok = cur();
        const std::string name = expectIdent("a function alias name");
        expect(TokenKind::Assign, "`=` and the function the alias names");
        a.target_span = cur().span;
        a.target.push_back(expectIdent("a function name"));
        while (match(TokenKind::Dot)) {
            a.target.push_back(expectIdent("a function name"));
        }
        a.target_span = spanFrom(a.target_span);
        endOfItemLine();
        finishDecl(a, s, name, name_tok.span);
        return a;
    }

    GlobalDecl parseGlobal(const DeclStart& s) {
        GlobalDecl g;
        advance(); // 'global'
        const Token& name_tok = cur();
        const std::string name = expectIdent("a global name");
        rejectTypeParams();
        expect(TokenKind::Colon, "`:` and the type of the global");
        g.type = parseType();
        if (match(TokenKind::Assign)) {
            g.init = parseExpr();
        }
        endOfItemLine();
        finishDecl(g, s, name, name_tok.span);
        return g;
    }

    /// `const NAME [: T] = expr` at module level. The type is optional.
    GlobalDecl parseConstItem(const DeclStart& s) {
        GlobalDecl g;
        g.is_const = true;
        advance(); // 'const'
        const Token& name_tok = cur();
        const std::string name = expectIdent("a constant name");
        rejectTypeParams();
        g.has_type = match(TokenKind::Colon);
        if (g.has_type) {
            g.type = parseType();
        }
        expect(TokenKind::Assign, "`=` and the value of the `const`");
        g.init = parseExpr();
        endOfItemLine();
        finishDecl(g, s, name, name_tok.span);
        return g;
    }

    // --- struct / class / interface / impl ---------------------------------------------------

    FieldDecl parseField(std::vector<Attribute> attrs) {
        FieldDecl field;
        field.span = attrs.empty() ? cur().span : attrs.front().span;
        field.attributes = std::move(attrs);
        field.name = expectIdent("a field name");
        expect(TokenKind::Colon, "`:` and the field type");
        field.type = parseType();
        field.span = spanFrom(field.span);
        return field;
    }

    /// Separator between members: `,`, or a line break, or the closing `}`.
    void memberSeparator() {
        if (match(TokenKind::Comma)) {
            return;
        }
        if (!at(TokenKind::RBrace) && !cur().newline_before) {
            expected("`,`, a line break or `}`");
        }
    }

    StructDecl parseStruct(const DeclStart& s) {
        StructDecl decl;
        advance(); // 'struct'
        const Token& name_tok = cur();
        const std::string name = expectIdent("a struct name");
        if (at(TokenKind::Lt)) {
            decl.generic_params = parseGenericParams();
        }
        if (at(TokenKind::LBrace)) {
            advance();
            const int saved_depth = nl_depth_;
            nl_depth_ = 0;
            while (!at(TokenKind::RBrace)) {
                rejectDirective();
                std::vector<Attribute> attrs = parseAttributes();
                if (at(TokenKind::Fn) || at(TokenKind::Virtual) || at(TokenKind::Override)) {
                    failAt(cur(),
                           "structs cannot have methods",
                           "method declared inside a struct",
                           "`struct` only carries data.",
                           "use `class` for methods and inheritance, or an `impl` block for "
                           "interfaces");
                }
                decl.fields.push_back(parseField(std::move(attrs)));
                memberSeparator();
            }
            nl_depth_ = saved_depth;
            advance(); // '}'
        } else if (s.modifiers.declared) {
            decl.opaque = true;
            endOfItemLine();
        } else {
            expected("`{` with the fields of the struct");
        }
        finishDecl(decl, s, name, name_tok.span);
        return decl;
    }

    ClassDecl parseClass(const DeclStart& s) {
        ClassDecl decl;
        advance(); // 'class'
        const Token& name_tok = cur();
        const std::string name = expectIdent("a class name");
        if (at(TokenKind::Lt)) {
            decl.generic_params = parseGenericParams();
        }
        if (match(TokenKind::Colon)) {
            decl.has_base = true;
            decl.base.push_back(expectIdent("a base class name"));
            while (at(TokenKind::Dot) && peekKind(1) == TokenKind::Ident) {
                advance();
                decl.base.push_back(advance().text);
            }
        }
        if (at(TokenKind::LBrace)) {
            advance();
            const int saved_depth = nl_depth_;
            nl_depth_ = 0;
            while (!at(TokenKind::RBrace)) {
                rejectDirective();
                std::vector<Attribute> attrs = parseAttributes();
                if (at(TokenKind::Fn) || at(TokenKind::Virtual) || at(TokenKind::Override)) {
                    decl.methods.push_back(parseMethod(std::move(attrs)));
                } else {
                    decl.fields.push_back(parseField(std::move(attrs)));
                    memberSeparator();
                }
            }
            nl_depth_ = saved_depth;
            advance(); // '}'
        } else if (s.modifiers.declared) {
            decl.opaque = true;
            endOfItemLine();
        } else {
            expected("`{` with the members of the class");
        }
        finishDecl(decl, s, name, name_tok.span);
        return decl;
    }

    /// `[virtual|override] fn ...` inside class/interface/impl bodies.
    FnDecl parseMethod(std::vector<Attribute> attrs, bool inherent = false) {
        DeclStart ms;
        ms.start = attrs.empty() ? cur().span : attrs.front().span;
        ms.attributes = std::move(attrs);
        bool is_virtual = false;
        bool is_override = false;
        if (match(TokenKind::Virtual)) {
            is_virtual = true;
        } else if (match(TokenKind::Override)) {
            is_override = true;
        }
        FnDecl fn = parseFn(ms, /*member=*/true);
        fn.is_virtual = is_virtual;
        fn.is_override = is_override;
        fn.span = spanFrom(ms.start);
        fn.attributes = ms.attributes;
        registerAllowScopes(fn.attributes, fn.span);
        if (!inherent) {
            for (const Param& p : fn.params) {
                if (p.typed_self) {
                    softError(p.span,
                              "`self: T` is only written in an inherent `impl` (`impl T { ... }`)",
                              "a class, interface or `impl ... for` method has an implicit `self`");
                }
            }
        }
        return fn;
    }

    InterfaceDecl parseInterface(const DeclStart& s) {
        InterfaceDecl decl;
        advance(); // 'interface'
        const Token& name_tok = cur();
        const std::string name = expectIdent("an interface name");
        if (at(TokenKind::Lt)) {
            decl.generic_params = parseGenericParams();
        }
        if (at(TokenKind::LBrace)) {
            advance();
            const int saved_depth = nl_depth_;
            nl_depth_ = 0;
            while (!at(TokenKind::RBrace)) {
                rejectDirective();
                std::vector<Attribute> attrs = parseAttributes();
                if (!at(TokenKind::Fn)) {
                    expected("`fn` (an interface only holds method signatures)");
                }
                decl.methods.push_back(parseMethod(std::move(attrs)));
            }
            nl_depth_ = saved_depth;
            advance(); // '}'
        } else if (s.modifiers.declared) {
            decl.opaque = true;
            endOfItemLine();
        } else {
            expected("`{` with the method signatures of the interface");
        }
        finishDecl(decl, s, name, name_tok.span);
        return decl;
    }

    /// `impl Iface for T { ... }` becomes an `ImplDecl`. The inherent form `impl T { fn +(self: T,
    /// o: T) -> T { } fn len(self: *T) -> usize { } }` holds operators and methods of a struct; each
    /// becomes a module-level function named `operator<symbol>` or `T.name` (`self` is its first
    /// parameter, `self` alone meaning `self: T`), so it overloads like any function and nothing
    /// after the parser knows about the block. Those functions are added to `program` and the
    /// result is empty.
    std::optional<ImplDecl> parseImpl(Program& program, DeclStart& s) {
        ImplDecl decl;
        advance(); // 'impl'
        if (at(TokenKind::Lt)) {
            decl.generic_params = parseGenericParams();
        }
        const Token& name_tok = cur();
        decl.interface_path.push_back(expectIdent("an interface or type name"));
        while (at(TokenKind::Dot) && peekKind(1) == TokenKind::Ident) {
            advance();
            decl.interface_path.push_back(advance().text);
        }
        if (at(TokenKind::Lt)) {
            decl.interface_args = parseTypeArgs();
        }
        const bool inherent = at(TokenKind::LBrace);
        if (inherent) {
            decl.target = Type::makeNamed(decl.interface_path);
            decl.target.args = decl.interface_args;
            decl.target.span = name_tok.span;
        } else {
            expect(TokenKind::For, "`for` and the implementing type");
            decl.target = parseType();
        }
        expect(TokenKind::LBrace, "`{` with the methods of the impl");
        const int saved_depth = nl_depth_;
        nl_depth_ = 0;
        while (!at(TokenKind::RBrace)) {
            rejectDirective();
            std::vector<Attribute> attrs = parseAttributes();
            if (!at(TokenKind::Fn)) {
                expected("`fn`");
            }
            decl.methods.push_back(parseMethod(std::move(attrs), inherent));
        }
        nl_depth_ = saved_depth;
        advance(); // '}'
        if (!inherent) {
            std::string name = decl.interface_path.back();
            finishDecl(decl, s, name, name_tok.span);
            return decl;
        }
        for (FnDecl& m : decl.methods) {
            if (!m.name.starts_with("operator")) {
                // A method of a struct: the function `T.name`, found by `value.name(...)` and by
                // `for x in value` (`iter`, `next`).
                m.name = decl.target.path.back() + "." + m.name;
            }
            for (Param& p : m.params) {
                if (p.is_self) { // `self` alone is the target by value; `self: T` names its type
                    p.is_self = false;
                    if (!p.typed_self) {
                        p.type = decl.target;
                    }
                }
            }
            m.generic_params.insert(
                m.generic_params.begin(), decl.generic_params.begin(), decl.generic_params.end());
            addItem(program, s, std::move(m));
        }
        return std::nullopt;
    }

    // --- enum / type / define ----------------------------------------------------------------

    EnumDecl parseEnum(const DeclStart& s) {
        EnumDecl decl;
        advance(); // 'enum'
        const Token& name_tok = cur();
        const std::string name = expectIdent("an enum name");
        if (at(TokenKind::Lt)) {
            decl.generic_params = parseGenericParams();
        }
        if (match(TokenKind::Colon)) {
            decl.has_underlying = true;
            decl.underlying = parseType();
        } else {
            decl.underlying = Type::makeInt(32, true);
        }
        expect(TokenKind::LBrace, "`{` with the enum members");
        const int saved_depth = nl_depth_;
        nl_depth_ = 0;
        while (!at(TokenKind::RBrace)) {
            rejectDirective();
            EnumMember member;
            member.attributes = parseAttributes();
            member.span = cur().span;
            member.name = expectIdent("an enum member name");
            if (match(TokenKind::LParen)) {
                member.has_payload = true;
                ++nl_depth_;
                while (!at(TokenKind::RParen)) {
                    member.payload.push_back(parseType());
                    if (!match(TokenKind::Comma)) {
                        break;
                    }
                }
                --nl_depth_;
                expect(TokenKind::RParen, "`)` closing the payload types");
            } else if (match(TokenKind::Assign)) {
                member.value = parseExpr();
            }
            member.span = spanFrom(member.span);
            decl.members.push_back(std::move(member));
            memberSeparator();
        }
        nl_depth_ = saved_depth;
        advance(); // '}'
        finishDecl(decl, s, name, name_tok.span);
        return decl;
    }

    /// A value enum is one item. A payload enum (`Add(A, B)` members) becomes five kinds of
    /// ordinary items: `Name$Tag` (the tag enum), `Name$Vi` (one struct per variant with data),
    /// `Name$Data` (their union) and `Name` (`{ tag, data }` plus the variant list). The union and
    /// the enum carry the enum's type parameters; the tag enum takes none (it is `i32` either
    /// way), and the payload structs take none of their own either, because their fields are the
    /// enum's payload types and they exist only as parts of an enum instance.
    void addEnum(Program& program, DeclStart& s, EnumDecl decl) {
        if (std::ranges::none_of(decl.members, &EnumMember::has_payload)) {
            addItem(program, s, std::move(decl));
            return;
        }
        if (decl.has_underlying || std::ranges::any_of(decl.members, [](const EnumMember& m) {
                return m.value != nullptr;
            })) {
            diag_.error(
                "E0747",
                decl.name_span,
                std::format("enum `{}` mixes payload variants with constant values", decl.name),
                "a payload enum has no `: T` and no `= value`; the tag is numbered by position");
        }
        const auto base = [&](DeclBase& out, std::string name) {
            out.name = std::move(name);
            out.attributes = {};
            out.span = decl.span;
            out.name_span = decl.name_span;
        };
        const std::string tag_name = decl.name + "$Tag";
        const std::string data_name = decl.name + "$Data";

        // The data part of a generic enum names its arguments: `Result<T, E>` holds
        // `data: Result$Data<T, E>` and each variant field is `Result$Ok<T, E>`. The tag enum does
        // not: it is `i32` by position whatever the parameters are, so `Result$Tag` has no
        // parameters and is never instantiated (`Sema::instantiate` shares it between instances).
        const auto selfArgs = [&](std::string_view suffix, const Span& span) {
            Type t = Type::makeNamed({decl.name + std::string(suffix)});
            t.span = span;
            if (decl.generic_params.empty()) {
                return t;
            }
            for (const std::string& param : decl.generic_params) {
                t.args.push_back(Type::makeNamed({param}));
            }
            return t;
        };
        const auto part = [&](std::string_view suffix, const Span& span) {
            Type t = Type::makeNamed({decl.name + std::string(suffix)});
            t.span = span;
            return t;
        };

        // The tag enum is `i32` by position whatever the enum's parameters are, so it stays an
        // ordinary enum; only the data part and the enum itself name the enum's arguments.
        EnumDecl tag;
        base(tag, tag_name);
        tag.underlying = Type::makeInt(32, true);
        StructDecl data;
        base(data, data_name);
        data.generic_params = decl.generic_params;
        data.is_union = true;
        StructDecl sum;
        base(sum, decl.name);
        sum.generic_params = decl.generic_params;
        sum.attributes = decl.attributes;
        sum.fields.push_back({"tag", part("$Tag", decl.name_span), {}, decl.name_span});
        sum.fields.push_back({"data", selfArgs("$Data", decl.name_span), {}, decl.name_span});

        std::vector<StructDecl> payloads;
        for (EnumMember& m : decl.members) {
            tag.members.push_back(EnumMember{.name = m.name, .span = m.span});
            sum.variants.push_back({m.name, m.payload});
            if (m.payload.empty()) {
                continue;
            }
            StructDecl& p = payloads.emplace_back();
            base(p, std::format("{}${}", decl.name, m.name));
            p.is_payload = true;
            // The payload struct of a generic enum carries **no** type parameters of its own: `T`
            // is the enum's parameter, and the struct only ever exists as an instance of the enum
            // (`Result$Ok<i32, E>`), made by `Sema::instantiate` together with the enum itself. The
            // generated items name each other with the enum's arguments, so a bare `Result$Ok`
            // would be an undecided name (E0724); each field keeps the enum's own type text and is
            // substituted by the enum's instance.
            for (const std::size_t i : std::views::iota(std::size_t{0}, m.payload.size())) {
                p.fields.push_back({std::format("_{}", i), m.payload[i], {}, m.span});
            }
            data.fields.push_back({m.name, selfArgs("$" + m.name, m.span), {}, m.span});
        }
        addItem(program, s, std::move(tag));
        for (StructDecl& p : payloads) {
            addItem(program, s, std::move(p));
        }
        addItem(program, s, std::move(data));
        addItem(program, s, std::move(sum));
    }

    TypeAliasDecl parseTypeAlias(const DeclStart& s) {
        TypeAliasDecl decl;
        advance(); // 'type'
        const Token& name_tok = cur();
        const std::string name = expectIdent("a type alias name");
        if (at(TokenKind::Lt)) {
            decl.generic_params = parseGenericParams();
        }
        expect(TokenKind::Assign, "`=` and the aliased type");
        decl.target = parseType();
        endOfItemLine();
        finishDecl(decl, s, name, name_tok.span);
        return decl;
    }

    Decl parseDefine(const DeclStart& s) {
        advance(); // 'define'
        const Token& name_tok = cur();
        const std::string name = expectIdent("a define name");
        if (at(TokenKind::LParen)) {
            DefineMacroDecl decl;
            advance();
            ++nl_depth_;
            while (!at(TokenKind::RParen)) {
                MacroParam param;
                param.span = cur().span;
                param.name = expectIdent("a macro parameter name");
                expect(TokenKind::Colon, "`:` and the parameter kind");
                const Token& kind_tok = cur();
                if (at(TokenKind::Type)) {
                    param.kind = MacroParam::Kind::Type;
                    advance();
                } else if (at(TokenKind::Ident) && kind_tok.text == "expr") {
                    param.kind = MacroParam::Kind::Expr;
                    advance();
                } else if (at(TokenKind::Ident) && kind_tok.text == "ident") {
                    param.kind = MacroParam::Kind::Ident;
                    advance();
                } else if (at(TokenKind::Ident) && kind_tok.text == "block") {
                    param.kind = MacroParam::Kind::Block;
                    advance();
                } else if (at(TokenKind::Ident) && kind_tok.text == "items") {
                    param.kind = MacroParam::Kind::Items;
                    advance();
                } else {
                    expected("a macro parameter kind (expr, type, ident, block or items)");
                }
                param.span = spanFrom(param.span);
                decl.params.push_back(std::move(param));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
            --nl_depth_;
            expect(TokenKind::RParen, "`)` closing the macro parameters");
            expect(TokenKind::LBrace, "`{` starting the macro body");
            int depth = 1;
            while (true) {
                if (at(TokenKind::End)) {
                    expected("`}` closing the macro body");
                }
                if (at(TokenKind::LBrace)) {
                    ++depth;
                } else if (at(TokenKind::RBrace) && --depth == 0) {
                    break;
                }
                decl.body.push_back(advance());
            }
            advance(); // closing '}'
            finishDecl(decl, s, name, name_tok.span);
            return decl;
        }
        DefineConstDecl decl;
        expect(TokenKind::Assign, "`=` and the constant value, or `(` for a macro");
        decl.value = parseExpr();
        endOfItemLine();
        finishDecl(decl, s, name, name_tok.span);
        return decl;
    }

    // --- import / export ---------------------------------------------------------------------

    std::vector<ImportName> parseNameList() {
        std::vector<ImportName> names;
        while (true) {
            ImportName n;
            n.span = cur().span;
            n.name = expectIdent("a name");
            if (match(TokenKind::As)) {
                n.alias = expectIdent("an alias");
            }
            n.span = spanFrom(n.span);
            names.push_back(std::move(n));
            if (!(at(TokenKind::Comma) && continues())) {
                break;
            }
            advance();
        }
        return names;
    }

    std::string parseModuleString(Span& span) {
        if (at(TokenKind::String)) {
            span = cur().span;
            return advance().text;
        }
        if (!at(TokenKind::Ident)) {
            expected("a module path (`json`, `std/mem` or a string)");
        }
        // Bare path: `ident` or `ident/ident/...`, written without whitespace.
        span = cur().span;
        std::string path = advance().text;
        while (at(TokenKind::Slash) && peekKind(1) == TokenKind::Ident && adjacent(prev(), cur()) &&
               adjacent(cur(), peek(1))) {
            advance();
            path += '/';
            path += advance().text;
        }
        span = spanFrom(span);
        return path;
    }

    ImportDecl parseImport(const DeclStart& s) {
        ImportDecl decl;
        const Token& name_tok = cur();
        if (match(TokenKind::From)) {
            decl.form = ImportDecl::Form::Names;
            decl.module = parseModuleString(decl.module_span);
            expect(TokenKind::Import, "`import` and the imported names");
            decl.names = parseNameList();
        } else {
            advance(); // 'import'
            if (at(TokenKind::String)) {
                decl.form = ImportDecl::Form::Module;
                decl.module = parseModuleString(decl.module_span);
                if (match(TokenKind::As)) {
                    decl.alias = expectIdent("a module alias");
                }
            } else if (match(TokenKind::Star)) {
                decl.form = ImportDecl::Form::Star;
                if (match(TokenKind::As)) {
                    decl.form = ImportDecl::Form::StarAs;
                    decl.alias = expectIdent("a namespace alias");
                }
                expect(TokenKind::From, "`from` and the module path");
                decl.module = parseModuleString(decl.module_span);
            } else if (at(TokenKind::Ident)) {
                decl.form = ImportDecl::Form::Names;
                decl.names = parseNameList();
                expect(TokenKind::From, "`from` and the module path");
                decl.module = parseModuleString(decl.module_span);
            } else {
                expected("a module path string, `*` or an imported name");
            }
        }
        endOfItemLine();
        finishDecl(decl, s, decl.module, name_tok.span);
        return decl;
    }

    void parseExportFrom(Program& program, DeclStart& s) {
        ExportFromDecl decl;
        const Token& name_tok = cur();
        advance(); // 'export'
        if (match(TokenKind::Star)) {
            decl.form = ExportFromDecl::Form::Star;
            if (match(TokenKind::As)) {
                decl.form = ExportFromDecl::Form::StarAs;
                decl.alias = expectIdent("a namespace name");
            }
            expect(TokenKind::From, "`from` and the module path");
            decl.module = parseModuleString(decl.module_span);
            if (at(TokenKind::Except) && continues()) {
                if (decl.form == ExportFromDecl::Form::StarAs) {
                    failAt(cur(),
                           "`except` cannot be combined with `export * as`",
                           "not valid here",
                           {},
                           "write `export * from \"m\" except a, b`");
                }
                advance();
                decl.except = parseNameList();
            }
        } else {
            expect(TokenKind::LBrace, "`{`");
            decl.form = ExportFromDecl::Form::Names;
            ++nl_depth_;
            while (!at(TokenKind::RBrace)) {
                ImportName n;
                n.span = cur().span;
                n.name = expectIdent("an exported name");
                if (match(TokenKind::As)) {
                    n.alias = expectIdent("an alias");
                }
                n.span = spanFrom(n.span);
                decl.names.push_back(std::move(n));
                if (!match(TokenKind::Comma)) {
                    break;
                }
            }
            --nl_depth_;
            expect(TokenKind::RBrace, "`}`");
            expect(TokenKind::From, "`from` and the module path");
            decl.module = parseModuleString(decl.module_span);
        }
        endOfItemLine();
        finishDecl(decl, s, decl.module, name_tok.span);
        Item item;
        item.modifiers.exported = true;
        item.decl = std::move(decl);
        registerAllowScopes(s.attributes, declBase(item).span);
        program.items.push_back(std::move(item));
    }

    // --- module-level calls ------------------------------------------------------------------

    Decl parseTopLevelCall(const DeclStart& s) {
        const Token& first = cur();
        ExprPtr expr = parseExpr();
        if (expr->kind == Expr::Kind::MacroCall) {
            MacroCallDecl decl;
            decl.args = std::move(expr->macro_args);
            endOfItemLine();
            finishDecl(decl, s, expr->text, first.span);
            return decl;
        }
        if (expr->kind == Expr::Kind::Call) {
            TopLevelCallDecl decl;
            const std::string name = pathOf(*expr->lhs);
            decl.call = std::move(expr);
            endOfItemLine();
            finishDecl(decl, s, name, first.span);
            return decl;
        }
        failAt(first,
               std::format("expected a declaration, found {}", describeToken(first)),
               "expected a declaration here",
               {},
               "module-level code is limited to declarations, macro invocations and calls such as "
               "`staticAssert(...)`");
    }

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
    std::vector<Item>
        lambdas_; // function literals lifted out of expressions, appended to the program at the end
    int lambda_count_{0};
};

} // namespace

Program parseTokens(std::vector<Token> tokens, FileId file, DiagnosticEngine& diagnostics) {
    Parser parser(std::move(tokens), file, diagnostics);
    return parser.run();
}

Program parseFile(const SourceManager& sources, FileId file, DiagnosticEngine& diagnostics) {
    return parseTokens(lex(sources, file, diagnostics), file, diagnostics);
}

} // namespace xlang
