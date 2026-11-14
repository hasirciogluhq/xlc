#pragma once

#include "xlang/analysis/analysis.h"
#include "xlang/hir/hir.h"
#include "xlang/lang/frontend.h"
#include "xlang/mir/build.h"
#include "xlang/sema/sema.h"

#include <sstream>
#include <string>

namespace xlang_test {

/// Parses, checks, lowers and analyzes one source text, like `xlc` does; `hir_text` and `mir_text`
/// are the HIR and MIR dumps when no error was reported.
struct Lowered {
    xlang::SourceManager sources;
    std::ostringstream out;
    xlang::BuildConfig config;
    std::unique_ptr<xlang::DiagnosticEngine> diag;
    xlang::Program program;
    xlang::hir::Module module;
    xlang::mir::Module mir;
    std::string hir_text;
    std::string mir_text;

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
            mir = xlang::mir::build(module);
            xlang::analysis::analyze(module, mir, *diag);
        }
        if (!diag->hasErrors()) {
            std::ostringstream h;
            xlang::hir::dumpModule(h, module);
            hir_text = h.str();
            std::ostringstream m;
            xlang::mir::dumpModule(m, mir);
            mir_text = m.str();
        }
        diag->flush();
    }

    [[nodiscard]] bool has(const std::string& needle) const {
        return hir_text.find(needle) != std::string::npos;
    }
    [[nodiscard]] bool hasMir(const std::string& needle) const {
        return mir_text.find(needle) != std::string::npos;
    }
    [[nodiscard]] bool failedWith(const std::string& code) const {
        return diag->hasErrors() && out.str().find(code) != std::string::npos;
    }
};

} // namespace xlang_test
