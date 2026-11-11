#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::hasTerminator;
using gen_detail::isUnknownType;

// Expressions: addresses, memory results, scalar values, casts, binary operators, calls,
// `new` and the `syscall` form.

// ---------------------------------------------------------------------------------------------
// Expressions

llvm::Value* Gen::genAddr(const hir::Expr& e) {
    setLoc(e.span);
    switch (e.kind) {
    case hir::ExprKind::LocalRef:
        return locals_[e.local];
    case hir::ExprKind::GlobalRef: {
        const auto it = globals_.find(e.text);
        if (it == globals_.end()) {
            error(e.span, std::format("unknown global `{}`", e.text));
            return llvm::Constant::getNullValue(ptrTy());
        }
        return it->second;
    }
    case hir::ExprKind::RawRead:
    case hir::ExprKind::Deref:
        return genScalar(*e.lhs);
    case hir::ExprKind::Field: {
        llvm::Value* base = e.through_pointer ? genScalar(*e.lhs) : genAddr(*e.lhs);
        const Type agg =
            e.through_pointer && e.lhs->type.elem != nullptr ? *e.lhs->type.elem : e.lhs->type;
        return fieldAddr(base, agg, e.index);
    }
    case hir::ExprKind::Index: {
        const bool is_array = e.lhs->type.kind == TypeKind::Array;
        const bool is_str = e.lhs->type.kind == TypeKind::Str;
        const bool is_slice = e.lhs->type.kind == TypeKind::Slice;
        llvm::Value* base = is_array || is_str || is_slice ? genAddr(*e.lhs) : genScalar(*e.lhs);
        llvm::Value* idx = genScalar(*e.rhs);
        const bool idx_signed = isSigned(e.rhs->type);
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx_);
        if (idx->getType()->getIntegerBitWidth() != 64) {
            idx = idx_signed ? B_.CreateSExt(idx, i64) : B_.CreateZExt(idx, i64);
        }
        if (is_str || is_slice) {
            // `{ ptr, len }`: check the index against len, then index the bytes at ptr.
            llvm::Value* len = B_.CreateZExt(B_.CreateAlignedLoad(sty(Type::makeInt(0, false)),
                                                                  byteOffset(base, ptrBits() / 8),
                                                                  llvm::Align(ptrBits() / 8)),
                                             i64);
            if (e.checked) {
                checkedBranch(B_.CreateICmpUGE(idx, len),
                              is_str ? "str index out of bounds" : "slice index out of bounds");
            }
            llvm::Value* data = B_.CreateAlignedLoad(ptrTy(), base, llvm::Align(ptrBits() / 8));
            if (is_str) {
                return B_.CreateInBoundsGEP(llvm::Type::getInt8Ty(ctx_), data, {idx});
            }
            // A slice steps by `sizeof(elem)`, not one byte.
            const auto elem_size = sizeAlign(*e.lhs->type.elem).first;
            llvm::Value* off = B_.CreateMul(idx, B_.getInt64(elem_size));
            return B_.CreateInBoundsGEP(llvm::Type::getInt8Ty(ctx_), data, {off});
        }
        if (is_array && e.checked) {
            const TypeLayout* l = layoutOf(e.lhs->type);
            const auto elem_size = sizeAlign(*e.lhs->type.elem).first;
            const std::uint64_t len = l != nullptr && elem_size != 0 ? l->size / elem_size : 0;
            checkedBranch(B_.CreateICmpUGE(idx, B_.getInt64(len)), "array index out of bounds");
        }
        const auto elem_size = sizeAlign(*e.lhs->type.elem).first;
        llvm::Value* off = B_.CreateMul(idx, B_.getInt64(elem_size));
        return B_.CreateInBoundsGEP(llvm::Type::getInt8Ty(ctx_), base, {off});
    }
    default:
        // An rvalue used as a place: materialize it.
        if (isMem(e.type)) {
            return genMem(e);
        }
        llvm::Value* v = genScalar(e);
        llvm::AllocaInst* tmp = makeTemp(e.type);
        storeScalar(e.type, v, tmp);
        return tmp;
    }
}

