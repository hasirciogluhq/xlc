// Parser: `struct`, `class`, `interface` and `impl`.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

/// @grammar `field: T`, `@attribute field: T`, member separated by `,` or a line break
FieldDecl Parser::parseField(std::vector<Attribute> attrs) {
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
void Parser::memberSeparator() {
    if (match(TokenKind::Comma)) {
        return;
    }
    if (!at(TokenKind::RBrace) && !cur().newline_before) {
        expected("`,`, a line break or `}`");
    }
}

/// @grammar `struct Name { fields }`, `struct Name<T, U> { fields }`, `@repr(c) struct ...`
StructDecl Parser::parseStruct(const DeclStart& s) {
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

/// @grammar `class Name [: Base] { members }`, `class Name<T> [: Base] { members }`
ClassDecl Parser::parseClass(const DeclStart& s) {
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
/// @grammar `[virtual|override] fn name(params) -> R { body }`, `fn +(...)`, `fn [](...)`
FnDecl Parser::parseMethod(std::vector<Attribute> attrs, bool inherent) {
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

/// @grammar `interface Name { fn name(params) -> R }`, `interface Name : Base { ... }`
InterfaceDecl Parser::parseInterface(const DeclStart& s) {
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
/// @grammar `impl T { fn +(...) ... }`, `impl<T> Box<T> { ... }`, `impl Iface for T { ... }`
std::optional<ImplDecl> Parser::parseImpl(Program& program, DeclStart& s) {
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

} // namespace xlang::parse
