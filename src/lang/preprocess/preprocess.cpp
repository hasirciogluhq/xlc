// The seam between the C++ compiler and the preprocessor written in xlang
// (src/lang/preprocess/preprocess.xl).
//
// The directive walk itself lives on the xlang side; this file turns the token stream and the
// resolved definitions into the flat arrays of include/xlang/lang/preprocess_c.h, calls
// `xlcPreprocess`, and turns the frozen result back into `Token` values, reporting the
// diagnostics the xlang side collected as E0003. Token kinds cross the boundary as the numbers of
// `enum class TokenKind`; src/lang/lexer/tokens.xl must agree number for number.
#include "xlang/lang/preprocess.h"

#include "xlang/lang/preprocess_c.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xlang {

namespace {

/// The bytes of an empty token text: the boundary wants a valid pointer even for length zero.
const char kEmptyText[1] = {'\0'};

}  // namespace

std::vector<Token> preprocess(std::vector<Token> tokens, const BuildConfig& config,
                              DiagnosticEngine& diagnostics) {
    // Every token of one call belongs to one file; the diagnostics and the result spans carry
    // that file id, which the boundary does not know.
    const FileId file = tokens.empty() ? FileId{} : tokens.front().span.file;

    // The texts the boundary sees point into the strings of `tokens` and `resolved`, which stay
    // alive until the call returns.
    const std::map<std::string, ConfigValue> resolved = config.resolved();
    std::vector<XlcDefine> defines;
    defines.reserve(resolved.size());
    for (const auto& [name, value] : resolved) {
        XlcDefine def;
        def.name = name.data();
        def.name_len = static_cast<std::uint32_t>(name.size());
        switch (value.kind) {
        case ConfigValue::Kind::Bool:
            def.kind = 0;
            def.int_value = value.bool_value ? 1 : 0;
            def.string_value = kEmptyText;
            def.string_len = 0;
            break;
        case ConfigValue::Kind::Int:
            def.kind = 1;
            def.int_value = value.int_value;
            def.string_value = kEmptyText;
            def.string_len = 0;
            break;
        case ConfigValue::Kind::String:
            def.kind = 2;
            def.int_value = 0;
            def.string_value = value.string_value.data();
            def.string_len = static_cast<std::uint32_t>(value.string_value.size());
            break;
        }
        defines.push_back(def);
    }

    std::vector<XlcLexToken> raw_tokens;
    raw_tokens.reserve(tokens.size());
    for (const Token& token : tokens) {
        XlcLexToken raw;
        raw.kind = static_cast<std::int32_t>(token.kind);
        raw.text = token.text.empty() ? kEmptyText : token.text.data();
        raw.text_len = static_cast<std::uint32_t>(token.text.size());
        raw.int_value = token.int_value;
        raw.float_value = token.float_value;
        raw.line = token.span.line;
        raw.column = token.span.column;
        raw.end_line = token.span.end_line;
        raw.end_column = token.span.end_column;
        raw.newline_before = token.newline_before ? 1 : 0;
        raw_tokens.push_back(raw);
    }

    const XlcPreprocessResult result =
        xlcPreprocess(raw_tokens.data(), static_cast<std::uint32_t>(raw_tokens.size()),
                      defines.data(), static_cast<std::uint32_t>(defines.size()));

    std::vector<Token> out;
    out.reserve(result.token_count);
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
        out.push_back(std::move(token));
    }
    for (std::uint32_t i = 0; i < result.error_count; ++i) {
        const XlcLexError& raw = result.errors[i];
        Span span;
        span.file = file;
        span.line = raw.line;
        span.column = raw.column;
        span.end_line = raw.end_line;
        span.end_column = raw.end_column;
        diagnostics.error("E0003", span, std::string(raw.message, raw.message_len),
                          std::string(raw.label, raw.label_len));
    }
    xlcPreprocessFree(result);
    return out;
}

}  // namespace xlang
