#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::hasTerminator;
using gen_detail::isUnknownType;

// Module level: declarations of globals and functions, vtables and itables, constant
// initializers and definitions of globals.

// ---------------------------------------------------------------------------------------------
// Module level

/// Per-module mode: does this module define (not just declare) the item of module `key`?
static bool definedHere(const CodegenInput& in, const std::string& key) {
    return !in.only_module || *in.only_module == key;
}

void Gen::declareFunction(const hir::Function& f) {
    std::vector<Type> params;
    for (std::uint32_t i = 0; i < f.param_count; ++i) {
        params.push_back(f.locals[i].type);
    }
    // `export(c)` and `declare` functions follow the target's C convention (c_abi/); every other
    // function the xlang one (an aggregate travels as the address of a copy).
    const c_abi::Signature* c = nullptr;
    llvm::FunctionType* fty = nullptr;
    if (usesCAbi(f)) {
        auto [sig, type] = cFunctionType(params, f.ret);
        c = &(c_sigs_[f.name] = std::move(sig));
        fty = type;
    } else {
        fty = fnTypeFor(params, f.ret);
    }
    llvm::GlobalValue::LinkageTypes link = llvm::GlobalValue::InternalLinkage;
    const bool root_main = f.module.empty() && f.name == "main";
    if (f.exported || f.declared || root_main || !f.has_body || in_.only_module) {
        link = llvm::GlobalValue::ExternalLinkage; // per-module objects reference each other
    }
    llvm::Function* fn =
        llvm::Function::Create(fty, link, f.symbol.empty() ? f.name : f.symbol, mod_);
    fn->setAttributes(signatureAttributes(params, f.ret, c));
    fn->addFnAttr(llvm::Attribute::NoUnwind);
    // No implicit library-call idioms: `memcpy`/`memset` are ordinary xl code (plan 7.2).
    fn->addFnAttr("no-builtins");
    // Frame pointers everywhere: stdlib context switches rely on a stable frame layout.
    fn->addFnAttr("frame-pointer", "all");
    if (f.ret.isNever()) {
        fn->addFnAttr(llvm::Attribute::NoReturn);
    }
    fns_[f.name] = fn;
    hir_fns_[f.name] = &f;
}

void Gen::declareGlobal(const hir::Global& g) {
    const auto [size, align] = sizeAlign(g.type);
    llvm::Type* ty = isMem(g.type)
                         ? static_cast<llvm::Type*>(llvm::ArrayType::get(
                               llvm::Type::getInt8Ty(ctx_), std::max<std::uint64_t>(size, 1)))
                         : mty(g.type);
    const bool here = definedHere(in_, g.module);
    llvm::Constant* init = (g.declared || !here) ? nullptr : llvm::Constant::getNullValue(ty);
    const bool external = g.exported || g.declared || in_.only_module;
    auto* gv = new llvm::GlobalVariable(mod_,
                                        ty,
                                        g.constant,
                                        external ? llvm::GlobalValue::ExternalLinkage
                                                 : llvm::GlobalValue::InternalLinkage,
                                        init,
                                        g.symbol.empty() ? g.name : g.symbol);
    gv->setAlignment(llvm::Align(std::max<std::uint64_t>(align, 1)));
    globals_[g.name] = gv;
    hir_globals_[g.name] = &g;
}

llvm::Constant* Gen::vtablePointer(const std::string& class_name) {
    const auto it = vtables_.find(class_name);
    if (it == vtables_.end()) {
        return nullptr;
    }
    const hir::Aggregate* a = aggregateNamed(class_name);
    llvm::GlobalVariable* gv = it->second;
    if (a != nullptr && a->repr_c) {
        // Itanium: [offset-to-top, typeinfo, functions...]; the vptr points at the first function.
        return llvm::ConstantExpr::getInBoundsGetElementPtr(
            gv->getValueType(),
            gv,
            llvm::ArrayRef<llvm::Constant*>{
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 0),
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), 2)});
    }
    return gv;
}

llvm::Constant* Gen::itablePointer(const std::string& type_name, const std::string& iface_name) {
    const auto it = itables_.find(type_name + "|" + iface_name);
    return it == itables_.end() ? nullptr : it->second;
}

