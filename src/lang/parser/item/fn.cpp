// Parser: `fn` declarations, operator function names, fn aliases, globals and constants.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// The parameter list, the optional return type and the body of a function (after its name):
/// shared by named functions and function literals. `needs_body` makes a missing body an error.
void Parser::parseFnTail(FnDecl& fn, const std::string& name, bool member, bool needs_body) {
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
        fn.return_type = parseReturnType();
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
std::optional<std::string> Parser::parseOperatorName() {
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

FnDecl Parser::parseFn(const DeclStart& s, bool member) {
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

/// `fn name = target` or `fn name = module.target` at module level.
FnAliasDecl Parser::parseFnAlias(const DeclStart& s) {
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

GlobalDecl Parser::parseGlobal(const DeclStart& s) {
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
GlobalDecl Parser::parseConstItem(const DeclStart& s) {
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

} // namespace xlang::parse
