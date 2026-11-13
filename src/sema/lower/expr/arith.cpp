// Lowering: unary and binary operators and `as` casts.

#include "sema/lower/lower.hpp"

namespace xlang::lower {

/// True when a constant arithmetic expression must not be folded here, because the folded value
/// would be emitted without the conversion back to the expression's type: `-2.0 * count` with
/// `count: f64` folded to `0.0 - 2.0` yields an `i32` constant -2 that the float-printing path
/// reads as a double bit pattern. Comparisons and `&&`/`||` stay folded: they produce a bool and
/// are always correct.
[[nodiscard]] static bool arithmeticFoldUnsafe(const Expr& e) {
    switch (e.kind) {
    case Expr::Kind::Unary:
        return e.unary_op == UnaryOp::Neg || e.unary_op == UnaryOp::BitNot;
    case Expr::Kind::Binary:
        return e.binary_op != BinaryOp::Lt && e.binary_op != BinaryOp::Le &&
               e.binary_op != BinaryOp::Gt && e.binary_op != BinaryOp::Ge &&
               e.binary_op != BinaryOp::Eq && e.binary_op != BinaryOp::Ne &&
               e.binary_op != BinaryOp::And && e.binary_op != BinaryOp::Or;
    case Expr::Kind::Cast:
        return true;
    default:
        return false;
    }
}

Lowerer::EP Lowerer::unaryExpr(const Expr& e, const Type* expected) {
    const std::size_t mark = preludeMark();
    switch (e.unary_op) {
    case UnaryOp::Neg: {
        const Type* hint = nullptr;
        Type want;
        if (expected != nullptr) {
            want = resolved(*expected);
            if (want.kind == TypeKind::Float || (want.kind == TypeKind::Int && want.is_signed)) {
                hint = &want;
            }
        }
        EP v = expr(*e.lhs, hint);
        if (!v || lower::isUnknown(v->type)) {
            return v;
        }
        if (lower::isAggregate(v->type)) {
            return userUnary(e, v->type, mark);
        }
        if (!(v->type.kind == TypeKind::Float ||
              (v->type.kind == TypeKind::Int && v->type.is_signed))) {
            err("E0709",
                e.span,
                std::format("cannot negate `{}`", typeToString(v->type)),
                "negation needs a signed integer or a float");
            return nullptr;
        }
        EP n = mk(hir::ExprKind::Unary, v->type, e.span);
        n->unary_op = UnaryOp::Neg;
        n->lhs = std::move(v);
        return n;
    }
    case UnaryOp::BitNot: {
        EP v = expr(*e.lhs, expected);
        if (!v || lower::isUnknown(v->type)) {
            return v;
        }
        if (lower::isAggregate(v->type)) {
            return userUnary(e, v->type, mark);
        }
        if (v->type.kind != TypeKind::Int) {
            err("E0709",
                e.span,
                std::format("cannot apply `~` to `{}`", typeToString(v->type)),
                "bitwise not needs an integer");
            return nullptr;
        }
        EP n = mk(hir::ExprKind::Unary, v->type, e.span);
        n->unary_op = UnaryOp::BitNot;
        n->lhs = std::move(v);
        return n;
    }
    case UnaryOp::Not: {
        const Type boolean = Type::makeBool();
        EP v = expr(*e.lhs, &boolean);
        if (v && lower::isAggregate(v->type)) {
            return userUnary(e, v->type, mark);
        }
        v = v ? coerce(std::move(v), boolean, e.lhs->span) : nullptr;
        if (!v) {
            return nullptr;
        }
        EP n = mk(hir::ExprKind::Unary, boolean, e.span);
        n->unary_op = UnaryOp::Not;
        n->lhs = std::move(v);
        return n;
    }
    case UnaryOp::Deref: {
        EP v = expr(*e.lhs, nullptr);
        if (!v) {
            return nullptr;
        }
        if (lower::isUnknown(v->type)) {
            return v;
        }
        if (lower::isAggregate(v->type)) {
            return userUnary(e, v->type, mark);
        }
        if (v->type.kind != TypeKind::Pointer || !v->type.elem) {
            err("E0709",
                e.span,
                std::format("cannot dereference `{}`", typeToString(v->type)),
                "not a pointer");
            return nullptr;
        }
        if (v->type.elem->isVoid()) {
            err("E0709", e.span, "cannot dereference `*void`", "cast it to a typed pointer first");
            return nullptr;
        }
        EP d = mk(hir::ExprKind::Deref, *v->type.elem, e.span);
        d->lvalue = true;
        d->lhs = std::move(v);
        return d;
    }
    case UnaryOp::AddrOf: {
        EP v = expr(*e.lhs, nullptr);
        if (!v) {
            return nullptr;
        }
        if (v->kind == hir::ExprKind::FuncRef) {
            return v; // `&fn` is the function pointer itself
        }
        if (!lower::isUnknown(v->type) && !v->lvalue) {
            // `&make()`: the temporary is held in a hidden local declared in front of the
            // statement, so the pointer stays valid until the enclosing block ends (where a
            // droppable temporary is dropped). It is evaluated before the statement's other
            // operands, like a `?`.
            if (prelude_ == nullptr || in_defer_ != 0 || !v->type.hasValue()) {
                err("E0709",
                    e.span,
                    "cannot take the address of this expression",
                    "not a place (variable, `*p`, field or element); bind the value to a local "
                    "first");
                return nullptr;
            }
            const Type t = v->type;
            const hir::LocalId tmp = hiddenLocal("tmp", t, e.span);
            hir::Stmt hold;
            hold.kind = hir::StmtKind::Local;
            hold.span = e.span;
            hold.local = tmp;
            hold.value = moveOut(std::move(v));
            prelude_->push_back(std::move(hold));
            if (needsDrop(t)) {
                registerDrop(tmp, t, true, e.span, *prelude_);
            }
            v = localRef(tmp, t, e.span);
            v->lvalue = true;
        }
        EP a = mk(hir::ExprKind::AddrOf,
                  lower::isUnknown(v->type) ? lower::unknownType() : Type::makePointer(v->type),
                  e.span);
        a->lhs = std::move(v);
        return a;
    }
    }
    return nullptr;
}

Lowerer::EP Lowerer::binaryExpr(const Expr& e, const Type* expected) {
    const BinaryOp op = e.binary_op;
    const Type boolean = Type::makeBool();
    const std::size_t mark = preludeMark();
    if (op == BinaryOp::And || op == BinaryOp::Or) {
        EP l = coerce(expr(*e.lhs, &boolean), boolean, e.lhs->span);
        // The right side runs only when the left one does not decide: what it hoists (`?`,
        // `&temp()`) goes on that path, `local t = l; if t { pre; t = r }` (`if !t` for `||`).
        std::vector<hir::Stmt> pre;
        EP r = hoistedInto(
            pre, [&] { return coerce(expr(*e.rhs, &boolean), boolean, e.rhs->span); });
        if (!l || !r) {
            return nullptr;
        }
        if (pre.empty()) {
            EP b = mk(hir::ExprKind::Binary, boolean, e.span);
            b->binary_op = op;
            b->lhs = std::move(l);
            b->rhs = std::move(r);
            return b;
        }
        const hir::LocalId t = hiddenLocal(op == BinaryOp::And ? "and" : "or", boolean, e.span);
        hir::Stmt hold;
        hold.kind = hir::StmtKind::Local;
        hold.span = e.span;
        hold.local = t;
        hold.value = std::move(l);
        prelude_->push_back(std::move(hold));
        hir::Stmt branch;
        branch.kind = hir::StmtKind::If;
        branch.span = e.span;
        branch.value = localRef(t, boolean, e.span);
        if (op == BinaryOp::Or) {
            EP n = mk(hir::ExprKind::Unary, boolean, e.span);
            n->unary_op = UnaryOp::Not;
            n->lhs = std::move(branch.value);
            branch.value = std::move(n);
        }
        branch.body = std::move(pre);
        hir::Stmt set;
        set.kind = hir::StmtKind::Assign;
        set.span = e.span;
        set.target = localRef(t, boolean, e.span);
        set.target->lvalue = true;
        set.value = std::move(r);
        branch.body.push_back(std::move(set));
        prelude_->push_back(std::move(branch));
        return localRef(t, boolean, e.span);
    }
    const bool is_cmp = op >= BinaryOp::Eq && op <= BinaryOp::Ge;
    const auto isLiteral = [](const Expr& x) {
        return x.kind == Expr::Kind::IntLiteral || x.kind == Expr::Kind::FloatLiteral ||
               (x.kind == Expr::Kind::Unary && x.unary_op == UnaryOp::Neg && x.lhs &&
                (x.lhs->kind == Expr::Kind::IntLiteral || x.lhs->kind == Expr::Kind::FloatLiteral));
    };
    const auto isFloatLiteral = [](const Expr& x) {
        return x.kind == Expr::Kind::FloatLiteral ||
               (x.kind == Expr::Kind::Unary && x.lhs && x.lhs->kind == Expr::Kind::FloatLiteral);
    };
    // A bare literal takes the type of the other operand.
    EP l;
    EP r;
    const Type* hint = is_cmp ? nullptr : expected;
    if (isLiteral(*e.lhs) && (!isLiteral(*e.rhs) || isFloatLiteral(*e.rhs))) {
        r = expr(*e.rhs, hint);
        const Type want = r ? r->type : lower::unknownType();
        l = expr(*e.lhs, r ? &want : hint);
    } else {
        l = expr(*e.lhs, hint);
        const Type want = l ? l->type : lower::unknownType();
        r = expr(*e.rhs, l && lower::isNumeric(l->type) ? &want : hint);
    }
    if (!l || !r) {
        return nullptr;
    }
    if (lower::isUnknown(l->type) || lower::isUnknown(r->type)) {
        EP b = mk(hir::ExprKind::Binary, is_cmp ? boolean : lower::unknownType(), e.span);
        b->binary_op = op;
        b->lhs = std::move(l);
        b->rhs = std::move(r);
        return b;
    }
    if (lower::isAggregate(l->type) || lower::isAggregate(r->type)) {
        return userBinary(e, l->type, r->type, mark); // `operator+` ... of a struct or class
    }

    if (op == BinaryOp::Shl || op == BinaryOp::Shr) {
        // The result has the type of the left operand; the count may be any integer type and is
        // converted to it. A count >= the bit width yields 0 (`>>` of a negative signed value: -1).
        if (l->type.kind != TypeKind::Int || r->type.kind != TypeKind::Int) {
            err("E0709",
                e.span,
                std::format("operator `{}` does not apply to `{}` and `{}`",
                            binaryOpName(op),
                            typeToString(l->type),
                            typeToString(r->type)),
                "shifts need integer operands");
            return nullptr;
        }
        if (!lower::sameType(l->type, r->type)) {
            EP c = mk(hir::ExprKind::Cast, l->type, e.rhs->span);
            c->lhs = std::move(r);
            r = std::move(c);
        }
        EP b = mk(hir::ExprKind::Binary, l->type, e.span);
        b->binary_op = op;
        b->lhs = std::move(l);
        b->rhs = std::move(r);
        return b;
    }
    Type common;
    if (lower::isNumeric(l->type) && lower::isNumeric(r->type)) {
        const CommonTypeResult c = commonBinaryType(l->type, r->type, ptrBits());
        if (c.status == ConversionStatus::Signedness) {
            err("E0601",
                e.span,
                std::format("`{}` and `{}` have different signedness",
                            typeToString(l->type),
                            typeToString(r->type)),
                "write `as` on one side");
            return nullptr;
        }
        if (c.status != ConversionStatus::Ok) {
            err("E0709", e.span, "operands cannot be combined", "incompatible numeric types");
            return nullptr;
        }
        if (c.warn) {
            s_.diag_.warning(
                "W0601",
                e.span,
                std::format("signed integer is implicitly converted to `{}`, precision may be lost",
                            typeToString(c.type)),
                "write `as` to silence");
        }
        common = c.type;
        l = castTo(std::move(l), common, true);
        r = castTo(std::move(r), common, true);
    } else if (is_cmp) {
        // Pointers (also against `nil`), bool, enums, same-type values.
        if (l->kind == hir::ExprKind::Nil && r->type.kind == TypeKind::Pointer) {
            l->type = r->type;
        } else if (r->kind == hir::ExprKind::Nil && l->type.kind == TypeKind::Pointer) {
            r->type = l->type;
        }
        const bool eq_only = op == BinaryOp::Eq || op == BinaryOp::Ne;
        const bool scalar = l->type.kind == TypeKind::Pointer || l->type.kind == TypeKind::Bool ||
                            l->type.kind == TypeKind::Enum || l->type.kind == TypeKind::FnPtr ||
                            (l->type.kind == TypeKind::Str && eq_only); // bytes, not the address
        if (!lower::sameType(l->type, r->type) || !scalar ||
            (!eq_only && l->type.kind != TypeKind::Enum)) {
            err("E0709",
                e.span,
                std::format(
                    "cannot compare `{}` with `{}`", typeToString(l->type), typeToString(r->type)),
                "operands must have the same comparable type");
            return nullptr;
        }
        common = l->type;
    } else {
        err("E0709",
            e.span,
            std::format("operator `{}` does not apply to `{}` and `{}`",
                        binaryOpName(op),
                        typeToString(l->type),
                        typeToString(r->type)),
            "arithmetic needs numeric operands; cast pointers with `as usize`");
        return nullptr;
    }
    if (op == BinaryOp::Mod && common.kind == TypeKind::Float) {
        err("E0709", e.span, "`%` needs integer operands", "floats have no remainder operator");
        return nullptr;
    }
    if ((op == BinaryOp::BitAnd || op == BinaryOp::BitOr || op == BinaryOp::BitXor) &&
        common.kind != TypeKind::Int) {
        err("E0709",
            e.span,
            std::format("operator `{}` needs integer operands", binaryOpName(op)),
            "bitwise operators apply to integers only");
        return nullptr;
    }
    EP b = mk(hir::ExprKind::Binary, is_cmp ? boolean : common, e.span);
    b->binary_op = op;
    b->checked = (op == BinaryOp::Div || op == BinaryOp::Mod) && common.kind == TypeKind::Int;
    b->lhs = std::move(l);
    b->rhs = std::move(r);
    return b;
}

Lowerer::EP Lowerer::castExpr(const Expr& e) {
    const Type to = resolved(e.type);
    EP v = expr(*e.lhs, nullptr);
    if (!v) {
        return nullptr;
    }
    if (v->kind == hir::ExprKind::Nil &&
        (to.kind == TypeKind::Pointer || to.kind == TypeKind::FnPtr)) {
        v->type = to;
        return v;
    }
    if (!castable(v->type, to)) {
        err("E0709",
            e.span,
            std::format("cannot cast `{}` to `{}`", typeToString(v->type), typeToString(to)),
            "no explicit conversion exists");
        return nullptr;
    }
    EP c = mk(hir::ExprKind::Cast, to, e.span);
    c->lhs = std::move(v);
    return c;
}

} // namespace xlang::lower
