// Parser: primary expressions, fn literals, closures, array literals and `new`.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// `fn(params) -> R { body }` as an expression: the body becomes a module-level function with a
/// generated name, and the expression is a reference to it. A literal captures nothing; an
/// outer local used inside it is an unknown name.
ExprPtr Parser::parseFnLiteral() {
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
ExprPtr Parser::parseClosure() {
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
        data->return_type = parseReturnType();
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

ExprPtr Parser::parsePrimary() {
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
bool Parser::atSliceType() const {
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

ExprPtr Parser::parseArrayLiteral() {
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

ExprPtr Parser::parseNew() {
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

} // namespace xlang::parse
