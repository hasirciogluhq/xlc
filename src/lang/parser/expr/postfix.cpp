// Parser: paths, struct field initializers, macro and call arguments, postfix operators.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// Dotted name of a `Name`/`Field` chain, empty when `e` is anything else.
std::string Parser::pathOf(const Expr& e) {
    if (e.kind == Expr::Kind::Name) {
        return e.text;
    }
    if (e.kind == Expr::Kind::Field && e.lhs) {
        std::string base = pathOf(*e.lhs);
        return base.empty() ? base : base + "." + e.text;
    }
    return {};
}

std::vector<std::string> Parser::splitPath(const std::string& path) {
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
bool Parser::looksLikeStructLiteral() const {
    if (peekKind(1) == TokenKind::RBrace) {
        return true;
    }
    return peekKind(1) == TokenKind::Ident && peekKind(2) == TokenKind::Colon;
}

std::vector<FieldInit> Parser::parseFieldInits() {
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
std::vector<std::vector<Token>> Parser::parseMacroArgs() {
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
ExprPtr Parser::parseCallArg() {
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

/// A `?` that propagates (`f()?`) rather than starts a ternary. It must touch the operand
/// before it. Then it propagates when what follows cannot be the first token of a ternary's
/// value: the end of the line or of the file, a closing or separating token (`;`, `)`, `]`,
/// `}`, `,`, `.`, `{`, `:`, another `?`), or an operator that only works between two operands
/// (`f()? + 1`, `a? == b?`, `as`). Otherwise (`-`, `*`, `&`, a name, a literal follow) it is a
/// ternary only when its `:` follows (`ternaryColonAhead`): `c? -1 : 1` is a ternary,
/// `f()? - 1` and `f()? * 2` propagate.
bool Parser::tryOperatorAhead() const {
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
               next.kind) ||
           !ternaryColonAhead();
}

/// Net count of brackets (`(`, `[`, `{`) opened and not closed by the tokens `from` .. `to - 1`.
int Parser::openBrackets(std::size_t from, std::size_t to) const {
    int depth = 0;
    for (std::size_t i = from; i < to; ++i) {
        using enum TokenKind;
        const TokenKind k = tokens_[i].kind;
        depth += (k == LParen || k == LBracket || k == LBrace) ? 1
                 : (k == RParen || k == RBracket || k == RBrace) ? -1
                                                                   : 0;
    }
    return depth;
}

/// Whether the `?` at the cursor has a ternary `:` of its own after it. The scan stays at the
/// `?`'s bracket level and stops where the expression ends there: a line break outside
/// brackets, a `,` or `;`, a closing bracket of an outer level, a block `{` in a condition, or
/// the end of the file. Each `?` met on the way takes the next `:`. The `:` left over belong
/// to the ternaries whose then-branch encloses the cursor at this same level
/// (`open_ternaries_`); one more than those is this `?`'s own.
bool Parser::ternaryColonAhead() const {
    int owed = 0;
    for (const std::size_t q : open_ternaries_) {
        owed += openBrackets(q + 1, pos_) == 0 ? 1 : 0;
    }
    int depth = 0;
    int pending = 0; // `?` met in the scan that still wait for their `:`
    int colons = 0;  // `:` left over for this `?` and the enclosing ternaries
    for (std::size_t i = pos_ + 1; i < tokens_.size(); ++i) {
        const Token& t = tokens_[i];
        using enum TokenKind;
        if (depth == 0 && t.newline_before && nl_depth_ == 0) {
            break;
        }
        if (t.kind == End || (depth == 0 && (t.kind == Comma || t.kind == Semicolon)) ||
            (depth == 0 && t.kind == LBrace && no_struct_)) {
            break;
        }
        if (t.kind == LParen || t.kind == LBracket || t.kind == LBrace) {
            ++depth;
        } else if (t.kind == RParen || t.kind == RBracket || t.kind == RBrace) {
            if (depth == 0) {
                break;
            }
            --depth;
        } else if (depth == 0 && t.kind == Question) {
            ++pending;
        } else if (depth == 0 && t.kind == Colon) {
            pending > 0 ? --pending : ++colons;
        }
    }
    return colons > owed;
}

ExprPtr Parser::parsePostfix(ExprPtr expr) {
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

} // namespace xlang::parse
