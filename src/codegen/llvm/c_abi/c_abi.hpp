// The C calling convention of one target, for the functions that cross into C: `export(c)`
// definitions and `declare` imports. Everything else uses the xlang convention, where an aggregate
// travels as the address of a copy (see `codegen/codegen.cpp`).
//
// The classifier only sees a flattened shape (size, alignment and the scalar pieces at their
// offsets), so it is independent of the rest of the backend and of the HIR. It answers, for the
// result and each parameter, how the value is passed; `signature.cpp` turns that into the LLVM
// function type, its attributes, the callee's prologue and return and the caller's argument setup.
//
// One file per convention:
//   - sysv_x86_64.cpp  System V AMD64 psABI 3.2.3 (Linux, macOS on x86-64)
//                      https://gitlab.com/x86-psABIs/x86-64-ABI
//   - aapcs64.cpp      Arm AAPCS64 6.8 (Linux, macOS and Windows on arm64)
//                      https://github.com/ARM-software/abi-aa/blob/main/aapcs64/aapcs64.rst
//   - win_x64.cpp      Microsoft x64 calling convention (Windows on x86-64)
//                      https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention
#pragma once

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Type.h>

#include <cstdint>
#include <string_view>
#include <vector>

namespace xlang::codegen::c_abi {

/// One scalar member of a flattened struct: nested structs and arrays are expanded into these.
struct Piece {
    std::uint64_t offset{0};
    std::uint64_t size{0};
    bool is_float{false};
};

/// A parameter or result as the classifier sees it. A scalar is one piece at offset 0.
struct Value {
    bool aggregate{false}; ///< a `@repr(c)` struct (it lives in memory on the xlang side)
    bool is_void{false};   ///< the result of a function that returns nothing
    std::uint64_t size{0};
    std::uint64_t align{1};
    bool misaligned{false}; ///< a packed struct with a member off its natural alignment
    std::vector<Piece> pieces;
};

enum class Pass {
    Direct,   ///< in registers: the scalar itself, or the struct reinterpreted as `coerce`
    Indirect, ///< the address of a copy the caller made (AAPCS64 and Win64 large structs; a result: `sret`)
    ByVal,    ///< copied onto the stack by the call (`byval`, the SysV MEMORY class)
    Ignore,   ///< an empty struct: nothing is passed
};

struct Arg {
    Pass pass{Pass::Direct};
    llvm::Type* coerce{nullptr}; ///< Direct aggregate: the register image (null for a scalar)
    std::uint64_t align{0};      ///< ByVal: the stack slot alignment
};

struct Signature {
    Arg ret;
    std::vector<Arg> params;
};

enum class Convention { SysV, Aapcs64, Win64 };

/// The convention of a target (`os` and `arch` as in `TargetInfo`).
[[nodiscard]] Convention conventionOf(std::string_view os, std::string_view arch);

[[nodiscard]] Signature
classify(Convention c, llvm::LLVMContext& ctx, const Value& ret, const std::vector<Value>& params);

// One per convention (the files listed above).
[[nodiscard]] Signature
classifySysV(llvm::LLVMContext& ctx, const Value& ret, const std::vector<Value>& params);
[[nodiscard]] Signature
classifyAapcs64(llvm::LLVMContext& ctx, const Value& ret, const std::vector<Value>& params);
[[nodiscard]] Signature
classifyWin64(llvm::LLVMContext& ctx, const Value& ret, const std::vector<Value>& params);

} // namespace xlang::codegen::c_abi
