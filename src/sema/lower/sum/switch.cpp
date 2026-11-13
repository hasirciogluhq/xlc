// Lowering: `switch` on a payload enum: arms, bindings and exhaustiveness.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

/// `switch` on a payload enum. A case names variants: `case E.A(x, y) { }` binds the payload,
/// `case E.A, E.B { }` matches without binding, `E.C` for a variant without data. Without
/// `default` every variant must be covered (E0750).
void Lowerer::lowerSumSwitch(const Stmt& s, EP scrutinee, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    const Sum sum = *sumOfType(scrutinee->type);
    const Type self = scrutinee->type;
    const Type tag_type = sumPart(sum, "$Tag", s.span);
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    const hir::LocalId id = hiddenLocal("sw", self, s.span);
    // Who owns the payloads the bindings view: a local with a drop flag (moving a binding out
    // clears it), a place (nothing can be moved out), or a temporary (no entry: it is not dropped).
    std::optional<std::optional<hir::LocalId>> owner;
    if (scrutinee->kind == hir::ExprKind::LocalRef) {
        if (const auto it = drop_flags_.find(scrutinee->local); it != drop_flags_.end()) {
            owner = it->second;
        } else if (const auto b = sum_bindings_.find(scrutinee->local); b != sum_bindings_.end()) {
            owner = b->second;
        }
    } else if (scrutinee->kind == hir::ExprKind::Field || scrutinee->kind == hir::ExprKind::Index ||
               scrutinee->kind == hir::ExprKind::Deref) {
        owner = std::optional<hir::LocalId>{};
    }
    {
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = id;
        d.value = std::move(scrutinee);
        block.body.push_back(std::move(d));
    }
    const auto tagIs = [&](std::size_t variant, const Span& at) {
        EP tag = mk(hir::ExprKind::Field, tag_type, at);
        tag->text = "tag";
        tag->index = 0;
        tag->lvalue = true;
        tag->lhs = mk(hir::ExprKind::LocalRef, self, at);
        tag->lhs->local = id;
        tag->lhs->lvalue = true;
        EP eq = mk(hir::ExprKind::Binary, boolean, at);
        eq->binary_op = BinaryOp::Eq;
        eq->lhs = std::move(tag);
        eq->rhs = intConst(variant, tag_type, at);
        return eq;
    };

    struct Arm {
        EP cond;
        std::vector<hir::Stmt> body;
    };
    std::vector<Arm> arms;
    std::vector<hir::Stmt> default_body;
    bool has_default = false;
    std::set<std::size_t> covered;
    for (const SwitchCase& c : s.cases) {
        if (c.is_default) {
            if (std::exchange(has_default, true)) {
                err("E0709",
                    c.span,
                    "a `switch` has more than one `default`",
                    "remove the duplicate");
            }
            lowerBlock(*c.body, default_body, true);
            continue;
        }
        Arm arm;
        scopes_.push(ScopeKind::Block);
        for (const ExprPtr& value : c.values) {
            const bool call = value->kind == Expr::Kind::Call;
            const Expr& pattern = call ? *value->lhs : *value;
            const ScopeEntry* head =
                pattern.kind == Expr::Kind::Field ? chainEntry(*pattern.lhs) : nullptr;
            const auto named = head != nullptr ? sumOfEntry(*head) : std::nullopt;
            // The pattern names the enum; the scrutinee may be one instance of it (`Result<i32, E>`
            // built its own aggregate), so the compared part is the declaration's variant list.
            if (!named) {
                // A name that is not an enum at all, such as `case 1`, names no variant of the
                // switched-on enum: the `case` syntax itself is wrong (E0749).
                err("E0749",
                    value->span,
                    std::format("`case` of a `{}` switch must name one of its variants",
                                sum.decl->name),
                    std::format("write `{}.Variant` or `{}.Variant(names...)`",
                                sum.decl->name,
                                sum.decl->name));
                continue;
            }
            if (!sameVariants(*named->decl, *sum.decl)) {
                // A pattern that names another enum: `case Other.A` in a `switch` on `E`.
                err("E0757",
                    value->span,
                    std::format("`case {}.{}` names a variant of `{}`, but the value is a `{}`",
                                named->decl->name,
                                pattern.text,
                                named->decl->name,
                                sum.decl->name),
                    std::format("write `{}.{}`", sum.decl->name, pattern.text));
                continue;
            }
            const auto variant = sumVariantIndex(sum, pattern.text);
            if (!variant) {
                err("E0706",
                    pattern.span,
                    std::format("enum `{}` has no member `{}`", sum.decl->name, pattern.text),
                    "unknown member");
                continue;
            }
            if (!covered.insert(*variant).second) {
                err("E0709",
                    value->span,
                    "duplicate `case` variant",
                    "this variant is already handled by an earlier `case`");
            }
            EP cond = tagIs(*variant, value->span);
            arm.cond = arm.cond ? [&] {
                EP both = mk(hir::ExprKind::Binary, boolean, value->span);
                both->binary_op = BinaryOp::Or;
                both->lhs = std::move(arm.cond);
                both->rhs = std::move(cond);
                return both;
            }()
                                : std::move(cond);
            if (!call) {
                continue;
            }
            const SumVariant& v = sum.decl->variants[*variant];
            if (c.values.size() != 1 || value->args.size() != v.payload.size() ||
                !std::ranges::all_of(
                    value->args, [](const ExprPtr& a) { return a->kind == Expr::Kind::Name; })) {
                err("E0749",
                    value->span,
                    std::format("pattern `{}.{}(...)` needs one name per payload value ({}), alone "
                                "in its `case`",
                                sum.decl->name,
                                v.name,
                                v.payload.size()),
                    "write `case E.A(x, y) { }`; use `_` to ignore a value");
                continue;
            }
            for (const std::size_t i : std::views::iota(std::size_t{0}, v.payload.size())) {
                const Expr& name = *value->args[i];
                if (name.text == "_") {
                    continue;
                }
                EP read = sumPayloadRead(sum, id, self, *variant, i, name.span);
                const Type field_type = read->type;
                // The binding owns the value: a droppable payload moves out of the enum. The
                // switch held the scrutinee; once its live payload is taken the holder is dropped
                // without it, so exactly one value exists.
                read = moveOut(std::move(read), true);
                if (read == nullptr) {
                    continue;
                }
                const hir::LocalId local = addLocal(name.text, field_type, false, name.span);
                hir::Stmt d;
                d.kind = hir::StmtKind::Local;
                d.span = name.span;
                d.local = local;
                d.value = std::move(read);
                arm.body.push_back(std::move(d));
                bindLocal(name.text, local, field_type, name.span);
                if (owner && needsDrop(field_type)) {
                    sum_bindings_[local] = *owner;
                }
            }
        }
        lowerBlock(*c.body, arm.body, true);
        scopes_.pop();
        if (arm.cond) {
            arms.push_back(std::move(arm));
        }
    }
    scopes_.pop();

    if (!has_default && covered.size() < sum.decl->variants.size()) {
        std::string missing;
        for (const auto& [i, v] :
             std::views::zip(std::views::iota(std::size_t{0}), sum.decl->variants)) {
            if (!covered.contains(i)) {
                missing +=
                    std::format("{}`{}.{}`", missing.empty() ? "" : ", ", sum.decl->name, v.name);
            }
        }
        err("E0750",
            s.span,
            std::format("`switch` on `{}` does not cover {}", sum.decl->name, missing),
            "add a `case` for each missing variant, or a `default`");
    }
    std::vector<hir::Stmt> chain = std::move(default_body);
    if (!has_default && covered.size() == sum.decl->variants.size() && !arms.empty()) {
        // Every variant is covered: the last arm is the `else` of the chain (its test cannot
        // fail), so a switch whose arms all return ends its path like an `if`/`else` does.
        hir::Stmt last;
        last.kind = hir::StmtKind::Block;
        last.span = s.span;
        last.body = std::move(arms.back().body);
        chain.push_back(std::move(last));
        arms.pop_back();
    }
    for (Arm& arm : arms | std::views::reverse) {
        hir::Stmt ifs;
        ifs.kind = hir::StmtKind::If;
        ifs.span = s.span;
        ifs.value = std::move(arm.cond);
        ifs.body = std::move(arm.body);
        ifs.else_body = std::move(chain);
        chain.clear();
        chain.push_back(std::move(ifs));
    }
    std::ranges::move(chain, std::back_inserter(block.body));
    out.push_back(std::move(block));
}

} // namespace xlang::lower
