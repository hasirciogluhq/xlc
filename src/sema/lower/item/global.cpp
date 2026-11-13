// Lowering: globals and local constants.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

void Lowerer::lowerGlobal(const GlobalDecl& g, const Modifiers& mods) {
    hir::Global out;
    out.name = s_.qualify(g.name);
    out.module = s_.key_;
    out.constant = g.is_const;
    out.type = globalType(s_, g);
    rejectDroppablePlace(out.type, g.name_span, "a global");
    if (droppableSum(out.type)) {
        err("E0758",
            g.name_span,
            std::format("`{}` is a payload enum that needs drop and cannot be a global",
                        typeToString(out.type)),
            "a global is plain data and is never dropped; use a local or a class field");
    } else if (needsDrop(out.type)) {
        err("E0721",
            g.name_span,
            std::format("`{}` needs drop and cannot be a global", typeToString(out.type)),
            "use a local, or a class field or `Vec` for owned values");
    }
    if (!g.has_type && g.init && !lower::literalConstType(s_, *g.init)) {
        err("E0715",
            g.init->span,
            "untyped `const` needs a literal value",
            "write its type: `const NAME: T = ...`");
    }
    out.exported = mods.exported || mods.export_c || mods.registered;
    out.export_c = mods.export_c;
    out.declared = mods.declared;
    out.registered = mods.registered;
    out.register_role = mods.register_role;
    out.span = g.span;
    if (mods.export_c && !cCompatible(s_, out.type)) {
        err("E0302",
            g.name_span,
            std::format("`{}` has a type that C cannot express", typeToString(out.type)),
            "`export(c)` needs C-compatible types (integers, floats, bool, pointers, `@repr(c)` "
            "structs)");
    }
    assignSymbol(
        g.attributes, mods.export_c, mods.declared, g.name, {}, false, out.symbol, g.name_span);
    if (g.init) {
        EP init = g.has_type ? coerce(expr(*g.init, &out.type), out.type, g.init->span)
                             : expr(*g.init, nullptr);
        if (init && !g.has_type) {
            out.type = init->type;
        }
        if (init) {
            const hir::Expr* offender = nullptr;
            if (!isConstValue(*init, &offender)) {
                const hir::Expr& at = offender != nullptr ? *offender : *init;
                if (g.is_const) {
                    err("E0715",
                        at.span,
                        "`const` initializer is not a compile-time constant",
                        "a `const` holds literals, arithmetic on them, arrays and struct literals; "
                        "it cannot call functions or read other names");
                } else {
                    err("E0707",
                        at.span,
                        "global initializer is not a compile-time constant",
                        "globals are statically initialized: no calls, no reads of other globals");
                }
            }
            out.init = std::move(init);
        }
    }
    mod_.globals.push_back(std::move(out));
}

void Lowerer::lowerLocalConst(const Stmt& s) {
    EP init;
    Type t;
    if (s.has_type) {
        t = resolved(s.type);
        init = coerce(expr(*s.expr, &t), t, s.expr->span);
    } else {
        init = expr(*s.expr, nullptr);
        t = init ? init->type : lower::unknownType();
    }
    if (init) {
        const hir::Expr* offender = nullptr;
        if (!isConstValue(*init, &offender)) {
            const hir::Expr& at = offender != nullptr ? *offender : *init;
            err("E0715",
                at.span,
                "`const` initializer is not a compile-time constant",
                "a `const` holds literals, arithmetic on them, arrays and struct literals; "
                "it cannot call functions or read other names");
            init.reset();
        }
    }
    // A `const` is a private read-only global with a unique name; only the block sees the name.
    hir::Global g;
    g.name = std::format("{}${}${}", fn_->name, s.name, hidden_counter_++);
    g.module = s_.key_;
    g.symbol = g.name;
    g.type = t;
    g.constant = true;
    g.init = std::move(init);
    g.span = s.span;
    ScopeEntry e;
    e.kind = ScopeEntry::Kind::Global;
    e.name = s.name;
    e.type = t;
    e.span = s.span;
    e.owner = &s_;
    e.qname = g.name;
    e.readonly = true;
    mod_.globals.push_back(std::move(g));
    if (const ScopeEntry* prev = scopes_.declare(std::move(e))) {
        Diagnostic d;
        d.code = "E0401";
        d.severity = Severity::Error;
        d.message = std::format("`{}` is defined twice in this scope", s.name);
        d.labels.push_back({prev->span, "first definition"});
        d.labels.push_back({s.span, "second definition"});
        d.fix = "rename one of them";
        s_.diag_.report(std::move(d));
    }
}

} // namespace xlang::lower