void Gen::genInto(const hir::Expr& e, llvm::Value* dest) {
    setLoc(e.span);
    switch (e.kind) {
    case hir::ExprKind::StructLit: {
        const TypeLayout* l = layoutOf(e.type);
        if (l == nullptr) {
            error(e.span, std::format("no layout for `{}`", typeToString(e.type)));
            return;
        }
        zeroMem(dest, l->size, l->align);
        if (l->has_vptr) {
            if (llvm::Constant* vp = vtablePointer(e.type.path.front())) {
                B_.CreateAlignedStore(vp, dest, llvm::Align(ptrBits() / 8));
            }
        }
        for (const hir::FieldInit& f : e.fields) {
            llvm::Value* addr = byteOffset(dest, l->fields[f.index].offset);
            if (isMem(f.value->type)) {
                genInto(*f.value, addr);
            } else {
                storeScalar(f.value->type, genScalar(*f.value), addr);
            }
        }
        return;
    }
    case hir::ExprKind::ArrayList: {
        const auto [esize, ealign] = sizeAlign(*e.type.elem);
        (void)ealign;
        for (std::size_t i = 0; i < e.args.size(); ++i) {
            llvm::Value* addr = byteOffset(dest, esize * i);
            if (isMem(e.args[i]->type)) {
                genInto(*e.args[i], addr);
            } else {
                storeScalar(e.args[i]->type, genScalar(*e.args[i]), addr);
            }
        }
        return;
    }
    case hir::ExprKind::TupleLit: {
        // A tuple is laid out like `@repr(c)`: element `i` sits at the i-th field offset.
        const TypeLayout* l = layoutOf(e.type);
        if (l == nullptr) {
            error(e.span, std::format("no layout for `{}`", typeToString(e.type)));
            return;
        }
        zeroMem(dest, l->size, l->align);
        for (std::size_t i = 0; i < e.args.size(); ++i) {
            llvm::Value* addr = byteOffset(dest, l->fields[i].offset);
            if (isMem(e.args[i]->type)) {
                genInto(*e.args[i], addr);
            } else {
                storeScalar(e.args[i]->type, genScalar(*e.args[i]), addr);
            }
        }
        return;
    }
    case hir::ExprKind::ArrayRepeat: {
        const auto [esize, ealign] = sizeAlign(*e.type.elem);
        (void)ealign;
        for (std::uint64_t i = 0; i < e.int_value; ++i) {
            llvm::Value* addr = byteOffset(dest, esize * i);
            if (isMem(e.lhs->type)) {
                genInto(*e.lhs, addr);
            } else {
                storeScalar(e.lhs->type, genScalar(*e.lhs), addr);
            }
        }
        return;
    }
    case hir::ExprKind::Select: {
        llvm::Value* c = genScalar(*e.lhs);
        llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(ctx_, "sel.then", cur_fn_);
        llvm::BasicBlock* else_bb = llvm::BasicBlock::Create(ctx_, "sel.else", cur_fn_);
        llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx_, "sel.end", cur_fn_);
        B_.CreateCondBr(c, then_bb, else_bb);
        B_.SetInsertPoint(then_bb);
        genInto(*e.rhs, dest);
        B_.CreateBr(end_bb);
        B_.SetInsertPoint(else_bb);
        genInto(*e.args[0], dest);
        B_.CreateBr(end_bb);
        B_.SetInsertPoint(end_bb);
        return;
    }
    case hir::ExprKind::ToInterface: {
        llvm::Value* data = genScalar(*e.lhs);
        llvm::Constant* itable = itablePointer(e.text, e.type.path.front());
        if (itable == nullptr) {
            error(e.span, std::format("no itable for `{}` as `{}`", e.text, e.type.path.front()));
            return;
        }
        B_.CreateAlignedStore(data, dest, llvm::Align(ptrBits() / 8));
        B_.CreateAlignedStore(itable, byteOffset(dest, ptrBits() / 8), llvm::Align(ptrBits() / 8));
        return;
    }
    case hir::ExprKind::Construct: {
        // A new object in `dest`: zeroed, vptr stored, then its `init` runs on it.
        const TypeLayout* l = layoutOf(e.target);
        if (l != nullptr) {
            zeroMem(dest, l->size, l->align);
        }
        if (e.target.kind == TypeKind::Class) {
            if (llvm::Constant* vp = vtablePointer(e.target.path.front())) {
                B_.CreateAlignedStore(vp, dest, llvm::Align(ptrBits() / 8));
            }
        }
        if (!e.text.empty()) {
            callInit(e.text, dest, e.args, e.span);
        }
        return;
    }
    case hir::ExprKind::StrConst: {
        if (e.type.kind != TypeKind::Str) {
            storeScalar(e.type, genScalar(e), dest);
            return;
        }
        B_.CreateAlignedStore(stringGlobal(e.text), dest, llvm::Align(ptrBits() / 8));
        B_.CreateAlignedStore(B_.getInt64(e.text.size()),
                              byteOffset(dest, ptrBits() / 8),
                              llvm::Align(ptrBits() / 8));
        return;
    }
    case hir::ExprKind::Slice: {
        // `{ ptr, len }`: the data pointer moves to the first in-range byte, `len` becomes the
        // span. Every offset is computed in 64 bits and clamped at 0, so a negative `lo`/`hi` (from
        // an `i64` bound) cannot wrap into a huge `usize`.
        llvm::Type* i64 = llvm::Type::getInt64Ty(ctx_);
        const unsigned align = ptrBits() / 8;
        const auto toI64 = [&](llvm::Value* v, const Type& t) {
            llvm::Value* x = v;
            const unsigned w = x->getType()->getIntegerBitWidth();
            if (w != 64) {
                x = isSigned(t) ? B_.CreateSExt(x, i64) : B_.CreateZExt(x, i64);
            }
            return x;
        };
        const auto atLeastZero = [&](llvm::Value* v, llvm::Value* fallback) {
            return B_.CreateSelect(B_.CreateICmpSLT(v, B_.getInt64(0)), fallback, v);
        };
        // The base: a `str`/slice view, or an array whose storage is used directly. `step` is the
        // byte size of one element (1 for `str` bytes); array/slice bounds are in elements and the
        // offsets are multiplied by `step`.
        llvm::Value* data = nullptr;
        llvm::Value* len = nullptr;
        std::uint64_t step = 1;
        if (e.lhs->type.kind == TypeKind::Str || e.lhs->type.kind == TypeKind::Slice) {
            llvm::Value* base = genAddr(*e.lhs);
            data = B_.CreateAlignedLoad(ptrTy(), base, llvm::Align(align));
            len = B_.CreateZExt(B_.CreateAlignedLoad(sty(Type::makeInt(0, false)),
                                                     byteOffset(base, align),
                                                     llvm::Align(align)),
                                i64);
            if (e.lhs->type.kind == TypeKind::Slice) {
                step = sizeAlign(*e.lhs->type.elem).first;
            }
        } else {
            const TypeLayout* l = layoutOf(e.lhs->type);
            const auto elem_size = sizeAlign(*e.lhs->type.elem).first;
            data = genAddr(*e.lhs);
            len = B_.getInt64(l != nullptr && elem_size != 0 ? l->size / elem_size : 0);
            step = elem_size;
        }
        llvm::Value* lo =
            e.args.empty()
                ? B_.getInt64(0)
                : atLeastZero(toI64(genScalar(*e.args[0]), e.args[0]->type), B_.getInt64(0));
        llvm::Value* hi = e.rhs ? atLeastZero(toI64(genScalar(*e.rhs), e.rhs->type), len) : len;
        if (e.checked) {
            checkedBranch(B_.CreateICmpUGT(lo, hi), "slice start past its end");
            checkedBranch(B_.CreateICmpUGT(hi, len), "slice end past the length");
        }
        llvm::Value* bytes = B_.CreateSub(hi, lo);
        llvm::Value* byte_off = B_.CreateMul(lo, B_.getInt64(step));
        B_.CreateAlignedStore(B_.CreateInBoundsGEP(llvm::Type::getInt8Ty(ctx_), data, {byte_off}),
                              dest,
                              llvm::Align(align));
        B_.CreateAlignedStore(B_.CreateTrunc(bytes, sty(Type::makeInt(0, false))),
                              byteOffset(dest, align),
                              llvm::Align(align));
        return;
    }
    default: {
        llvm::Value* src = genMem(e);
        if (src != dest) {
            copyMem(dest, src, e.type);
        }
        return;
    }
    }
}

