// Internal header of the LLVM backend (plan 4.7): the `Gen` class that turns one HIR module into
// LLVM IR, plus the helpers its parts share. Not installed; include as `codegen/llvm/gen.hpp`.
#pragma once

#include "../artifact.h"
#include "codegen/llvm/c_abi/c_abi.hpp"

#include "xlang/util/error.h"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <map>
#include <optional>
#include <ranges>
#include <set>

namespace xlang::codegen {

namespace gen_detail {

/// An empty block has no terminator (`getTerminator()` must not be called on it).
[[nodiscard]] inline bool hasTerminator(const llvm::BasicBlock* bb) {
    return !bb->empty() && bb->back().isTerminator();
}

/// A type from another module, still unresolved: it has no LLVM type yet.
[[nodiscard]] inline bool isUnknownType(const Type& t) {
    return t.kind == TypeKind::Named;
}

/// Per-module mode: does this module define (not just declare) the item of module `key`?
[[nodiscard]] inline bool definedHere(const CodegenInput& in, const std::string& key) {
    return !in.only_module || *in.only_module == key;
}

/// `MemoryOrder` member index -> LLVM atomic ordering (see xlang-docs/language for the enum).
[[nodiscard]] inline llvm::AtomicOrdering orderingOf(std::uint32_t order) {
    switch (order) {
    case 1:
        return llvm::AtomicOrdering::Acquire;
    case 2:
        return llvm::AtomicOrdering::Release;
    case 3:
        return llvm::AtomicOrdering::AcquireRelease;
    case 4:
        return llvm::AtomicOrdering::SequentiallyConsistent;
    default:
        return llvm::AtomicOrdering::Monotonic;
    }
}

/// Failure ordering of a compare-exchange: no release semantics, not stronger than success.
[[nodiscard]] inline llvm::AtomicOrdering failureOrdering(llvm::AtomicOrdering success) {
    switch (success) {
    case llvm::AtomicOrdering::SequentiallyConsistent:
        return llvm::AtomicOrdering::SequentiallyConsistent;
    case llvm::AtomicOrdering::Acquire:
    case llvm::AtomicOrdering::AcquireRelease:
        return llvm::AtomicOrdering::Acquire;
    default:
        return llvm::AtomicOrdering::Monotonic;
    }
}

} // namespace gen_detail

class Gen {
  public:
    Gen(const CodegenInput& input, Artifact::Impl& out)
        : in_(input), hm_(input.module), ctx_(*out.context), mod_(*out.module), B_(*out.context),
          AB_(*out.context), DL_(out.machine->createDataLayout()), out_(out) {}

    bool run();

  private:
    // ---- types
    [[nodiscard]] unsigned ptrBits() const { return in_.config.target.pointer_width; }
    [[nodiscard]] llvm::Type* ptrTy() { return llvm::PointerType::getUnqual(ctx_); }
    [[nodiscard]] bool isMem(const Type& t) const;
    [[nodiscard]] llvm::Type* sty(const Type& t); ///< SSA type of a scalar type
    [[nodiscard]] llvm::Type* mty(const Type& t); ///< in-memory type of a scalar (bool -> i8)
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t> sizeAlign(const Type& t);
    [[nodiscard]] const TypeLayout* layoutOf(const Type& t);
    [[nodiscard]] llvm::FunctionType* fnTypeFor(const std::vector<Type>& params, const Type& ret);
    [[nodiscard]] bool isSigned(const Type& t) const;
    /// `zeroext`/`signext` for a scalar narrower than 32 bits (C callers widen these), else none.
    [[nodiscard]] std::optional<llvm::Attribute::AttrKind> extensionOf(const Type& t) const;
    /// `[size x i8]`: the memory image of an aggregate (alloca, `byval`, `sret` type).
    [[nodiscard]] llvm::Type* bytesOf(const Type& t);

