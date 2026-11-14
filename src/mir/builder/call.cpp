// MIR builder: calls of every kind, the compiler's builtins, `new` and class constructors. Each call
// ends its block, so a panic inside it has an edge to the enclosing `catch`.

#include "mir/builder/body.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace xlang::mir::builder {

namespace {

[[nodiscard]] bool returns(const hir::Expr& e) {
    return e.type.kind != TypeKind::Never && !(e.kind == hir::ExprKind::Asm && e.noreturn);
}

[[nodiscard]] Operand copyOf(const Place& p) {
    return {.kind = Operand::Kind::Copy, .place = p};
}

[[nodiscard]] Operand constantOperand(Constant::Kind kind, Type type, decltype(Constant::value) value) {
    return {.kind = Operand::Kind::Constant, .constant = {kind, std::move(type), std::move(value)}};
}

} // namespace

/// A call to `callee`; with `returns` false (`-> never`) nothing follows it.
void BodyBuilder::call(Callee callee,
                       std::vector<Operand> args,
                       std::optional<Place> dest,
                       bool returns,
                       const Span& at) {
    Terminator t{.kind = Terminator::Kind::Call,
                 .callee = std::move(callee),
                 .args = std::move(args),
                 .destination = std::move(dest),
                 .unwind = unwindTarget(),
                 .span = at};
    if (!returns) {
        terminate(std::move(t));
        return;
    }
    const BlockId next = newBlock();
    t.targets = {next};
    terminate(std::move(t));
    enter(next);
}

void BodyBuilder::lowerCall(const hir::Expr& e, const std::optional<Place>& dest) {
    const bool has_value = e.type.kind != TypeKind::Void && e.type.kind != TypeKind::Never;
    const std::optional<Place> to = has_value ? dest : std::nullopt;
    Callee callee;
    switch (e.kind) {
    case hir::ExprKind::Call: callee = {.kind = Callee::Kind::Direct, .name = e.text}; break;
    case hir::ExprKind::ExternalCall:
        callee = {.kind = Callee::Kind::Direct, .name = e.text + "." + e.member};
        break;
    case hir::ExprKind::CallIndirect:
        callee = {.kind = Callee::Kind::Indirect, .value = operand(*e.lhs)};
        break;
    case hir::ExprKind::CallVirtual:
        callee = {.kind = Callee::Kind::Virtual, .name = e.text, .index = e.index};
        break;
    case hir::ExprKind::CallItable:
        callee = {.kind = Callee::Kind::Interface, .name = e.text, .index = e.index, .value = operand(*e.lhs)};
        break;
    case hir::ExprKind::Syscall: callee = {.kind = Callee::Kind::Syscall}; break;
    default: lowerBuiltin(e, to); return;
    }
    std::vector<Operand> args = operands(e.args);
    call(std::move(callee), std::move(args), to, returns(e), e.span);
}

/// Atomics, fences, `asm` and the unwind table: the operands in the order the HIR gives them.
void BodyBuilder::lowerBuiltin(const hir::Expr& e, const std::optional<Place>& dest) {
    Callee callee{.kind = Callee::Kind::Builtin, .index = e.index};
    std::vector<Operand> args;
    const auto arg = [&](const hir::ExprPtr& x) {
        if (x) {
            args.push_back(operand(*x));
        }
    };
    switch (e.kind) {
    case hir::ExprKind::AtomicLoad: callee.builtin = Builtin::AtomicLoad; break;
    case hir::ExprKind::AtomicStore: callee.builtin = Builtin::AtomicStore; break;
    case hir::ExprKind::AtomicXchg: callee.builtin = Builtin::AtomicXchg; break;
    case hir::ExprKind::AtomicCas: callee.builtin = Builtin::AtomicCas; break;
    case hir::ExprKind::AtomicAdd: callee.builtin = Builtin::AtomicAdd; break;
    case hir::ExprKind::Fence: callee.builtin = Builtin::Fence; break;
    case hir::ExprKind::UnwindPads: callee.builtin = Builtin::UnwindPads; break;
    case hir::ExprKind::UnwindPadCount: callee.builtin = Builtin::UnwindPadCount; break;
    case hir::ExprKind::Asm:
        callee.builtin = Builtin::Asm;
        callee.name = e.text;
        // An input is read; an output (and an in-out) is written through its address.
        for (const hir::AsmOperand& op : e.asm_operands) {
            args.push_back(op.kind == hir::AsmOperand::Kind::In
                               ? operand(*op.value)
                               : addressOf(place(*op.value), op.value->type, op.value->span));
        }
        break;
    default: break;
    }
    if (e.kind != hir::ExprKind::Asm) {
        arg(e.lhs);
        arg(e.rhs);
        for (const hir::ExprPtr& x : e.args) {
            arg(x);
        }
    }
    call(std::move(callee), std::move(args), dest, returns(e), e.span);
}

/// `new T { fields }` and `new T(args)`, placed (`new(p) T`) or from the `register(alloc)`
/// function. A nil pointer (a failed allocation) is the result as is: nothing is written.
///
///   dest = alloc(T) | p
///   branch dest != nil -> init, end
///   init: *dest = zeroed T; T.init(dest, args); (*dest).f = v ...; goto end
void BodyBuilder::lowerNew(const hir::Expr& e, const Place& dest) {
    if (e.lhs) {
        assignTo(dest, *e.lhs);
    } else {
        call({.kind = Callee::Kind::Builtin, .builtin = Builtin::Alloc, .type = e.target}, {}, dest, true, e.span);
    }
    const LocalId ok = newTemp(Type::makeBool(), e.span);
    emit({ok},
         {.kind = Rvalue::Kind::Binary,
          .type = Type::makeBool(),
          .operands = {copyOf(dest), constantOperand(Constant::Kind::Nil, e.type, std::monostate{})},
          .binary_op = BinaryOp::Ne},
         e.span);
    const BlockId init = newBlock();
    const BlockId end = newBlock();
    terminate({.kind = Terminator::Kind::Branch,
               .operand = {.kind = Operand::Kind::Move, .place = {ok}},
               .targets = {init, end},
               .span = e.span});
    enter(init);
    Place object = dest;
    object.projections.push_back({.kind = Projection::Kind::Deref});
    emit(object, {.kind = Rvalue::Kind::Zero, .type = e.target}, e.span);
    if (!e.text.empty()) {
        std::vector<Operand> args{copyOf(dest)};
        std::ranges::move(operands(e.args), std::back_inserter(args));
        call({.kind = Callee::Kind::Direct, .name = e.text}, std::move(args), std::nullopt, true, e.span);
    }
    for (const hir::FieldInit& f : e.fields) {
        Place field = object;
        field.projections.push_back({.kind = Projection::Kind::Field, .field = f.index});
        assignTo(field, *f.value);
    }
    fallTo(end, e.span);
    enter(end);
}

/// `T(args)` of a class: a zeroed object with its vptr, then its `init` on it.
void BodyBuilder::lowerConstruct(const hir::Expr& e, const Place& dest) {
    emit(dest, {.kind = Rvalue::Kind::Zero, .type = e.target}, e.span);
    if (e.text.empty()) {
        return;
    }
    std::vector<Operand> args{addressOf(dest, e.target, e.span)};
    std::ranges::move(operands(e.args), std::back_inserter(args));
    call({.kind = Callee::Kind::Direct, .name = e.text}, std::move(args), std::nullopt, true, e.span);
}

} // namespace xlang::mir::builder
