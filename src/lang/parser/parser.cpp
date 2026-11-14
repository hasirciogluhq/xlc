// Parser: the parser driver: run(), state snapshots and the public entry points.

#include "lang/parser/parser.hpp"

#include "xlang/lang/parser.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace xlang {

namespace {

/// The parser the `XLC_PARSER` environment variable picks while the xlang port settles:
/// `xl` (the port), `cpp` (the reference) and `diff` (both, compared).
enum class ParserChoice { Cpp, Xl, Diff };

ParserChoice parserChoice() {
    const char* mode = std::getenv("XLC_PARSER");
    if (mode == nullptr) {
        return ParserChoice::Cpp;
    }
    const std::string choice(mode);
    if (choice == "xl") {
        return ParserChoice::Xl;
    }
    if (choice == "diff") {
        return ParserChoice::Diff;
    }
    return ParserChoice::Cpp;
}

/// The full dump of one program as a string (the differential test compares these).
std::string dumpToString(const Program& program) {
    std::ostringstream out;
    dumpProgram(out, program, false);
    return out.str();
}

}  // namespace

Program parseTokens(std::vector<Token> tokens, FileId file, DiagnosticEngine& diagnostics) {
    const ParserChoice choice = parserChoice();
    if (choice == ParserChoice::Cpp) {
        parse::Parser parser(std::move(tokens), file, diagnostics);
        return parser.run();
    }
    if (choice == ParserChoice::Xl) {
        return parseTokensFlat(std::move(tokens), file, diagnostics);
    }
    // Diff: both parsers over the same tokens, each with a throwaway diagnostic engine, then the
    // full dumps compared. On a difference both dumps land in /tmp and the run stops; on a match
    // the xlang parser runs again against the real engine and its program is returned.
    SourceManager quietSources;
    std::ostringstream quietOut;
    DiagnosticEngine quiet(quietSources, quietOut);
    Program cpp = parse::Parser(tokens, file, quiet).run();
    Program xl = parseTokensFlat(tokens, file, quiet);
    const std::string xlDump = dumpToString(xl);
    const std::string cppDump = dumpToString(cpp);
    if (cppDump != xlDump) {
        const std::string base = "/tmp/astdiff-" + std::to_string(static_cast<long>(file));
        std::ofstream(base + ".cpp") << cppDump;
        std::ofstream(base + ".xl") << xlDump;
        std::cerr << "AST-DIFF file id " << file << ": dumps at " << base << ".{cpp,xl}\n";
        std::exit(3);
    }
    return parseTokensFlat(std::move(tokens), file, diagnostics);
}

}  // namespace xlang

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

Program parseFile(const SourceManager& sources, FileId file, DiagnosticEngine& diagnostics) {
    return parseTokens(lex(sources, file, diagnostics), file, diagnostics);
}

} // namespace xlang