    // ---- the C convention (c_abi/signature.cpp): `export(c)` definitions and `declare` imports
    [[nodiscard]] static bool usesCAbi(const hir::Function& f) { return f.export_c || f.declared; }
    [[nodiscard]] c_abi::Value cValue(const Type& t);
    void flatten(const Type& t, std::uint64_t base, c_abi::Value& out);
    [[nodiscard]] std::pair<c_abi::Signature, llvm::FunctionType*>
    cFunctionType(const std::vector<Type>& params, const Type& ret);
    /// Parameter and result attributes: `sret`, `byval`/`align`, `noalias`, `zeroext`/`signext`.
    /// `c` is null for the xlang convention, where `params` follow the result slot.
    [[nodiscard]] llvm::AttributeList signatureAttributes(const std::vector<Type>& params,
                                                          const Type& ret,
                                                          const c_abi::Signature* c);
    /// The callee side: the parameter locals of a C-convention function (its prologue).
    void bindCParams(const hir::Function& f, const c_abi::Signature& c);
    /// The callee side: `return value` of a C-convention function.
    void returnC(const hir::Expr& value);
    /// The caller side: a direct call of a C-convention function.
    llvm::Value* genCCall(llvm::Function* callee,
                          const c_abi::Signature& c,
                          const std::vector<const hir::Expr*>& args,
                          const Type& ret,
                          llvm::Value** mem_result,
                          const Span& at);
    /// A temporary that holds `t` and is big enough to load or store `coerce` from it.
    llvm::AllocaInst* coerceSlot(const Type& t, llvm::Type* coerce);

    // ---- module level
    void declareFunction(const hir::Function& f);
    void declareGlobal(const hir::Global& g);
    void buildTables();
    void defineGlobal(const hir::Global& g);
    void defineFunction(const hir::Function& f);
    /// `name()` runs the functions of `fns` in order; teardown passes them reversed.
    template <std::ranges::input_range R> llvm::Function* emitHookAll(std::string name, R&& fns);
    void emitEntry();
    void emitSharedHooks(llvm::Function* init_all, llvm::Function* fini_all);
    [[nodiscard]] const hir::Function* entryDecl() const;
    llvm::GlobalVariable* stringGlobal(const std::string& text);
    llvm::Constant* vtablePointer(const std::string& class_name);
    llvm::Constant* itablePointer(const std::string& type_name, const std::string& iface_name);

    // ---- statements
    void genStmts(const std::vector<hir::Stmt>& body);
    void genStmt(const hir::Stmt& s);

    // ---- expressions
    llvm::Value* genScalar(const hir::Expr& e);
    llvm::Value* genAddr(const hir::Expr& e);
    llvm::Value* genMem(const hir::Expr& e);
    void genInto(const hir::Expr& e, llvm::Value* dest);
    llvm::Value* genCall(const hir::Expr& e, llvm::Value** mem_result);
    /// Calls the `init` function `key` on the object at `self` with the given arguments.
    void callInit(const std::string& key,
                  llvm::Value* self,
                  const std::vector<hir::ExprPtr>& args,
                  const Span& span);
    llvm::Value* genCallCommon(llvm::FunctionType* fty,
                               llvm::Value* callee,
                               const std::vector<const hir::Expr*>& args,
                               llvm::Value* self,
                               const Type& ret,
                               llvm::Value** mem_result,
                               const Span& at);
    /// After a call: a `-> never` callee does not come back, so the code after it is unreachable.
    void afterCall(llvm::CallInst* call, const Type& ret);
    llvm::Value* genSyscall(const hir::Expr& e);
    llvm::Value* genBinary(const hir::Expr& e);
    /// `a == b` / `a != b` on two `str` values: equal lengths and equal bytes (an `i1`).
    llvm::Value* genStrCompare(const hir::Expr& e);
    llvm::Value* genCast(const hir::Expr& e);
    llvm::Value* castScalar(llvm::Value* v, const Type& from, const Type& to);
    llvm::Value* genNew(const hir::Expr& e);
    llvm::Value* genAtomic(const hir::Expr& e);
    llvm::Value* genAsm(const hir::Expr& e);
    llvm::Constant* genConst(const hir::Expr& e);
    llvm::Value* loadScalar(const Type& t, llvm::Value* addr);
    void storeScalar(const Type& t, llvm::Value* value, llvm::Value* addr);
    void copyMem(llvm::Value* dest, llvm::Value* src, const Type& t);
    void zeroMem(llvm::Value* dest, std::uint64_t size, std::uint64_t align);
    llvm::Value* fieldAddr(llvm::Value* base, const Type& agg, std::uint32_t index);
    llvm::Value* byteOffset(llvm::Value* base, std::uint64_t offset);
    llvm::AllocaInst* makeAlloca(llvm::Type* ty, std::uint64_t align, const std::string& name = "");
    llvm::AllocaInst* makeTemp(const Type& t);
    void panicCall(const char* message);
    void checkedBranch(llvm::Value* bad, const char* message);
    void setLoc(const Span& span);
    void error(const Span& at, const std::string& message);
    [[nodiscard]] llvm::Function* functionNamed(const std::string& name);
    [[nodiscard]] const hir::Interface* interfaceNamed(const std::string& name) const;
    [[nodiscard]] const hir::Aggregate* aggregateNamed(const std::string& name) const;
    void ensureTerminated();