llvm::Value* Gen::genMem(const hir::Expr& e) {
    setLoc(e.span);
    switch (e.kind) {
    case hir::ExprKind::LocalRef:
    case hir::ExprKind::GlobalRef:
    case hir::ExprKind::RawRead:
    case hir::ExprKind::Deref:
    case hir::ExprKind::Field:
    case hir::ExprKind::Index:
        return genAddr(e);
    case hir::ExprKind::Call:
    case hir::ExprKind::CallIndirect:
    case hir::ExprKind::CallVirtual:
    case hir::ExprKind::CallItable: {
        llvm::Value* mem = nullptr;
        (void)genCall(e, &mem);
        return mem;
    }
    case hir::ExprKind::Move: {
        // The value is read first; the drop flag is cleared, so the owner no longer drops it.
        // A field move (the payload of a `case` pattern) has no flag of its own: `local` is 0.
        llvm::Value* src = genMem(*e.lhs);
        if (e.local != 0) {
            storeScalar(Type::makeBool(), B_.getInt1(false), locals_[e.local]);
        }
        return src;
    }
    case hir::ExprKind::Materialize: {
        llvm::Value* tmp = locals_[e.local];
        genInto(*e.lhs, tmp);
        storeScalar(Type::makeBool(), B_.getInt1(true), locals_[e.index]);
        return tmp;
    }
    case hir::ExprKind::External:
    case hir::ExprKind::ExternalCall:
        error(e.span, std::format("unresolved external reference `{}.{}`", e.text, e.member));
        return makeTemp(e.type);
    default: {
        llvm::AllocaInst* tmp = makeTemp(e.type);
        genInto(e, tmp);
        return tmp;
    }
    }
}

