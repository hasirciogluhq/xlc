// The C convention in the backend: the LLVM signature of an `export(c)` or `declare` function, its
// attributes, the callee's prologue and `return`, and the caller's arguments. What goes where is
// decided by the classifiers (c_abi.hpp); this file only moves bytes between the xlang form (an
// aggregate lives in memory) and the register image the classifier chose.

#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

c_abi::Value Gen::cValue(const Type& t) {
    c_abi::Value v;
    v.aggregate = isMem(t);
    std::tie(v.size, v.align) = sizeAlign(t);
    flatten(t, 0, v);
    return v;
}

void Gen::flatten(const Type& t, std::uint64_t base, c_abi::Value& out) {
    switch (t.kind) {
    case TypeKind::Struct:
        if (const TypeLayout* l = layoutOf(t)) {
            for (const FieldLayout& f : l->fields) {
                flatten(f.type, base + f.offset, out);
            }
        }
        return;
    case TypeKind::Array: {
        const std::uint64_t size = sizeAlign(t).first;
        const std::uint64_t step = sizeAlign(*t.elem).first;
        for (std::uint64_t at = 0; step != 0 && at < size; at += step) {
            flatten(*t.elem, base + at, out);
        }
        return;
    }
    default: {
        const auto [size, align] = sizeAlign(t);
        out.pieces.push_back({base, size, t.kind == TypeKind::Float});
        out.misaligned = out.misaligned || base % align != 0;
        return;
    }
    }
}

std::pair<c_abi::Signature, llvm::FunctionType*>
Gen::cFunctionType(const std::vector<Type>& params, const Type& ret) {
    c_abi::Value result;
    if (ret.hasValue()) {
        result = cValue(ret);
    } else {
        result.is_void = true;
    }
    std::vector<c_abi::Value> values;
    for (const Type& p : params) {
        values.push_back(cValue(p));
    }
    const TargetInfo& target = in_.config.target;
    c_abi::Signature sig =
        c_abi::classify(c_abi::conventionOf(target.os, target.arch), ctx_, result, values);

    llvm::Type* r = llvm::Type::getVoidTy(ctx_);
    std::vector<llvm::Type*> ps;
    if (!result.is_void) {
        switch (sig.ret.pass) {
        case c_abi::Pass::Direct:
            r = sig.ret.coerce != nullptr ? sig.ret.coerce : sty(ret);
            break;
        case c_abi::Pass::Indirect:
            ps.push_back(ptrTy()); // the result address comes first
            break;
        case c_abi::Pass::ByVal:
        case c_abi::Pass::Ignore:
            break;
        }
    }
    for (const auto& [p, a] : std::views::zip(params, sig.params)) {
        switch (a.pass) {
        case c_abi::Pass::Direct:
            ps.push_back(a.coerce != nullptr ? a.coerce : sty(p));
            break;
        case c_abi::Pass::Indirect:
        case c_abi::Pass::ByVal:
            ps.push_back(ptrTy());
            break;
        case c_abi::Pass::Ignore:
            break;
        }
    }
    return {std::move(sig), llvm::FunctionType::get(r, ps, false)};
}

llvm::AttributeList Gen::signatureAttributes(const std::vector<Type>& params,
                                             const Type& ret,
                                             const c_abi::Signature* c) {
    const auto scalar = [&](const Type& t) {
        llvm::AttrBuilder b(ctx_);
        if (const auto k = extensionOf(t)) {
            b.addAttribute(*k);
        }
        return llvm::AttributeSet::get(ctx_, b);
    };
    std::vector<llvm::AttributeSet> ps;
    llvm::AttributeSet result;
    // The result slot: a fresh temporary of the caller. Under the C convention it is `sret` (the
    // callee hands the address back in rax on x86-64; arm64 passes it in x8).
    if (c != nullptr ? c->ret.pass == c_abi::Pass::Indirect : isMem(ret)) {
        llvm::AttrBuilder b(ctx_);
        b.addAttribute(llvm::Attribute::NoAlias);
        if (c != nullptr) {
            b.addStructRetAttr(bytesOf(ret));
        }
        ps.push_back(llvm::AttributeSet::get(ctx_, b));
    } else if (!isMem(ret)) {
        result = scalar(ret);
    }
    for (std::size_t i = 0; i < params.size(); ++i) {
        const Type& p = params[i];
        const c_abi::Pass pass = c != nullptr ? c->params[i].pass : c_abi::Pass::Direct;
        if (pass == c_abi::Pass::Ignore) {
            continue;
        }
        if (pass == c_abi::Pass::ByVal) {
            llvm::AttrBuilder b(ctx_);
            b.addByValAttr(bytesOf(p));
            b.addAlignmentAttr(llvm::Align(c->params[i].align));
            ps.push_back(llvm::AttributeSet::get(ctx_, b));
            continue;
        }
        ps.push_back(isMem(p) ? llvm::AttributeSet() : scalar(p));
    }
    return llvm::AttributeList::get(ctx_, llvm::AttributeSet(), result, ps);
}

