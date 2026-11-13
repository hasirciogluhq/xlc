// Parser: the parser driver: run(), state snapshots and the public entry points.

#include "lang/parser/parser.hpp"

namespace xlang::parse {

Program Parser::run() {
    Program program;
    program.file = file_;

    // `@file(...)` attributes at the very top.
    while (at(TokenKind::At) && peekKind(1) == TokenKind::Ident && peek(1).text == "file") {
        try {
            program.file_attributes.push_back(parseAttribute());
        } catch (const ParseAbort&) {
            syncItem(pos_);
        }
    }

    while (!at(TokenKind::End)) {
        const std::size_t start = pos_;
        try {
            parseItem(program);
        } catch (const ParseAbort&) {
            restore(State{});
            syncItem(start);
        }
    }
    for (auto& lifted : lambdas_) {
        program.items.push_back(std::move(lifted));
    }
    lambdas_.clear();
    return program;
}

Parser::State Parser::state() const {
    return {nl_depth_, prefix_operand_, prefix_nl_base_, no_struct_, for_header_, cast_type_,
            open_ternaries_.size()};
}

void Parser::restore(const State& s) {
    nl_depth_ = s.nl_depth;
    prefix_operand_ = s.prefix_operand;
    prefix_nl_base_ = s.prefix_nl_base;
    no_struct_ = s.no_struct;
    for_header_ = s.for_header;
    cast_type_ = s.cast_type;
    open_ternaries_.resize(s.open_ternaries);
}

} // namespace xlang::parse

namespace xlang {

Program parseTokens(std::vector<Token> tokens, FileId file, DiagnosticEngine& diagnostics) {
    parse::Parser parser(std::move(tokens), file, diagnostics);
    return parser.run();
}

Program parseFile(const SourceManager& sources, FileId file, DiagnosticEngine& diagnostics) {
    return parseTokens(lex(sources, file, diagnostics), file, diagnostics);
}

} // namespace xlang