llvm::Value* Gen::castScalar(llvm::Value* v, const Type& from, const Type& to) {
    llvm::Type* dst = sty(to);
    llvm::Type* src = v->getType();
    if (src == dst) {
        return v;
    }
    const Type& f = from.kind == TypeKind::Enum && from.elem ? *from.elem : from;
    const Type& t = to.kind == TypeKind::Enum && to.elem ? *to.elem : to;
    const bool f_int = f.kind == TypeKind::Int || f.kind == TypeKind::Bool;
    const bool t_int = t.kind == TypeKind::Int || t.kind == TypeKind::Bool;
    if (f_int && t_int) {
        if (t.kind == TypeKind::Bool) {
            return B_.CreateICmpNE(v, llvm::ConstantInt::get(src, 0));
        }
        const unsigned sb = src->getIntegerBitWidth();
        const unsigned db = dst->getIntegerBitWidth();
        if (sb == db) {
            return v;
        }
        if (sb > db) {
            return B_.CreateTrunc(v, dst);
        }
        return f.kind == TypeKind::Int && f.is_signed ? B_.CreateSExt(v, dst)
                                                      : B_.CreateZExt(v, dst);
    }
    if (f_int && t.kind == TypeKind::Float) {
        return f.kind == TypeKind::Int && f.is_signed ? B_.CreateSIToFP(v, dst)
                                                      : B_.CreateUIToFP(v, dst);
    }
    if (f.kind == TypeKind::Float && t_int) {
        if (t.kind == TypeKind::Bool) {
            return B_.CreateFCmpUNE(v, llvm::ConstantFP::get(src, 0.0));
        }
        return t.is_signed ? B_.CreateFPToSI(v, dst) : B_.CreateFPToUI(v, dst);
    }
    if (f.kind == TypeKind::Float && t.kind == TypeKind::Float) {
        return f.bits < t.bits ? B_.CreateFPExt(v, dst) : B_.CreateFPTrunc(v, dst);
    }
    const bool f_ptr = f.kind == TypeKind::Pointer || f.kind == TypeKind::FnPtr;
    const bool t_ptr = t.kind == TypeKind::Pointer || t.kind == TypeKind::FnPtr;
    if (f_ptr && t.kind == TypeKind::Int) {
        return B_.CreatePtrToInt(v, dst);
    }
    if (f.kind == TypeKind::Int && t_ptr) {
        return B_.CreateIntToPtr(v, dst);
    }
    return v; // pointer <-> pointer
}

llvm::Value* Gen::genCast(const hir::Expr& e) {
    llvm::Value* v = genScalar(*e.lhs);
    return castScalar(v, e.lhs->type, e.type);
}

llvm::Value* Gen::genStrCompare(const hir::Expr& e) {
    // A `str` is `{ ptr, len }`. There is no C runtime to call, so the bytes are compared by an
    // inline loop: lengths first, then index 0 up to len.
    const unsigned word = ptrBits() / 8;
    llvm::Type* usize = sty(Type::makeInt(0, false));
    llvm::Value* a = genAddr(*e.lhs);
    llvm::Value* b = genAddr(*e.rhs);
    const auto len = [&](llvm::Value* s) {
        return B_.CreateAlignedLoad(usize, byteOffset(s, word), llvm::Align(word));
    };
    const auto data = [&](llvm::Value* s) {
        return B_.CreateAlignedLoad(ptrTy(), s, llvm::Align(word));
    };
    llvm::Value* n = len(a);
    llvm::Value* pa = data(a);
    llvm::Value* pb = data(b);
    llvm::BasicBlock* start = B_.GetInsertBlock();
    llvm::BasicBlock* head = llvm::BasicBlock::Create(ctx_, "streq.head", cur_fn_);
    llvm::BasicBlock* body = llvm::BasicBlock::Create(ctx_, "streq.body", cur_fn_);
    llvm::BasicBlock* end = llvm::BasicBlock::Create(ctx_, "streq.end", cur_fn_);
    B_.CreateCondBr(B_.CreateICmpEQ(n, len(b)), head, end);
    B_.SetInsertPoint(head);
    llvm::PHINode* i = B_.CreatePHI(usize, 2);
    i->addIncoming(llvm::ConstantInt::get(usize, 0), start);
    B_.CreateCondBr(B_.CreateICmpUGE(i, n), end, body);
    B_.SetInsertPoint(body);
    llvm::Type* byte = llvm::Type::getInt8Ty(ctx_);
    llvm::Value* x = B_.CreateLoad(byte, B_.CreateInBoundsGEP(byte, pa, {i}));
    llvm::Value* y = B_.CreateLoad(byte, B_.CreateInBoundsGEP(byte, pb, {i}));
    i->addIncoming(B_.CreateAdd(i, llvm::ConstantInt::get(usize, 1)), body);
    B_.CreateCondBr(B_.CreateICmpNE(x, y), end, head);
    B_.SetInsertPoint(end);
    llvm::PHINode* equal = B_.CreatePHI(llvm::Type::getInt1Ty(ctx_), 3);
    equal->addIncoming(B_.getInt1(false), start); // the lengths differ
    equal->addIncoming(B_.getInt1(true), head);   // every byte matched
    equal->addIncoming(B_.getInt1(false), body);  // a byte differs
    return e.binary_op == BinaryOp::Eq ? static_cast<llvm::Value*>(equal) : B_.CreateNot(equal);
}

