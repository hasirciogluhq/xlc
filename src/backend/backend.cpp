// The frontend of the backend's C API (include/xlang/backend_c.h): the session, the sema, the
// lowering, the dumps and the boundary queries the output step needs. The entry points convert
// the flat boundary values, keep the ordering the CLI asks for, and turn every `XlangError` into
// a reported diagnostic and a failing exit code. Code generation is src/backend/codegen.cpp.
#include "xlang/backend_c.h"

#include "backend/session.hpp"
#include "xlang/hir/hir.h"
#include "xlang/sema/layout.h"

#include <format>
#include <iostream>

using namespace xlang;
using namespace xlang::backend;

void XLC_BACKEND_CALL xlcSetColor(int32_t on) {
    setColor(on != 0);
}

int32_t XLC_BACKEND_CALL xlcExplain(XlcStr code) {
    return guard([&] {
        const std::string text = DiagnosticEngine::explain(stringOf(code));
        if (text.empty()) {
            printError(std::format("unknown diagnostic code `{}`", textOf(code)));
            throw XlangError("");
        }
        std::cout << text;
        std::cout.flush();
    });
}

void XLC_BACKEND_CALL xlcVersion(void) {
    std::cout << XLANG_VERSION << "\n";
    std::cout.flush();
}

int32_t XLC_BACKEND_CALL xlcPrintCfg(const XlcConfig* config) {
    return guard([&] {
        const BuildConfig built = buildConfig(*config);
        std::cout << std::format("target = \"{}\"\n", built.target.triple);
        for (const auto& [name, value] : built.resolved()) {
            std::cout << std::format("{} = {}\n", name, value.toString());
        }
        std::cout.flush();
    });
}

int32_t XLC_BACKEND_CALL xlcDumpAst(const XlcConfig* config, XlcStr path) {
    return guard([&] {
        BuildConfig built = buildConfig(*config);
        SourceManager sources;
        DiagnosticEngine diagnostics(sources, std::cerr);
        diagnostics.configure(diagnosticOptions(*config));
        const FileId file = sources.loadFile(stringOf(path));
        const Program program = parseSource(sources, file, built, diagnostics);
        diagnostics.flush();
        if (diagnostics.hasErrors()) {
            printSummary(diagnostics);
            throw XlangError("");
        }
        dumpProgram(std::cout, program, true);
        std::cout.flush();
    });
}

XlcSession* XLC_BACKEND_CALL xlcSessionNew(const XlcConfig* config) {
    try {
        return reinterpret_cast<XlcSession*>(new Session(*config));
    } catch (const XlangError& error) {
        printError(error.what());
        return nullptr;
    } catch (const std::exception& error) {
        printError(error.what());
        return nullptr;
    }
}

void XLC_BACKEND_CALL xlcSessionFree(XlcSession* session) {
    delete reinterpret_cast<Session*>(session);
}

int32_t XLC_BACKEND_CALL xlcSessionAddModuleRoot(XlcSession* session, XlcStr spec) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        const std::string text = stringOf(spec);
        const std::size_t eq = text.find('=');
        if (eq == std::string::npos || eq == 0 || eq + 1 == text.size()) {
            throw XlangError(std::format("`--module-root {}` must be NAME=DIR", text));
        }
        const fs::path directory = text.substr(eq + 1);
        if (!fs::is_directory(directory)) {
            throw XlangError(std::format("`--module-root {}`: `{}` is not a directory", text,
                                         directory.string()));
        }
        s->addModuleRoot(text.substr(0, eq), directory);
    });
}

int32_t XLC_BACKEND_CALL xlcSessionAddImplicitImport(XlcSession* session, XlcStr spec) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] { s->addImplicitImport(stringOf(spec)); });
}

int32_t XLC_BACKEND_CALL xlcSessionLoadRoot(XlcSession* session, XlcStr path) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] { s->loadRoot(stringOf(path)); });
}

int32_t XLC_BACKEND_CALL xlcSessionAnalyze(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] { s->analyze(); });
}

int32_t XLC_BACKEND_CALL xlcSessionLower(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] { s->ensureLowered(); });
}

int32_t XLC_BACKEND_CALL xlcSessionDumpHir(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        s->ensureLowered();
        if (!s->diagnostics().hasErrors()) {
            s->diagnostics().flush();
            hir::dumpModule(std::cout, s->hirModule());
            std::cout.flush();
        }
    });
}

int32_t XLC_BACKEND_CALL xlcSessionDumpMir(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        s->ensureLowered();
        if (!s->diagnostics().hasErrors()) {
            s->diagnostics().flush();
            std::cout << s->mirModule().dump();
            std::cout.flush();
        }
    });
}

