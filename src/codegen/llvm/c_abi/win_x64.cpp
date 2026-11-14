// Microsoft x64: https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention
//
// "Structs and unions of size 8, 16, 32, or 64 bits ... are passed as if they were integers of the
// same size. Structs or unions of other sizes are passed as a pointer to memory allocated by the
// caller." Floats inside such a struct still travel in the integer register. A result of 1, 2, 4
// or 8 bytes comes back in RAX; any other struct is written through a hidden pointer the caller
// passes first (RCX), and the callee returns that pointer in RAX (LLVM's `sret` does both).

#include "codegen/llvm/c_abi/c_abi.hpp"

#include <llvm/IR/DerivedTypes.h>

namespace xlang::codegen::c_abi {

namespace {

[[nodiscard]] Arg classifyOne(llvm::LLVMContext& ctx, const Value& v) {
    if (!v.aggregate) {
        return {};
    }
    if (v.size == 0) {
        return {Pass::Ignore};
    }
    if (v.size == 1 || v.size == 2 || v.size == 4 || v.size == 8) {
        return {Pass::Direct, llvm::Type::getIntNTy(ctx, static_cast<unsigned>(v.size * 8))};
    }
    return {Pass::Indirect};
}

} // namespace

/// @abi Microsoft x64: an aggregate of 1, 2, 4 or 8 bytes is passed as an integer of that size,
/// @abi every other aggregate goes by hidden reference; four register slots, then the stack.
/// @spec https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention
Signature
classifyWin64(llvm::LLVMContext& ctx, const Value& ret, const std::vector<Value>& params) {
    Signature s;
    s.ret = ret.is_void ? Arg{} : classifyOne(ctx, ret);
    for (const Value& p : params) {
        s.params.push_back(classifyOne(ctx, p));
    }
    return s;
}

} // namespace xlang::codegen::c_abi
