#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::hasTerminator;
using gen_detail::isUnknownType;

// Unwinding (landing pads and `try`): see xlang-docs/language/index.md "Panics and `try`".

// ---------------------------------------------------------------------------------------------
// Unwinding: landing pads and `try`

/// Forces `slot` to live in memory: the pad and the `catch` block have no CFG predecessors, so
/// everything they read must be in memory when a callee panics.
void Gen::escapeLocal(llvm::Value* slot) {
    llvm::FunctionType* fty = llvm::FunctionType::get(B_.getVoidTy(), {slot->getType()}, false);
    llvm::InlineAsm* barrier = llvm::InlineAsm::get(fty, "", "r,~{memory}", true, false);
    B_.CreateCall(barrier, {slot});
}

void Gen::callUnwind(int op, llvm::Value* snap, llvm::Value* msg) {
    llvm::Function* unwind = functionNamed(hm_.unwind_function);
    llvm::PointerType* ptr = llvm::PointerType::getUnqual(ctx_);
    B_.CreateCall(unwind,
                  {B_.getInt32(static_cast<std::uint32_t>(op)),
                   snap != nullptr ? snap : llvm::ConstantPointerNull::get(ptr),
                   msg != nullptr ? msg : llvm::ConstantPointerNull::get(ptr)});
}

/// Fills `buf[1..]` with the state the handler needs: sp, fp and the callee-saved registers.
void Gen::emitSnapshot(llvm::Value* buf) {
    const TargetInfo& t = in_.config.target;
    std::string text;
    std::string constraints = "r,~{memory}";
    bool intel = false;
    if (t.arch == "x86_64" && t.os != "windows") {
        intel = true;
        text = "mov qword ptr [$0 + 8], rsp\n\tmov qword ptr [$0 + 16], rbp\n\tmov qword ptr [$0 + "
               "24], rbx\n\t"
               "mov qword ptr [$0 + 32], r12\n\tmov qword ptr [$0 + 40], r13\n\tmov qword ptr [$0 "
               "+ 48], r14\n\t"
               "mov qword ptr [$0 + 56], r15";
    } else if (t.arch == "aarch64" && t.os != "windows") {
        constraints = "r,~{x9},~{memory}";
        text = "mov x9, sp\n\tstp x9, x29, [$0, #8]\n\tstp x19, x20, [$0, #24]\n\tstp x21, x22, "
               "[$0, #40]\n\t"
               "stp x23, x24, [$0, #56]\n\tstp x25, x26, [$0, #72]\n\tstp x27, x28, [$0, #88]\n\t"
               "stp d8, d9, [$0, #104]\n\tstp d10, d11, [$0, #120]\n\tstp d12, d13, [$0, "
               "#136]\n\tstp d14, d15, [$0, #152]";
    } else {
        error(Span{}, std::format("`try` is not supported on target `{}/{}`", t.os, t.arch));
        return;
    }
    llvm::FunctionType* fty = llvm::FunctionType::get(B_.getVoidTy(), {buf->getType()}, false);
    llvm::InlineAsm* ia =
        llvm::InlineAsm::get(fty,
                             text,
                             constraints,
                             true,
                             false,
                             intel ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT);
    B_.CreateCall(ia, {buf});
}

void Gen::genTry(const hir::Stmt& s) {
    llvm::Value* snap = locals_[s.local];
    llvm::Value* msg = locals_[s.msg_local];
    llvm::BasicBlock* handler = llvm::BasicBlock::Create(ctx_, "try.catch", cur_fn_);
    llvm::BasicBlock* end = llvm::BasicBlock::Create(ctx_, "try.end", cur_fn_);
    B_.CreateAlignedStore(
        llvm::BlockAddress::get(cur_fn_, handler), snap, llvm::Align(ptrBits() / 8));
    emitSnapshot(snap);
    callUnwind(1, snap, msg);
    // The handler is entered by a jump the optimizer cannot see. An opaque, never-true test leading
    // to an `indirectbr` gives it a CFG predecessor, so the block (and the address stored above)
    // survives; everything the handler reads is in memory, behind the unknown `unwind` call.
    llvm::BasicBlock* dispatch = llvm::BasicBlock::Create(ctx_, "try.dispatch", cur_fn_);
    llvm::BasicBlock* body = llvm::BasicBlock::Create(ctx_, "try.body", cur_fn_);
    // The test reads snapshot word 31, which nothing ever writes (volatile: never folded).
    llvm::Value* word = B_.CreateConstInBoundsGEP1_64(B_.getInt8Ty(), snap, 31 * (ptrBits() / 8));
    llvm::LoadInst* never =
        B_.CreateAlignedLoad(B_.getIntNTy(ptrBits()), word, llvm::Align(ptrBits() / 8), true);
    B_.CreateCondBr(B_.CreateIsNotNull(never), dispatch, body);
    B_.SetInsertPoint(dispatch);
    llvm::IndirectBrInst* jump = B_.CreateIndirectBr(llvm::BlockAddress::get(cur_fn_, handler), 1);
    jump->addDestination(handler);
    B_.SetInsertPoint(body);
    genStmts(s.body);
    if (!hasTerminator(B_.GetInsertBlock())) {
        B_.CreateBr(end);
    }
    B_.SetInsertPoint(handler);
    genStmts(s.else_body);
    if (!hasTerminator(B_.GetInsertBlock())) {
        B_.CreateBr(end);
    }
    B_.SetInsertPoint(end);
}

/// Emits the landing pad of the current function (if it has cleanups) and records it in the table.
void Gen::emitPad(const hir::Function& f) {
    if (pad_ != nullptr) {
        B_.SetInsertPoint(pad_);
        genStmts(f.unwind_body);
        callUnwind(2, nullptr, nullptr);
        B_.CreateUnreachable();
    }
    pads_.push_back({cur_fn_, pad_});
    pad_ = nullptr;
}

void Gen::finishPadTable() {
    llvm::PointerType* ptr = llvm::PointerType::getUnqual(ctx_);
    llvm::StructType* entry_t = llvm::StructType::get(ctx_, {ptr, ptr});
    std::vector<llvm::Constant*> entries;
    for (const PadEntry& p : pads_) {
        entries.push_back(llvm::ConstantStruct::get(
            entry_t,
            {p.fn,
             p.pad != nullptr ? static_cast<llvm::Constant*>(llvm::BlockAddress::get(p.fn, p.pad))
                              : llvm::ConstantPointerNull::get(ptr)}));
    }
    // Replace the placeholder table by one of the final size.
    llvm::ArrayType* table_t = llvm::ArrayType::get(entry_t, entries.size());
    auto* table = new llvm::GlobalVariable(mod_,
                                           table_t,
                                           true,
                                           llvm::GlobalValue::InternalLinkage,
                                           llvm::ConstantArray::get(table_t, entries),
                                           "_xl_unwindTable");
    pads_ptr_->setInitializer(table);
    pads_count_->setInitializer(llvm::ConstantInt::get(DL_.getIntPtrType(ctx_), entries.size()));
}

} // namespace xlang::codegen
