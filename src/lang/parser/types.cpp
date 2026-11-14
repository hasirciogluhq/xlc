// Parser: types, generic parameters and arguments, and the `<` lookahead that tells them from comparisons.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// @grammar `<T, U>` after a `fn`, `struct`, `class` or `interface` name, `<>` empty
std::vector<std::string> Parser::parseGenericParams() {
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
void Parser::rejectTypeParams() {
    if (at(TokenKind::Lt)) {
        diag_.error("E0724",
                    cur().span,
                    "type parameters are only allowed on `fn`, `struct` and `class`",
                    "remove the `<...>`");
        (void)parseGenericParams();
    }
}

/// @grammar `<A, B>` after a type name or an identifier in an expression, `>>` closing two levels
/// @invariant inside `<...>` a nested `<` always opens type arguments, also in the type of a
/// @invariant cast, so `cast_type_` is cleared here and restored on exit
std::vector<Type> Parser::parseTypeArgs() {
    std::vector<Type> args;
    expect(TokenKind::Lt, "`<` starting the type arguments");
    // Inside `<...>` a nested `<` always opens type arguments, also in the type of a cast.
    const bool saved_cast = std::exchange(cast_type_, false);
    while (!at(TokenKind::Gt) && !at(TokenKind::Shr) && !at(TokenKind::End)) {
        args.push_back(parseType());
        if (!match(TokenKind::Comma)) {
            break;
        }
    }
    cast_type_ = saved_cast;
    closeAngle();
    return args;
}

/// Consumes one `>`. A `>>` (or `>=`, `>>=`) token gives up its first character, so nested
/// type arguments `Vec<Vec<i32>>` close one level at a time.
void Parser::closeAngle() {
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

/// Index of the `>` that closes the angle brackets opened by the `<` at the cursor, when
/// everything in between is made of type tokens: names, primitives, `*`, `.`, `,`, array and
/// slice brackets with an integer length, tuple parentheses and `fn(...) -> R`. Nothing is
/// returned when another token comes first, or when a `)`/`]`/`>` has no opener inside.
std::optional<std::size_t> Parser::typeArgsClose() const {
    int angles = 0;
    int groups = 0; // `(` and `[` inside the arguments
    for (std::size_t i = pos_; i < tokens_.size(); ++i) {
        switch (tokens_[i].kind) {
        case TokenKind::Lt:
            ++angles;
            break;
        case TokenKind::Gt:
            --angles;
            break;
        case TokenKind::Shr:
            angles -= 2;
            break;
        case TokenKind::LParen:
        case TokenKind::LBracket:
            ++groups;
            break;
        case TokenKind::RParen:
        case TokenKind::RBracket:
            if (--groups < 0) {
                return std::nullopt;
            }
            break;
        case TokenKind::Ident:
        case TokenKind::PrimType:
        case TokenKind::Star:
        case TokenKind::Comma:
        case TokenKind::Dot:
        case TokenKind::Int:
        case TokenKind::Semicolon:
        case TokenKind::Fn:
        case TokenKind::Arrow:
            break;
        default:
            return std::nullopt;
        }
        if (angles < 0) {
            return std::nullopt;
        }
        if (angles == 0) {
            // A `>` inside `(` or `[` cannot close the arguments: types never hold one there
            // except as a nested argument list, which is already counted.
            return groups == 0 ? std::optional{i} : std::nullopt;
        }
    }
    return std::nullopt;
}

/// Whether the `<` at the cursor opens type arguments of an identifier in an expression: they
/// hold only type tokens (`typeArgsClose`) and the `>` is followed by what no comparison's right
/// operand starts with: `(`, `{`, `)`, `]`, `,`, `;` or the end of the line. Otherwise the `<`
/// is a comparison.
bool Parser::genericArgsAhead() const {
    const std::optional<std::size_t> close = typeArgsClose();
    if (!close) {
        return false;
    }
    const std::size_t i = *close;
    if (i + 1 >= tokens_.size() || tokens_[i + 1].newline_before) {
        return true;
    }
    using enum TokenKind;
    return std::ranges::contains(std::array{LParen, LBrace, RParen, RBracket, Comma, Semicolon},
                                 tokens_[i + 1].kind);
}

/// Whether the `<` after the type name of `x as T` opens type arguments (`x as Box<i32>`) rather
/// than comparing the cast value (`x as Idx < n`): they hold only type tokens (`typeArgsClose`)
/// and the `>` is not followed, on its line, by a token that starts an operand (a name, a
/// literal, `(`, `[`, `self`, `!`, `~`, a closure), which only a comparison `a < b > c` has.
bool Parser::castTypeArgsAhead() const {
    const std::optional<std::size_t> close = typeArgsClose();
    if (!close) {
        return false;
    }
    const std::size_t i = *close;
    if (i + 1 >= tokens_.size() || tokens_[i + 1].newline_before) {
        return true;
    }
    using enum TokenKind;
    return !std::ranges::contains(std::array{Ident, PrimType, Int, Float, String, ByteLit, True,
                                             False, SelfKw, LParen, LBracket, Bang, Tilde, Pipe,
                                             OrOr, New, Fn},
                                  tokens_[i + 1].kind);
}

/// @grammar the type after `->`, which may be `never` (the one place it may be written)
Type Parser::parseReturnType() {
    if (at(TokenKind::PrimType) && cur().text == "never") {
        advance();
        return Type::makeNever();
    }
    return parseType();
}

/// @grammar `*T`, `[]T`, `[]mut T`, `[T; N]`, `(T,)`, `(A, B)`, `()`
/// @grammar `fn(A) -> R`, `fn(A)`, `Name`, `a.b.Name`, `Name<T, U>`
/// @grammar `*T` is written with a star, never a postfix form: the type grammar is prefix only
/// @grammar `(T)` without the trailing comma is `T` in parentheses, not a one-element tuple
/// @grammar `[T; N]` keeps N as an expression, parsed with `no_struct_` cleared
/// @grammar `never` is accepted here and rejected later with E0765 unless it is a return type
/// @invariant `nl_depth_` is raised around every bracket the rule consumes and lowered on every
/// @invariant exit path, so a newline inside a type does not end the enclosing statement
Type Parser::parseType() {
    const Span start = cur().span;
    Type type;
    switch (cur().kind) {
    case TokenKind::Star: {
        advance();
        type = Type::makePointer(parseType());
        break;
    }
    case TokenKind::PrimType: {
        const Token& t = advance();
        type = *primitiveFromName(t.text);
        if (type.isNever()) {
            diag_.error("E0765", t.span, "`never` is only a return type",
                        "write it after `->` of a function that does not return");
        }
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
            ret = parseReturnType();
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
        if (at(TokenKind::Lt) && (!cast_type_ || castTypeArgsAhead())) {
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

} // namespace xlang::parse
