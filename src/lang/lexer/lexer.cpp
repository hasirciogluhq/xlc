// The seam between the C++ compiler and the lexer written in xlang (src/lang/lexer/lexer.xl).
//
// The scanner itself lives on the xlang side; this file turns its frozen result into `Token`
// values and reports the diagnostics it collected as E0001. Token kinds cross the boundary as
// the numbers of `enum class TokenKind`; src/lang/lexer/tokens.xl must agree number for number.
// The structs are the ones of include/xlang/lang/lexer_c.h.
#include "xlang/lang/lexer.h"

#include "xlang/lang/lexer_c.h"

#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace xlang {

/// @grammar the whole token stream of one file, always ending with `TokenKind::End`
std::vector<Token> lex(const SourceManager& sources, FileId file, DiagnosticEngine& diagnostics) {
    const std::string& text = sources.file(file).text();
    const XlcLexResult result = xlcLex(text.data(), static_cast<std::uint32_t>(text.size()));

    std::vector<Token> tokens;
    tokens.reserve(result.token_count);
    for (std::uint32_t i = 0; i < result.token_count; ++i) {
        const XlcLexToken& raw = result.tokens[i];
        Token token;
        token.kind = static_cast<TokenKind>(raw.kind);
        token.text.assign(raw.text, raw.text_len);
        token.int_value = raw.int_value;
        token.float_value = raw.float_value;
        token.span.file = file;
        token.span.line = raw.line;
        token.span.column = raw.column;
        token.span.end_line = raw.end_line;
        token.span.end_column = raw.end_column;
        token.newline_before = raw.newline_before != 0;
        tokens.push_back(std::move(token));
    }
    for (std::uint32_t i = 0; i < result.error_count; ++i) {
        const XlcLexError& raw = result.errors[i];
        Span span;
        span.file = file;
        if (raw.label_count > 0) {
            span.line = raw.labels[0].line;
            span.column = raw.labels[0].column;
            span.end_line = raw.labels[0].end_line;
            span.end_column = raw.labels[0].end_column;
        }
        diagnostics.error("E0001", span, std::string(raw.message, raw.message_len),
                          raw.label_count > 0
                              ? std::string(raw.labels[0].text, raw.labels[0].text_len)
                              : std::string());
    }
    xlcLexFree(result);
    return tokens;
}

const char* tokenKindName(TokenKind kind) {
    switch (kind) {
    case TokenKind::End: return "end of file";
    case TokenKind::Ident: return "identifier";
    case TokenKind::PrimType: return "type name";
    case TokenKind::Int: return "integer literal";
    case TokenKind::Float: return "float literal";
    case TokenKind::String: return "string literal";
    case TokenKind::ByteLit: return "byte literal";
    case TokenKind::Fn: return "fn";
    case TokenKind::Local: return "local";
    case TokenKind::Return: return "return";
    case TokenKind::If: return "if";
    case TokenKind::Else: return "else";
    case TokenKind::While: return "while";
    case TokenKind::Continue: return "continue";
    case TokenKind::Break: return "break";
    case TokenKind::Do: return "do";
    case TokenKind::Switch: return "switch";
    case TokenKind::Case: return "case";
    case TokenKind::Default: return "default";
    case TokenKind::Defer: return "defer";
    case TokenKind::Try: return "try";
    case TokenKind::Catch: return "catch";
    case TokenKind::Unsafe: return "unsafe";
    case TokenKind::Import: return "import";
    case TokenKind::From: return "from";
    case TokenKind::As: return "as";
    case TokenKind::Export: return "export";
    case TokenKind::Declare: return "declare";
    case TokenKind::Register: return "register";
    case TokenKind::Global: return "global";
    case TokenKind::Const: return "const";
    case TokenKind::Delete: return "delete";
    case TokenKind::Struct: return "struct";
    case TokenKind::Class: return "class";
    case TokenKind::Interface: return "interface";
    case TokenKind::Impl: return "impl";
    case TokenKind::Enum: return "enum";
    case TokenKind::Type: return "type";
    case TokenKind::Define: return "define";
    case TokenKind::New: return "new";
    case TokenKind::Virtual: return "virtual";
    case TokenKind::Override: return "override";
    case TokenKind::SelfKw: return "self";
    case TokenKind::For: return "for";
    case TokenKind::Except: return "except";
    case TokenKind::True: return "true";
    case TokenKind::False: return "false";
    case TokenKind::LParen: return "(";
    case TokenKind::RParen: return ")";
    case TokenKind::LBrace: return "{";
    case TokenKind::RBrace: return "}";
    case TokenKind::LBracket: return "[";
    case TokenKind::RBracket: return "]";
    case TokenKind::Comma: return ",";
    case TokenKind::Colon: return ":";
    case TokenKind::ColonAssign: return ":=";
    case TokenKind::Semicolon: return ";";
    case TokenKind::Dot: return ".";
    case TokenKind::Arrow: return "->";
    case TokenKind::At: return "@";
    case TokenKind::Bang: return "!";
    case TokenKind::Plus: return "+";
    case TokenKind::Minus: return "-";
    case TokenKind::Star: return "*";
    case TokenKind::Slash: return "/";
    case TokenKind::Percent: return "%";
    case TokenKind::Amp: return "&";
    case TokenKind::Pipe: return "|";
    case TokenKind::Caret: return "^";
    case TokenKind::Tilde: return "~";
    case TokenKind::Shl: return "<<";
    case TokenKind::Shr: return ">>";
    case TokenKind::Assign: return "=";
    case TokenKind::EqEq: return "==";
    case TokenKind::NotEq: return "!=";
    case TokenKind::Lt: return "<";
    case TokenKind::Le: return "<=";
    case TokenKind::Gt: return ">";
    case TokenKind::Ge: return ">=";
    case TokenKind::AndAnd: return "&&";
    case TokenKind::OrOr: return "||";
    case TokenKind::DotDot: return "..";
    case TokenKind::DotDotEq: return "..=";
    case TokenKind::Question: return "?";
    case TokenKind::PlusAssign: return "+=";
    case TokenKind::MinusAssign: return "-=";
    case TokenKind::StarAssign: return "*=";
    case TokenKind::SlashAssign: return "/=";
    case TokenKind::PercentAssign: return "%=";
    case TokenKind::AmpAssign: return "&=";
    case TokenKind::PipeAssign: return "|=";
    case TokenKind::CaretAssign: return "^=";
    case TokenKind::ShlAssign: return "<<=";
    case TokenKind::ShrAssign: return ">>=";
    case TokenKind::PlusPlus: return "++";
    case TokenKind::MinusMinus: return "--";
    case TokenKind::DirIf: return "#if";
    case TokenKind::DirElif: return "#elif";
    case TokenKind::DirElse: return "#else";
    case TokenKind::DirEndif: return "#endif";
    case TokenKind::DirIfdef: return "#ifdef";
    case TokenKind::DirIfndef: return "#ifndef";
    case TokenKind::DirError: return "#error";
    }
    return "?";
}

std::string describeToken(const Token& token) {
    switch (token.kind) {
    case TokenKind::End: return "end of file";
    case TokenKind::Ident: return std::format("identifier `{}`", token.text);
    case TokenKind::PrimType: return std::format("type `{}`", token.text);
    case TokenKind::Int:
    case TokenKind::Float: return std::format("number `{}`", token.text);
    case TokenKind::String: return "string literal";
    case TokenKind::ByteLit: return std::format("byte literal `{}`", token.int_value);
    default: return std::format("`{}`", tokenKindName(token.kind));
    }
}

}  // namespace xlang
