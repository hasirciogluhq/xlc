// Arm AAPCS64 6.8: https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
// Apple arm64 follows it for structs (its deviations are variadic calls and the packing of stack
// arguments): https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms
// Windows on Arm follows it for non-variadic calls:
// https://learn.microsoft.com/en-us/cpp/build/arm64-windows-abi-conventions
//
//   - A Homogeneous Floating-point Aggregate (1 to 4 members, all `f32` or all `f64`) goes in
//     consecutive SIMD registers (v0..v7). As `[N x float]` LLVM allocates all of them or, when too
//     few are left, puts the whole value on the stack (C.3, C.4).
//   - Any other struct of at most 16 bytes goes in one or two general registers, rounded up to a
//     multiple of 8 (B.4); `[2 x i64]` again takes both registers or none (C.10 to C.13).
//   - A larger struct is copied by the caller and its address is passed instead (B.4).
//   - A result: an HFA in v0..v3, a struct of at most 16 bytes in x0/x1, anything larger through the
//     address in x8 (`sret`).
// Clang's lowering is the reference for the exact LLVM types (clang/lib/CodeGen/Targets/AArch64.cpp).

#include "codegen/llvm/c_abi/c_abi.hpp"

#include <llvm/IR/DerivedTypes.h>

#include <algorithm>

namespace xlang::codegen::c_abi {

namespace {

/// `[N x float]` / `[N x double]` when `v` is an HFA, else null.
[[nodiscard]] llvm::Type* hfa(llvm::LLVMContext& ctx, const Value& v) {
    const auto& p = v.pieces;
    if (p.empty() || p.size() > 4 || !p.front().is_float ||
        !std::ranges::all_of(p, [&](const Piece& x) { return x.is_float && x.size == p.front().size; }) ||
        p.size() * p.front().size != v.size) {
        return nullptr;
    }
    llvm::Type* elem = p.front().size == 4 ? llvm::Type::getFloatTy(ctx) : llvm::Type::getDoubleTy(ctx);
    return llvm::ArrayType::get(elem, p.size());
}

[[nodiscard]] Arg classifyOne(llvm::LLVMContext& ctx, const Value& v, bool result) {
    if (!v.aggregate) {
        return {};
    }
    if (v.size == 0) {
        return {Pass::Ignore};
    }
    if (llvm::Type* h = hfa(ctx, v)) {
        return {Pass::Direct, h};
    }
    if (v.size > 16) {
        return {Pass::Indirect};
    }
    if (v.align >= 16) {
        return {Pass::Direct, llvm::Type::getInt128Ty(ctx)};
    }
    if (v.size <= 8) {
        // A result is the low bytes of x0; an argument fills the whole register.
        return {Pass::Direct, llvm::Type::getIntNTy(ctx, result ? static_cast<unsigned>(v.size * 8) : 64)};
    }
    return {Pass::Direct, llvm::ArrayType::get(llvm::Type::getInt64Ty(ctx), 2)};
}

} // namespace

Signature
classifyAapcs64(llvm::LLVMContext& ctx, const Value& ret, const std::vector<Value>& params) {
    Signature s;
    s.ret = ret.is_void ? Arg{} : classifyOne(ctx, ret, true);
    for (const Value& p : params) {
        s.params.push_back(classifyOne(ctx, p, false));
    }
    return s;
}

} // namespace xlang::codegen::c_abi
