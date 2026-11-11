#pragma once

#include "xlang/hir/hir.h"
#include "xlang/lang/frontend.h"
#include "xlang/sema/sema.h"

#include <sstream>
#include <string>

namespace xlang_test {

/// Parses, checks and lowers one source text; `hir_text` is the HIR dump when no error was reported.
struct Lowered {
    xlang::SourceManager sources;
    std::ostringstream out;
    xlang::BuildConfig config;
    std::unique_ptr<xlang::DiagnosticEngine> diag;
    xlang::Program program;
    xlang::hir::Module module;
    std::string hir_text;

    explicit Lowered(std::string text, xlang::BuildConfig cfg = xlang::BuildConfig{})
        : config(std::move(cfg)) {
        const xlang::FileId file = sources.addFile("t.xl", std::move(text));
        diag = std::make_unique<xlang::DiagnosticEngine>(sources, out);
        program = xlang::parseSource(sources, file, config, *diag);
        xlang::Sema sema(program, config, *diag);
        if (!diag->hasErrors()) {
            sema.analyze();
        }
        if (!diag->hasErrors()) {
            module = sema.lower();
            std::ostringstream h;
            xlang::hir::dumpModule(h, module);
            hir_text = h.str();
        }
        diag->flush();
    }

    [[nodiscard]] bool has(const std::string& needle) const {
        return hir_text.find(needle) != std::string::npos;
    }
    [[nodiscard]] bool failedWith(const std::string& code) const {
        return diag->hasErrors() && out.str().find(code) != std::string::npos;
    }
};

} // namespace xlang_test
