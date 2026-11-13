#include "xlang/lang/lexer.h"

#include "xlang/lang/types.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <locale>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace xlang {

namespace {

[[nodiscard]] const std::unordered_map<std::string_view, TokenKind>& keywords() {
    static const std::unordered_map<std::string_view, TokenKind> table = {
        {"fn", TokenKind::Fn},         {"local", TokenKind::Local},
        {"return", TokenKind::Return}, {"if", TokenKind::If},
        {"else", TokenKind::Else},     {"while", TokenKind::While},
        {"continue", TokenKind::Continue}, {"break", TokenKind::Break},
        {"do", TokenKind::Do},         {"switch", TokenKind::Switch},
        {"case", TokenKind::Case},     {"default", TokenKind::Default},
        {"defer", TokenKind::Defer},
        {"try", TokenKind::Try},       {"catch", TokenKind::Catch},
        {"unsafe", TokenKind::Unsafe}, {"import", TokenKind::Import},
        {"from", TokenKind::From},     {"as", TokenKind::As},
        {"export", TokenKind::Export}, {"declare", TokenKind::Declare},
        {"register", TokenKind::Register}, {"global", TokenKind::Global},
        {"const", TokenKind::Const}, {"delete", TokenKind::Delete},
        {"struct", TokenKind::Struct}, {"class", TokenKind::Class},
        {"interface", TokenKind::Interface}, {"impl", TokenKind::Impl},
        {"enum", TokenKind::Enum},     {"type", TokenKind::Type},
        {"define", TokenKind::Define}, {"new", TokenKind::New},
        {"virtual", TokenKind::Virtual}, {"override", TokenKind::Override},
        {"self", TokenKind::SelfKw},   {"for", TokenKind::For},
        {"except", TokenKind::Except},
        {"true", TokenKind::True},     {"false", TokenKind::False},
    };
    return table;
}

[[nodiscard]] const std::unordered_map<std::string_view, TokenKind>& directives() {
    static const std::unordered_map<std::string_view, TokenKind> table = {
        {"if", TokenKind::DirIf},       {"elif", TokenKind::DirElif},
        {"else", TokenKind::DirElse},   {"endif", TokenKind::DirEndif},
        {"ifdef", TokenKind::DirIfdef}, {"ifndef", TokenKind::DirIfndef},
        {"error", TokenKind::DirError},
    };
    return table;
}

[[nodiscard]] bool isIdentStart(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

[[nodiscard]] bool isIdentPart(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

[[nodiscard]] int hexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

class Scanner {
public:
    Scanner(const SourceFile& source, FileId file, DiagnosticEngine& diagnostics)
        : text_(source.text()), file_(file), diagnostics_(diagnostics) {}

    std::vector<Token> run() {
        if (text_.starts_with("\xEF\xBB\xBF")) {
            pos_ = 3;
        }
        while (true) {
            skipTrivia();
            if (pos_ >= text_.size()) {
                Token end = begin(TokenKind::End);
                finish(end);
                tokens_.push_back(std::move(end));
                break;
            }
            scanToken();
        }
        return std::move(tokens_);
    }

private:
    [[nodiscard]] char peek(std::size_t ahead = 0) const {
        return pos_ + ahead < text_.size() ? text_[pos_ + ahead] : '\0';
    }

    char advance() {
        const char c = text_[pos_++];
        if (c == '\n') {
            ++line_;
            column_ = 1;
            at_line_start_ = true;
            newline_pending_ = true;
        } else {
            ++column_;
        }
        return c;
    }

    [[nodiscard]] Span here(std::uint32_t length = 1) const {
        Span s;
        s.file = file_;
        s.line = line_;
        s.column = column_;
        s.end_line = line_;
        s.end_column = column_ + length;
        return s;
    }

    Token begin(TokenKind kind) {
        Token t;
        t.kind = kind;
        t.span.file = file_;
        t.span.line = line_;
        t.span.column = column_;
        t.newline_before = newline_pending_;
        return t;
    }

    void finish(Token& t) {
        t.span.end_line = line_;
        t.span.end_column = column_;
    }

    void skipTrivia() {
        while (pos_ < text_.size()) {
            const char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                advance();
            } else if (c == '/' && peek(1) == '/') {
                while (pos_ < text_.size() && peek() != '\n') {
                    advance();
                }
            } else {
                break;
            }
        }
    }

    void error(const Span& span, std::string message, std::string label) {
        diagnostics_.error("E0001", span, std::move(message), std::move(label));
    }

    void emit(Token t) {
        finish(t);
        tokens_.push_back(std::move(t));
        newline_pending_ = false;
        at_line_start_ = false;
    }

    void single(TokenKind kind, std::size_t length = 1) {
        Token t = begin(kind);
        for (std::size_t i = 0; i < length; ++i) {
            advance();
        }
        emit(std::move(t));
    }

    void scanToken() {
        const char c = peek();

        // `b'x'`: a byte literal. Checked before the identifier rule, because `b` is a letter.
        if (c == 'b' && peek(1) == '\'') {
            scanByte();
            return;
        }
        if (isIdentStart(c)) {
            scanWord();
            return;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            scanNumber();
            return;
        }
        switch (c) {
        case '"': scanString(); return;
        case '#': scanDirective(); return;
        case '(': single(TokenKind::LParen); return;
        case ')': single(TokenKind::RParen); return;
        case '{': single(TokenKind::LBrace); return;
        case '}': single(TokenKind::RBrace); return;
        case '[': single(TokenKind::LBracket); return;
        case ']': single(TokenKind::RBracket); return;
        case ',': single(TokenKind::Comma); return;
        case ':':
            if (peek(1) == '=') {
                single(TokenKind::ColonAssign, 2);
            } else {
                single(TokenKind::Colon);
            }
            return;
        case ';': single(TokenKind::Semicolon); return;
        case '.':
            if (peek(1) == '.') {
                if (peek(2) == '=') {
                    single(TokenKind::DotDotEq, 3);
                } else {
                    single(TokenKind::DotDot, 2);
                }
            } else {
                single(TokenKind::Dot);
            }
            return;
        case '@': single(TokenKind::At); return;
        case '?': single(TokenKind::Question); return;
        case '+':
            if (peek(1) == '=') {
                single(TokenKind::PlusAssign, 2);
            } else if (peek(1) == '+') {
                single(TokenKind::PlusPlus, 2);
            } else {
                single(TokenKind::Plus);
            }
            return;
        case '*':
            if (peek(1) == '=') {
                single(TokenKind::StarAssign, 2);
            } else {
                single(TokenKind::Star);
            }
            return;
        case '/':
            if (peek(1) == '=') {
                single(TokenKind::SlashAssign, 2);
            } else {
                single(TokenKind::Slash);
            }
            return;
        case '%':
            if (peek(1) == '=') {
                single(TokenKind::PercentAssign, 2);
            } else {
                single(TokenKind::Percent);
            }
            return;
        case '-':
            if (peek(1) == '>') {
                single(TokenKind::Arrow, 2);
            } else if (peek(1) == '=') {
                single(TokenKind::MinusAssign, 2);
            } else if (peek(1) == '-') {
                single(TokenKind::MinusMinus, 2);
            } else {
                single(TokenKind::Minus);
            }
            return;
        case '=':
            if (peek(1) == '=') {
                single(TokenKind::EqEq, 2);
            } else {
                single(TokenKind::Assign);
            }
            return;
        case '!':
            if (peek(1) == '=') {
                single(TokenKind::NotEq, 2);
            } else {
                single(TokenKind::Bang);
            }
            return;
        case '<':
            if (peek(1) == '=') {
                single(TokenKind::Le, 2);
            } else if (peek(1) == '<' && peek(2) == '=') {
                single(TokenKind::ShlAssign, 3);
            } else if (peek(1) == '<') {
                single(TokenKind::Shl, 2);
            } else {
                single(TokenKind::Lt);
            }
            return;
        case '>':
            if (peek(1) == '=') {
                single(TokenKind::Ge, 2);
            } else if (peek(1) == '>' && peek(2) == '=') {
                single(TokenKind::ShrAssign, 3);
            } else if (peek(1) == '>') {
                single(TokenKind::Shr, 2);
            } else {
                single(TokenKind::Gt);
            }
            return;
        case '^':
            if (peek(1) == '=') {
                single(TokenKind::CaretAssign, 2);
            } else {
                single(TokenKind::Caret);
            }
            return;
        case '~': single(TokenKind::Tilde); return;
        case '&':
            if (peek(1) == '&') {
                single(TokenKind::AndAnd, 2);
            } else if (peek(1) == '=') {
                single(TokenKind::AmpAssign, 2);
            } else {
                single(TokenKind::Amp);
            }
            return;
        case '|':
            if (peek(1) == '|') {
                single(TokenKind::OrOr, 2);
            } else if (peek(1) == '=') {
                single(TokenKind::PipeAssign, 2);
            } else {
                single(TokenKind::Pipe);
            }
            return;
        default: break;
        }

        const Span span = here();
        error(span, std::format("unexpected character `{}`", printable(c)),
              "this character is not part of the language");
        advance();
    }

    [[nodiscard]] static std::string printable(char c) {
        const auto u = static_cast<unsigned char>(c);
        if (u >= 0x20 && u < 0x7F) {
            return std::string(1, c);
        }
        return std::format("\\x{:02x}", static_cast<unsigned>(u));
    }

    void scanWord() {
        Token t = begin(TokenKind::Ident);
        const std::size_t start = pos_;
        while (isIdentPart(peek())) {
            advance();
        }
        t.text = std::string(text_.substr(start, pos_ - start));
        if (const auto it = keywords().find(t.text); it != keywords().end()) {
            t.kind = it->second;
        } else if (isPrimitiveTypeName(t.text)) {
            t.kind = TokenKind::PrimType;
        }
        emit(std::move(t));
    }

    void scanDirective() {
        Token t = begin(TokenKind::DirIf);
        const bool line_start = at_line_start_ || tokens_.empty() || newline_pending_;
        advance();  // '#'
        const std::size_t start = pos_;
        while (std::isalpha(static_cast<unsigned char>(peek())) != 0) {
            advance();
        }
        t.text = std::string(text_.substr(start, pos_ - start));
        finish(t);
        if (!line_start) {
            error(t.span, "preprocessor directive is not the first token on its line",
                  "directives must start their line");
            newline_pending_ = false;
            return;
        }
        const auto it = directives().find(t.text);
        if (it == directives().end()) {
            error(t.span, std::format("unknown directive `#{}`", t.text),
                  "expected if, elif, else, endif, ifdef, ifndef or error");
            return;
        }
        t.kind = it->second;
        emit(std::move(t));
    }

    /// `b'x'`: exactly one byte, written as a printable character, a `\` escape, a `\xHH` pair or a
    /// `\u{...}` code point below 128. The byte lands in `int_value` (0..255), so the parser makes a
    /// `u8` literal from it without touching `text`.
    void scanByte() {
        Token t = begin(TokenKind::ByteLit);
        advance();  // 'b'
        advance();  // opening quote
        const Span at = here(1);
        std::uint64_t value = 0;
        if (pos_ >= text_.size() || peek() == '\n') {
            error(at, "unterminated byte literal", "write one byte as `b'x'`");
        } else if (peek() == '\\') {
            advance();
            const char e = peek();
            advance();
            switch (e) {
            case 'n': value = '\n'; break;
            case 't': value = '\t'; break;
            case 'r': value = '\r'; break;
            case '0': value = 0; break;
            case '\\': value = '\\'; break;
            case '\'': value = '\''; break;
            case '"': value = '"'; break;
            case 'x': {
                const int hi = hexValue(peek());
                const int lo = hi >= 0 ? hexValue(peek(1)) : -1;
                if (hi < 0 || lo < 0) {
                    error(at, "malformed `\\x` escape in a byte literal", "expected two hex digits");
                } else {
                    advance();
                    advance();
                    value = static_cast<std::uint64_t>(hi * 16 + lo);
                }
                break;
            }
            default:
                error(at, std::format("unknown escape `\\{}` in a byte literal", printable(e)),
                      "valid escapes: \\n \\t \\r \\0 \\\\ \\' \\\" \\xHH");
            }
        } else {
            const unsigned char raw = static_cast<unsigned char>(advance());
            if (raw >= 0x80) {
                error(at, "a byte literal holds one ASCII character",
                      "write non-ASCII bytes as `b'\\xHH'`");
            }
            value = raw;
        }
        if (pos_ < text_.size() && peek() != '\'') {
            error(at, "a byte literal holds exactly one byte", "write `b'x'`, or use a string literal");
            while (pos_ < text_.size() && peek() != '\'' && peek() != '\n') {
                advance();
            }
        }
        if (pos_ >= text_.size() || peek() != '\'') {
            error(at, "unterminated byte literal", "close it with `'`");
        } else {
            advance();
        }
        finish(t);
        t.int_value = value;
        emit(std::move(t));
    }

    void scanString() {
        Token t = begin(TokenKind::String);
        advance();  // opening quote
        std::string value;
        bool terminated = false;
        while (pos_ < text_.size()) {
            const char c = peek();
            if (c == '\n') {
                break;
            }
            if (c == '"') {
                advance();
                terminated = true;
                break;
            }
            if (c != '\\') {
                value += advance();
                continue;
            }
            const Span escape_span = here(2);
            advance();  // backslash
            const char e = peek();
            if (pos_ >= text_.size() || e == '\n') {
                break;
            }
            advance();
            switch (e) {
            case 'n': value += '\n'; break;
            case 't': value += '\t'; break;
            case 'r': value += '\r'; break;
            case '0': value += '\0'; break;
            case '\\': value += '\\'; break;
            case '"': value += '"'; break;
            case '\'': value += '\''; break;
            case 'x': {
                const int hi = hexValue(peek());
                const int lo = hi >= 0 ? hexValue(peek(1)) : -1;
                if (hi < 0 || lo < 0) {
                    error(escape_span, "malformed `\\x` escape", "expected two hex digits");
                } else {
                    advance();
                    advance();
                    value += static_cast<char>(hi * 16 + lo);
                }
                break;
            }
            default:
                error(escape_span, std::format("unknown escape `\\{}`", printable(e)),
                      "valid escapes: \\n \\t \\r \\0 \\\\ \\\" \\' \\xHH");
            }
        }
        finish(t);
        if (!terminated) {
            error(t.span, "unterminated string literal", "the string is not closed before the end of the line");
        }
        t.text = std::move(value);
        emit(std::move(t));
    }

    void scanNumber() {
        Token t = begin(TokenKind::Int);
        const std::size_t start = pos_;
        // Right after a `.` the number is a tuple position (`t.0.1` is `t`, `.`, `0`, `.`, `1`), so
        // it never takes a fraction or an exponent: `0.1` there would swallow the second access.
        const bool after_dot = !tokens_.empty() && tokens_.back().kind == TokenKind::Dot &&
                               tokens_.back().span.end_line == t.span.line &&
                               tokens_.back().span.end_column == t.span.column;
        unsigned base = 10;
        if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
            base = 16;
            advance();
            advance();
        } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
            base = 2;
            advance();
            advance();
        } else if (peek() == '0' && (peek(1) == 'o' || peek(1) == 'O')) {
            base = 8; // `0o644`: a leading `0` alone stays decimal, so `0644` is never silently octal
            advance();
            advance();
        }
        const std::size_t digits_start = pos_;
        std::string digits;
        while (true) {
            const char c = peek();
            if (c == '_') {
                advance();
                continue;
            }
            const int v = hexValue(c);
            const bool ok = base == 16 ? v >= 0 : (v >= 0 && static_cast<unsigned>(v) < base && c != 'e' && c != 'E');
            if (!ok) {
                break;
            }
            digits += c;
            advance();
        }

        bool is_float = false;
        if (base == 10 && !after_dot) {
            if (peek() == '.'&& std::isdigit(static_cast<unsigned char>(peek(1))) != 0) {
                is_float = true;
                digits += advance();
                while (std::isdigit(static_cast<unsigned char>(peek())) != 0 || peek() == '_') {
                    const char c = advance();
                    if (c != '_') {
                        digits += c;
                    }
                }
            }
            if ((peek() == 'e' || peek() == 'E') &&
                (std::isdigit(static_cast<unsigned char>(peek(1))) != 0 ||
                 ((peek(1) == '+' || peek(1) == '-') &&
                  std::isdigit(static_cast<unsigned char>(peek(2))) != 0))) {
                is_float = true;
                digits += advance();
                if (peek() == '+' || peek() == '-') {
                    digits += advance();
                }
                while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                    digits += advance();
                }
            }
        }

