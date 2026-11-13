// Lowering: assignment, compound assignment and raw writes.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

void Lowerer::lowerRawWrite(const Expr& call, const Span& span, std::vector<hir::Stmt>& out) {
    if (call.args.size() != 2) {
        err("E0710",
            call.span,
            "`__builtin_write` takes a pointer and a value",
            "write it as `__builtin_write(p, v)`");
        return;
    }
    EP p = expr(*call.args[0], nullptr);
    if (!p) {
        return;
    }
    if (p->type.kind != TypeKind::Pointer || !p->type.elem) {
        err("E0709",
            call.args[0]->span,
            std::format("`__builtin_write` needs a pointer, found `{}`", typeToString(p->type)),
            "pass `&x`");
        return;
    }
    const Type t = *p->type.elem;
    EP v = expr(*call.args[1], &t);
    if (v) {
        v = coerce(std::move(v), t, call.args[1]->span);
    }
    v = moveOut(std::move(v));
    if (!v) {
        return;
    }
    EP target = mk(hir::ExprKind::Deref, t, span);
    target->lhs = std::move(p);
    target->lvalue = true;
    hir::Stmt h;
    h.kind = hir::StmtKind::Assign;
    h.span = span;
    h.target = std::move(target);
    h.value = std::move(v);
    out.push_back(std::move(h));
}

void Lowerer::lowerAssign(const Expr& target_ast,
                          const Expr& value_ast,
                          const Span& span,
                          std::vector<hir::Stmt>& out) {
    if (target_ast.kind == Expr::Kind::Index && userIndexAssign(target_ast, value_ast, span, out)) {
        return; // `a[i] = v` through `operator[]=`
    }
    EP target = expr(target_ast, nullptr);
    if (target && lower::isStrPlace(*target)) {
        err("E0716",
            target_ast.span,
            "cannot assign through a `str`; it is read-only",
            "copy the bytes into a `[u8; N]` or a `String`");
        target.reset();
    }
    if (target && target->readonly) {
        err("E0716",
            target_ast.span,
            std::format("cannot assign to `const` `{}`", lower::placeRootName(*target)),
            "a `const` is read-only; copy it into a `local` to change it");
        target.reset();
    }
    if (target && !target->lvalue && !lower::isUnknown(target->type)) {
        // A write through a read-only view is the common case here: say so, instead of the generic
        // "not a place" line, so the fix (`[]mut T`) is obvious.
        if (target->kind == hir::ExprKind::Index && target->lhs &&
            target->lhs->type.kind == TypeKind::Slice && !target->lhs->type.is_mutable) {
            err("E0716",
                target_ast.span,
                "cannot write through a read-only slice `[]T`",
                "a writable view is `[]mut T`; take one from a mutable owner (`buf[..]`)");
        } else {
            err("E0709",
                target_ast.span,
                "cannot assign to this expression",
                "not a place (variable, `*p`, field or element)");
        }
        target.reset();
    }
    // In a class `init`, `self.f = v` initializes a field that holds zeroed memory, so nothing is
    // dropped.
    const bool raw = in_ctor_ && target_ast.kind == Expr::Kind::Field &&
                     target_ast.lhs != nullptr && target_ast.lhs->kind == Expr::Kind::Name &&
                     target_ast.lhs->text == "self";
    assignTo(std::move(target), value_ast, span, out, raw);
}

void Lowerer::assignTo(
    EP target, const Expr& value_ast, const Span& span, std::vector<hir::Stmt>& out, bool raw) {
    const Type want = target ? target->type : lower::unknownType();
    EP value = expr(value_ast, target ? &want : nullptr);
    if (target && value) {
        value = coerce(std::move(value), want, value_ast.span);
    }
    value = moveOut(std::move(value));
    if (!target || !value) {
        return;
    }
    if (!raw && needsDrop(target->type)) {
        lowerDropAssign(std::move(target), std::move(value), span, out);
        return;
    }
    hir::Stmt h;
    h.span = span;
    h.kind = hir::StmtKind::Assign;
    h.target = std::move(target);
    h.value = std::move(value);
    out.push_back(std::move(h));
}

/// `t op= v`  =>  `t = t op v`; a target with side effects is evaluated once through a pointer.
void Lowerer::lowerCompound(const Stmt& s, std::vector<hir::Stmt>& out) {
    if (!lower::astHasSideEffects(*s.target)) {
        if (userCompound(s, *s.target, out)) {
            return; // `operator+=` / `operator++` of a struct or class place
        }
        const ExprPtr bin = lower::astBinary(
            s.compound_op, lower::cloneAst(*s.target), lower::cloneAst(*s.expr), s.span);
        lowerAssign(*s.target, *bin, s.span, out);
        return;
    }
    EP target = expr(*s.target, nullptr);
    if (!target) {
        return;
    }
    if (target->readonly) {
        err("E0716",
            s.target->span,
            std::format("cannot assign to `const` `{}`", lower::placeRootName(*target)),
            "a `const` is read-only; copy it into a `local` to change it");
        return;
    }
    if (!target->lvalue && !lower::isUnknown(target->type)) {
        err("E0709",
            s.target->span,
            "cannot assign to this expression",
            "not a place (variable, `*p`, field or element)");
        return;
    }
    if (lower::isUnknown(target->type)) {
        return;
    }
    const Type ptr = Type::makePointer(target->type);
    const hir::LocalId id = hiddenLocal("lhs", ptr, s.span);
    const std::string name = fn_->locals[id].name;
    hir::Stmt decl;
    decl.kind = hir::StmtKind::Local;
    decl.span = s.span;
    decl.local = id;
    EP addr = mk(hir::ExprKind::AddrOf, ptr, s.span);
    addr->lhs = std::move(target);
    decl.value = std::move(addr);
    out.push_back(std::move(decl));
    bindLocal(name, id, ptr, s.span);
    const ExprPtr place =
        lower::astUnary(UnaryOp::Deref, lower::astName(name, s.target->span), s.target->span);
    if (userCompound(s, *place, out)) {
        return;
    }
    const ExprPtr bin = lower::astBinary(
        s.compound_op,
        lower::astUnary(UnaryOp::Deref, lower::astName(name, s.target->span), s.target->span),
        lower::cloneAst(*s.expr),
        s.span);
    lowerAssign(*place, *bin, s.span, out);
}

} // namespace xlang::lower