void Gen::buildTables() {
    llvm::PointerType* pty = llvm::PointerType::getUnqual(ctx_);
    for (const hir::Aggregate& a : hm_.aggregates) {
        aggregates_[a.name] = &a;
    }
    for (const hir::Interface& i : hm_.interfaces) {
        interfaces_[i.name] = &i;
    }
    for (const hir::Aggregate& a : hm_.aggregates) {
        if (!a.is_class || a.vtable.empty()) {
            continue;
        }
        std::vector<llvm::Constant*> slots;
        if (a.repr_c) {
            slots.push_back(llvm::Constant::getNullValue(pty)); // offset-to-top (0)
            slots.push_back(llvm::Constant::getNullValue(
                pty)); // typeinfo: not provided (plan decision pending)
        }
        for (const hir::VtableSlot& s : a.vtable) {
            llvm::Function* f = functionNamed(s.function);
            slots.push_back(f != nullptr ? static_cast<llvm::Constant*>(f)
                                         : llvm::Constant::getNullValue(pty));
        }
        auto* ty = llvm::ArrayType::get(pty, slots.size());
        auto* gv = new llvm::GlobalVariable(mod_,
                                            ty,
                                            true,
                                            in_.only_module ? llvm::GlobalValue::LinkOnceODRLinkage
                                                            : llvm::GlobalValue::InternalLinkage,
                                            llvm::ConstantArray::get(ty, slots),
                                            "vtable." + a.name);
        gv->setAlignment(llvm::Align(ptrBits() / 8));
        vtables_[a.name] = gv;
    }
    for (const hir::Impl& im : hm_.impls) {
        std::vector<llvm::Constant*> slots;
        for (const std::string& fname : im.methods) {
            llvm::Function* f = functionNamed(fname);
            slots.push_back(f != nullptr ? static_cast<llvm::Constant*>(f)
                                         : llvm::Constant::getNullValue(pty));
        }
        auto* ty = llvm::ArrayType::get(pty, slots.size());
        auto* gv = new llvm::GlobalVariable(mod_,
                                            ty,
                                            true,
                                            in_.only_module ? llvm::GlobalValue::LinkOnceODRLinkage
                                                            : llvm::GlobalValue::InternalLinkage,
                                            llvm::ConstantArray::get(ty, slots),
                                            "itable." + im.type + "." + im.interface);
        gv->setAlignment(llvm::Align(ptrBits() / 8));
        itables_[im.type + "|" + im.interface] = gv;
    }
}

