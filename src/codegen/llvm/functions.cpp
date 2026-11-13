#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::hasTerminator;
using gen_detail::isUnknownType;

// Function bodies: the prologue/epilogue of a defined function and statement lowering.

// ---------------------------------------------------------------------------------------------
// Functions and statements

void Gen::defineFunction(const hir::Function& f) {
    if (!f.has_body) {
        return;
    }
    llvm::Function* fn = functionNamed(f.name);
    cur_ = &f;
    cur_fn_ = fn;
    locals_.assign(f.locals.size(), nullptr);
    escape_ = !f.unwind_body.empty() || f.has_try;
    if (escape_) {
        fn->addFnAttr(llvm::Attribute::NoInline);
    }
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(ctx_, "entry", fn);
    B_.SetInsertPoint(entry);

    if (di_) {
        std::vector<llvm::Metadata*> sig;
        sig.push_back(di_basic(f.ret));
        for (std::uint32_t i = 0; i < f.param_count; ++i) {
            sig.push_back(di_basic(f.locals[i].type));
        }
        llvm::DIFile* file = di_file(f.span.file);
        llvm::DISubroutineType* st = di_->createSubroutineType(di_->getOrCreateTypeArray(sig));
        cur_sp_ = di_->createFunction(file,
                                      f.name,
                                      fn->getName(),
                                      file,
                                      f.span.line,
                                      st,
                                      f.span.line,
                                      llvm::DINode::FlagPrototyped,
                                      llvm::DISubprogram::SPFlagDefinition);
        fn->setSubprogram(cur_sp_);
    }

    auto arg = fn->arg_begin();
    sret_ = nullptr;
    const auto c = c_sigs_.find(f.name);
    cur_c_ = c != c_sigs_.end() ? &c->second : nullptr;
    if (cur_c_ != nullptr) {
        bindCParams(f, *cur_c_);
    } else if (isMem(f.ret)) {
        sret_ = &*arg++;
    }
    for (std::uint32_t i = 0; cur_c_ == nullptr && i < f.param_count; ++i, ++arg) {
        const Type& pt = f.locals[i].type;
        arg->setName(f.locals[i].name);
        if (isMem(pt)) {
            locals_[i] = &*arg; // the caller passed its own copy
        } else {
            llvm::AllocaInst* slot =
                makeAlloca(mty(pt), DL_.getABITypeAlign(mty(pt)).value(), f.locals[i].name);
            setLoc(f.locals[i].span);
            storeScalar(pt, &*arg, slot);
            locals_[i] = slot;
        }
    }
    if (!f.unwind_body.empty()) {
        // The pad is entered by a jump the optimizer cannot see; an opaque never-taken `indirectbr`
        // gives it a CFG predecessor so the block (and its table entry) survives.
        pad_ = llvm::BasicBlock::Create(ctx_, "unwind.pad", fn);
        llvm::BasicBlock* dispatch = llvm::BasicBlock::Create(ctx_, "unwind.dispatch", fn);
        llvm::BasicBlock* start = llvm::BasicBlock::Create(ctx_, "start", fn);
        llvm::LoadInst* never = B_.CreateLoad(B_.getInt8Ty(), never_, true);
        B_.CreateCondBr(B_.CreateIsNotNull(never), dispatch, start);
        B_.SetInsertPoint(dispatch);
        B_.CreateIndirectBr(llvm::BlockAddress::get(fn, pad_), 1)->addDestination(pad_);
        B_.SetInsertPoint(start);
    }
    genStmts(f.body);
    ensureTerminated();
    emitPad(f);
    escape_ = false;
    cur_c_ = nullptr;
    cur_sp_ = nullptr;
    B_.SetCurrentDebugLocation(llvm::DebugLoc());
    cur_ = nullptr;
}

void Gen::genStmts(const std::vector<hir::Stmt>& body) {
    for (const hir::Stmt& s : body) {
        genStmt(s);
    }
}

