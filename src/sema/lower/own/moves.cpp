// Lowering: drop flags, drop statements, moves out of locals and drop-on-assign.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

Lowerer::EP Lowerer::localRef(hir::LocalId id, const Type& type, const Span& span) {
    EP r = mk(hir::ExprKind::LocalRef, type, span);
    r->local = id;
    r->text = fn_ != nullptr && id < fn_->locals.size() ? fn_->locals[id].name : std::string{};
    r->lvalue = true;
    return r;
}

Lowerer::EP Lowerer::boolConst(bool value, const Span& span) {
    EP b = mk(hir::ExprKind::BoolConst, Type::makeBool(), span);
    b->bool_value = value;
    return b;
}

Lowerer::EP Lowerer::dropCallOn(EP addr, const Type& t, const Span& span) {
    EP call = mk(hir::ExprKind::Call, Type::makeVoid(), span);
    call->text = t.kind == TypeKind::Tuple ? tupleDropGlue(t, span) : dropGlueName(t.path.front());
    call->args.push_back(std::move(addr));
    return call;
}

hir::Stmt Lowerer::dropStmt(hir::LocalId var, hir::LocalId flag, const Type& t, const Span& span) {
    hir::Stmt s;
    s.kind = hir::StmtKind::If;
    s.span = span;
    s.value = localRef(flag, Type::makeBool(), span);
    hir::Stmt drop;
    drop.kind = hir::StmtKind::Expr;
    drop.span = span;
    EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(t), span);
    addr->lhs = localRef(var, t, span);
    drop.value = dropCallOn(std::move(addr), t, span);
    hir::Stmt clear;
    clear.kind = hir::StmtKind::Assign;
    clear.span = span;
    clear.target = localRef(flag, Type::makeBool(), span);
    clear.value = boolConst(false, span);
    s.body.push_back(std::move(drop));
    s.body.push_back(std::move(clear));
    return s;
}

void Lowerer::registerDrop(hir::LocalId var,
                           const Type& t,
                           bool initialized,
                           const Span& span,
                           std::vector<hir::Stmt>& decls) {
    const hir::LocalId flag = hiddenLocal("live", Type::makeBool(), span);
    // The flag itself is declared `false` at the function head; here it is set.
    entry_flags_.push_back(flag);
    hir::Stmt decl;
    decl.kind = hir::StmtKind::Assign;
    decl.span = span;
    decl.target = localRef(flag, Type::makeBool(), span);
    decl.value = boolConst(initialized, span);
    decls.push_back(std::move(decl));
    drop_flags_[var] = flag;
    if (!defers_.empty()) {
        hir::Stmt cleanup = dropStmt(var, flag, t, span);
        unwind_entries_.push_back(lower::cloneHirStmt(cleanup));
        defers_.back().push_back(std::move(cleanup));
    }
}

Lowerer::EP Lowerer::moveOut(EP value, bool fromSum) {
    if (!value || !needsDrop(value->type)) {
        return value;
    }
    if (value->kind == hir::ExprKind::LocalRef) {
        hir::LocalId flag;
        if (const auto it = drop_flags_.find(value->local); it != drop_flags_.end()) {
            flag = it->second;
        } else if (sum_views_.contains(value->local)) {
            err("E0720",
                value->span,
                "cannot move a payload out of a `switch` on a field or pointer",
                "switch on a local that owns the value (`switch r`, `r` taken by value), "
                "or use `?`");
            return nullptr;
        } else {
            return value;
        }
        EP m = mk(hir::ExprKind::Move, value->type, value->span);
        m->local = flag;
        m->lhs = std::move(value);
        return m;
    }
    if (value->kind == hir::ExprKind::Field || value->kind == hir::ExprKind::Index ||
        value->kind == hir::ExprKind::Deref) {
        // The payload of a `case` pattern of a payload enum: the switch owns the scrutinee and the
        // glue drops only the live variant, so the binding takes the value and the owner is
        // consumed afterwards.
        if (fromSum) {
            return value;
        }
        err("E0720",
            value->span,
            "cannot move out of a field, element or pointer",
            "take its address (`&x.f`) or copy it through a `clone()` method");
        return nullptr;
    }
    return value;
}
Lowerer::EP Lowerer::materializeTemp(EP value, const Span& span) {
    if (defers_.empty()) {
        return value;
    }
    const Type t = value->type;
    const hir::LocalId tmp = hiddenLocal("tmp", t, span);
    const hir::LocalId flag = hiddenLocal("live", Type::makeBool(), span);
    entry_temps_.emplace_back(tmp, flag);
    hir::Stmt cleanup = dropStmt(tmp, flag, t, span);
    unwind_entries_.push_back(lower::cloneHirStmt(cleanup));
    defers_.back().push_back(std::move(cleanup));
    EP m = mk(hir::ExprKind::Materialize, t, span);
    m->local = tmp;
    m->index = flag;
    m->lhs = std::move(value);
    m->lvalue = true;
    return m;
}

void Lowerer::lowerDropAssign(EP target, EP value, const Span& span, std::vector<hir::Stmt>& out) {
    const Type t = target->type;
    const hir::LocalId tmp = hiddenLocal("asg", t, span);
    hir::Stmt store;
    store.kind = hir::StmtKind::Local;
    store.span = span;
    store.local = tmp;
    store.value = std::move(value);
    out.push_back(std::move(store));
    // The new value is computed first (it may read the old one); then the old one is dropped.
    const auto flag = target->kind == hir::ExprKind::LocalRef ? drop_flags_.find(target->local)
                                                              : drop_flags_.end();
    const bool tracked = flag != drop_flags_.end();
    if (tracked) {
        out.push_back(dropStmt(target->local, flag->second, t, span));
    } else {
        hir::Stmt old;
        old.kind = hir::StmtKind::Expr;
        old.span = span;
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(t), span);
        addr->lhs = lower::cloneHirExpr(*target);
        old.value = dropCallOn(std::move(addr), t, span);
        out.push_back(std::move(old));
    }
    hir::Stmt assign;
    assign.kind = hir::StmtKind::Assign;
    assign.span = span;
    assign.target = std::move(target);
    assign.value = localRef(tmp, t, span);
    out.push_back(std::move(assign));
    if (tracked) {
        hir::Stmt live;
        live.kind = hir::StmtKind::Assign;
        live.span = span;
        live.target = localRef(flag->second, Type::makeBool(), span);
        live.value = boolConst(true, span);
        out.push_back(std::move(live));
    }
}

} // namespace xlang::lower
