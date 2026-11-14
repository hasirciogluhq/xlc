// Parser: `enum`, `type` aliases and `define` macros.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// @grammar `enum Name: T { A, B }`, `enum Name<T> { A(T), B, C(A, B) }`
EnumDecl Parser::parseEnum(const DeclStart& s) {
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
void Parser::addEnum(Program& program, DeclStart& s, EnumDecl decl) {
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

/// @grammar `type Name = T`, `type Name<T> = T`
TypeAliasDecl Parser::parseTypeAlias(const DeclStart& s) {
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

/// @grammar `define name(a: expr, b: stmt) { body }`, used as `name!(...)`
Decl Parser::parseDefine(const DeclStart& s) {
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

} // namespace xlang::parse
