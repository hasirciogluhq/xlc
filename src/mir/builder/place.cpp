// MIR builder: places. A HIR lvalue becomes a local with projections; any other value used where a
// place is needed (`f().x`, `&g()`) is first stored in a temporary.

#include "mir/builder/body.hpp"

#include <utility>

namespace xlang::mir::builder {

namespace {

[[nodiscard]] bool isPlace(const hir::Expr& e) {
    switch (e.kind) {
    case hir::ExprKind::LocalRef:
    case hir::ExprKind::GlobalRef:
    case hir::ExprKind::External:
    case hir::ExprKind::Field:
    case hir::ExprKind::Index:
    case hir::ExprKind::Deref:
    case hir::ExprKind::RawRead:
    case hir::ExprKind::Materialize: return true;
    default: return false;
    }
}

[[nodiscard]] Place project(Place base, Projection p) {
    base.projections.push_back(p);
    return base;
}

} // namespace

Place BodyBuilder::place(const hir::Expr& e) {
    switch (e.kind) {
    case hir::ExprKind::LocalRef: return {e.local};
    case hir::ExprKind::Field: {
        Place base = e.through_pointer ? pointee(*e.lhs) : place(*e.lhs);
        return project(std::move(base), {.kind = Projection::Kind::Field, .field = e.index});
    }
    case hir::ExprKind::Index: {
        Place base = place(*e.lhs);
        const LocalId index = newTemp(e.rhs->type, e.rhs->span);
        assignTo(Place{index}, *e.rhs);
        return project(std::move(base), {.kind = Projection::Kind::Index, .index = index, .checked = e.checked});
    }
    case hir::ExprKind::Deref:
    case hir::ExprKind::RawRead: return pointee(*e.lhs);
    case hir::ExprKind::Materialize: {
        // The temporary is stored and its drop flag set; the place is the temporary itself.
        assignTo(Place{e.local}, *e.lhs);
        emit({e.index},
             {.kind = Rvalue::Kind::Use,
              .operands = {{.kind = Operand::Kind::Constant,
                            .constant = {Constant::Kind::Bool, Type::makeBool(), true}}}},
             e.span);
        return {e.local};
    }
    case hir::ExprKind::GlobalRef:
    case hir::ExprKind::External: {
        // A global is the place behind its address: `*(&g)`.
        const std::string name = e.kind == hir::ExprKind::GlobalRef ? e.text : e.text + "." + e.member;
        const Type ptr = Type::makePointer(e.type);
        const LocalId addr = newTemp(ptr, e.span);
        emit({addr},
             {.kind = Rvalue::Kind::Use,
              .operands = {{.kind = Operand::Kind::Constant, .constant = {Constant::Kind::Global, ptr, name}}}},
             e.span);
        return project({addr}, {.kind = Projection::Kind::Deref});
    }
    default: return spill(e);
    }
}

/// The place `pointer` points at: `*p` of a pointer place, or of a temporary holding the value.
Place BodyBuilder::pointee(const hir::Expr& pointer) {
    Place base = isPlace(pointer) ? place(pointer) : spill(pointer);
    return project(std::move(base), {.kind = Projection::Kind::Deref});
}

Place BodyBuilder::spill(const hir::Expr& e) {
    const LocalId tmp = newTemp(e.type, e.span);
    assignTo(Place{tmp}, e);
    return {tmp};
}

/// `&p` in a temporary, as an operand.
Operand BodyBuilder::addressOf(Place p, const Type& pointee, const Span& at) {
    const Type ptr = Type::makePointer(pointee);
    const LocalId tmp = newTemp(ptr, at);
    emit({tmp}, {.kind = Rvalue::Kind::AddressOf, .type = ptr, .place = std::move(p)}, at);
    return {.kind = Operand::Kind::Move, .place = {tmp}};
}

} // namespace xlang::mir::builder