llvm::Value* Gen::genBinary(const hir::Expr& e) {
    const BinaryOp op = e.binary_op;
    if (e.lhs->type.kind == TypeKind::Str && (op == BinaryOp::Eq || op == BinaryOp::Ne)) {
        return genStrCompare(e);
    }
    if (op == BinaryOp::And || op == BinaryOp::Or) {
        llvm::Value* l = genScalar(*e.lhs);
        if (llvm::isa<llvm::Constant>(l) && B_.GetInsertBlock() == nullptr) {
            llvm::Value* r = genScalar(*e.rhs);
            return op == BinaryOp::And ? B_.CreateAnd(l, r) : B_.CreateOr(l, r);
        }
        llvm::BasicBlock* lhs_bb = B_.GetInsertBlock();
        llvm::BasicBlock* rhs_bb = llvm::BasicBlock::Create(ctx_, "sc.rhs", cur_fn_);
        llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx_, "sc.end", cur_fn_);
        if (op == BinaryOp::And) {
            B_.CreateCondBr(l, rhs_bb, end_bb);
        } else {
            B_.CreateCondBr(l, end_bb, rhs_bb);
        }
        B_.SetInsertPoint(rhs_bb);
        llvm::Value* r = genScalar(*e.rhs);
        llvm::BasicBlock* rhs_end = B_.GetInsertBlock();
        B_.CreateBr(end_bb);
        B_.SetInsertPoint(end_bb);
        llvm::PHINode* phi = B_.CreatePHI(llvm::Type::getInt1Ty(ctx_), 2);
        phi->addIncoming(B_.getInt1(op == BinaryOp::Or), lhs_bb);
        phi->addIncoming(r, rhs_end);
        return phi;
    }
    llvm::Value* l = genScalar(*e.lhs);
    llvm::Value* r = genScalar(*e.rhs);
    const Type& t = e.lhs->type;
    const bool is_float = t.kind == TypeKind::Float;
    const bool sign = isSigned(t);
    switch (op) {
    case BinaryOp::Add:
        return is_float ? B_.CreateFAdd(l, r) : B_.CreateAdd(l, r);
    case BinaryOp::Sub:
        return is_float ? B_.CreateFSub(l, r) : B_.CreateSub(l, r);
    case BinaryOp::Mul:
        return is_float ? B_.CreateFMul(l, r) : B_.CreateMul(l, r);
    case BinaryOp::Div:
    case BinaryOp::Mod: {
        if (is_float) {
            return B_.CreateFDiv(l, r);
        }
        if (e.checked && B_.GetInsertBlock() != nullptr) {
            checkedBranch(B_.CreateICmpEQ(r, llvm::ConstantInt::get(r->getType(), 0)),
                          "division by zero");
        }
        if (op == BinaryOp::Div) {
            return sign ? B_.CreateSDiv(l, r) : B_.CreateUDiv(l, r);
        }
        return sign ? B_.CreateSRem(l, r) : B_.CreateURem(l, r);
    }
    case BinaryOp::BitAnd:
        return B_.CreateAnd(l, r);
    case BinaryOp::BitOr:
        return B_.CreateOr(l, r);
    case BinaryOp::BitXor:
        return B_.CreateXor(l, r);
    case BinaryOp::Shl:
    case BinaryOp::Shr: {
        // A count >= the bit width gives 0 (`>>` of a negative signed value: -1), never poison.
        const unsigned bits = l->getType()->getIntegerBitWidth();
        llvm::Value* zero = llvm::ConstantInt::get(l->getType(), 0);
        llvm::Value* over = B_.CreateICmpUGE(r, llvm::ConstantInt::get(r->getType(), bits));
        llvm::Value* count = B_.CreateSelect(over, zero, r);
        if (op == BinaryOp::Shl) {
            return B_.CreateSelect(over, zero, B_.CreateShl(l, count));
        }
        if (sign) {
            llvm::Value* fill = B_.CreateAShr(l, llvm::ConstantInt::get(l->getType(), bits - 1));
            return B_.CreateSelect(over, fill, B_.CreateAShr(l, count));
        }
        return B_.CreateSelect(over, zero, B_.CreateLShr(l, count));
    }
    case BinaryOp::Eq:
        return is_float ? B_.CreateFCmpOEQ(l, r) : B_.CreateICmpEQ(l, r);
    case BinaryOp::Ne:
        return is_float ? B_.CreateFCmpUNE(l, r) : B_.CreateICmpNE(l, r);
    case BinaryOp::Lt:
        return is_float ? B_.CreateFCmpOLT(l, r)
                        : (sign ? B_.CreateICmpSLT(l, r) : B_.CreateICmpULT(l, r));
    case BinaryOp::Le:
        return is_float ? B_.CreateFCmpOLE(l, r)
                        : (sign ? B_.CreateICmpSLE(l, r) : B_.CreateICmpULE(l, r));
    case BinaryOp::Gt:
        return is_float ? B_.CreateFCmpOGT(l, r)
                        : (sign ? B_.CreateICmpSGT(l, r) : B_.CreateICmpUGT(l, r));
    case BinaryOp::Ge:
        return is_float ? B_.CreateFCmpOGE(l, r)
                        : (sign ? B_.CreateICmpSGE(l, r) : B_.CreateICmpUGE(l, r));
    default:
        break;
    }
    return l;
}

