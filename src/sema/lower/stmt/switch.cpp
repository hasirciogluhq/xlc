// Lowering: `switch` on integers, bools and enums, and the subject-less `switch`.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

void Lowerer::lowerSwitch(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    if (!s.expr) {
        lowerConditionSwitch(s, out);
        return;
    }
    EP scrutinee = expr(*s.expr, nullptr);
    const auto sum = scrutinee ? sumOfType(scrutinee->type) : std::nullopt;
    if (scrutinee && sum) {
        lowerSumSwitch(s, std::move(scrutinee), out);
        return;
    }
    const bool usable = scrutinee && (scrutinee->type.kind == TypeKind::Int ||
                                      scrutinee->type.kind == TypeKind::Bool ||
                                      scrutinee->type.kind == TypeKind::Enum);
    if (scrutinee && !usable && !lower::isUnknown(scrutinee->type)) {
        err("E0708",
            s.expr->span,
            std::format("cannot `switch` on `{}`", typeToString(scrutinee->type)),
            "integers, bools and enums can be switched on; use `if` for other types");
    }
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    const Type st = usable ? scrutinee->type : lower::unknownType();
    const hir::LocalId id = hiddenLocal("sw", st, s.span);
    const std::string name = fn_->locals[id].name;
    if (usable) {
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = id;
        d.value = std::move(scrutinee);
        block.body.push_back(std::move(d));
    }
    bindLocal(name, id, st, s.span);

    struct Arm {
        EP cond;
        std::vector<hir::Stmt> body;
    };
    std::vector<Arm> arms;
    std::vector<hir::Stmt> default_body;
    bool has_default = false;
    std::set<std::uint64_t> seen;
    for (const SwitchCase& c : s.cases) {
        Arm arm;
        if (c.is_default) {
            if (has_default) {
                err("E0709",
                    c.span,
                    "a `switch` has more than one `default`",
                    "remove the duplicate");
            }
            has_default = true;
            lowerBlock(*c.body, default_body, true);
            continue;
        }
        if (c.values.empty()) {
            err("E0709", c.span, "`case` needs at least one value", "write `case 1, 2 { ... }`");
        }
        ExprPtr cond_ast;
        for (const ExprPtr& v : c.values) {
            ExprPtr eq = lower::astBinary(
                BinaryOp::Eq, lower::astName(name, v->span), lower::cloneAst(*v), v->span);
            cond_ast =
                cond_ast
                    ? lower::astBinary(BinaryOp::Or, std::move(cond_ast), std::move(eq), v->span)
                    : std::move(eq);
            const auto cv = s_.evalConst(*v);
            if (cv && cv->kind == ConstValue::Kind::Int &&
                !seen.insert(static_cast<std::uint64_t>(cv->int_value)).second) {
                err("E0709",
                    v->span,
                    "duplicate `case` value",
                    "this value is already handled by an earlier `case`");
            }
        }
        if (cond_ast && usable) {
            arm.cond = coerce(expr(*cond_ast, &boolean), boolean, c.span);
        }
        lowerBlock(*c.body, arm.body, true);
        if (arm.cond) {
            arms.push_back(std::move(arm));
        }
    }
    scopes_.pop();

    std::vector<hir::Stmt> chain = std::move(default_body);
    for (std::size_t i = arms.size(); i-- > 0;) {
        hir::Stmt ifs;
        ifs.kind = hir::StmtKind::If;
        ifs.span = s.span;
        ifs.value = std::move(arms[i].cond);
        ifs.body = std::move(arms[i].body);
        ifs.else_body = std::move(chain);
        chain.clear();
        chain.push_back(std::move(ifs));
    }
    if (!usable) {
        return;
    }
    for (hir::Stmt& c : chain) {
        block.body.push_back(std::move(c));
    }
    out.push_back(std::move(block));
}

void Lowerer::lowerConditionSwitch(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    // `switch { case c1, c2 { } case c3 { } default { } }` is the chain
    // `if c1 || c2 { } else if c3 { } else { }`: the first arm whose condition holds runs.
    struct Arm {
        std::vector<hir::Stmt> pre; ///< what the condition hoists; it runs only when reached
        EP cond;
        std::vector<hir::Stmt> body;
    };
    std::vector<Arm> arms;
    std::vector<hir::Stmt> default_body;
    bool has_default = false;
    for (const SwitchCase& c : s.cases) {
        if (c.is_default) {
            if (has_default) {
                err("E0709", c.span, "a `switch` has more than one `default`", "remove the duplicate");
            }
            has_default = true;
            lowerBlock(*c.body, default_body, true);
            continue;
        }
        ExprPtr cond_ast;
        for (const ExprPtr& v : c.values) {
            cond_ast = cond_ast ? lower::astBinary(
                                      BinaryOp::Or, std::move(cond_ast), lower::cloneAst(*v), v->span)
                                : lower::cloneAst(*v);
        }
        Arm arm;
        arm.cond = hoistedInto(
            arm.pre, [&] { return coerce(expr(*cond_ast, &boolean), boolean, c.span); });
        lowerBlock(*c.body, arm.body, true);
        if (arm.cond) {
            arms.push_back(std::move(arm));
        }
    }
    // Built from the last arm back: each arm's hoisted statements run in the `else` of the arm
    // before it, so a condition is evaluated only when every earlier one failed.
    std::vector<hir::Stmt> chain = std::move(default_body);
    for (std::size_t i = arms.size(); i-- > 0;) {
        hir::Stmt ifs;
        ifs.kind = hir::StmtKind::If;
        ifs.span = s.span;
        ifs.value = std::move(arms[i].cond);
        ifs.body = std::move(arms[i].body);
        ifs.else_body = std::move(chain);
        chain = std::move(arms[i].pre);
        chain.push_back(std::move(ifs));
    }
    for (hir::Stmt& c : chain) {
        out.push_back(std::move(c));
    }
}

} // namespace xlang::lower
