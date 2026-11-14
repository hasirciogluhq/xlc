// Parser: ternary, binary operators (Go precedence), `as` and prefix operators.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// @grammar `cond ? a : b` is right associative and the lowest precedence level
/// @invariant `open_ternaries_` holds the token index of every `?` whose `:` is still owed, so
/// @invariant `ternaryColonAhead` can tell a ternary `?` from a propagating one
ExprPtr Parser::parseTernary() {
    ExprPtr cond = parseOr();
    if (at(TokenKind::Question) && continues()) {
        open_ternaries_.push_back(pos_);
        advance();
        ExprPtr then_value = parseTernary();
        open_ternaries_.pop_back();
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

ExprPtr Parser::makeBinary(BinaryOp op, ExprPtr lhs, ExprPtr rhs) {
    const Span span = Span::merge(lhs->span, rhs->span);
    ExprPtr e = makeExpr(Expr::Kind::Binary, span);
    e->binary_op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

/// @grammar `a || b`
ExprPtr Parser::parseOr() {
    ExprPtr lhs = parseAnd();
    while (at(TokenKind::OrOr) && continues()) {
        advance();
        lhs = makeBinary(BinaryOp::Or, std::move(lhs), parseAnd());
    }
    return lhs;
}

/// @grammar `a && b`
ExprPtr Parser::parseAnd() {
    ExprPtr lhs = parseEquality();
    while (at(TokenKind::AndAnd) && continues()) {
        advance();
        lhs = makeBinary(BinaryOp::And, std::move(lhs), parseEquality());
    }
    return lhs;
}

/// @grammar `a == b`, `a != b`
ExprPtr Parser::parseEquality() {
    ExprPtr lhs = parseComparison();
    while ((at(TokenKind::EqEq) || at(TokenKind::NotEq)) && continues()) {
        const BinaryOp op = advance().kind == TokenKind::EqEq ? BinaryOp::Eq : BinaryOp::Ne;
        lhs = makeBinary(op, std::move(lhs), parseComparison());
    }
    return lhs;
}

/// @grammar `a < b`, `a <= b`, `a > b`, `a >= b`
ExprPtr Parser::parseComparison() {
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

/// @grammar `a + b`, `a - b`, `a | b`, `a ^ b`
ExprPtr Parser::parseAdditive() {
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

/// @grammar `a * b`, `a / b`, `a % b`, `a & b`, `a << b`, `a >> b`
ExprPtr Parser::parseMultiplicative() {
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

/// @grammar `e as T` binds tighter than every binary operator, so `a + b as i64` casts `b`
/// @grammar `[cast_type_]` tells `parseType` that a `<` after a name may be a comparison
/// @grammar (`x as Idx < n`) rather than type arguments; `castTypeArgsAhead` decides
/// @invariant `cast_type_` stays true for the whole target type and is cleared right after
ExprPtr Parser::parseCast() {
    ExprPtr value = parseUnary();
    while (at(TokenKind::As) && continues()) {
        advance();
        const bool saved_cast = std::exchange(cast_type_, true);
        Type target = parseType(); // a `ParseAbort` here is undone by the recovery's `restore`
        cast_type_ = saved_cast;
        ExprPtr e = makeExpr(Expr::Kind::Cast, Span::merge(value->span, prev().span));
        e->lhs = std::move(value);
        e->type = std::move(target);
        value = std::move(e);
    }
    return value;
}

/// @grammar `-e`, `!e`, `~e`, `*e`, `&e`, `&mut e`, repeated (`--x`, `*&p`)
/// @invariant `prefix_operand_` and `prefix_nl_base_` are raised around the operand, so a
/// @invariant newline in front of a binary `&`/`*` ends the operand, not the next argument
ExprPtr Parser::parseUnary() {
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

} // namespace xlang::parse
