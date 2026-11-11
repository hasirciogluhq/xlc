#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::failureOrdering;
using gen_detail::hasTerminator;
using gen_detail::isUnknownType;
using gen_detail::orderingOf;

// Atomics and inline assembly: `__builtin_atomic*`/`__builtin_fence` and `__builtin_asm`.

llvm::Value* Gen::genAtomic(const hir::Expr& e) {
    llvm::Value* ptr = genScalar(*e.lhs);
    const Type& t = e.lhs->type.elem ? *e.lhs->type.elem : e.type;
    llvm::Type* mt = mty(t); // bool is stored as i8
    const llvm::Align align(std::max<std::uint64_t>(DL_.getTypeStoreSize(mt).getFixedValue(), 1));
    const llvm::AtomicOrdering ord = orderingOf(e.index);
    const auto toMem = [&](llvm::Value* v) {
        return sty(t)->isIntegerTy(1) ? B_.CreateZExt(v, mt) : v;
    };
    const auto fromMem = [&](llvm::Value* v) -> llvm::Value* {
        return sty(t)->isIntegerTy(1) ? B_.CreateICmpNE(v, llvm::ConstantInt::get(mt, 0)) : v;
    };
    switch (e.kind) {
    case hir::ExprKind::AtomicLoad: {
        llvm::LoadInst* l = B_.CreateAlignedLoad(mt, ptr, align);
        l->setAtomic(ord);
        return fromMem(l);
    }
    case hir::ExprKind::AtomicStore: {
        llvm::StoreInst* s = B_.CreateAlignedStore(toMem(genScalar(*e.rhs)), ptr, align);
        s->setAtomic(ord);
        return nullptr;
    }
    case hir::ExprKind::AtomicXchg:
        return fromMem(B_.CreateAtomicRMW(
            llvm::AtomicRMWInst::Xchg, ptr, toMem(genScalar(*e.rhs)), align, ord));
    case hir::ExprKind::AtomicAdd:
        return B_.CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr, genScalar(*e.rhs), align, ord);
    case hir::ExprKind::AtomicCas: {
        llvm::Value* expected = toMem(genScalar(*e.rhs));
        llvm::Value* desired = toMem(genScalar(*e.args[0]));
        llvm::Value* r =
            B_.CreateAtomicCmpXchg(ptr, expected, desired, align, ord, failureOrdering(ord));
        return fromMem(B_.CreateExtractValue(r, 0));
    }
    default:
        break;
    }
    return nullptr;
}

llvm::Value* Gen::genAsm(const hir::Expr& e) {
    // Template placeholders `{k}` follow the operand order of the call; LLVM numbers outputs
    // first, then inputs: build the mapping.
    std::vector<int> llvm_index(e.asm_operands.size(), 0);
    int outs = 0;
    for (const hir::AsmOperand& op : e.asm_operands) {
        if (op.kind != hir::AsmOperand::Kind::In) {
            ++outs;
        }
    }
    int out_i = 0;
    int in_i = 0;
    for (std::size_t k = 0; k < e.asm_operands.size(); ++k) {
        if (e.asm_operands[k].kind == hir::AsmOperand::Kind::In) {
            llvm_index[k] = outs + in_i++;
        } else {
            llvm_index[k] = out_i++;
            if (e.asm_operands[k].kind == hir::AsmOperand::Kind::InOut) {
                ++in_i; // the tied input
            }
        }
    }
    std::string text;
    for (std::size_t i = 0; i < e.text.size(); ++i) {
        const char c = e.text[i];
        if (c == '$') {
            text += "$$";
        } else if (c == '{' && i + 1 < e.text.size() &&
                   std::isdigit(static_cast<unsigned char>(e.text[i + 1])) != 0) {
            std::size_t j = i + 1;
            int n = 0;
            while (j < e.text.size() && std::isdigit(static_cast<unsigned char>(e.text[j])) != 0) {
                n = n * 10 + (e.text[j] - '0');
                ++j;
            }
            if (j < e.text.size() && e.text[j] == '}' &&
                static_cast<std::size_t>(n) < e.asm_operands.size()) {
                text += "$" + std::to_string(llvm_index[n]);
                i = j;
                continue;
            }
            text += c;
        } else {
            text += c;
        }
    }

    std::vector<llvm::Type*> out_types;
    std::vector<llvm::Type*> in_types;
    std::vector<llvm::Value*> in_values;
    std::vector<const hir::AsmOperand*> out_ops;
    std::string out_constraints;
    std::string in_constraints;
    const auto regSpec = [&](const hir::AsmOperand& op, const char* prefix) {
        return op.reg == "reg" ? std::string(prefix) + "r"
                               : std::string(prefix) + "{" + op.reg + "}";
    };
    for (const hir::AsmOperand& op : e.asm_operands) {
        if (op.kind == hir::AsmOperand::Kind::In) {
            continue;
        }
        out_constraints += (out_constraints.empty() ? "" : ",") + regSpec(op, "=");
        out_types.push_back(sty(op.value->type));
        out_ops.push_back(&op);
    }
    int out_idx = 0;
    for (const hir::AsmOperand& op : e.asm_operands) {
        if (op.kind == hir::AsmOperand::Kind::In) {
            in_constraints += (in_constraints.empty() ? "" : ",") + regSpec(op, "");
            in_types.push_back(sty(op.value->type));
            in_values.push_back(genScalar(*op.value));
        } else {
            if (op.kind == hir::AsmOperand::Kind::InOut) {
                in_constraints += (in_constraints.empty() ? "" : ",") + std::to_string(out_idx);
                in_types.push_back(sty(op.value->type));
                in_values.push_back(genScalar(*op.value));
            }
            ++out_idx;
        }
    }
    std::string constraints = out_constraints;
    if (!in_constraints.empty()) {
        constraints += (constraints.empty() ? "" : ",") + in_constraints;
    }
    for (const std::string& c : e.clobbers) {
        constraints += (constraints.empty() ? "" : ",") + std::string("~{") + c + "}";
    }
    if (!e.nomem) {
        constraints += (constraints.empty() ? "" : ",") + std::string("~{memory}");
    }
    llvm::Type* ret = llvm::Type::getVoidTy(ctx_);
    if (out_types.size() == 1) {
        ret = out_types[0];
    } else if (out_types.size() > 1) {
        ret = llvm::StructType::get(ctx_, out_types);
    }
    llvm::FunctionType* fty = llvm::FunctionType::get(ret, in_types, false);
    const bool intel = in_.config.target.arch == "x86_64";
    llvm::InlineAsm* ia =
        llvm::InlineAsm::get(fty,
                             text,
                             constraints,
                             true,
                             false,
                             intel ? llvm::InlineAsm::AD_Intel : llvm::InlineAsm::AD_ATT);
    llvm::CallInst* call = B_.CreateCall(ia, in_values);
    for (std::size_t i = 0; i < out_ops.size(); ++i) {
        llvm::Value* v =
            out_ops.size() == 1 ? static_cast<llvm::Value*>(call) : B_.CreateExtractValue(call, i);
        storeScalar(out_ops[i]->value->type, v, genAddr(*out_ops[i]->value));
    }
    if (e.noreturn) {
        B_.CreateUnreachable();
        llvm::BasicBlock* dead = llvm::BasicBlock::Create(ctx_, "after.asm", cur_fn_);
        B_.SetInsertPoint(dead);
    }
    return nullptr;
}

} // namespace xlang::codegen