llvm::Constant* Gen::genConst(const hir::Expr& e) {
    switch (e.kind) {
    case hir::ExprKind::StructLit: {
        const TypeLayout* l = layoutOf(e.type);
        if (l == nullptr) {
            error(e.span, std::format("no layout for `{}`", typeToString(e.type)));
            return nullptr;
        }
        // Build the object as a packed struct of [padding, field, padding, ...] by offset.
        struct Part {
            std::uint64_t offset;
            llvm::Constant* value;
            std::uint64_t size;
        };
        std::vector<Part> parts;
        if (l->has_vptr) {
            if (llvm::Constant* vp = vtablePointer(e.type.path.front())) {
                parts.push_back({0, vp, ptrBits() / 8});
            }
        }
        for (const hir::FieldInit& f : e.fields) {
            llvm::Constant* c = genConst(*f.value);
            if (c == nullptr) {
                return nullptr;
            }
            const FieldLayout& fl = l->fields[f.index];
            parts.push_back({fl.offset, c, fl.size});
        }
        std::sort(parts.begin(), parts.end(), [](const Part& a, const Part& b) {
            return a.offset < b.offset;
        });
        std::vector<llvm::Constant*> elems;
        std::vector<llvm::Type*> types;
        std::uint64_t cursor = 0;
        const auto pad = [&](std::uint64_t to) {
            if (to > cursor) {
                auto* at = llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_), to - cursor);
                elems.push_back(llvm::ConstantAggregateZero::get(at));
                types.push_back(at);
                cursor = to;
            }
        };
        for (const Part& p : parts) {
            pad(p.offset);
            elems.push_back(p.value);
            types.push_back(p.value->getType());
            cursor = p.offset + p.size;
        }
        pad(l->size);
        auto* st = llvm::StructType::get(ctx_, types, true);
        return llvm::ConstantStruct::get(st, elems);
    }
    case hir::ExprKind::ArrayList:
    case hir::ExprKind::ArrayRepeat: {
        const TypeLayout* l = layoutOf(e.type);
        if (l == nullptr || !e.type.elem) {
            error(e.span, "array without layout");
            return nullptr;
        }
        std::vector<llvm::Constant*> elems;
        if (e.kind == hir::ExprKind::ArrayList) {
            for (const hir::ExprPtr& a : e.args) {
                llvm::Constant* c = genConst(*a);
                if (c == nullptr) {
                    return nullptr;
                }
                elems.push_back(c);
            }
        } else {
            llvm::Constant* c = genConst(*e.lhs);
            if (c == nullptr) {
                return nullptr;
            }
            elems.assign(e.int_value, c);
        }
        if (elems.empty()) {
            return llvm::ConstantAggregateZero::get(
                llvm::ArrayType::get(llvm::Type::getInt8Ty(ctx_), 1));
        }
        // Each element is its own packed unit; keep them in a struct so mixed element types fit.
        std::vector<llvm::Type*> types;
        for (llvm::Constant* c : elems) {
            types.push_back(c->getType());
        }
        auto* st = llvm::StructType::get(ctx_, types, true);
        return llvm::ConstantStruct::get(st, elems);
    }
    case hir::ExprKind::ToInterface: {
        llvm::Constant* data = genConst(*e.lhs);
        llvm::Constant* itable = itablePointer(e.text, e.type.path.front());
        if (data == nullptr || itable == nullptr) {
            error(e.span, "interface constant without an itable");
            return nullptr;
        }
        auto* st = llvm::StructType::get(ctx_, {ptrTy(), ptrTy()}, true);
        return llvm::ConstantStruct::get(st, {data, itable});
    }
    case hir::ExprKind::StrConst: {
        llvm::GlobalVariable* bytes = stringGlobal(e.text);
        if (e.type.kind != TypeKind::Str) {
            return bytes;
        }
        auto* st = llvm::StructType::get(ctx_, {ptrTy(), llvm::Type::getInt64Ty(ctx_)}, true);
        return llvm::ConstantStruct::get(
            st, {bytes, llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), e.text.size())});
    }
    case hir::ExprKind::Nil:
        return llvm::Constant::getNullValue(ptrTy());
    case hir::ExprKind::FuncRef: {
        llvm::Function* f = functionNamed(e.text);
        if (f == nullptr) {
            error(e.span, std::format("unknown function `{}`", e.text));
        }
        return f;
    }
    case hir::ExprKind::BoolConst:
        return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), e.bool_value ? 1 : 0);
    case hir::ExprKind::AddrOf: {
        // &global, &global[const], &global.field
        const hir::Expr* cur = e.lhs.get();
        std::uint64_t offset = 0;
        while (cur != nullptr && cur->kind != hir::ExprKind::GlobalRef) {
            if (cur->kind == hir::ExprKind::Index) {
                const auto [size, align] = sizeAlign(cur->type);
                (void)align;
                offset += size * cur->rhs->int_value;
                cur = cur->lhs.get();
            } else if (cur->kind == hir::ExprKind::Field) {
                const TypeLayout* l = layoutOf(cur->lhs->type);
                if (l == nullptr) {
                    return nullptr;
                }
                offset += l->fields[cur->index].offset;
                cur = cur->lhs.get();
            } else {
                break;
            }
        }
        if (cur == nullptr || cur->kind != hir::ExprKind::GlobalRef) {
            error(e.span, "unsupported address constant");
            return nullptr;
        }
        const auto it = globals_.find(cur->text);
        if (it == globals_.end()) {
            error(e.span, std::format("unknown global `{}`", cur->text));
            return nullptr;
        }
        llvm::Constant* base = it->second;
        if (offset == 0) {
            return base;
        }
        return llvm::ConstantExpr::getInBoundsGetElementPtr(
            llvm::Type::getInt8Ty(ctx_),
            base,
            llvm::ArrayRef<llvm::Constant*>{
                llvm::ConstantInt::get(llvm::Type::getInt64Ty(ctx_), offset)});
    }
    case hir::ExprKind::IntConst:
    case hir::ExprKind::FloatConst:
    default: {
        // Scalar constant expressions fold through the IR builder (no insertion point).
        B_.ClearInsertionPoint();
        llvm::Value* v = genScalar(e);
        auto* c = v != nullptr ? llvm::dyn_cast<llvm::Constant>(v) : nullptr;
        if (c == nullptr) {
            error(e.span, "initializer is not a constant");
            return nullptr;
        }
        if (c->getType()->isIntegerTy(1)) {
            return llvm::ConstantInt::get(llvm::Type::getInt8Ty(ctx_), c->isOneValue() ? 1 : 0);
        }
        return c;
    }
    }
}

void Gen::defineGlobal(const hir::Global& g) {
    llvm::GlobalVariable* old = globals_[g.name];
    if (!g.init || g.declared) {
        return;
    }
    cur_fn_ = nullptr;
    llvm::Constant* init = genConst(*g.init);
    if (init == nullptr) {
        return;
    }
    if (init->getType() == old->getValueType()) {
        old->setInitializer(init);
        return;
    }
    // The initializer has its own (packed struct) type: replace the placeholder variable.
    auto* gv = new llvm::GlobalVariable(
        mod_, init->getType(), old->isConstant(), old->getLinkage(), init, "");
    gv->setAlignment(old->getAlign().value_or(llvm::Align(1)));
    gv->takeName(old);
    old->replaceAllUsesWith(gv);
    old->eraseFromParent();
    globals_[g.name] = gv;
}

} // namespace xlang::codegen
