// The seam between the C++ compiler and the frontend written in xlang
// (src/lang/frontend/frontend.xl): the lexer, the preprocessor, the macro expander and the
// parser all live on the xlang side, and `xlcFrontendParse` runs the whole pipeline in one call.
// This file flattens the C++ inputs the pipeline needs (the resolved `-D` definitions and the
// seeded macros), hands them over with the engine and source-manager handles, and rebuilds the
// `Program` tree from the flat AST result.
//
// `lex()` stays as a shim because the lexer unit test reads the raw token stream; the pipeline
// itself never materializes C++ tokens.

#include "xlang/lang/frontend.h"
#include "xlang/lang/frontend_c.h"
#include "xlang/lang/lexer_c.h"
#include "xlang/lang/macro_c.h"
#include "parser/bridge.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace xlang {

namespace {

/// The bytes of an empty string field: the boundary wants a valid pointer even for length zero.
const char kEmptyText[1] = {'\0'};

/// The resolved `-D` definitions as the flat array the preprocessor reads.
/// The resolved `-D` definitions as the flat array the preprocessor reads. `resolved` must stay
/// alive for the whole call: the name and string-value bytes point into its keys and values.
std::vector<XlcDefine> flatDefines(const std::map<std::string, ConfigValue>& resolved) {
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
    return defines;
}

/// The seeded macros as the flat arrays the expander reads. The params and bodies of all macros
/// share two arrays; the `XlcMacroDef` fields are assigned only after every push, because a push
/// can reallocate and an empty vector's `data()` is null.
std::vector<XlcMacroDef> flatMacros(const MacroTable& table, std::vector<XlcMacroParam>& params,
                                    std::vector<XlcLexToken>& bodies) {
    struct DefRange {
        std::size_t params_begin;
        std::size_t params_count;
        std::size_t body_begin;
        std::size_t body_count;
    };
    std::vector<DefRange> ranges;
    ranges.reserve(table.macros.size());
    for (const auto& [name, def] : table.macros) {
        const std::size_t params_begin = params.size();
        for (const MacroParam& p : def.params) {
            XlcMacroParam raw_param;
            raw_param.kind = static_cast<std::int32_t>(p.kind);
            raw_param.name = p.name.data();
            raw_param.name_len = static_cast<std::uint32_t>(p.name.size());
            params.push_back(raw_param);
        }
        const std::size_t body_begin = bodies.size();
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
        ranges.push_back({params_begin, params.size() - params_begin, body_begin,
                          bodies.size() - body_begin});
    }
    std::vector<XlcMacroDef> defs;
    defs.reserve(table.macros.size());
    std::size_t i = 0;
    for (const auto& [name, def] : table.macros) {
        const DefRange& r = ranges[i++];
        XlcMacroDef raw;
        raw.name = name.data();
        raw.name_len = static_cast<std::uint32_t>(name.size());
        raw.params = params.data() + r.params_begin;
        raw.param_count = static_cast<std::uint32_t>(r.params_count);
        raw.body = bodies.data() + r.body_begin;
        raw.body_len = static_cast<std::uint32_t>(r.body_count);
        raw.line = def.span.line;
        raw.column = def.span.column;
        raw.end_line = def.span.end_line;
        raw.end_column = def.span.end_column;
        defs.push_back(raw);
    }
    return defs;
}

}  // namespace

Program parseSource(const SourceManager& sources, FileId file, const BuildConfig& config,
                    DiagnosticEngine& diagnostics, MacroTable& macros) {
    const std::map<std::string, ConfigValue> resolved = config.resolved();
    std::vector<XlcMacroParam> params;
    std::vector<XlcLexToken> bodies;
    const std::vector<XlcMacroDef> defs = flatMacros(macros, params, bodies);
    const std::vector<XlcDefine> defines = flatDefines(resolved);
    const XlcParseResult result =
        xlcFrontendParse(sources.handle(), file, defines.data(),
                         static_cast<std::uint32_t>(defines.size()), defs.data(),
                         static_cast<std::uint32_t>(defs.size()), diagnostics.handle());
    Program program = readFlatProgram(result, file, diagnostics);
    xlcParseFree(result);
    return program;
}

Program parseSource(const SourceManager& sources, FileId file, const BuildConfig& config,
                    DiagnosticEngine& diagnostics) {
    MacroTable macros;
    return parseSource(sources, file, config, diagnostics, macros);
}

Program parseFile(const SourceManager& sources, FileId file, DiagnosticEngine& diagnostics) {
    BuildConfig config;
    return parseSource(sources, file, config, diagnostics);
}

/// Tokenizes one file for the lexer unit test: the frozen result of the xlang scanner turned
/// back into `Token` values, the errors reported as E0001. The pipeline itself never
/// materializes C++ tokens; `xlcFrontendParse` chains the stages in xlang.
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

}  // namespace xlang
