#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::hasTerminator;
using gen_detail::isUnknownType;
using gen_detail::orderingOf;

// The scalar expression dispatch (`genScalar`) and the constant-expression evaluator.

llvm::Value* Gen::genScalar(const hir::Expr& e) {
    setLoc(e.span);
    switch (e.kind) {
    case hir::ExprKind::IntConst: {
        const Type& t = e.type.kind == TypeKind::Enum && e.type.elem ? *e.type.elem : e.type;
        if (t.kind == TypeKind::Float) {
            return llvm::ConstantFP::get(
                sty(t), static_cast<double>(static_cast<std::int64_t>(e.int_value)));
        }
        // `int_value` is the two's-complement pattern in 64 bits (-2 is 0xff..fe); the type keeps
        // its low bits, so mask before building the `APInt`: an unmasked value keeps the high bits
        // (a release LLVM only asserts, never clears), and an `i32 -2` compares above every normal
        // value: IPSCCP then merged a function's returns into a range that left out 0 and -1.
        auto *ity = llvm::cast<llvm::IntegerType>(sty(t));
        unsigned width = ity->getBitWidth();
        uint64_t pattern = width < 64 ? (e.int_value & ((1ull << width) - 1)) : e.int_value;
        return llvm::ConstantInt::get(ity->getContext(), llvm::APInt(width, pattern));
    }
    case hir::ExprKind::FloatConst:
        return llvm::ConstantFP::get(sty(e.type), e.float_value);
    case hir::ExprKind::BoolConst:
        return B_.getInt1(e.bool_value);
    case hir::ExprKind::StrConst:
        return stringGlobal(e.text);
    case hir::ExprKind::Nil:
        return llvm::Constant::getNullValue(ptrTy());
    case hir::ExprKind::FuncRef: {
        llvm::Function* f = functionNamed(e.text);
        if (f == nullptr) {
            error(e.span, std::format("unknown function `{}`", e.text));
            return llvm::Constant::getNullValue(ptrTy());
        }
        return f;
    }
    case hir::ExprKind::LocalRef:
    case hir::ExprKind::GlobalRef:
    case hir::ExprKind::RawRead:
    case hir::ExprKind::Deref:
    case hir::ExprKind::Field:
    case hir::ExprKind::Index:
        return loadScalar(e.type, genAddr(e));
    case hir::ExprKind::AddrOf: {
        if (e.lhs->kind == hir::ExprKind::GlobalRef || B_.GetInsertBlock() == nullptr) {
            // Constant context: fold through genConst.
            if (B_.GetInsertBlock() == nullptr) {
                return genConst(e);
            }
        }
        return genAddr(*e.lhs);
    }
    case hir::ExprKind::Unary: {
        llvm::Value* v = genScalar(*e.lhs);
        if (e.unary_op == UnaryOp::Not || e.unary_op == UnaryOp::BitNot) {
            return B_.CreateNot(v);
        }
        return e.type.kind == TypeKind::Float ? B_.CreateFNeg(v) : B_.CreateNeg(v);
    }
    case hir::ExprKind::Binary:
        return genBinary(e);
    case hir::ExprKind::Select: {
        llvm::Value* c = genScalar(*e.lhs);
        llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(ctx_, "sel.then", cur_fn_);
        llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(ctx_, "sel.else", cur_fn_);
        llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx_, "sel.end", cur_fn_);
        B_.CreateCondBr(c, then_bb, else_bb);
        B_.SetInsertPoint(then_bb);
        llvm::Value* a = genScalar(*e.rhs);
        llvm::BasicBlock* then_end = B_.GetInsertBlock();
        B_.CreateBr(end_bb);
        B_.SetInsertPoint(else_bb);
        llvm::Value* b = genScalar(*e.args[0]);
        llvm::BasicBlock* else_end = B_.GetInsertBlock();
        B_.CreateBr(end_bb);
        B_.SetInsertPoint(end_bb);
        llvm::PHINode* phi = B_.CreatePHI(a->getType(), 2);
        phi->addIncoming(a, then_end);
        phi->addIncoming(b, else_end);
        return phi;
    }
    case hir::ExprKind::Cast:
        return genCast(e);
    case hir::ExprKind::New:
        return genNew(e);
    case hir::ExprKind::Call:
    case hir::ExprKind::CallIndirect:
    case hir::ExprKind::CallVirtual:
    case hir::ExprKind::CallItable:
    case hir::ExprKind::Syscall: {
        llvm::Value* mem = nullptr;
        return genCall(e, &mem);
    }
    case hir::ExprKind::AtomicLoad:
    case hir::ExprKind::AtomicStore:
    case hir::ExprKind::AtomicXchg:
    case hir::ExprKind::AtomicCas:
    case hir::ExprKind::AtomicAdd:
        return genAtomic(e);
    case hir::ExprKind::Fence:
        B_.CreateFence(orderingOf(e.index));
        return nullptr;
    case hir::ExprKind::Asm:
        return genAsm(e);
    case hir::ExprKind::UnwindPads:
        return B_.CreateLoad(llvm::PointerType::getUnqual(ctx_), pads_ptr_);
    case hir::ExprKind::UnwindPadCount:
        return B_.CreateLoad(DL_.getIntPtrType(ctx_), pads_count_);
    case hir::ExprKind::External:
    case hir::ExprKind::ExternalCall:
        error(e.span, std::format("unresolved external reference `{}.{}`", e.text, e.member));
        return llvm::UndefValue::get(isUnknownType(e.type) ? llvm::Type::getInt32Ty(ctx_)
                                                           : sty(e.type));
    default:
        error(e.span, "unsupported expression in code generation");
        return llvm::UndefValue::get(llvm::Type::getInt32Ty(ctx_));
    }
}

} // namespace xlang::codegen
