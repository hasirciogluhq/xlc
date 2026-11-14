#pragma once

#include "xlang/hir/hir.h"
#include "xlang/mir/mir.h"

#include <optional>
#include <vector>

namespace xlang::mir::builder {

/// Builds the MIR body of one HIR function.
///
/// The HIR locals keep their ids (a MIR local `n` is HIR local `n`); temporaries come after them.
/// Blocks are appended in the order the builder opens them, so the same HIR always gives the same
/// block numbers.
///
/// Evaluation order is the HIR's: every operand that is not a constant is first stored in its own
/// temporary, so a later operand (a call, an assignment) cannot change a value already read.
///
/// The member functions are split by what they lower: `body.cpp` the frame and the blocks,
/// `stmt.cpp` statements, `drop.cpp` the drops of locals, `expr.cpp` values, `place.cpp` places,
/// `call.cpp` calls, builtins, `new` and constructors.
class BodyBuilder {
public:
    explicit BodyBuilder(const hir::Function& f);

    [[nodiscard]] Body finish() &&;

private:
    /// Where `continue` and `break` of an enclosing loop jump to.
    struct Loop {
        BlockId next;
        BlockId exit;
    };

    // body.cpp
    void declareLocals();
    void lowerBody();
    [[nodiscard]] LocalId newTemp(const Type& type, const Span& at);
    [[nodiscard]] BlockId newBlock();
    void enter(BlockId block);
    void emit(Place dest, Rvalue value, const Span& at);
    void terminate(Terminator t);
    void fallTo(BlockId target, const Span& at);
    [[nodiscard]] std::optional<BlockId> unwindTarget() const;
    [[nodiscard]] BasicBlock& cursor();

    // stmt.cpp
    void lowerStmts(const std::vector<hir::Stmt>& list);
    void lowerStmt(const hir::Stmt& s);
    void lowerLocal(const hir::Stmt& s);
    void lowerAssign(const hir::Stmt& s);
    void lowerIf(const hir::Stmt& s);
    void lowerWhile(const hir::Stmt& s);
    void lowerJump(const hir::Stmt& s);
    void lowerReturn(const hir::Stmt& s);
    void lowerTry(const hir::Stmt& s);

    // drop.cpp
    [[nodiscard]] bool lowerDrop(const hir::Stmt& s);

    // expr.cpp
    [[nodiscard]] Operand operand(const hir::Expr& e);
    [[nodiscard]] std::vector<Operand> operands(const std::vector<hir::ExprPtr>& list);
    [[nodiscard]] Rvalue rvalue(const hir::Expr& e);
    void assignTo(const Place& dest, const hir::Expr& e);
    void effect(const hir::Expr& e);
    void lowerSelect(const hir::Expr& e, const std::optional<Place>& dest);
    void lowerShortCircuit(const hir::Expr& e, const Place& dest);

    // place.cpp
    [[nodiscard]] Place place(const hir::Expr& e);
    [[nodiscard]] Place pointee(const hir::Expr& pointer);
    [[nodiscard]] Place spill(const hir::Expr& e);
    [[nodiscard]] Operand addressOf(Place p, const Type& pointee, const Span& at);

    // call.cpp
    void lowerCall(const hir::Expr& e, const std::optional<Place>& dest);
    void lowerBuiltin(const hir::Expr& e, const std::optional<Place>& dest);
    void lowerNew(const hir::Expr& e, const Place& dest);
    void lowerConstruct(const hir::Expr& e, const Place& dest);
    void call(Callee callee,
              std::vector<Operand> args,
              std::optional<Place> dest,
              bool returns,
              const Span& at);

    const hir::Function& fn_;
    Body body_;
    BlockId current_{0};
    bool open_{false}; ///< `current_` still takes statements (no terminator yet)
    std::vector<Loop> loops_;
    std::vector<BlockId> catches_; ///< the `catch` blocks of the enclosing `try`s, innermost last
};

/// The HIR expressions that need control flow or a call, so they are lowered into a destination
/// (`assignTo`) rather than into one rvalue.
[[nodiscard]] bool needsControl(const hir::Expr& e);

} // namespace xlang::mir::builder
