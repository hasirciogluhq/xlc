#include "sema/lower/lower.hpp"

namespace xlang::lower {

// The `?` operator. `e?` evaluates `e`, a payload enum with a success variant and a failure
// variant, and either yields the success value or returns the failure from the function.
//
// Shapes (found structurally, no name is known to the compiler):
//   - two variants, the first with one value and the second with at most one: the first is the
//     success (`Ok(T)` / `Err(E)`; `Some(T)` / `None`);
//   - two variants, the first without data and the second with one value: the second is the
//     success (`None` / `Some(T)`).
// The function must return an enum of such a shape whose failure carries the same type.
//
// An expression cannot return, so the lowering is hoisted: the statement being lowered gets
//     local try = e;  if try is failure { return Failure(try.payload) }  local ok = try.payload
// in front of it (`prelude_`) and `ok` stands where `e?` was. Because of that, the `?` of a
// statement run before its other operands, in the order written. An operand that runs only on
// some paths hoists into a prelude of its own that is placed on that path (`hoistedInto`): the
// right side of `&&`/`||` and a `?:` branch become an `if`, a loop condition is tested at the top
// of the body (`guardLoop`).

namespace {

struct Shape {
    std::size_t value; ///< the success variant
    std::size_t fail;  ///< the failure variant
};

std::optional<Shape> shapeOfVariants(const std::vector<SumVariant>& v) {
    if (v.size() != 2) {
        return std::nullopt;
    }
    if (v[0].payload.size() == 1 && v[1].payload.size() <= 1) {
        return Shape{0, 1};
    }
    if (v[0].payload.empty() && v[1].payload.size() == 1) {
        return Shape{1, 0};
    }
    return std::nullopt;
}

} // namespace

Lowerer::EP Lowerer::tryExpr(const Expr& e) {
    if (in_defer_ != 0) {
        // A deferred statement runs while the block is being left, possibly while a `return`
        // is already under way: a second, early return from there has no meaning.
        err("E0761",
            e.span,
            "`?` cannot be used in a `defer`",
            "a deferred statement runs while the block is left and cannot return; handle the "
            "failure with `switch` or `if` inside the deferred statement");
        return nullptr;
    }
    if (prelude_ == nullptr) {
        err("E0761", e.span, "`?` cannot be used here", "it needs a function body to return from");
        return nullptr;
    }
    EP inner = expr(*e.lhs, nullptr);
    if (!inner || lower::isUnknown(inner->type)) {
        return nullptr;
    }
    const auto sum = sumOfType(inner->type);
    const auto shape = sum ? shapeOfVariants(sum->decl->variants) : std::nullopt;
    if (!shape) {
        err("E0762",
            e.lhs->span,
            std::format("`?` needs a `Result`-like or `Option`-like enum, found `{}`",
                        typeToString(inner->type)),
            "its variants are a success with one value and a failure (`Ok(T)`/`Err(E)`, "
            "`Some(T)`/`None`)");
        return nullptr;
    }
    const auto result = sumOfType(ret_);
    const auto result_shape = result ? shapeOfVariants(result->decl->variants) : std::nullopt;
    if (!result_shape) {
        if (!lower::isUnknown(ret_)) {
            err("E0763",
                e.span,
                std::format("`?` returns the failure, but the function returns `{}`",
                            typeToString(ret_)),
                "the function must return a `Result`-like or `Option`-like enum too");
        }
        return nullptr;
    }
    const SumVariant& failure = sum->decl->variants[shape->fail];
    const SumVariant& returned = result->decl->variants[result_shape->fail];
    if (failure.payload.size() != returned.payload.size() ||
        (!failure.payload.empty() &&
         typeToString(resolvedIn(*sum->owner, failure.payload[0])) !=
             typeToString(resolvedIn(*result->owner, returned.payload[0])))) {
        err("E0763",
            e.span,
            std::format("`?` cannot return the failure of `{}` from a function returning `{}`",
                        typeToString(inner->type),
                        typeToString(ret_)),
            "the failure must carry the same type in both (there is no conversion)");
        return nullptr;
    }

    const Type self = inner->type;
    const Type boolean = Type::makeBool();
    const hir::LocalId held = hiddenLocal("try", self, e.span);
    // The value is moved into `held`; its payload is taken below, so `held` is never dropped.
    inner = moveOut(std::move(inner));
    if (!inner) {
        return nullptr;
    }
    hir::Stmt keep;
    keep.kind = hir::StmtKind::Local;
    keep.span = e.span;
    keep.local = held;
    keep.value = std::move(inner);
    prelude_->push_back(std::move(keep));

    // if (held.tag == failure) { return Failure(held.payload) }
    EP tag = mk(hir::ExprKind::Field, sumPart(*sum, "$Tag", e.span), e.span);
    tag->text = "tag";
    tag->index = 0;
    tag->lvalue = true;
    tag->lhs = localRef(held, self, e.span);
    tag->lhs->lvalue = true;
    EP is_failure = mk(hir::ExprKind::Binary, boolean, e.span);
    is_failure->binary_op = BinaryOp::Eq;
    is_failure->lhs = std::move(tag);
    is_failure->rhs = intConst(shape->fail, sumPart(*sum, "$Tag", e.span), e.span);

    hir::Stmt branch;
    branch.kind = hir::StmtKind::If;
    branch.span = e.span;
    branch.value = std::move(is_failure);
    std::vector<EP> carried;
    if (!failure.payload.empty()) {
        carried.push_back(moveOut(sumPayloadRead(*sum, held, self, shape->fail, 0, e.span), true));
    }
    hir::Stmt leave;
    leave.kind = hir::StmtKind::Return;
    leave.span = e.span;
    leave.value = sumConstruct(*result, result_shape->fail, std::move(carried), e.span);
    if (std::ranges::any_of(defers_, [](const auto& frame) { return !frame.empty(); })) {
        // The returned value is computed before the deferred statements run.
        const Type returned_type = leave.value->type;
        const hir::LocalId ret = hiddenLocal("ret", returned_type, e.span);
        hir::Stmt store;
        store.kind = hir::StmtKind::Local;
        store.span = e.span;
        store.local = ret;
        store.value = std::move(leave.value);
        branch.body.push_back(std::move(store));
        leave.value = localRef(ret, returned_type, e.span);
        leave.value->lvalue = true;
        emitDefers(0, branch.body);
    }
    branch.body.push_back(std::move(leave));
    prelude_->push_back(std::move(branch));

    // local ok = held.payload; it owns a droppable value, so the statement that uses `ok` moves it.
    EP read = moveOut(sumPayloadRead(*sum, held, self, shape->value, 0, e.span), true);
    const Type value_type = read->type;
    const hir::LocalId ok = hiddenLocal("ok", value_type, e.span);
    if (needsDrop(value_type)) {
        registerDrop(ok, value_type, true, e.span, *prelude_);
    }
    hir::Stmt take;
    take.kind = hir::StmtKind::Local;
    take.span = e.span;
    take.local = ok;
    take.value = std::move(read);
    prelude_->push_back(std::move(take));
    EP result_value = localRef(ok, value_type, e.span);
    result_value->lvalue = true;
    return result_value;
}

} // namespace xlang::lower
