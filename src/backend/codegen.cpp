// The code generation entries of the backend's C API (include/xlang/codegen/codegen_c.h): the
// LLVM generator runs on the session's lowered HIR, and the artifact it returns is handed to the
// output step, which decides what is optimized, written, archived or linked. The generator itself
// is src/codegen (LLVM, C++); this file is only the boundary.
#include "xlang/codegen/codegen_c.h"

#include "backend/session.hpp"
#include "xlang/codegen/archive.h"
#include "xlang/codegen/codegen.h"

#include <memory>
#include <optional>

using namespace xlang;
using namespace xlang::backend;

/// The artifact behind the opaque handle: the LLVM module and its target machine.
struct XlcArtifact {
    std::unique_ptr<codegen::Artifact> artifact;
};

XlcArtifact* XLC_BACKEND_CALL xlcCodegenGenerate(XlcSession* session, const XlcCodegenRequest* request) {
    Session* s = reinterpret_cast<Session*>(session);
    try {
        s->ensureLowered();
        if (s->diagnostics().hasErrors()) {
            return nullptr;
        }
        const codegen::LayoutQuery layout = [s](const Type& t) {
            return s->compilation().rootSema()->layoutOf(t, Span{});
        };
        codegen::CodegenInput input{s->hirModule(), s->config(), s->sources(), s->diagnostics(), layout,
                                    stringOf(request->name), codegen::outputOf(stringOf(request->emit))};
        if (request->has_only != 0) {
            input.only_module = stringOf(request->only);
        }
        auto artifact = codegen::generate(input);
        if (!artifact) {
            return nullptr;
        }
        return new XlcArtifact{std::move(artifact)};
    } catch (const XlangError& error) {
        if (error.what()[0] != '\0') {
            printError(error.what());
        }
        return nullptr;
    } catch (const std::exception& error) {
        printError(error.what());
        return nullptr;
    }
}

void XLC_BACKEND_CALL xlcArtifactOptimize(XlcArtifact* artifact, int32_t opt_level) {
    artifact->artifact->optimize(opt_level);
}

int32_t XLC_BACKEND_CALL xlcArtifactWriteIR(XlcArtifact* artifact, XlcStr path) {
    return guard([&] { artifact->artifact->writeIR(stringOf(path)); });
}

int32_t XLC_BACKEND_CALL xlcArtifactWriteBitcode(XlcArtifact* artifact, XlcStr path) {
    return guard([&] { artifact->artifact->writeBitcode(stringOf(path)); });
}

int32_t XLC_BACKEND_CALL xlcArtifactWriteObject(XlcArtifact* artifact, XlcStr path) {
    return guard([&] { artifact->artifact->writeObject(stringOf(path)); });
}

void XLC_BACKEND_CALL xlcArtifactFree(XlcArtifact* artifact) {
    delete artifact;
}

int32_t XLC_BACKEND_CALL xlcArchive(XlcStrList objects, XlcStr output, XlcStr os) {
    return guard([&] {
        std::vector<std::filesystem::path> paths;
        for (const std::string& o : listOf(objects)) {
            paths.emplace_back(o);
        }
        TargetInfo target;
        target.os = stringOf(os);
        codegen::archive(paths, stringOf(output), target);
    });
}
