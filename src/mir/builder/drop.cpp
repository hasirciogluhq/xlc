// MIR builder: the drop of a local.
//
// Lowering writes the drop of a droppable local as a flag-guarded statement
// (`dropStmt` in sema/lower/own/moves.cpp):
//
//   if $live { glue(&x); $live = false }
//
// The builder turns it into a `Drop` terminator behind the flag test, so the analyses see a drop
// of the place `x`, not a call that borrows it:
//
//   branch copy $live -> drop, join
//   drop:  drop x (glue) -> clear
//   clear: $live = false; goto join

#include "mir/builder/body.hpp"

#include <optional>
#include <string>

namespace xlang::mir::builder {

namespace {

struct DropSite {
    LocalId var;
    LocalId flag;
    std::string glue;
};

[[nodiscard]] bool isLocalRef(const hir::Expr* e) {
    return e != nullptr && e->kind == hir::ExprKind::LocalRef;
}

/// The local, flag and glue of `s` when it has exactly the shape `dropStmt` builds.
[[nodiscard]] std::optional<DropSite> dropSite(const hir::Stmt& s, const Body& body) {
    if (!s.else_body.empty() || s.body.size() != 2 || !isLocalRef(s.value.get()) ||
        body.locals[s.value->local].kind != Local::Kind::DropFlag) {
        return std::nullopt;
    }
    const hir::Stmt& drop = s.body[0];
    const hir::Stmt& clear = s.body[1];
    const hir::Expr* call = drop.kind == hir::StmtKind::Expr ? drop.value.get() : nullptr;
    if (call == nullptr || call->kind != hir::ExprKind::Call || call->args.size() != 1 ||
        call->args[0]->kind != hir::ExprKind::AddrOf || !isLocalRef(call->args[0]->lhs.get())) {
        return std::nullopt;
    }
    const LocalId flag = s.value->local;
    if (clear.kind != hir::StmtKind::Assign || !isLocalRef(clear.target.get()) ||
        clear.target->local != flag || clear.value->kind != hir::ExprKind::BoolConst ||
        clear.value->bool_value) {
        return std::nullopt;
    }
    return DropSite{call->args[0]->lhs->local, flag, call->text};
}

} // namespace

/// Lowers `s` as a drop and returns true, or returns false when `s` is not one.
bool BodyBuilder::lowerDrop(const hir::Stmt& s) {
    const std::optional<DropSite> site = dropSite(s, body_);
    if (!site) {
        return false;
    }
    const BlockId drop = newBlock();
    const BlockId clear = newBlock();
    const BlockId join = newBlock();
    terminate({.kind = Terminator::Kind::Branch,
               .operand = {.kind = Operand::Kind::Copy, .place = {site->flag}},
               .targets = {drop, join},
               .span = s.span});
    enter(drop);
    terminate({.kind = Terminator::Kind::Drop,
               .place = {site->var},
               .glue = site->glue,
               .targets = {clear},
               .unwind = unwindTarget(),
               .span = s.span});
    enter(clear);
    emit({site->flag},
         {.kind = Rvalue::Kind::Use,
          .operands = {{.kind = Operand::Kind::Constant,
                        .constant = {.kind = Constant::Kind::Bool, .type = Type::makeBool(), .value = false}}}},
         s.span);
    fallTo(join, s.span);
    enter(join);
    return true;
}

} // namespace xlang::mir::builder
