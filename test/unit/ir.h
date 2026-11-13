#pragma once

#include "xlang/codegen/codegen.h"
#include "xlang/lang/frontend.h"
#include "xlang/sema/sema.h"

#include <iostream>
#include <sstream>
#include <string>

// LLVM IR for a given target: what the generator writes depends only on the source and the triple,
// not on the host, so these checks are unit tests.

namespace xlang_test {

/// The unoptimized IR of `text` built for `triple` as an object file; empty after an error.
inline std::string irFor(const std::string& triple, std::string text) {
    xlang::SourceManager sources;
    std::ostringstream out;
    xlang::BuildConfig config;
    config.target = *xlang::parseTargetTriple(triple);
    xlang::DiagnosticEngine diag(sources, out);
    const xlang::FileId file = sources.addFile("t.xl", std::move(text));
    xlang::Program program = xlang::parseSource(sources, file, config, diag);
    xlang::Sema sema(program, config, diag);
    if (!diag.hasErrors()) {
        sema.analyze();
    }
    if (diag.hasErrors()) {
        diag.flush();
        std::cerr << out.str();
        return {};
    }
    const xlang::hir::Module module = sema.lower();
    const xlang::codegen::CodegenInput input{
        module, config, sources, diag,
        [&sema](const xlang::Type& t) { return sema.layoutOf(t, xlang::Span{}); }, "t",
        xlang::codegen::Output::Object};
    const auto artifact = xlang::codegen::generate(input);
    return artifact ? artifact->ir() : std::string{};
}

/// `needle` is in `text`; prints the text when it is not.
inline bool contains(const std::string& text, const std::string& needle) {
    const bool found = text.find(needle) != std::string::npos;
    if (!found) {
        std::cerr << "--- missing `" << needle << "` in ---\n" << text << "\n";
    }
    return found;
}

} // namespace xlang_test
