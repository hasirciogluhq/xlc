// Parser: `import`, `export ... from` and module-level calls.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

std::vector<ImportName> Parser::parseNameList() {
    std::vector<ImportName> names;
    while (true) {
        ImportName n;
        n.span = cur().span;
        n.name = expectIdent("a name");
        if (match(TokenKind::As)) {
            n.alias = expectIdent("an alias");
        }
        n.span = spanFrom(n.span);
        names.push_back(std::move(n));
        if (!(at(TokenKind::Comma) && continues())) {
            break;
        }
        advance();
    }
    return names;
}

std::string Parser::parseModuleString(Span& span) {
    if (at(TokenKind::String)) {
        span = cur().span;
        return advance().text;
    }
    if (!at(TokenKind::Ident)) {
        expected("a module path (`json`, `std/mem` or a string)");
    }
    // Bare path: `ident` or `ident/ident/...`, written without whitespace.
    span = cur().span;
    std::string path = advance().text;
    while (at(TokenKind::Slash) && peekKind(1) == TokenKind::Ident && adjacent(prev(), cur()) &&
           adjacent(cur(), peek(1))) {
        advance();
        path += '/';
        path += advance().text;
    }
    span = spanFrom(span);
    return path;
}

ImportDecl Parser::parseImport(const DeclStart& s) {
    ImportDecl decl;
    const Token& name_tok = cur();
    if (match(TokenKind::From)) {
        decl.form = ImportDecl::Form::Names;
        decl.module = parseModuleString(decl.module_span);
        expect(TokenKind::Import, "`import` and the imported names");
        decl.names = parseNameList();
    } else {
        advance(); // 'import'
        if (at(TokenKind::String)) {
            decl.form = ImportDecl::Form::Module;
            decl.module = parseModuleString(decl.module_span);
            if (match(TokenKind::As)) {
                decl.alias = expectIdent("a module alias");
            }
        } else if (match(TokenKind::Star)) {
            decl.form = ImportDecl::Form::Star;
            if (match(TokenKind::As)) {
                decl.form = ImportDecl::Form::StarAs;
                decl.alias = expectIdent("a namespace alias");
            }
            expect(TokenKind::From, "`from` and the module path");
            decl.module = parseModuleString(decl.module_span);
        } else if (at(TokenKind::Ident)) {
            decl.form = ImportDecl::Form::Names;
            decl.names = parseNameList();
            expect(TokenKind::From, "`from` and the module path");
            decl.module = parseModuleString(decl.module_span);
        } else {
            expected("a module path string, `*` or an imported name");
        }
    }
    endOfItemLine();
    finishDecl(decl, s, decl.module, name_tok.span);
    return decl;
}

void Parser::parseExportFrom(Program& program, DeclStart& s) {
    ExportFromDecl decl;
    const Token& name_tok = cur();
    advance(); // 'export'
    if (match(TokenKind::Star)) {
        decl.form = ExportFromDecl::Form::Star;
        if (match(TokenKind::As)) {
            decl.form = ExportFromDecl::Form::StarAs;
            decl.alias = expectIdent("a namespace name");
        }
        expect(TokenKind::From, "`from` and the module path");
        decl.module = parseModuleString(decl.module_span);
        if (at(TokenKind::Except) && continues()) {
            if (decl.form == ExportFromDecl::Form::StarAs) {
                failAt(cur(),
                       "`except` cannot be combined with `export * as`",
                       "not valid here",
                       {},
                       "write `export * from \"m\" except a, b`");
            }
            advance();
            decl.except = parseNameList();
        }
    } else {
        expect(TokenKind::LBrace, "`{`");
        decl.form = ExportFromDecl::Form::Names;
        ++nl_depth_;
        while (!at(TokenKind::RBrace)) {
            ImportName n;
            n.span = cur().span;
            n.name = expectIdent("an exported name");
            if (match(TokenKind::As)) {
                n.alias = expectIdent("an alias");
            }
            n.span = spanFrom(n.span);
            decl.names.push_back(std::move(n));
            if (!match(TokenKind::Comma)) {
                break;
            }
        }
        --nl_depth_;
        expect(TokenKind::RBrace, "`}`");
        expect(TokenKind::From, "`from` and the module path");
        decl.module = parseModuleString(decl.module_span);
    }
    endOfItemLine();
    finishDecl(decl, s, decl.module, name_tok.span);
    Item item;
    item.modifiers.exported = true;
    item.decl = std::move(decl);
    registerAllowScopes(s.attributes, declBase(item).span);
    program.items.push_back(std::move(item));
}

Decl Parser::parseTopLevelCall(const DeclStart& s) {
    const Token& first = cur();
    ExprPtr expr = parseExpr();
    if (expr->kind == Expr::Kind::MacroCall) {
        MacroCallDecl decl;
        decl.args = std::move(expr->macro_args);
        endOfItemLine();
        finishDecl(decl, s, expr->text, first.span);
        return decl;
    }
    if (expr->kind == Expr::Kind::Call) {
        TopLevelCallDecl decl;
        const std::string name = pathOf(*expr->lhs);
        decl.call = std::move(expr);
        endOfItemLine();
        finishDecl(decl, s, name, first.span);
        return decl;
    }
    failAt(first,
           std::format("expected a declaration, found {}", describeToken(first)),
           "expected a declaration here",
           {},
           "module-level code is limited to declarations, macro invocations and calls such as "
           "`staticAssert(...)`");
}

} // namespace xlang::parse
