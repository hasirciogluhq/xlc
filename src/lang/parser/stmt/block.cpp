// Parser: blocks, statements, locals and simple statements.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

Block Parser::parseBlock() {
    Block block;
    block.span = cur().span;
    expect(TokenKind::LBrace, "`{`");
    // A block starts a fresh statement context: line breaks end statements again, `{` may
    // start a struct literal, and a closure body inside a `for` header is not the header.
    const State saved = state();
    nl_depth_ = 0;
    no_struct_ = false;
    for_header_ = false;
    prefix_operand_ = false;
    cast_type_ = false;
    const State inside = state();
    while (!at(TokenKind::RBrace)) {
        if (at(TokenKind::End)) {
            restore(saved);
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
            restore(inside);
            syncStatement(start);
        }
    }
    restore(saved);
    advance(); // '}'
    block.span = spanFrom(block.span);
    return block;
}

/// After a statement that did not end in a block: a line break or `}` must follow.
void Parser::endOfStatement() {
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

Stmt Parser::parseStatement() {
    rejectDirective();
    std::vector<Attribute> attrs = parseAttributes();
    const Span start = attrs.empty() ? cur().span : attrs.front().span;
    Stmt stmt = parseStatementBody();
    stmt.attributes = std::move(attrs);
    stmt.span = spanFrom(start);
    registerAllowScopes(stmt.attributes, stmt.span);
    return stmt;
}

Stmt Parser::parseStatementBody() {
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
void Parser::parseLocalTail(Stmt& stmt) {
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

Stmt Parser::parseSimpleStatement() {
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

} // namespace xlang::parse