    const CodegenInput& in_;
    const hir::Module& hm_;
    llvm::LLVMContext& ctx_;
    llvm::Module& mod_;
    llvm::IRBuilder<> B_;
    llvm::IRBuilder<> AB_; ///< inserts allocas at the start of the entry block (plan 5.1)
    llvm::DataLayout DL_;
    Artifact::Impl& out_;

    std::map<std::string, llvm::Function*> fns_;
    std::map<std::string, llvm::GlobalVariable*> globals_;
    std::map<std::string, llvm::GlobalVariable*> vtables_;
    std::map<std::string, llvm::GlobalVariable*> itables_; ///< key: type + "|" + interface
    std::map<std::string, llvm::GlobalVariable*> strings_;
    std::map<std::string, const hir::Interface*> interfaces_;
    std::map<std::string, const hir::Aggregate*> aggregates_;
    std::map<std::string, const hir::Function*> hir_fns_;
    std::map<std::string, const hir::Global*> hir_globals_;
    std::map<std::string, std::unique_ptr<TypeLayout>> layouts_;
    std::map<std::string, c_abi::Signature> c_sigs_; ///< C-convention functions, by HIR name

    // per-function state
    const hir::Function* cur_{nullptr};
    llvm::Function* cur_fn_{nullptr};
    std::vector<llvm::Value*> locals_;
    llvm::Value* sret_{nullptr};
    const c_abi::Signature* cur_c_{nullptr}; ///< the current function uses the C convention
    struct LoopTargets {
        llvm::BasicBlock* cont; ///< where `continue` goes (step block, or the condition)
        llvm::BasicBlock* brk;  ///< where `break` goes
    };
    std::vector<LoopTargets> loops_; ///< enclosing loops, innermost last
    bool failed_{false};

    // ---- unwinding (landing pads and `try`): see xlang-docs/language/index.md "Panics and `try`"
    struct PadEntry {
        llvm::Function* fn;
        llvm::BasicBlock* pad; ///< null = the function has no cleanups
    };
    std::vector<PadEntry> pads_;     ///< one per defined function, in definition order
    bool escape_{false};             ///< the current function keeps every local in memory
    llvm::BasicBlock* pad_{nullptr}; ///< landing pad of the function being defined
    llvm::GlobalVariable* never_{
        nullptr}; ///< a word that is always 0 (volatile reads are never folded)
    llvm::GlobalVariable* pads_ptr_{
        nullptr}; ///< `ptr` to the table, set when all functions are done
    llvm::GlobalVariable* pads_count_{nullptr};
    void escapeLocal(llvm::Value* slot); ///< forces `slot` to stay in memory (an opaque use)
    void genTry(const hir::Stmt& s);
    void
    emitSnapshot(llvm::Value* buf); ///< writes sp, fp and the callee-saved registers to `buf[1..]`
    void callUnwind(int op, llvm::Value* snap, llvm::Value* msg);
    void emitPad(const hir::Function& f);
    void finishPadTable();

    // debug info
    std::unique_ptr<llvm::DIBuilder> di_;
    llvm::DICompileUnit* cu_{nullptr};
    std::map<FileId, llvm::DIFile*> di_files_;
    llvm::DISubprogram* cur_sp_{nullptr};
    llvm::DIFile* di_file(FileId id);
    llvm::DIType* di_basic(const Type& t);
};

/// The driver's entry point (`generate` in `artifact.cpp`): builds the `Gen` for one input.
[[nodiscard]] bool generateModule(const CodegenInput& input, Artifact::Impl& out);

} // namespace xlang::codegen
