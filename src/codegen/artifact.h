#pragma once

#include "xlang/codegen/codegen.h"

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>

#include <memory>

namespace xlang::codegen {

struct Artifact::Impl {
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::unique_ptr<llvm::TargetMachine> machine;
    bool has_entry{false};
    std::string triple;
};

/// Registers the LLVM targets once and creates a machine for `triple` (empty = host).
[[nodiscard]] std::unique_ptr<llvm::TargetMachine> createMachine(const BuildConfig& config, std::string& triple,
                                                                 std::string& error);

}  // namespace xlang::codegen
