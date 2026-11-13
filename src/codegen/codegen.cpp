// Plan 4.7: HIR -> LLVM IR. The pass itself is split by topic in `src/codegen/llvm/`; this file is
// the driver: it builds the LLVM context, module, target machine and data layout for one input and
// hands them to `Gen::run` (see `codegen/llvm/gen.hpp`).
//
// Value model: scalars (integers, floats, bool, pointers, function pointers, enums over scalars)
// are SSA values; aggregates (struct, class, array, interface fat pointers) live in memory and
// expressions of those types evaluate to the *address* of the object. Passing and returning
// aggregates is by pointer (callee gets its own copy / an sret slot), except for `export(c)` and
// `declare` functions, which follow the target's C convention (`codegen/llvm/c_abi/`).

#include "artifact.h"

#include "codegen/llvm/gen.hpp"

#include "xlang/util/error.h"

#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>

#include <string>

namespace xlang::codegen {

bool generateModule(const CodegenInput& input, Artifact::Impl& impl) {
    Gen gen(input, impl);
    return gen.run();
}

std::unique_ptr<Artifact> generate(const CodegenInput& input) {
    auto impl = std::make_unique<Artifact::Impl>();
    std::string error;
    impl->machine = createMachine(input.config, impl->triple, error);
    if (!impl->machine) {
        input.diagnostics.error("E0714", Span{}, error, "code generation");
        return nullptr;
    }
    impl->context = std::make_unique<llvm::LLVMContext>();
    impl->module = std::make_unique<llvm::Module>(input.module_name, *impl->context);
    impl->module->setTargetTriple(llvm::Triple(impl->triple));
    impl->module->setDataLayout(impl->machine->createDataLayout());
    if (!generateModule(input, *impl)) {
        return nullptr;
    }
    return std::make_unique<Artifact>(std::move(impl));
}

}  // namespace xlang::codegen
