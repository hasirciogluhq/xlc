// Parser: token access, error reporting and error recovery.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

const Token& Parser::peek(std::size_t ahead) const {
    return tokens_[std::min(pos_ + ahead, tokens_.size() - 1)];
}

const Token& Parser::advance() {
    const Token& t = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) {
        ++pos_;
    }
    return t;
}

bool Parser::match(TokenKind kind) {
    if (at(kind)) {
        advance();
        return true;
    }
    return false;
}

/// The current token may continue the expression/statement being parsed: it is on the same
/// line, or we are inside `(...)`/`[...]` where line breaks do not end anything. While a prefix
/// `*` or `&` operand is being parsed, the operand's own `(...)` is not counted, so a newline
/// in front of a binary `&`/`*` ends the operand instead of turning the next argument into its
/// right side.
bool Parser::continues() const {
    if (cur().newline_before && prefix_operand_) {
        return nl_depth_ > prefix_nl_base_;
    }
    return !cur().newline_before || nl_depth_ > 0;
}

bool Parser::adjacent(const Token& a, const Token& b) {
    return a.span.end_line == b.span.line && a.span.end_column == b.span.column;
}

void Parser::failAt(const Token& token, std::string message, std::string label, std::string detail, std::string fix) {
    Diagnostic d;
    d.code = "E0002";
    d.severity = Severity::Error;
    d.message = std::move(message);
    d.labels.push_back({token.span, std::move(label)});
    d.detail = std::move(detail);
    d.fix = std::move(fix);
    diag_.report(std::move(d));
    throw ParseAbort{};
}

void Parser::expected(std::string_view what) {
    failAt(cur(),
           std::format("expected {}, found {}", what, describeToken(cur())),
           std::format("expected {} here", what));
}

const Token& Parser::expect(TokenKind kind, std::string_view what) {
    if (!at(kind)) {
        expected(what);
    }
    return advance();
}

std::string Parser::expectIdent(std::string_view what) {
    if (!at(TokenKind::Ident)) {
        expected(what);
    }
    return advance().text;
}

/// A member name: an identifier, or a word keyword where only a name can stand (after `fn` in a
/// class, interface or impl body, and after `.`), so `fn delete(...)` and `c.delete(url)` work.
bool Parser::atMemberName() const {
    const std::string& t = cur().text;
    return at(TokenKind::Ident) ||
           (!t.empty() && ((t[0] >= 'a' && t[0] <= 'z') || (t[0] >= 'A' && t[0] <= 'Z')) &&
            !at(TokenKind::Int) && !at(TokenKind::Float));
}

void Parser::softError(const Span& span, std::string message, std::string label) {
    diag_.error("E0002", span, std::move(message), std::move(label));
}

bool Parser::startsItem(TokenKind kind) {
    switch (kind) {
    case TokenKind::At:
    case TokenKind::Export:
    case TokenKind::Declare:
    case TokenKind::Register:
    case TokenKind::Fn:
    case TokenKind::Global:
    case TokenKind::Const:
    case TokenKind::Struct:
    case TokenKind::Class:
    case TokenKind::Interface:
    case TokenKind::Impl:
    case TokenKind::Enum:
    case TokenKind::Type:
    case TokenKind::Define:
    case TokenKind::Import:
    case TokenKind::From:
        return true;
    default:
        return false;
    }
}

void Parser::syncItem(std::size_t start) {
    if (pos_ == start && !at(TokenKind::End)) {
        advance();
    }
    // Brace depth at the error position, counted from the start of the failed item, so the
    // rest of its body is skipped instead of being re-parsed as top-level code.
    int depth = 0;
    for (std::size_t i = start; i < pos_; ++i) {
        if (tokens_[i].kind == TokenKind::LBrace) {
            ++depth;
        } else if (tokens_[i].kind == TokenKind::RBrace && depth > 0) {
            --depth;
        }
    }
    while (!at(TokenKind::End)) {
        if (depth == 0 && cur().newline_before && startsItem(cur().kind)) {
            return;
        }
        if (at(TokenKind::LBrace)) {
            ++depth;
        } else if (at(TokenKind::RBrace) && depth > 0) {
            --depth;
        }
        advance();
    }
}

void Parser::syncStatement(std::size_t start) {
    if (pos_ == start && !at(TokenKind::End) && !at(TokenKind::RBrace)) {
        advance();
    }
    // Braces the failed statement opened and did not close (a `switch` body, a struct
    // literal, a closure), counted from its start like `syncItem`, so the rest of them is
    // skipped instead of being parsed as statements of the enclosing block.
    int depth = 0;
    for (std::size_t i = start; i < pos_; ++i) {
        if (tokens_[i].kind == TokenKind::LBrace) {
            ++depth;
        } else if (tokens_[i].kind == TokenKind::RBrace && depth > 0) {
            --depth;
        }
    }
    while (!at(TokenKind::End)) {
        if (depth == 0 && (cur().newline_before || at(TokenKind::RBrace))) {
            return;
        }
        if (at(TokenKind::LBrace)) {
            ++depth;
        } else if (at(TokenKind::RBrace)) {
            --depth;
        }
        advance();
    }
}

void Parser::rejectDirective() {
    if (cur().kind >= TokenKind::DirIf && cur().kind <= TokenKind::DirError) {
        failAt(
            cur(),
            std::format("`{}` is not supported yet", tokenKindName(cur().kind)),
            "conditional-compilation directive",
            "The lexer recognizes `#if`/`#elif`/`#else`/`#endif`/`#ifdef`/`#ifndef`/`#error`, "
            "but the preprocessor that evaluates them is not implemented yet.",
            "remove the directive for now");
    }
}

} // namespace xlang::parse
