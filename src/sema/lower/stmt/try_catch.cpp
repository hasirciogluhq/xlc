// Lowering: `try { } catch e { }`.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

void Lowerer::lowerTry(const Stmt& s, std::vector<hir::Stmt>& out) {
    if (mod_.unwind_function.empty()) {
        err("E0501",
            s.span,
            "`try` has no registered `unwind` function",
            "register one with `register(unwind) fn ...`, or import a library that does");
        return;
    }
    has_try_ = true;
    const Type usize_t = Type::makeInt(0, false);
    const Type byte_t = Type::makeInt(8, false);
    const auto arrayOf = [&](const Type& elem, std::uint64_t n) {
        auto len = std::make_shared<Expr>();
        len->kind = Expr::Kind::IntLiteral;
        len->int_value = n;
        len->span = s.span;
        return Type::makeArray(elem, len);
    };
    const Type snap_t = arrayOf(usize_t, 32);
    const Type msg_t = arrayOf(byte_t, 256);
    const hir::LocalId snap = hiddenLocal("snap", snap_t, s.span);
    const hir::LocalId msg = hiddenLocal("msg", msg_t, s.span);
    for (const hir::LocalId id : {snap, msg}) {
        hir::Stmt decl;
        decl.kind = hir::StmtKind::Local;
        decl.span = s.span;
        decl.local = id;
        out.push_back(std::move(decl));
    }
    hir::Stmt t;
    t.kind = hir::StmtKind::Try;
    t.span = s.span;
    t.local = snap;
    t.msg_local = msg;

    // The body: leaving it by any way runs `leave`; cleanups registered inside it also belong to
    // the handler.
    const std::size_t first = unwind_entries_.size();
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();
    defers_.back().push_back(leaveCall(snap, s.span));
    for (const Stmt& b : s.body->statements) {
        lowerStmt(b, t.body);
    }
    const bool jumps = !t.body.empty() && (t.body.back().kind == hir::StmtKind::Return ||
                                           t.body.back().kind == hir::StmtKind::Break ||
                                           t.body.back().kind == hir::StmtKind::Continue);
    if (!jumps) {
        emitDefers(defers_.size() - 1, t.body);
    }
    defers_.pop_back();
    scopes_.pop();
    const std::size_t last = unwind_entries_.size();

    // The handler runs the body's pending cleanups first, then binds the message.
    scopes_.push(ScopeKind::Block);
    for (std::size_t i = last; i-- > first;) {
        t.else_body.push_back(lower::cloneHirStmt(unwind_entries_[i]));
    }
    if (!s.name.empty()) {
        const Type ptr = Type::makePointer(byte_t);
        const hir::LocalId id = addLocal(s.name, ptr, false, s.span);
        bindLocal(s.name, id, ptr, s.span);
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(msg_t), s.span);
        addr->lhs = localRef(msg, msg_t, s.span);
        EP cast = mk(hir::ExprKind::Cast, ptr, s.span);
        cast->lhs = std::move(addr);
        hir::Stmt bind;
        bind.kind = hir::StmtKind::Local;
        bind.span = s.span;
        bind.local = id;
        bind.value = std::move(cast);
        t.else_body.push_back(std::move(bind));
    }
    lowerBlock(*s.else_body, t.else_body, false);
    scopes_.pop();
    out.push_back(std::move(t));
}

} // namespace xlang::lower