llvm::Value* Gen::genSyscall(const hir::Expr& e) {
    // Plan 5.3: arguments are converted to the register width; the result is `i64` with errors
    // as negative `-errno` on every target.
    llvm::Type* i64 = llvm::Type::getInt64Ty(ctx_);
    std::vector<llvm::Value*> args;
    for (const hir::ExprPtr& a : e.args) {
        llvm::Value* v = genScalar(*a);
        if (v->getType()->isPointerTy()) {
            v = B_.CreatePtrToInt(v, i64);
        } else if (v->getType()->getIntegerBitWidth() < 64) {
            v = isSigned(a->type) ? B_.CreateSExt(v, i64) : B_.CreateZExt(v, i64);
        }
        args.push_back(v);
    }
    const TargetInfo& t = in_.config.target;
    const bool x86 = t.arch == "x86_64";
    std::string constraints;
    std::string text;
    std::vector<llvm::Value*> inputs;
    const std::size_t n = args.size() - 1; // arguments after the number
    const auto addInput = [&](const std::string& reg, llvm::Value* v) {
        constraints += (constraints.empty() ? "" : ",") + std::string("{") + reg + "}";
        inputs.push_back(v);
    };
    llvm::Value* result = nullptr;
    if (t.os == "linux" && x86) {
        static const char* regs[] = {"rdi", "rsi", "rdx", "r10", "r8", "r9"};
        constraints = "={rax}";
        addInput("rax", args[0]);
        for (std::size_t i = 0; i < n; ++i) {
            addInput(regs[i], args[i + 1]);
        }
        constraints += ",~{rcx},~{r11},~{memory}";
        text = "syscall";
    } else if (t.os == "linux") { // aarch64
        constraints = "={x0}";
        addInput("x8", args[0]);
        for (std::size_t i = 0; i < n; ++i) {
            if (i == 0) {
                constraints += ",0";
                inputs.push_back(args[1]);
            } else {
                addInput("x" + std::to_string(i), args[i + 1]);
            }
        }
        if (n == 0) {
            // x0 is an output only; no input needed.
        }
        constraints += ",~{memory}";
        text = "svc #0";
    } else if (t.os == "macos" && x86) {
        static const char* regs[] = {"rdi", "rsi", "rdx", "r10", "r8", "r9"};
        constraints = "={rax}";
        llvm::Value* nr = B_.CreateOr(args[0], llvm::ConstantInt::get(i64, 0x2000000));
        addInput("rax", nr);
        for (std::size_t i = 0; i < n; ++i) {
            addInput(regs[i], args[i + 1]);
        }
        constraints += ",~{rcx},~{r11},~{cc},~{memory}";
        text = "syscall\n\tjnc 1f\n\tnegq %rax\n1:";
    } else if (t.os == "macos") { // aarch64
        constraints = "={x0}";
        addInput("x16", args[0]);
        for (std::size_t i = 0; i < n; ++i) {
            if (i == 0) {
                constraints += ",0";
                inputs.push_back(args[1]);
            } else {
                addInput("x" + std::to_string(i), args[i + 1]);
            }
        }
        constraints += ",~{cc},~{memory}";
        text = "svc #0x80\n\tb.cc 1f\n\tneg x0, x0\n1:";
    } else if (t.os == "windows" && x86) {
        if (n > 4) {
            error(e.span,
                  "`syscall` with more than 4 arguments on Windows is not supported yet (stack "
                  "passing)");
            return llvm::ConstantInt::get(i64, 0);
        }
        static const char* regs[] = {"r10", "rdx", "r8", "r9"};
        constraints = "={rax}";
        addInput("rax", args[0]);
        for (std::size_t i = 0; i < n; ++i) {
            addInput(regs[i], args[i + 1]);
        }
        constraints += ",~{rcx},~{r11},~{memory}";
        text = "syscall";
    } else {
        error(e.span, std::format("`syscall` is not supported on target `{}/{}`", t.os, t.arch));
        return llvm::ConstantInt::get(i64, 0);
    }
    std::vector<llvm::Type*> in_types;
    for (llvm::Value* v : inputs) {
        in_types.push_back(v->getType());
    }
    llvm::FunctionType* fty = llvm::FunctionType::get(i64, in_types, false);
    const bool intel = false;
    (void)intel;
    llvm::InlineAsm* ia =
        llvm::InlineAsm::get(fty, text, constraints, true, false, llvm::InlineAsm::AD_ATT);
    result = B_.CreateCall(ia, inputs);
    if (t.os == "windows") {
        // NTSTATUS is the 32-bit `eax`, sign-extended.
        result = B_.CreateSExt(B_.CreateTrunc(result, llvm::Type::getInt32Ty(ctx_)), i64);
    }
    return result;
}

