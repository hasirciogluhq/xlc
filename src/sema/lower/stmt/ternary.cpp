// Lowering: the ternary `c ? a : b`.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

Lowerer::EP Lowerer::ternaryExpr(const Expr& e, const Type* expected) {
    const Type boolean = Type::makeBool();
    EP cond = coerce(expr(*e.lhs, &boolean), boolean, e.lhs->span);
    const Expr& then_ast = *e.rhs;
    const Expr& else_ast = *e.args[0];
    const auto isLit = [](const Expr& x) {
        return x.kind == Expr::Kind::IntLiteral || x.kind == Expr::Kind::FloatLiteral;
    };
    EP a;
    EP b;
    // Only one branch runs, so each hoists (`?`, `&temp()`) into a prelude of its own.
    std::vector<hir::Stmt> pre_a;
    std::vector<hir::Stmt> pre_b;
    if (isLit(then_ast) && !isLit(else_ast)) {
        b = hoistedInto(pre_b, [&] { return expr(else_ast, expected); });
        const Type want = b ? b->type : lower::unknownType();
        a = hoistedInto(pre_a, [&] { return expr(then_ast, b ? &want : expected); });
    } else {
        a = hoistedInto(pre_a, [&] { return expr(then_ast, expected); });
        const Type want = a ? a->type : lower::unknownType();
        b = hoistedInto(pre_b, [&] { return expr(else_ast, a ? &want : expected); });
    }
    if (!cond || !a || !b) {
        return nullptr;
    }
    if (!a->type.hasValue() || lower::isUnknown(a->type)) {
        err("E0708",
            e.span,
            "the branches of `?:` need a value",
            "a `void`, `never` or unknown expression cannot be selected");
        return nullptr;
    }
    const Type t = a->type;
    b = hoistedInto(pre_b, [&] { return coerce(std::move(b), t, else_ast.span); });
    if (!b) {
        return nullptr;
    }
    // A droppable result also takes this form: a `select` would evaluate both branches, so it
    // could not move only the chosen local out (its drop flag stays set and frees the result).
    if (!pre_a.empty() || !pre_b.empty() ||
        (needsDrop(t) && prelude_ != nullptr && in_defer_ == 0)) {
        // `local r; if cond { pre_a; r = a } else { pre_b; r = b }`: `r` starts zeroed and takes
        // the chosen value raw (moved, nothing to drop first); it is dropped like a temporary.
        const hir::LocalId r = hiddenLocal("sel", t, e.span);
        hir::Stmt decl;
        decl.kind = hir::StmtKind::Local;
        decl.span = e.span;
        decl.local = r;
        prelude_->push_back(std::move(decl));
        const auto store = [&](std::vector<hir::Stmt>& path, EP v) {
            hir::Stmt set;
            set.kind = hir::StmtKind::Assign;
            set.span = e.span;
            set.target = localRef(r, t, e.span);
            set.target->lvalue = true;
            set.value = needsDrop(t) ? moveOut(std::move(v)) : std::move(v);
            path.push_back(std::move(set));
            return std::move(path);
        };
        hir::Stmt branch;
        branch.kind = hir::StmtKind::If;
        branch.span = e.span;
        branch.value = std::move(cond);
        branch.body = store(pre_a, std::move(a));
        branch.else_body = store(pre_b, std::move(b));
        prelude_->push_back(std::move(branch));
        if (needsDrop(t)) {
            registerDrop(r, t, true, e.span, *prelude_);
        }
        EP out = localRef(r, t, e.span);
        out->lvalue = true;
        return out;
    }
    EP sel = mk(hir::ExprKind::Select, t, e.span);
    sel->lhs = std::move(cond);
    sel->rhs = std::move(a);
    sel->args.push_back(std::move(b));
    return sel;
}

} // namespace xlang::lower