void Gen::genStmt(const hir::Stmt& s) {
    setLoc(s.span);
    switch (s.kind) {
    case hir::StmtKind::Local: {
        const hir::Local& l = cur_->locals[s.local];
        if (isMem(l.type)) {
            const auto [size, align] = sizeAlign(l.type);
            llvm::AllocaInst* slot = makeAlloca(
                llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_), std::max<std::uint64_t>(size, 1)),
                align,
                l.name);
            locals_[s.local] = slot;
            if (s.value) {
                genInto(*s.value, slot);
            } else {
                zeroMem(slot, size, align);
                if (l.type.kind == TypeKind::Class) {
                    if (llvm::Constant* vp = vtablePointer(l.type.path.front())) {
                        B_.CreateAlignedStore(vp, slot, llvm::Align(ptrBits() / 8));
                    }
                }
            }
        } else {
            llvm::AllocaInst* slot =
                makeAlloca(mty(l.type), DL_.getABITypeAlign(mty(l.type)).value(), l.name);
            locals_[s.local] = slot;
            if (s.value) {
                storeScalar(l.type, genScalar(*s.value), slot);
            } else {
                storeScalar(l.type, llvm::Constant::getNullValue(sty(l.type)), slot);
            }
        }
        break;
    }
    case hir::StmtKind::Assign: {
        if (isMem(s.target->type)) {
            // The value is built in its own slot and copied, never built in the target: a
            // literal or a constructor zeroes its destination before reading its operands, so
            // `p = V(p.x + 1, p.y)` would read the zeroed fields. LLVM's memcpyopt drops the copy
            // when the value cannot alias the target.
            llvm::Value* src = genMem(*s.value);
            llvm::Value* dest = genAddr(*s.target);
            copyMem(dest, src, s.target->type);
        } else {
            llvm::Value* v = genScalar(*s.value);
            llvm::Value* dest = genAddr(*s.target);
            storeScalar(s.target->type, v, dest);
        }
        break;
    }
    case hir::StmtKind::Return: {
        if (s.value) {
            if (cur_c_ != nullptr) {
                returnC(*s.value);
            } else if (isMem(cur_->ret)) {
                genInto(*s.value, sret_);
                B_.CreateRetVoid();
            } else {
                B_.CreateRet(genScalar(*s.value));
            }
        } else {
            B_.CreateRetVoid();
        }
        llvm::BasicBlock* dead = llvm::BasicBlock::Create(ctx_, "after.ret", cur_fn_);
        B_.SetInsertPoint(dead);
        break;
    }
    case hir::StmtKind::Expr: {
        const hir::Expr& e = *s.value;
        if (isMem(e.type)) {
            (void)genMem(e);
        } else if (e.kind == hir::ExprKind::AtomicStore || e.kind == hir::ExprKind::Fence ||
                   e.kind == hir::ExprKind::Asm) {
            (void)genScalar(e);
        } else if (!e.type.hasValue() || e.kind == hir::ExprKind::Call ||
                   e.kind == hir::ExprKind::CallIndirect || e.kind == hir::ExprKind::CallVirtual ||
                   e.kind == hir::ExprKind::CallItable || e.kind == hir::ExprKind::Syscall) {
            llvm::Value* mem = nullptr;
            (void)genCall(e, &mem);
        } else {
            (void)genScalar(e);
        }
        break;
    }
    case hir::StmtKind::If: {
        llvm::Value* c = genScalar(*s.value);
        llvm::BasicBlock* then_bb = llvm::BasicBlock::Create(ctx_, "then", cur_fn_);
        llvm::BasicBlock* else_bb =
            s.else_body.empty() ? nullptr : llvm::BasicBlock::Create(ctx_, "else", cur_fn_);
        llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx_, "endif", cur_fn_);
        B_.CreateCondBr(c, then_bb, else_bb != nullptr ? else_bb : end_bb);
        B_.SetInsertPoint(then_bb);
        genStmts(s.body);
        if (!hasTerminator(B_.GetInsertBlock())) {
            B_.CreateBr(end_bb);
        }
        if (else_bb != nullptr) {
            B_.SetInsertPoint(else_bb);
            genStmts(s.else_body);
            if (!hasTerminator(B_.GetInsertBlock())) {
                B_.CreateBr(end_bb);
            }
        }
        B_.SetInsertPoint(end_bb);
        break;
    }
    case hir::StmtKind::While: {
        llvm::BasicBlock* cond_bb = llvm::BasicBlock::Create(ctx_, "while.cond", cur_fn_);
        llvm::BasicBlock* body_bb = llvm::BasicBlock::Create(ctx_, "while.body", cur_fn_);
        llvm::BasicBlock* end_bb = llvm::BasicBlock::Create(ctx_, "while.end", cur_fn_);
        llvm::BasicBlock* step_bb =
            s.step.empty() ? nullptr : llvm::BasicBlock::Create(ctx_, "while.step", cur_fn_);
        // `continue` runs the step (a `for`), then re-tests the condition.
        llvm::BasicBlock* next_bb = step_bb != nullptr ? step_bb : cond_bb;
        B_.CreateBr(s.post_test ? body_bb : cond_bb);
        B_.SetInsertPoint(cond_bb);
        setLoc(s.span);
        llvm::Value* c = genScalar(*s.value);
        B_.CreateCondBr(c, body_bb, end_bb);
        B_.SetInsertPoint(body_bb);
        loops_.push_back({next_bb, end_bb});
        genStmts(s.body);
        loops_.pop_back();
        if (!hasTerminator(B_.GetInsertBlock())) {
            B_.CreateBr(next_bb);
        }
        if (step_bb != nullptr) {
            // A `break` in the step leaves this loop (a `do { } while c?` tests `c` there).
            B_.SetInsertPoint(step_bb);
            loops_.push_back({cond_bb, end_bb});
            genStmts(s.step);
            loops_.pop_back();
            if (!hasTerminator(B_.GetInsertBlock())) {
                B_.CreateBr(cond_bb);
            }
        }
        B_.SetInsertPoint(end_bb);
        break;
    }
    case hir::StmtKind::Continue:
    case hir::StmtKind::Break: {
        const LoopTargets& t = loops_[loops_.size() - 1 - s.depth];
        B_.CreateBr(s.kind == hir::StmtKind::Break ? t.brk : t.cont);
        llvm::BasicBlock* dead = llvm::BasicBlock::Create(ctx_, "after.jump", cur_fn_);
        B_.SetInsertPoint(dead);
        break;
    }
    case hir::StmtKind::Block:
        genStmts(s.body);
        break;
    case hir::StmtKind::Try:
        genTry(s);
        break;
    }
}

} // namespace xlang::codegen