void Gen::callInit(const std::string& key,
                   llvm::Value* self,
                   const std::vector<hir::ExprPtr>& args,
                   const Span& span) {
    llvm::Function* f = functionNamed(key);
    if (f == nullptr) {
        error(span, std::format("unknown function `{}`", key));
        return;
    }
    std::vector<Type> params{Type::makePointer(Type::makeVoid())};
    std::vector<const hir::Expr*> av;
    for (const hir::ExprPtr& a : args) {
        params.push_back(a->type);
        av.push_back(a.get());
    }
    genCallCommon(
        fnTypeFor(params, Type::makeVoid()), f, av, self, Type::makeVoid(), nullptr, span);
}

llvm::Value* Gen::genCallCommon(llvm::FunctionType* fty,
                                llvm::Value* callee,
                                const std::vector<const hir::Expr*>& args,
                                llvm::Value* self,
                                const Type& ret,
                                llvm::Value** mem_result,
                                const Span& at) {
    std::vector<llvm::Value*> av;
    llvm::Value* sret = nullptr;
    if (isMem(ret)) {
        sret = makeTemp(ret);
        av.push_back(sret);
    }
    if (self != nullptr) {
        av.push_back(self);
    }
    for (const hir::Expr* a : args) {
        if (isMem(a->type)) {
            // The callee owns its argument: pass a copy.
            llvm::AllocaInst* tmp = makeTemp(a->type);
            genInto(*a, tmp);
            av.push_back(tmp);
        } else {
            av.push_back(genScalar(*a));
        }
    }
    setLoc(at);
    llvm::CallInst* call = B_.CreateCall(fty, callee, av);
    if (mem_result != nullptr) {
        *mem_result = sret;
    }
    return call;
}