int32_t XLC_BACKEND_CALL xlcSessionDumpLayout(XlcSession* session, XlcStr type_name, int32_t record) {
    Session* s = reinterpret_cast<Session*>(session);
    return guard([&] {
        Sema& sema = *s->compilation().rootSema();
        const std::string name = stringOf(type_name);
        const std::vector<std::string> names = name.empty() ? sema.aggregateNames() : std::vector<std::string>{name};
        for (const std::string& aggregate : names) {
            if (auto layout = sema.layoutOfName(aggregate, Span{})) {
                LayoutRecord layout_record{aggregate, *layout};
                std::cout << formatLayoutRecord(layout_record);
                if (record != 0) {
                    s->history().put(std::move(layout_record));
                }
            }
        }
        std::cout.flush();
        if (record != 0 && !s->diagnostics().hasErrors()) {
            s->history().save(s->historyPath());
        }
    });
}

void XLC_BACKEND_CALL xlcSessionTarget(XlcSession* session, XlcTarget* target) {
    const TargetInfo& t = reinterpret_cast<Session*>(session)->config().target;
    *target = XlcTarget{viewOf(t.triple), viewOf(t.os), viewOf(t.arch)};
}

int32_t XLC_BACKEND_CALL xlcSessionOptLevel(XlcSession* session) {
    return reinterpret_cast<Session*>(session)->config().opt_level;
}

const void* XLC_BACKEND_CALL xlcSessionHirModule(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    s->ensureLowered();
    return &s->hirModule();
}

void XLC_BACKEND_CALL xlcSessionKeep(XlcSession* session, XlcIndexList functions, XlcIndexList globals) {
    const auto indices = [](XlcIndexList list) {
        return list.count == 0 ? std::vector<std::uint32_t>() : std::vector<std::uint32_t>(list.items, list.items + list.count);
    };
    reinterpret_cast<Session*>(session)->keep(indices(functions), indices(globals));
}

int32_t XLC_BACKEND_CALL xlcSessionSizeOf(XlcSession* session, XlcStr name, int64_t* size) {
    const auto bytes = reinterpret_cast<Session*>(session)->sizeOf(stringOf(name));
    if (!bytes) {
        return 1;
    }
    *size = static_cast<int64_t>(*bytes);
    return 0;
}

uint32_t XLC_BACKEND_CALL xlcSessionModuleCount(XlcSession* session) {
    return static_cast<uint32_t>(reinterpret_cast<Session*>(session)->modules().size());
}

int32_t XLC_BACKEND_CALL xlcSessionModule(XlcSession* session, uint32_t index, XlcModule* module) {
    const std::vector<XlcModule>& modules = reinterpret_cast<Session*>(session)->modules();
    if (index >= modules.size()) {
        return 1;
    }
    *module = modules[index];
    return 0;
}

void XLC_BACKEND_CALL xlcSessionConfigKey(XlcSession* session, XlcStr emit, XlcStr* key) {
    const std::string& text = reinterpret_cast<Session*>(session)->configKey(stringOf(emit));
    *key = viewOf(text);
}

void XLC_BACKEND_CALL xlcSessionDiagnose(XlcSession* session, XlcStr code, const XlcSpan* span, XlcStr message,
                                         XlcStr hint) {
    Session* s = reinterpret_cast<Session*>(session);
    Span at{};
    if (span != nullptr) {
        at = Span{span->file, span->line, span->column, span->end_line, span->end_column};
    }
    s->diagnostics().error(stringOf(code), at, stringOf(message), stringOf(hint));
}

int32_t XLC_BACKEND_CALL xlcSessionHasErrors(XlcSession* session) {
    return reinterpret_cast<Session*>(session)->diagnostics().hasErrors() ? 1 : 0;
}

int64_t XLC_BACKEND_CALL xlcSessionErrorCount(XlcSession* session) {
    return static_cast<int64_t>(reinterpret_cast<Session*>(session)->diagnostics().errorCount());
}

int64_t XLC_BACKEND_CALL xlcSessionWarningCount(XlcSession* session) {
    return static_cast<int64_t>(reinterpret_cast<Session*>(session)->diagnostics().warningCount());
}

void XLC_BACKEND_CALL xlcSessionPrintSummary(XlcSession* session) {
    Session* s = reinterpret_cast<Session*>(session);
    s->diagnostics().flush();
    printSummary(s->diagnostics());
}

void XLC_BACKEND_CALL xlcSessionFlush(XlcSession* session) {
    reinterpret_cast<Session*>(session)->diagnostics().flush();
}
