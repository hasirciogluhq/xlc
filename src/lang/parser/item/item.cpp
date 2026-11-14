// Parser: item dispatch and modifiers.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

void Parser::finishDecl(DeclBase& base, const DeclStart& s, std::string name, const Span& name_span) {
    base.name = std::move(name);
    base.attributes = s.attributes;
    base.name_span = name_span;
    base.span = spanFrom(s.start);
}

/// Items that end without a closing brace must be followed by a line break.
void Parser::endOfItemLine() {
    if (at(TokenKind::End) || at(TokenKind::RBrace) || cur().newline_before) {
        return;
    }
    failAt(cur(),
           std::format("unexpected {} after the end of a declaration", describeToken(cur())),
           "a declaration ends at a line break",
           {},
           "put this on a new line");
}

/// @grammar `fn name<T>(params) -> R { body }`, `fn +(...)` and the other operator names
/// @grammar `fn name: sig`, `global name: T = c`, `const name: T = c`
/// @grammar `struct`, `class`, `interface`, `enum`, `impl`, `type Name = T`, `define`
/// @grammar `import`, `export name from "m"`, `export *`, a module-level `name!(...)` call
/// @grammar `@attribute` before any of them, `export` / `export(c)` / `@library` modifiers
void Parser::parseItem(Program& program) {
    rejectDirective();
    DeclStart s;
    s.start = cur().span;
    s.attributes = parseAttributes();
    if (!s.attributes.empty()) {
        s.start = s.attributes.front().span;
    }
    rejectDirective();

    // Re-exports: `export * ...` / `export { ... } from ...`.
    if (at(TokenKind::Export) &&
        (peekKind(1) == TokenKind::Star || peekKind(1) == TokenKind::LBrace)) {
        parseExportFrom(program, s);
        return;
    }

    parseModifiers(s.modifiers);

    const Token& kw = cur();
    switch (kw.kind) {
    case TokenKind::Fn:
        if (peek(1).kind == TokenKind::Ident && peek(2).kind == TokenKind::Assign) {
            addItem(program, s, parseFnAlias(s));
        } else {
            addItem(program, s, parseFn(s, /*member=*/false));
        }
        return;
    case TokenKind::Global:
        addItem(program, s, parseGlobal(s));
        return;
    case TokenKind::Const:
        addItem(program, s, parseConstItem(s));
        return;
    case TokenKind::Struct:
        addItem(program, s, parseStruct(s));
        return;
    case TokenKind::Class:
        addItem(program, s, parseClass(s));
        return;
    case TokenKind::Interface:
        addItem(program, s, parseInterface(s));
        return;
    case TokenKind::Impl:
        if (auto impl = parseImpl(program, s)) {
            addItem(program, s, std::move(*impl));
        }
        return;
    case TokenKind::Enum:
        addEnum(program, s, parseEnum(s));
        return;
    case TokenKind::Type:
        addItem(program, s, parseTypeAlias(s));
        return;
    case TokenKind::Define:
        addItem(program, s, parseDefine(s));
        return;
    case TokenKind::Import:
    case TokenKind::From:
        addItem(program, s, parseImport(s));
        return;
    case TokenKind::Ident:
        addItem(program, s, parseTopLevelCall(s));
        return;
    default:
        failAt(
            kw,
            std::format("expected a declaration, found {}", describeToken(kw)),
            "expected a declaration here",
            {},
            "declarations start with fn, global, struct, class, interface, impl, enum, type, "
            "define, import, from or export");
    }
}

void Parser::addItem(Program& program, DeclStart& s, Decl decl) {
    Item item;
    item.modifiers = s.modifiers;
    item.decl = std::move(decl);
    registerAllowScopes(s.attributes, declBase(item).span);
    checkModifiers(item);
    program.items.push_back(std::move(item));
}

/// @grammar `export`, `export(c)`, `@library` and the other item modifiers before the keyword
void Parser::parseModifiers(Modifiers& mods) {
    while (true) {
        if (at(TokenKind::Export)) {
            if (mods.exported) {
                failAt(cur(), "duplicate `export`", "already exported");
            }
            advance();
            mods.exported = true;
            if (at(TokenKind::LParen) && peekKind(1) == TokenKind::Ident &&
                peekKind(2) == TokenKind::RParen) {
                advance();
                const Token& abi = advance();
                advance();
                if (abi.text != "c") {
                    failAt(abi,
                           std::format("unknown export ABI `{}`", abi.text),
                           "only `export(c)` exists");
                }
                mods.export_c = true;
            }
        } else if (at(TokenKind::Declare)) {
            if (mods.declared) {
                failAt(cur(), "duplicate `declare`", "already declared");
            }
            advance();
            mods.declared = true;
        } else if (at(TokenKind::Register)) {
            if (mods.registered) {
                failAt(cur(), "duplicate `register`", "already registered");
            }
            advance();
            mods.registered = true;
            if (at(TokenKind::LParen) && peekKind(1) == TokenKind::Ident &&
                peekKind(2) == TokenKind::RParen) {
                advance();
                mods.register_role = advance().text;
                advance();
            }
        } else {
            return;
        }
    }
}

/// Prefix applicability (soft errors: parsing continues).
void Parser::checkModifiers(const Item& item) {
    const Modifiers& m = item.modifiers;
    const DeclBase& base = declBase(item);
    const std::string kind = declKindName(item.decl);
    const auto reject = [&](std::string_view prefix) {
        softError(base.span,
                  std::format("`{}` cannot be applied to {} `{}`", prefix, kind, base.name),
                  std::format("`{}` is not valid on {}", prefix, kind));
    };
    const bool is_import = std::holds_alternative<ImportDecl>(item.decl);
    const bool is_call = std::holds_alternative<TopLevelCallDecl>(item.decl) ||
                         std::holds_alternative<MacroCallDecl>(item.decl);
    const bool is_impl = std::holds_alternative<ImplDecl>(item.decl);
    const bool is_define = std::holds_alternative<DefineConstDecl>(item.decl) ||
                           std::holds_alternative<DefineMacroDecl>(item.decl);
    const bool is_enum = std::holds_alternative<EnumDecl>(item.decl);
    if (m.exported && (is_import || is_call || is_impl)) {
        reject("export");
    }
    if (m.declared && (is_import || is_call || is_impl || is_define)) {
        reject("declare");
    }
    if (m.declared && is_enum) {
        reject("declare");
    }
    if (m.registered && (is_import || is_call || is_impl || is_define)) {
        reject("register");
    }
    if (m.export_c && !(std::holds_alternative<FnDecl>(item.decl) ||
                        std::holds_alternative<GlobalDecl>(item.decl))) {
        reject("export(c)");
    }
}

} // namespace xlang::parse
