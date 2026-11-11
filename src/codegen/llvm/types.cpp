#include "codegen/llvm/gen.hpp"

namespace xlang::codegen {

using gen_detail::hasTerminator;
using gen_detail::isUnknownType;

// Types: the SSA type of a scalar, its in-memory form (bool -> i8), size/align and the
// function type of a signature.

// ---------------------------------------------------------------------------------------------
// Types

bool Gen::isMem(const Type& t) const {
    return t.kind == TypeKind::Struct || t.kind == TypeKind::Class || t.kind == TypeKind::Array ||
           t.kind == TypeKind::Interface || t.kind == TypeKind::Str ||
           t.kind == TypeKind::Slice || t.kind == TypeKind::Tuple;
}

bool Gen::isSigned(const Type& t) const {
    if (t.kind == TypeKind::Enum && t.elem) {
        return isSigned(*t.elem);
    }
    return t.kind == TypeKind::Int && t.is_signed;
}

llvm::Type* Gen::sty(const Type& t) {
    switch (t.kind) {
    case TypeKind::Void:
        return llvm::Type::getVoidTy(ctx_);
    case TypeKind::Bool:
        return llvm::Type::getInt1Ty(ctx_);
    case TypeKind::Int:
        return llvm::Type::getIntNTy(ctx_, t.bits == 0 ? ptrBits() : t.bits);
    case TypeKind::Float:
        return t.bits == 32 ? llvm::Type::getFloatTy(ctx_) : llvm::Type::getDoubleTy(ctx_);
    case TypeKind::Pointer:
    case TypeKind::FnPtr:
        return ptrTy();
    case TypeKind::Enum:
        return t.elem ? sty(*t.elem) : llvm::Type::getInt32Ty(ctx_);
    default:
        return ptrTy(); // aggregates are handled through their address
    }
}

llvm::Type* Gen::mty(const Type& t) {
    llvm::Type* s = sty(t);
    return s->isIntegerTy(1) ? llvm::Type::getInt8Ty(ctx_) : s;
}

const TypeLayout* Gen::layoutOf(const Type& t) {
    const std::string key = typeToString(t);
    if (const auto it = layouts_.find(key); it != layouts_.end()) {
        return it->second.get();
    }
    auto l = in_.layout(t);
    if (!l) {
        return nullptr;
    }
    return (layouts_[key] = std::make_unique<TypeLayout>(std::move(*l))).get();
}

std::pair<std::uint64_t, std::uint64_t> Gen::sizeAlign(const Type& t) {
    switch (t.kind) {
    case TypeKind::Bool:
        return {1, 1};
    case TypeKind::Int: {
        const unsigned bits = t.bits == 0 ? ptrBits() : t.bits;
        return {bits / 8, bits / 8};
    }
    case TypeKind::Float:
        return {t.bits / 8, t.bits / 8};
    case TypeKind::Pointer:
    case TypeKind::FnPtr:
        return {ptrBits() / 8, ptrBits() / 8};
    case TypeKind::Enum:
        return t.elem ? sizeAlign(*t.elem) : std::make_pair<std::uint64_t, std::uint64_t>(4, 4);
    default:
        break;
    }
    if (const TypeLayout* l = layoutOf(t)) {
        return {l->size, l->align};
    }
    return {0, 1};
}

llvm::FunctionType* Gen::fnTypeFor(const std::vector<Type>& params, const Type& ret) {
    std::vector<llvm::Type*> ps;
    llvm::Type* r = llvm::Type::getVoidTy(ctx_);
    if (isMem(ret)) {
        ps.push_back(ptrTy()); // sret
    } else if (!ret.isVoid()) {
        r = sty(ret);
    }
    for (const Type& p : params) {
        ps.push_back(isMem(p) ? ptrTy() : sty(p));
    }
    return llvm::FunctionType::get(r, ps, false);
}

void Gen::error(const Span& at, const std::string& message) {
    failed_ = true;
    in_.diagnostics.error("E0714", at, message, "code generation");
}

void Gen::setLoc(const Span& span) {
    if (di_ && cur_sp_ != nullptr && span.valid()) {
        B_.SetCurrentDebugLocation(llvm::DILocation::get(ctx_, span.line, span.column, cur_sp_));
    }
}

llvm::DIFile* Gen::di_file(FileId id) {
    if (const auto it = di_files_.find(id); it != di_files_.end()) {
        return it->second;
    }
    std::string path = "<unknown>";
    if (id != kNoFile && id < in_.sources.fileCount()) {
        path = in_.sources.file(id).path().string();
    }
    const std::filesystem::path p(path);
    llvm::DIFile* f = di_->createFile(p.filename().string(), p.parent_path().string());
    di_files_[id] = f;
    return f;
}

llvm::DIType* Gen::di_basic(const Type& t) {
    const std::string name = typeToString(t);
    switch (t.kind) {
    case TypeKind::Bool:
        return di_->createBasicType(name, 8, llvm::dwarf::DW_ATE_boolean);
    case TypeKind::Int: {
        const unsigned bits = t.bits == 0 ? ptrBits() : t.bits;
        return di_->createBasicType(
            name, bits, t.is_signed ? llvm::dwarf::DW_ATE_signed : llvm::dwarf::DW_ATE_unsigned);
    }
    case TypeKind::Float:
        return di_->createBasicType(name, t.bits, llvm::dwarf::DW_ATE_float);
    case TypeKind::Void:
        return nullptr;
    default:
        return di_->createBasicType(name, ptrBits(), llvm::dwarf::DW_ATE_address);
    }
}

} // namespace xlang::codegen
