#pragma once

#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xlang {

enum class TokenKind {
    End,

    // Literals and names
    Ident,
    PrimType,  ///< i8 .. i64, isize, u8 .. u64, usize, f32, f64, bool, void
    Int,
    Float,
    String,
    ByteLit,   ///< `b')'`: one byte (0..255) in `int_value`, typed `u8`

    // Keywords
    Fn,
    Local,
    Return,
    If,
    Else,
    While,
    Continue,
    Break,
    Do,
    Switch,
    Case,
    Default,
    Defer,
    Try,
    Catch,
    Unsafe,
    Import,
    From,
    As,
    Export,
    Declare,
    Register,
    Global,
    Const,
    Delete,
    Struct,
    Class,
    Interface,
    Impl,
    Enum,
    Type,
    Define,
    New,
    Virtual,
    Override,
    SelfKw,
    For,
    Except,
    True,
    False,

    // Punctuation
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Colon,
    ColonAssign, ///< `:=` (inferred local type)
    Semicolon,
    Dot,
    Arrow,
    At,
    Bang,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Amp,
    Pipe,
    Caret,
    Tilde,
    Shl,
    Shr,
    Assign,
    EqEq,
    NotEq,
    Lt,
    Le,
    Gt,
    Ge,
    AndAnd,
    OrOr,
    DotDot,        ///< `..` (range in `for i in a..b`)
    DotDotEq,      ///< `..=` (inclusive range in `for i in a..=b`)
    Question,      ///< `?` (ternary)
    PlusAssign,    ///< `+=`
    MinusAssign,   ///< `-=`
    StarAssign,    ///< `*=`
    SlashAssign,   ///< `/=`
    PercentAssign, ///< `%=`
    AmpAssign,     ///< `&=`
    PipeAssign,    ///< `|=`
    CaretAssign,   ///< `^=`
    ShlAssign,     ///< `<<=`
    ShrAssign,     ///< `>>=`
    PlusPlus,      ///< `++` (statement only)
    MinusMinus,    ///< `--` (statement only)

    // Conditional-compilation directives (`#if` ...). The lexer only recognizes them; the
    // preprocessor (plan phase 3) consumes them.
    DirIf,
    DirElif,
    DirElse,
    DirEndif,
    DirIfdef,
    DirIfndef,
    DirError,
};

struct Token {
    TokenKind kind{TokenKind::End};
    /// Identifier/keyword spelling, decoded string value, or the directive name.
    std::string text;
    std::uint64_t int_value{0};
    double float_value{0.0};
    Span span;
    /// A line break (or the file start) precedes this token. Statements end at line breaks.
    bool newline_before{false};
};

/// Tokenizes `file`. Lexical problems are reported to `diagnostics` (E0001) and lexing continues.
/// The result always ends with a `TokenKind::End` token.
[[nodiscard]] std::vector<Token> lex(const SourceManager& sources, FileId file,
                                     DiagnosticEngine& diagnostics);

[[nodiscard]] const char* tokenKindName(TokenKind kind);
/// Human-readable spelling for diagnostics: "`fn`", "identifier", "end of file", ...
[[nodiscard]] std::string describeToken(const Token& token);

}  // namespace xlang
