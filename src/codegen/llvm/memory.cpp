#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::hasTerminator;
using gen_detail::isUnknownType;

// Memory helpers: allocas, scalar load/store, aggregate copy and zero, field and byte
// addresses, string constants, and the `panic`/bounds-check calls.

// ---------------------------------------------------------------------------------------------
// Helpers

llvm::AllocaInst* Gen::makeAlloca(llvm::Type* ty, std::uint64_t align, const std::string& name) {
    llvm::BasicBlock& entry = cur_fn_->getEntryBlock();
    AB_.SetInsertPoint(&entry, entry.begin());
    AB_.SetCurrentDebugLocation(llvm::DebugLoc()); // never inherit another instruction's location
    llvm::AllocaInst* a = AB_.CreateAlloca(ty, nullptr, name);
    if (align != 0) {
        a->setAlignment(llvm::Align(align));
    }
    if (escape_) {
        escapeLocal(a);
    }
    return a;
}

llvm::AllocaInst* Gen::makeTemp(const Type& t) {
    if (isMem(t)) {
        const auto [size, align] = sizeAlign(t);
        return makeAlloca(
            llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_), std::max<std::uint64_t>(size, 1)),
            align,
            "tmp");
    }
    llvm::Type* m = mty(t);
    return makeAlloca(m, DL_.getABITypeAlign(m).value(), "tmp");
}

llvm::Value* Gen::loadScalar(const Type& t, llvm::Value* addr) {
    llvm::Type* m = mty(t);
    llvm::LoadInst* l = B_.CreateAlignedLoad(m, addr, llvm::Align(DL_.getABITypeAlign(m).value()));
    if (sty(t)->isIntegerTy(1)) {
        return B_.CreateICmpNE(l, llvm::ConstantInt::get(m, 0));
    }
    return l;
}

void Gen::storeScalar(const Type& t, llvm::Value* value, llvm::Value* addr) {
    llvm::Type* m = mty(t);
    if (sty(t)->isIntegerTy(1)) {
        value = B_.CreateZExt(value, m);
    }
    B_.CreateAlignedStore(value, addr, llvm::Align(DL_.getABITypeAlign(m).value()));
}

void Gen::copyMem(llvm::Value* dest, llvm::Value* src, const Type& t) {
    const auto [size, align] = sizeAlign(t);
    if (size == 0) {
        return;
    }
    B_.CreateMemCpy(dest, llvm::Align(align), src, llvm::Align(align), size);
}

void Gen::zeroMem(llvm::Value* dest, std::uint64_t size, std::uint64_t align) {
    if (size == 0) {
        return;
    }
    B_.CreateMemSet(dest, B_.getInt8(0), size, llvm::Align(std::max<std::uint64_t>(align, 1)));
}

llvm::Value* Gen::byteOffset(llvm::Value* base, std::uint64_t offset) {
    if (offset == 0) {
        return base;
    }
    return B_.CreateInBoundsGEP(llvm::Type::getInt8Ty(ctx_), base, {B_.getInt64(offset)});
}

llvm::Value* Gen::fieldAddr(llvm::Value* base, const Type& agg, std::uint32_t index) {
    const TypeLayout* l = layoutOf(agg);
    if (l == nullptr || index >= l->fields.size()) {
        error(Span{}, std::format("no layout for `{}`", typeToString(agg)));
        return base;
    }
    return byteOffset(base, l->fields[index].offset);
}

llvm::GlobalVariable* Gen::stringGlobal(const std::string& text) {
    if (const auto it = strings_.find(text); it != strings_.end()) {
        return it->second;
    }
    llvm::Constant* data = llvm::ConstantDataArray::getString(ctx_, text, true);
    auto* g = new llvm::GlobalVariable(
        mod_, data->getType(), true, llvm::GlobalValue::PrivateLinkage, data, ".str");
    g->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
    g->setAlignment(llvm::Align(1));
    return strings_[text] = g;
}

llvm::Function* Gen::functionNamed(const std::string& name) {
    const auto it = fns_.find(name);
    return it == fns_.end() ? nullptr : it->second;
}

const hir::Interface* Gen::interfaceNamed(const std::string& name) const {
    const auto it = interfaces_.find(name);
    return it == interfaces_.end() ? nullptr : it->second;
}

const hir::Aggregate* Gen::aggregateNamed(const std::string& name) const {
    const auto it = aggregates_.find(name);
    return it == aggregates_.end() ? nullptr : it->second;
}

void Gen::panicCall(const char* message) {
    llvm::Function* panic =
        hm_.panic_function.empty() ? nullptr : functionNamed(hm_.panic_function);
    if (panic != nullptr) {
        B_.CreateCall(panic, {stringGlobal(message)});
    } else {
        B_.CreateCall(llvm::Intrinsic::getOrInsertDeclaration(&mod_, llvm::Intrinsic::trap));
    }
    B_.CreateUnreachable();
}

/// `if (bad) panic(message)`; emission continues after the check.
void Gen::checkedBranch(llvm::Value* bad, const char* message) {
    llvm::BasicBlock* fail = llvm::BasicBlock::Create(ctx_, "panic", cur_fn_);
    llvm::BasicBlock* ok = llvm::BasicBlock::Create(ctx_, "ok", cur_fn_);
    B_.CreateCondBr(bad, fail, ok);
    B_.SetInsertPoint(fail);
    panicCall(message);
    B_.SetInsertPoint(ok);
}

void Gen::ensureTerminated() {
    llvm::BasicBlock* bb = B_.GetInsertBlock();
    if (bb == nullptr || hasTerminator(bb)) {
        return;
    }
    llvm::Type* rt = cur_fn_->getReturnType();
    if (rt->isVoidTy()) {
        B_.CreateRetVoid();
    } else {
        B_.CreateRet(llvm::Constant::getNullValue(rt));
    }
}

} // namespace xlang::codegen
