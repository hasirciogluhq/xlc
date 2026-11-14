// MIR builder: values. An expression becomes an operand (a constant or a temporary), an rvalue
// (one MIR computation over operands), or a sequence of blocks that stores into a destination.

#include "mir/builder/body.hpp"

#include <optional>
#include <utility>

namespace xlang::mir::builder {

namespace {

[[nodiscard]] Operand constantOperand(Constant c) {
    return {.kind = Operand::Kind::Constant, .constant = std::move(c)};
}

/// The constant `e` is, when it is one.
[[nodiscard]] std::optional<Constant> constantOf(const hir::Expr& e) {
    using K = Constant::Kind;
    switch (e.kind) {
    case hir::ExprKind::IntConst: return Constant{K::Int, e.type, e.int_value};
    case hir::ExprKind::FloatConst: return Constant{K::Float, e.type, e.float_value};
    case hir::ExprKind::BoolConst: return Constant{K::Bool, e.type, e.bool_value};
    case hir::ExprKind::StrConst: return Constant{K::Str, e.type, e.text};
    case hir::ExprKind::Nil: return Constant{K::Nil, e.type, std::monostate{}};
    case hir::ExprKind::FuncRef: return Constant{K::Function, e.type, e.text};
    default: return std::nullopt;
    }
}

[[nodiscard]] bool isShortCircuit(const hir::Expr& e) {
    return e.kind == hir::ExprKind::Binary &&
           (e.binary_op == BinaryOp::And || e.binary_op == BinaryOp::Or);
}

[[nodiscard]] bool isCall(const hir::Expr& e) {
    switch (e.kind) {
    case hir::ExprKind::Call:
    case hir::ExprKind::CallIndirect:
    case hir::ExprKind::CallVirtual:
    case hir::ExprKind::CallItable:
    case hir::ExprKind::ExternalCall:
    case hir::ExprKind::Syscall:
    case hir::ExprKind::AtomicLoad:
    case hir::ExprKind::AtomicStore:
    case hir::ExprKind::AtomicXchg:
    case hir::ExprKind::AtomicCas:
    case hir::ExprKind::AtomicAdd:
    case hir::ExprKind::Fence:
    case hir::ExprKind::Asm:
    case hir::ExprKind::UnwindPads:
    case hir::ExprKind::UnwindPadCount: return true;
    default: return false;
    }
}

[[nodiscard]] Rvalue use(Operand o) {
    return {.kind = Rvalue::Kind::Use, .operands = {std::move(o)}};
}

} // namespace

bool needsControl(const hir::Expr& e) {
    return isCall(e) || isShortCircuit(e) || e.kind == hir::ExprKind::Select ||
           e.kind == hir::ExprKind::New || e.kind == hir::ExprKind::Construct;
}

/// A constant stays a constant; anything else is evaluated now into its own temporary, which the
/// operand then moves out of.
Operand BodyBuilder::operand(const hir::Expr& e) {
    if (std::optional<Constant> c = constantOf(e)) {
        return constantOperand(std::move(*c));
    }
    const LocalId tmp = newTemp(e.type, e.span);
    assignTo(Place{tmp}, e);
    return {.kind = Operand::Kind::Move, .place = {tmp}};
}

std::vector<Operand> BodyBuilder::operands(const std::vector<hir::ExprPtr>& list) {
    std::vector<Operand> out;
    out.reserve(list.size());
    for (const hir::ExprPtr& e : list) {
        out.push_back(operand(*e));
    }
    return out;
}

/// `e` as one rvalue. The statements its operands need are emitted first.
Rvalue BodyBuilder::rvalue(const hir::Expr& e) {
    if (std::optional<Constant> c = constantOf(e)) {
        return use(constantOperand(std::move(*c)));
    }
    if (needsControl(e)) {
        return use(operand(e));
    }
    switch (e.kind) {
    case hir::ExprKind::Move: {
        // The flag is cleared first: nothing between the two statements reads it.
        const hir::Expr& from = *e.lhs;
        emit({e.local}, use(constantOperand({Constant::Kind::Bool, Type::makeBool(), false})), e.span);
        return use({.kind = Operand::Kind::Move, .place = place(from)});
    }
    case hir::ExprKind::Unary:
        return {.kind = Rvalue::Kind::Unary, .type = e.type, .operands = {operand(*e.lhs)}, .unary_op = e.unary_op};
    case hir::ExprKind::Binary: {
        Operand lhs = operand(*e.lhs);
        Operand rhs = operand(*e.rhs);
        return {.kind = Rvalue::Kind::Binary,
                .type = e.type,
                .operands = {std::move(lhs), std::move(rhs)},
                .binary_op = e.binary_op,
                .checked = e.checked};
    }
    case hir::ExprKind::Cast:
        return {.kind = Rvalue::Kind::Cast, .type = e.type, .operands = {operand(*e.lhs)}};
    case hir::ExprKind::AddrOf:
        return {.kind = Rvalue::Kind::AddressOf, .type = e.type, .place = place(*e.lhs)};
    case hir::ExprKind::Slice: {
        Place base = place(*e.lhs);
        const auto bound = [&](const hir::Expr* b) {
            return b != nullptr ? operand(*b) : constantOperand({Constant::Kind::None, Type::makeVoid(), std::monostate{}});
        };
        Operand lo = bound(e.args.empty() ? nullptr : e.args[0].get());
        Operand hi = bound(e.rhs.get());
        return {.kind = Rvalue::Kind::Slice,
                .type = e.type,
                .place = std::move(base),
                .operands = {std::move(lo), std::move(hi)},
                .checked = e.checked};
    }
    case hir::ExprKind::ArrayList:
    case hir::ExprKind::TupleLit:
        return {.kind = Rvalue::Kind::Aggregate, .type = e.type, .operands = operands(e.args)};
    case hir::ExprKind::StructLit: {
        Rvalue r{.kind = Rvalue::Kind::Aggregate, .type = e.type};
        for (const hir::FieldInit& f : e.fields) {
            r.operands.push_back(operand(*f.value));
            r.fields.push_back(f.index);
        }
        return r;
    }
    case hir::ExprKind::ArrayRepeat:
        return {.kind = Rvalue::Kind::Repeat, .type = e.type, .operands = {operand(*e.lhs)}, .count = e.int_value};
    case hir::ExprKind::ToInterface:
        return {.kind = Rvalue::Kind::ToInterface, .type = e.type, .operands = {operand(*e.lhs)}, .name = e.text};
    default:
        // A place read by value: a local, a field, an element, `*p`, `__builtin_read(p)`, a
        // materialized temporary, a global.
        return use({.kind = Operand::Kind::Copy, .place = place(e)});
    }
}

/// Stores the value of `e` into `dest`.
void BodyBuilder::assignTo(const Place& dest, const hir::Expr& e) {
    if (isCall(e)) {
        lowerCall(e, dest);
    } else if (isShortCircuit(e)) {
        lowerShortCircuit(e, dest);
    } else if (e.kind == hir::ExprKind::Select) {
        lowerSelect(e, dest);
    } else if (e.kind == hir::ExprKind::New) {
        lowerNew(e, dest);
    } else if (e.kind == hir::ExprKind::Construct) {
        lowerConstruct(e, dest);
    } else {
        Rvalue value = rvalue(e);
        emit(dest, std::move(value), e.span);
    }
}

/// `e` evaluated for its effects only (an expression statement).
void BodyBuilder::effect(const hir::Expr& e) {
    const bool is_void = e.type.kind == TypeKind::Void || e.type.kind == TypeKind::Never;
    if (is_void && isCall(e)) {
        lowerCall(e, std::nullopt);
    } else if (is_void && e.kind == hir::ExprKind::Select) {
        lowerSelect(e, std::nullopt);
    } else {
        assignTo(Place{newTemp(e.type, e.span)}, e);
    }
}

/// `c ? a : b`: only the chosen arm runs. Without a destination the arms run for their effects.
void BodyBuilder::lowerSelect(const hir::Expr& e, const std::optional<Place>& dest) {
    const Operand cond = operand(*e.lhs);
    const BlockId yes = newBlock();
    const BlockId no = newBlock();
    const BlockId join = newBlock();
    terminate({.kind = Terminator::Kind::Branch, .operand = cond, .targets = {yes, no}, .span = e.span});
    const auto arm = [&](BlockId block, const hir::Expr& value) {
        enter(block);
        if (dest) {
            assignTo(*dest, value);
        } else {
            effect(value);
        }
        fallTo(join, e.span);
    };
    arm(yes, *e.rhs);
    arm(no, *e.args[0]);
    enter(join);
}

/// `a && b` and `a || b`: `b` runs only when `a` does not decide the result.
void BodyBuilder::lowerShortCircuit(const hir::Expr& e, const Place& dest) {
    const bool is_and = e.binary_op == BinaryOp::And;
    const Operand lhs = operand(*e.lhs);
    const BlockId rhs = newBlock();
    const BlockId decided = newBlock();
    const BlockId join = newBlock();
    terminate({.kind = Terminator::Kind::Branch,
               .operand = lhs,
               .targets = is_and ? std::vector{rhs, decided} : std::vector{decided, rhs},
               .span = e.span});
    enter(decided);
    emit(dest, use(constantOperand({Constant::Kind::Bool, Type::makeBool(), !is_and})), e.span);
    fallTo(join, e.span);
    enter(rhs);
    assignTo(dest, *e.rhs);
    fallTo(join, e.span);
    enter(join);
}

} // namespace xlang::mir::builder
