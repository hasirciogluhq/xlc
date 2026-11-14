// Parser: `@name(args)` attributes and `@allow` scopes.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// @grammar `@name`, `@name(a, key = v, ...)`, `@file(name(...))` at the top of a file
Attribute Parser::parseAttribute() {
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

/// @grammar the value of an attribute argument: an identifier, an integer literal or a string
void Parser::parseAttributeValue(AttributeArg& arg) {
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

/// @grammar one or more `@attribute` lines before an item, a statement or a parameter
std::vector<Attribute> Parser::parseAttributes() {
    std::vector<Attribute> attrs;
    while (at(TokenKind::At)) {
        attrs.push_back(parseAttribute());
    }
    return attrs;
}

void Parser::registerAllowScopes(const std::vector<Attribute>& attrs, const Span& scope) {
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

} // namespace xlang::parse
