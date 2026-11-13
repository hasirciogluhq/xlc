// Parser: `for`, `switch` and `if`.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// `for init; cond; step { body }` (every part optional) or `for i in lo..hi { body }`.
Stmt Parser::parseFor() {
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

Stmt Parser::parseForClause() {
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
Stmt Parser::parseSwitch() {
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
ExprPtr Parser::parseCondition() {
    const bool saved = no_struct_;
    no_struct_ = true;
    ExprPtr cond = parseExpr();
    no_struct_ = saved;
    return cond;
}

Stmt Parser::parseIf() {
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

} // namespace xlang::parse