llvm::AllocaInst* Gen::coerceSlot(const Type& t, llvm::Type* coerce) {
    const auto [size, align] = sizeAlign(t);
    const std::uint64_t bytes =
        std::max<std::uint64_t>({size, DL_.getTypeAllocSize(coerce).getFixedValue(), 1});
    return makeAlloca(llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_), bytes),
                      std::max<std::uint64_t>(align, DL_.getABITypeAlign(coerce).value()));
}

void Gen::bindCParams(const hir::Function& f, const c_abi::Signature& c) {
    auto arg = cur_fn_->arg_begin();
    sret_ = c.ret.pass == c_abi::Pass::Indirect ? &*arg++ : nullptr;
    for (std::uint32_t i = 0; i < f.param_count; ++i) {
        const hir::Local& l = f.locals[i];
        const c_abi::Arg& a = c.params[i];
        setLoc(l.span);
        if (a.pass == c_abi::Pass::Ignore) {
            locals_[i] = makeTemp(l.type); // an empty struct: nothing came in
            continue;
        }
        arg->setName(l.name);
        if (a.pass != c_abi::Pass::Direct) {
            locals_[i] = &*arg; // the caller's copy (Indirect) or the stack copy (ByVal)
        } else if (a.coerce != nullptr) {
            llvm::AllocaInst* slot = coerceSlot(l.type, a.coerce);
            B_.CreateAlignedStore(&*arg, slot, slot->getAlign());
            locals_[i] = slot;
        } else {
            llvm::AllocaInst* slot =
                makeAlloca(mty(l.type), DL_.getABITypeAlign(mty(l.type)).value(), l.name);
            storeScalar(l.type, &*arg, slot);
            locals_[i] = slot;
        }
        ++arg;
    }
}

void Gen::returnC(const hir::Expr& value) {
    const c_abi::Arg& r = cur_c_->ret;
    if (r.pass == c_abi::Pass::Direct && r.coerce == nullptr) {
        B_.CreateRet(genScalar(value));
        return;
    }
    if (r.pass == c_abi::Pass::Direct) {
        llvm::AllocaInst* slot = coerceSlot(cur_->ret, r.coerce);
        genInto(value, slot);
        B_.CreateRet(B_.CreateAlignedLoad(r.coerce, slot, slot->getAlign()));
        return;
    }
    genInto(value, r.pass == c_abi::Pass::Indirect ? sret_ : makeTemp(cur_->ret));
    B_.CreateRetVoid();
}

llvm::Value* Gen::genCCall(llvm::Function* callee,
                           const c_abi::Signature& c,
                           const std::vector<const hir::Expr*>& args,
                           const Type& ret,
                           llvm::Value** mem_result,
                           const Span& at) {
    std::vector<llvm::Value*> av;
    std::vector<Type> types;
    llvm::Value* result = nullptr;
    if (c.ret.pass == c_abi::Pass::Indirect) {
        result = makeTemp(ret);
        av.push_back(result);
    }
    for (const auto& [e, a] : std::views::zip(args, c.params)) {
        types.push_back(e->type);
        if (a.pass == c_abi::Pass::Direct && a.coerce == nullptr) {
            av.push_back(genScalar(*e));
            continue;
        }
        // The value is built in a temporary: the callee's copy (Indirect), the source of the
        // stack copy (ByVal), the register image (Direct), or only its side effects (Ignore).
        llvm::AllocaInst* tmp =
            a.pass == c_abi::Pass::Direct ? coerceSlot(e->type, a.coerce) : makeTemp(e->type);
        genInto(*e, tmp);
        if (a.pass == c_abi::Pass::Direct) {
            av.push_back(B_.CreateAlignedLoad(a.coerce, tmp, tmp->getAlign()));
        } else if (a.pass != c_abi::Pass::Ignore) {
            av.push_back(tmp);
        }
    }
    setLoc(at);
    llvm::CallInst* call = B_.CreateCall(callee->getFunctionType(), callee, av);
    call->setAttributes(signatureAttributes(types, ret, &c));
    if (isMem(ret)) {
        if (c.ret.pass == c_abi::Pass::Direct) {
            llvm::AllocaInst* slot = coerceSlot(ret, c.ret.coerce);
            B_.CreateAlignedStore(call, slot, slot->getAlign());
            result = slot;
        } else if (result == nullptr) {
            result = makeTemp(ret); // an empty struct
        }
    }
    if (mem_result != nullptr) {
        *mem_result = result;
    }
    afterCall(call, ret);
    return call;
}

} // namespace xlang::codegen
