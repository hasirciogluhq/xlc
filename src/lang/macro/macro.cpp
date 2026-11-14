// The seam between the C++ compiler and the macro expander written in xlang
// (src/lang/macro/expand.xl).
//
// The expander itself lives on the xlang side; this file turns the token stream and the seed
// macros into the flat arrays of include/xlang/lang/macro_c.h, calls `xlcExpandMacros`, and turns
// the frozen result back into `Token` values, reporting the diagnostics the xlang side collected
// as E0004. Token kinds cross the boundary as the numbers of `enum class TokenKind`;
// src/lang/lexer/tokens.xl must agree number for number.
#include "xlang/lang/macro.h"

#include "xlang/lang/macro_c.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xlang {

namespace {

/// The bytes of an empty token text: the boundary wants a valid pointer even for length zero.
const char kEmptyText[1] = {'\0'};

/// Copies one boundary token back into a `Token`.
Token tokenOf(const XlcLexToken& raw, FileId file) {
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
    return token;
}

}  // namespace

std::vector<Token> expandMacros(std::vector<Token> tokens, MacroTable& table,
                                DiagnosticEngine& diagnostics) {
    // Every token of one call belongs to one file; the diagnostics and the result spans carry
    // that file id, which the boundary does not know.
    const FileId file = tokens.empty() ? FileId{} : tokens.front().span.file;

    // The texts the boundary sees point into the strings of `tokens` and `table`, which stay
    // alive until the call returns.
    std::vector<XlcMacroParam> params;
    std::vector<XlcLexToken> bodies;
    std::vector<XlcMacroDef> defs;
    defs.reserve(table.macros.size());
    for (const auto& [name, def] : table.macros) {
        XlcMacroDef raw;
        raw.name = name.data();
        raw.name_len = static_cast<std::uint32_t>(name.size());
        raw.params = nullptr;
        raw.param_count = static_cast<std::uint32_t>(def.params.size());
        if (!def.params.empty()) {
            raw.params = params.data() + params.size();
            for (const MacroParam& p : def.params) {
                XlcMacroParam raw_param;
                raw_param.kind = static_cast<std::int32_t>(p.kind);
                raw_param.name = p.name.data();
                raw_param.name_len = static_cast<std::uint32_t>(p.name.size());
                params.push_back(raw_param);
            }
        }
        raw.body = nullptr;
        raw.body_len = static_cast<std::uint32_t>(def.body.size());
        if (!def.body.empty()) {
            raw.body = bodies.data() + bodies.size();
            for (const Token& t : def.body) {
                XlcLexToken raw_token;
                raw_token.kind = static_cast<std::int32_t>(t.kind);
                raw_token.text = t.text.empty() ? kEmptyText : t.text.data();
                raw_token.text_len = static_cast<std::uint32_t>(t.text.size());
                raw_token.int_value = t.int_value;
                raw_token.float_value = t.float_value;
                raw_token.line = t.span.line;
                raw_token.column = t.span.column;
                raw_token.end_line = t.span.end_line;
                raw_token.end_column = t.span.end_column;
                raw_token.newline_before = t.newline_before ? 1 : 0;
                bodies.push_back(raw_token);
            }
        }
        defs.push_back(raw);
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

    const XlcMacroResult result =
        xlcExpandMacros(raw_tokens.data(), static_cast<std::uint32_t>(raw_tokens.size()),
                        defs.data(), static_cast<std::uint32_t>(defs.size()));

    std::vector<Token> out;
    out.reserve(result.token_count);
    for (std::uint32_t i = 0; i < result.token_count; ++i) {
        out.push_back(tokenOf(result.tokens[i], file));
    }
    for (std::uint32_t i = 0; i < result.error_count; ++i) {
        const XlcLexError& raw = result.errors[i];
        Diagnostic d;
        d.code = "E0004";
        d.severity = Severity::Error;
        d.message = std::string(raw.message, raw.message_len);
        for (std::uint32_t l = 0; l < raw.label_count; ++l) {
            Span span;
            span.file = file;
            span.line = raw.labels[l].line;
            span.column = raw.labels[l].column;
            span.end_line = raw.labels[l].end_line;
            span.end_column = raw.labels[l].end_column;
            d.labels.push_back({span, std::string(raw.labels[l].text, raw.labels[l].text_len)});
        }
        diagnostics.report(std::move(d));
    }
    // The macros the run collected from the token stream itself join the table, so the next file
    // of the compilation sees them too.
    for (std::uint32_t i = 0; i < result.def_count; ++i) {
        const XlcMacroDef& raw = result.defs[i];
        MacroDef def;
        def.name.assign(raw.name, raw.name_len);
        def.span.file = file;
        def.span.line = raw.line;
        def.span.column = raw.column;
        def.span.end_line = raw.end_line;
        def.span.end_column = raw.end_column;
        for (std::uint32_t p = 0; p < raw.param_count; ++p) {
            const XlcMacroParam& raw_param = raw.params[p];
            MacroParam param;
            param.kind = static_cast<MacroParam::Kind>(raw_param.kind);
            param.name.assign(raw_param.name, raw_param.name_len);
            def.params.push_back(std::move(param));
        }
        for (std::uint32_t b = 0; b < raw.body_len; ++b) {
            def.body.push_back(tokenOf(raw.body[b], file));
        }
        table.macros.emplace(def.name, std::move(def));
    }
    xlcExpandMacrosFree(result);
    return out;
}

}  // namespace xlang