llvm::Value* Gen::genCall(const hir::Expr& e, llvm::Value** mem_result) {
    setLoc(e.span);
    std::vector<const hir::Expr*> args;
    switch (e.kind) {
    case hir::ExprKind::Syscall:
        return genSyscall(e);
    case hir::ExprKind::Call:
    case hir::ExprKind::CallVirtual: {
        llvm::Function* f = functionNamed(e.text);
        if (f == nullptr) {
            error(e.span, std::format("call of unknown function `{}`", e.text));
            return llvm::UndefValue::get(sty(e.type)->isVoidTy() ? llvm::Type::getInt32Ty(ctx_)
                                                                 : sty(e.type));
        }
        for (const hir::ExprPtr& a : e.args) {
            args.push_back(a.get());
        }
        if (e.kind == hir::ExprKind::Call) {
            llvm::Value* r =
                genCallCommon(f->getFunctionType(), f, args, nullptr, e.type, mem_result, e.span);
            if (!hm_.panic_function.empty() && e.text == hm_.panic_function) {
                // The registered `panic` never returns (plan 4.12): the rest is unreachable.
                B_.CreateUnreachable();
                B_.SetInsertPoint(llvm::BasicBlock::Create(ctx_, "after.panic", cur_fn_));
            }
            return r;
        }
        // Virtual: args[0] is `self`; the target comes from the vtable slot.
        llvm::Value* self = genScalar(*args[0]);
        const auto it = hir_fns_.find(e.text);
        std::vector<Type> params;
        if (it != hir_fns_.end()) {
            for (std::uint32_t i = 0; i < it->second->param_count; ++i) {
                params.push_back(it->second->locals[i].type);
            }
        }
        llvm::FunctionType* fty = f->getFunctionType();
        llvm::Value* vptr = B_.CreateAlignedLoad(ptrTy(), self, llvm::Align(ptrBits() / 8));
        llvm::Value* slot =
            B_.CreateInBoundsGEP(llvm::Type::getInt8Ty(ctx_),
                                 vptr,
                                 {B_.getInt64(std::uint64_t(e.index) * (ptrBits() / 8))});
        llvm::Value* fn = B_.CreateAlignedLoad(ptrTy(), slot, llvm::Align(ptrBits() / 8));
        std::vector<const hir::Expr*> rest(args.begin() + 1, args.end());
        return genCallCommon(fty, fn, rest, self, e.type, mem_result, e.span);
    }
    case hir::ExprKind::CallIndirect: {
        llvm::Value* fn = genScalar(*e.lhs);
        std::vector<Type> params = e.lhs->type.params;
        llvm::FunctionType* fty = fnTypeFor(params, e.type);
        for (const hir::ExprPtr& a : e.args) {
            args.push_back(a.get());
        }
        return genCallCommon(fty, fn, args, nullptr, e.type, mem_result, e.span);
    }
    case hir::ExprKind::CallItable: {
        const hir::Interface* iface =
            interfaceNamed(e.lhs->type.path.empty() ? std::string{} : e.lhs->type.path.front());
        if (iface == nullptr || e.index >= iface->methods.size()) {
            error(e.span, "call through an unknown interface");
            return llvm::UndefValue::get(llvm::Type::getInt32Ty(ctx_));
        }
        const hir::InterfaceMethod& m = iface->methods[e.index];
        llvm::Value* ia = genMem(*e.lhs);
        llvm::Value* data = B_.CreateAlignedLoad(ptrTy(), ia, llvm::Align(ptrBits() / 8));
        llvm::Value* itable = B_.CreateAlignedLoad(
            ptrTy(), byteOffset(ia, ptrBits() / 8), llvm::Align(ptrBits() / 8));
        llvm::Value* slot =
            B_.CreateInBoundsGEP(llvm::Type::getInt8Ty(ctx_),
                                 itable,
                                 {B_.getInt64(std::uint64_t(e.index) * (ptrBits() / 8))});
        llvm::Value* fn = B_.CreateAlignedLoad(ptrTy(), slot, llvm::Align(ptrBits() / 8));
        std::vector<Type> params;
        params.push_back(Type::makePointer(Type::makeVoid())); // self
        for (const Type& p : m.params) {
            params.push_back(p);
        }
        llvm::FunctionType* fty = fnTypeFor(params, e.type);
        for (const hir::ExprPtr& a : e.args) {
            args.push_back(a.get());
        }
        return genCallCommon(fty, fn, args, data, e.type, mem_result, e.span);
    }
    default:
        break;
    }
    return nullptr;
}

llvm::Value* Gen::genNew(const hir::Expr& e) {
    const Type& t = e.target;
    const auto [size, align] = sizeAlign(t);
    llvm::Value* mem = nullptr;
    llvm::Type* uty = llvm::Type::getIntNTy(ctx_, ptrBits());
    if (e.lhs) {
        // Placement: the memory is given; the compiler allocates nothing.
        mem = genScalar(*e.lhs);
    } else {
        llvm::Function* alloc = functionNamed(hm_.alloc_function);
        if (alloc == nullptr) {
            error(e.span, "`new` has no registered `alloc` function");
            return llvm::Constant::getNullValue(ptrTy());
        }
        mem = B_.CreateCall(
            alloc, {llvm::ConstantInt::get(uty, size), llvm::ConstantInt::get(uty, align)});
    }
    // A nil pointer (a failed allocation): then `new` yields nil and writes nothing.
    llvm::BasicBlock* init_bb = llvm::BasicBlock::Create(ctx_, "new.init", cur_fn_);
    llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx_, "new.end", cur_fn_);
    llvm::BasicBlock* from_bb = B_.GetInsertBlock();
    B_.CreateCondBr(B_.CreateICmpNE(mem, llvm::Constant::getNullValue(ptrTy())), init_bb, end_bb);
    B_.SetInsertPoint(init_bb);
    hir::Expr lit; // reuse StructLit initialization
    zeroMem(mem, size, align);
    if (t.kind == TypeKind::Class) {
        if (llvm::Constant* vp = vtablePointer(t.path.front())) {
            B_.CreateAlignedStore(vp, mem, llvm::Align(ptrBits() / 8));
        }
    }
    if (!e.text.empty()) {
        callInit(e.text, mem, e.args, e.span);
    }
    const TypeLayout* l = layoutOf(t);
    for (const hir::FieldInit& f : e.fields) {
        llvm::Value* addr = l != nullptr ? byteOffset(mem, l->fields[f.index].offset) : mem;
        if (isMem(f.value->type)) {
            genInto(*f.value, addr);
        } else {
            storeScalar(f.value->type, genScalar(*f.value), addr);
        }
    }
    B_.CreateBr(end_bb);
    B_.SetInsertPoint(end_bb);
    llvm::PHINode* phi = B_.CreatePHI(ptrTy(), 2);
    phi->addIncoming(llvm::Constant::getNullValue(ptrTy()), from_bb);
    phi->addIncoming(mem, init_bb);
    return phi;
}

} // namespace xlang::codegen