        if (isIdentPart(peek())) {
            // Digits glued to letters (`12abc`, `0b12`): consume the whole word, report once.
            while (isIdentPart(peek())) {
                advance();
            }
            finish(t);
            error(t.span, "malformed number literal",
                  std::format("`{}` is not a valid number", text_.substr(start, pos_ - start)));
            t.text = std::string(text_.substr(start, pos_ - start));
            emit(std::move(t));
            return;
        }
        (void)digits_start;
        finish(t);
        t.text = std::string(text_.substr(start, pos_ - start));

        if (is_float) {
            t.kind = TokenKind::Float;
            // Read in the classic locale, so `.` is the decimal point whatever the process locale
            // is (`strtod` follows the C locale). Not `std::from_chars`: the libc++ of the macOS
            // SDK has no floating-point overload. On overflow `num_get` sets `failbit`.
            // https://en.cppreference.com/w/cpp/locale/num_get/get
            std::istringstream in(digits);
            in.imbue(std::locale::classic());
            in >> t.float_value;
            if (in.fail() || std::isinf(t.float_value)) {
                error(t.span, "float literal is out of range", "does not fit in an `f64`");
            }
            emit(std::move(t));
            return;
        }
        if (digits.empty()) {
            error(t.span, "malformed number literal", "expected digits after the base prefix");
            emit(std::move(t));
            return;
        }
        std::uint64_t value = 0;
        bool overflow = false;
        for (const char c : digits) {
            const auto digit = static_cast<std::uint64_t>(hexValue(c));
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / base) {
                overflow = true;
                break;
            }
            value = value * base + digit;
        }
        if (overflow) {
            error(t.span, "integer literal is too large", "does not fit in 64 bits");
        }
        t.int_value = value;
        emit(std::move(t));
    }

    const std::string& text_;
    FileId file_;
    DiagnosticEngine& diagnostics_;
    std::size_t pos_{0};
    std::uint32_t line_{1};
    std::uint32_t column_{1};
    bool at_line_start_{true};
    bool newline_pending_{true};
    std::vector<Token> tokens_;
};

}  // namespace

std::vector<Token> lex(const SourceManager& sources, FileId file, DiagnosticEngine& diagnostics) {
    Scanner scanner(sources.file(file), file, diagnostics);
    return scanner.run();
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
