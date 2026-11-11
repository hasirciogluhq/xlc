// Plan 5.2 (PassBuilder pipeline, CPU) and 5.6 (obj / ir / bc writers).

#include "artifact.h"

#include "xlang/util/error.h"

#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>

#include <mutex>

namespace xlang::codegen {

namespace {

void initTargetsOnce() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        // x86_64 and aarch64 are the targets of the language (plan: Linux/macOS/Windows).
        LLVMInitializeAArch64TargetInfo();
        LLVMInitializeAArch64Target();
        LLVMInitializeAArch64TargetMC();
        LLVMInitializeAArch64AsmPrinter();
        LLVMInitializeAArch64AsmParser();
        LLVMInitializeX86TargetInfo();
        LLVMInitializeX86Target();
        LLVMInitializeX86TargetMC();
        LLVMInitializeX86AsmPrinter();
        LLVMInitializeX86AsmParser();
    });
}

}  // namespace

std::unique_ptr<llvm::TargetMachine> createMachine(const BuildConfig& config, std::string& triple_out,
                                                   std::string& error) {
    initTargetsOnce();
    std::string triple = config.target.triple;
    if (triple.empty()) {
        triple = llvm::sys::getDefaultTargetTriple();
    }
    const llvm::Triple t(triple);
    std::string lookup_error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(t, lookup_error);
    if (target == nullptr) {
        error = "no LLVM backend for target `" + triple + "`: " + lookup_error;
        return nullptr;
    }
    llvm::TargetOptions options;
    llvm::CodeGenOptLevel level = llvm::CodeGenOptLevel::None;
    switch (config.opt_level) {
    case 1: level = llvm::CodeGenOptLevel::Less; break;
    case 2: level = llvm::CodeGenOptLevel::Default; break;
    case 3: level = llvm::CodeGenOptLevel::Aggressive; break;
    default: break;
    }
    const std::string cpu = config.cpu.empty() ? "generic" : config.cpu;
    triple_out = triple;
    return std::unique_ptr<llvm::TargetMachine>(
        target->createTargetMachine(t, cpu, "", options, llvm::Reloc::PIC_, std::nullopt, level));
}

Artifact::Artifact(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Artifact::~Artifact() = default;
Artifact::Artifact(Artifact&&) noexcept = default;
Artifact& Artifact::operator=(Artifact&&) noexcept = default;

std::string Artifact::triple() const {
    return impl_->triple;
}

bool Artifact::hasEntry() const {
    return impl_->has_entry;
}

void Artifact::optimize(int opt_level) {
    if (opt_level <= 0) {
        return;
    }
    llvm::LoopAnalysisManager lam;
    llvm::FunctionAnalysisManager fam;
    llvm::CGSCCAnalysisManager cgam;
    llvm::ModuleAnalysisManager mam;
    llvm::PassBuilder pb(impl_->machine.get());
    pb.registerModuleAnalyses(mam);
    pb.registerCGSCCAnalyses(cgam);
    pb.registerFunctionAnalyses(fam);
    pb.registerLoopAnalyses(lam);
    pb.crossRegisterProxies(lam, fam, cgam, mam);
    llvm::OptimizationLevel level = llvm::OptimizationLevel::O1;
    if (opt_level == 2) {
        level = llvm::OptimizationLevel::O2;
    } else if (opt_level >= 3) {
        level = llvm::OptimizationLevel::O3;
    }
    llvm::ModulePassManager mpm = pb.buildPerModuleDefaultPipeline(level);
    mpm.run(*impl_->module, mam);
}

void Artifact::writeIR(const std::filesystem::path& path) const {
    std::error_code ec;
    llvm::raw_fd_ostream out(path.string(), ec, llvm::sys::fs::OF_Text);
    if (ec) {
        throw XlangError("cannot write `" + path.string() + "`: " + ec.message());
    }
    impl_->module->print(out, nullptr);
}

void Artifact::writeBitcode(const std::filesystem::path& path) const {
    std::error_code ec;
    llvm::raw_fd_ostream out(path.string(), ec, llvm::sys::fs::OF_None);
    if (ec) {
        throw XlangError("cannot write `" + path.string() + "`: " + ec.message());
    }
    llvm::WriteBitcodeToFile(*impl_->module, out);
}

void Artifact::writeObject(const std::filesystem::path& path) const {
    std::error_code ec;
    llvm::raw_fd_ostream out(path.string(), ec, llvm::sys::fs::OF_None);
    if (ec) {
        throw XlangError("cannot write `" + path.string() + "`: " + ec.message());
    }
    llvm::legacy::PassManager pm;
    if (impl_->machine->addPassesToEmitFile(pm, out, nullptr, llvm::CodeGenFileType::ObjectFile)) {
        throw XlangError("the target machine cannot emit object files");
    }
    pm.run(*impl_->module);
    out.flush();
}

}  // namespace xlang::codegen
