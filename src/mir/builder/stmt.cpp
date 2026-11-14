// MIR builder: statements and the control flow between them.

#include "mir/builder/body.hpp"

namespace xlang::mir::builder {

void BodyBuilder::lowerStmts(const std::vector<hir::Stmt>& list) {
    for (const hir::Stmt& s : list) {
        lowerStmt(s);
    }
}

void BodyBuilder::lowerStmt(const hir::Stmt& s) {
    switch (s.kind) {
    case hir::StmtKind::Local: lowerLocal(s); break;
    case hir::StmtKind::Assign: lowerAssign(s); break;
    case hir::StmtKind::Return: lowerReturn(s); break;
    case hir::StmtKind::Expr: effect(*s.value); break;
    case hir::StmtKind::If: lowerIf(s); break;
    case hir::StmtKind::While: lowerWhile(s); break;
    case hir::StmtKind::Continue:
    case hir::StmtKind::Break: lowerJump(s); break;
    case hir::StmtKind::Block: lowerStmts(s.body); break;
    case hir::StmtKind::Try: lowerTry(s); break;
    }
}

/// `local x = v` stores `v`; `local x` without a value is zeroed (a class also gets its vptr).
void BodyBuilder::lowerLocal(const hir::Stmt& s) {
    const Place dest{s.local};
    if (s.value) {
        assignTo(dest, *s.value);
        return;
    }
    emit(dest, {.kind = Rvalue::Kind::Zero, .type = body_.locals[s.local].type}, s.span);
}

/// The value is evaluated before the target, as code generation does.
void BodyBuilder::lowerAssign(const hir::Stmt& s) {
    Rvalue value = rvalue(*s.value);
    emit(place(*s.target), std::move(value), s.span);
}

void BodyBuilder::lowerIf(const hir::Stmt& s) {
    if (lowerDrop(s)) {
        return;
    }
    const Operand cond = operand(*s.value);
    const BlockId then_block = newBlock();
    const BlockId else_block = newBlock();
    const BlockId join = newBlock();
    terminate({.kind = Terminator::Kind::Branch,
               .operand = cond,
               .targets = {then_block, else_block},
               .span = s.span});
    enter(then_block);
    lowerStmts(s.body);
    fallTo(join, s.span);
    enter(else_block);
    lowerStmts(s.else_body);
    fallTo(join, s.span);
    enter(join);
}

/// `while c { body }`, `do { body } while c` and `for` (the `step` runs after the body and after
/// every `continue`):
///
///   test:  branch c -> body, exit        (a `do` loop enters at `body`)
///   body:  ...; goto step
///   step:  ...; goto test                 (no step: `continue` goes to `test`)
void BodyBuilder::lowerWhile(const hir::Stmt& s) {
    const BlockId test = newBlock();
    const BlockId body = newBlock();
    const BlockId step = s.step.empty() ? test : newBlock();
    const BlockId exit = newBlock();
    fallTo(s.post_test ? body : test, s.span);

    enter(test);
    const Operand cond = operand(*s.value);
    terminate({.kind = Terminator::Kind::Branch,
               .operand = cond,
               .targets = {body, exit},
               .span = s.span});

    enter(body);
    loops_.push_back({step, exit});
    lowerStmts(s.body);
    loops_.pop_back();
    fallTo(step, s.span);

    if (step != test) {
        enter(step);
        lowerStmts(s.step);
        fallTo(test, s.span);
    }
    enter(exit);
}

void BodyBuilder::lowerJump(const hir::Stmt& s) {
    const Loop& loop = loops_[loops_.size() - 1 - s.depth];
    const BlockId target = s.kind == hir::StmtKind::Break ? loop.exit : loop.next;
    terminate({.kind = Terminator::Kind::Goto, .targets = {target}, .span = s.span});
}

void BodyBuilder::lowerReturn(const hir::Stmt& s) {
    Operand value{.kind = Operand::Kind::Constant,
                  .constant = {.kind = Constant::Kind::None, .type = fn_.ret}};
    if (s.value) {
        value = operand(*s.value);
    }
    terminate({.kind = Terminator::Kind::Return, .operand = std::move(value), .span = s.span});
}

/// `try { body } catch { handler }`: the `Try` terminator enters the body; every call and drop in
/// the body unwinds to the handler, so the handler sees the state of any point a panic left from.
void BodyBuilder::lowerTry(const hir::Stmt& s) {
    const BlockId body = newBlock();
    const BlockId handler = newBlock();
    const BlockId end = newBlock();
    terminate({.kind = Terminator::Kind::Try,
               .place = {s.local},
               .message = s.msg_local,
               .targets = {body, handler},
               .span = s.span});
    enter(body);
    catches_.push_back(handler);
    lowerStmts(s.body);
    catches_.pop_back();
    fallTo(end, s.span);
    enter(handler);
    lowerStmts(s.else_body);
    fallTo(end, s.span);
    enter(end);
}

} // namespace xlang::mir::builder
