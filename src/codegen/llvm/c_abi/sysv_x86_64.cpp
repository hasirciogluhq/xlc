// System V AMD64 psABI 3.2.3 "Parameter Passing": https://gitlab.com/x86-psABIs/x86-64-ABI
// (Linux and macOS on x86-64; Apple uses it unchanged:
// https://developer.apple.com/documentation/xcode/writing-64-bit-intel-code-for-apple-platforms)
//
//   - A struct larger than 16 bytes, or one with a member off its natural alignment, is class
//     MEMORY: the call copies it onto the stack (`byval`, 8-byte aligned at least).
//   - Otherwise each eightbyte is classified: INTEGER when any member in it is an integer or a
//     pointer, else SSE (only `f32`/`f64` members). An INTEGER eightbyte takes the next of the six
//     general registers (rdi, rsi, rdx, rcx, r8, r9), an SSE one the next of xmm0..xmm7.
//   - "If there are no registers available for any eightbyte of an argument, the whole argument is
//     passed on the stack": a struct that does not fit in the registers that are left becomes
//     MEMORY too (registers already given to earlier arguments stay given).
//   - A result: INTEGER eightbytes in rax/rdx, SSE ones in xmm0/xmm1; a MEMORY result is written
//     through the address the caller passes in rdi (`sret`, which then costs a general register).
// Clang's lowering is the reference for the exact LLVM types (clang/lib/CodeGen/Targets/X86.cpp):
// an INTEGER eightbyte is the smallest integer covering its bytes, an SSE one `double`, `float` or
// `<2 x float>`; two eightbytes are a `{lo, hi}` struct.

#include "codegen/llvm/c_abi/c_abi.hpp"

#include <llvm/IR/DerivedTypes.h>

#include <algorithm>
#include <array>

namespace xlang::codegen::c_abi {

namespace {

enum class Class { None, Integer, Sse };

struct Eightbytes {
    std::array<Class, 2> cls{Class::None, Class::None};
    std::array<llvm::Type*, 2> type{nullptr, nullptr};
    unsigned ints{0};
    unsigned sses{0};
    [[nodiscard]] unsigned count() const { return cls[1] == Class::None ? 1 : 2; }
};

/// The SSE type of eightbyte `i`: `double`, `float` or `<2 x float>`.
[[nodiscard]] llvm::Type* sseType(llvm::LLVMContext& ctx, const Value& v, unsigned i) {
    const std::uint64_t base = std::uint64_t{i} * 8;
    const auto floatAt = [&](std::uint64_t at) {
        return std::ranges::any_of(v.pieces, [&](const Piece& p) { return p.offset == at && p.size == 4; });
    };
    if (floatAt(base) && floatAt(base + 4)) {
        return llvm::FixedVectorType::get(llvm::Type::getFloatTy(ctx), 2);
    }
    if (floatAt(base) && v.size - base <= 4) {
        return llvm::Type::getFloatTy(ctx);
    }
    return llvm::Type::getDoubleTy(ctx);
}

[[nodiscard]] Eightbytes eightbytes(llvm::LLVMContext& ctx, const Value& v) {
    Eightbytes e;
    for (const Piece& p : v.pieces) {
        Class& c = e.cls[p.offset / 8];
        c = !p.is_float || c == Class::Integer ? Class::Integer : Class::Sse;
    }
    for (const unsigned i : {0U, 1U}) {
        if (e.cls[i] == Class::Integer) {
            const std::uint64_t bytes = std::min<std::uint64_t>(8, v.size - std::uint64_t{i} * 8);
            e.type[i] = llvm::Type::getIntNTy(ctx, static_cast<unsigned>(bytes * 8));
            ++e.ints;
        } else if (e.cls[i] == Class::Sse) {
            e.type[i] = sseType(ctx, v, i);
            ++e.sses;
        }
    }
    return e;
}

[[nodiscard]] llvm::Type* coerceOf(llvm::LLVMContext& ctx, const Eightbytes& e) {
    return e.count() == 1 ? e.type[0] : llvm::StructType::get(ctx, {e.type[0], e.type[1]});
}

[[nodiscard]] bool memoryClass(const Value& v) {
    return v.size > 16 || v.misaligned || v.pieces.empty();
}

} // namespace

/// @abi System V AMD64 3.2.3: INTEGER and SSE eightbytes go in registers, MEMORY on the stack
/// @abi (6 integer and 8 SSE register slots); an aggregate over 16 bytes is always MEMORY.
/// @spec https://gitlab.com/x86-psABIs/x86-64-ABI
Signature classifySysV(llvm::LLVMContext& ctx, const Value& ret, const std::vector<Value>& params) {
    Signature s;
    unsigned free_ints = 6;
    unsigned free_sses = 8;
    if (!ret.is_void && ret.aggregate) {
        if (ret.size == 0) {
            s.ret = {Pass::Ignore};
        } else if (memoryClass(ret)) {
            s.ret = {Pass::Indirect};
            --free_ints; // the result address takes rdi
        } else {
            s.ret = {Pass::Direct, coerceOf(ctx, eightbytes(ctx, ret))};
        }
    }
    for (const Value& p : params) {
        if (!p.aggregate) {
            unsigned& free = !p.pieces.empty() && p.pieces.front().is_float ? free_sses : free_ints;
            free -= free > 0 ? 1 : 0;
            s.params.push_back({});
            continue;
        }
        if (p.size == 0) {
            s.params.push_back({Pass::Ignore});
            continue;
        }
        const Eightbytes e = eightbytes(ctx, p);
        if (memoryClass(p) || e.ints > free_ints || e.sses > free_sses) {
            s.params.push_back({Pass::ByVal, nullptr, std::max<std::uint64_t>(p.align, 8)});
            continue;
        }
        free_ints -= e.ints;
        free_sses -= e.sses;
        s.params.push_back({Pass::Direct, coerceOf(ctx, e)});
    }
    return s;
}

} // namespace xlang::codegen::c_abi
