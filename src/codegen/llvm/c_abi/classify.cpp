// The C convention of a target and the dispatch to its classifier (see c_abi.hpp).

#include "codegen/llvm/c_abi/c_abi.hpp"

namespace xlang::codegen::c_abi {

/// @abi the C convention of a target follows the architecture first, the OS second: every arm64
/// @abi target uses AAPCS64, x86-64 uses Win64 on Windows and SysV elsewhere.
Convention conventionOf(std::string_view os, std::string_view arch) {
    if (arch == "aarch64" || arch == "arm64") {
        return Convention::Aapcs64; // Windows on Arm follows AAPCS64 for non-variadic calls
    }
    return os == "windows" ? Convention::Win64 : Convention::SysV;
}

Signature
classify(Convention c, llvm::LLVMContext& ctx, const Value& ret, const std::vector<Value>& params) {
    switch (c) {
    case Convention::Aapcs64:
        return classifyAapcs64(ctx, ret, params);
    case Convention::Win64:
        return classifyWin64(ctx, ret, params);
    case Convention::SysV:
        break;
    }
    return classifySysV(ctx, ret, params);
}

} // namespace xlang::codegen::c_abi
