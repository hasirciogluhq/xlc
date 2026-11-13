// Lowering: locals, blocks, scopes and the deferred statements run when a block ends.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

// ---------------------------------------------------------------------------------------------
// Statements

hir::LocalId
Lowerer::addLocal(const std::string& name, const Type& type, bool is_param, const Span& span) {
    fn_->locals.push_back(hir::Local{name, type, is_param, span});
    return static_cast<hir::LocalId>(fn_->locals.size() - 1);
}

void Lowerer::bindLocal(const std::string& name,
                        hir::LocalId id,
                        const Type& type,
                        const Span& span) {
    if (isRegisteredName(name)) {
        s_.diag_.warning("W0402",
                         span,
                         std::format("`{}` shadows a registered global", name),
                         "a `register`ed name is hidden here; its role binding is unaffected");
    }
    ScopeEntry e;
    e.kind = ScopeEntry::Kind::Local;
    e.name = name;
    e.type = type;
    e.span = span;
    e.local = id;
    if (const ScopeEntry* prev = scopes_.declare(std::move(e))) {
        Diagnostic d;
        d.code = "E0401";
        d.severity = Severity::Error;
        d.message = std::format("`{}` is defined twice in this scope", name);
        d.labels.push_back({prev->span, "first definition"});
        d.labels.push_back({span, "second definition"});
        d.fix = "rename one of them";
        s_.diag_.report(std::move(d));
    }
}

void Lowerer::lowerBlock(const Block& block, std::vector<hir::Stmt>& out, bool new_scope) {
    if (new_scope) {
        scopes_.push(ScopeKind::Block);
    }
    defers_.emplace_back();
    for (const Stmt& s : block.statements) {
        lowerStmt(s, out);
    }
    // Deferred statements run when the block is left by falling off its end; `return`, `break`
    // and `continue` emit them at the jump.
    const bool jumps = !out.empty() && (out.back().kind == hir::StmtKind::Return ||
                                        out.back().kind == hir::StmtKind::Break ||
                                        out.back().kind == hir::StmtKind::Continue);
    if (!jumps) {
        emitDefers(defers_.size() - 1, out);
    }
    defers_.pop_back();
    if (new_scope) {
        scopes_.pop();
    }
}

hir::Stmt Lowerer::leaveCall(hir::LocalId snap, const Span& span) {
    const Type usize_t = Type::makeInt(0, false);
    EP call = mk(hir::ExprKind::Call, Type::makeVoid(), span);
    call->text = mod_.unwind_function;
    call->args.push_back(intConst(0, Type::makeInt(32, true), span));
    EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(fn_->locals[snap].type), span);
    addr->lhs = localRef(snap, fn_->locals[snap].type, span);
    EP cast = mk(hir::ExprKind::Cast, Type::makePointer(usize_t), span);
    cast->lhs = std::move(addr);
    call->args.push_back(std::move(cast));
    call->args.push_back(mk(hir::ExprKind::Nil, Type::makePointer(Type::makeInt(8, false)), span));
    hir::Stmt st;
    st.kind = hir::StmtKind::Expr;
    st.span = span;
    st.value = std::move(call);
    return st;
}

void Lowerer::emitDefers(std::size_t from, std::vector<hir::Stmt>& out) const {
    for (std::size_t f = defers_.size(); f-- > from;) {
        const auto& frame = defers_[f];
        for (std::size_t i = frame.size(); i-- > 0;) {
            out.push_back(lower::cloneHirStmt(frame[i]));
        }
    }
}

hir::LocalId Lowerer::hiddenLocal(const char* prefix, const Type& type, const Span& span) {
    return addLocal(std::format("${}{}", prefix, hidden_counter_++), type, false, span);
}

} // namespace xlang::lower
