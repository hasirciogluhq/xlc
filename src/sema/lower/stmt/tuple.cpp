// Lowering: `local (a, b) = expr` destructuring.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

/// `local (a, b) = expr`: pure sugar over `local $t = expr` plus one `local a = $t.0` per name.
/// The hidden tuple lives in the same block, so the names are visible exactly as if each had been
/// declared by a plain `local`; the field reads are ordinary `Field` accesses on a `Tuple` type.
void Lowerer::lowerTupleLocal(const Stmt& s, std::vector<hir::Stmt>& out) {
    if (!s.expr) {
        err("E0708", s.span, "a tuple target needs an initializer", "write `local (a, b) = expr`");
        return;
    }
    EP init = expr(*s.expr, nullptr);
    if (!init) {
        return;
    }
    if (lower::isUnknown(init->type)) {
        return;
    }
    if (init->type.kind != TypeKind::Tuple) {
        err("E0708",
            s.expr->span,
            std::format("cannot take `{}` apart: it is not a tuple", typeToString(init->type)),
            "a tuple target needs a tuple value on the right");
        return;
    }
    const Type tuple_type = init->type;
    if (tuple_type.args.size() != s.names.size()) {
        err("E0708",
            s.span,
            std::format("the tuple target names {} variable(s), the value has {}",
                        s.names.size(),
                        tuple_type.args.size()),
            "write one name per tuple element");
        return;
    }
    // The whole tuple is held once, unnamed, so every element is read from one evaluation of the
    // right-hand side (a call returning the tuple runs exactly once).
    const hir::LocalId tmp = hiddenLocal("tuple", tuple_type, s.span);
    hir::Stmt hold;
    hold.kind = hir::StmtKind::Local;
    hold.span = s.span;
    hold.local = tmp;
    // The elements move into the names, so the hidden tuple itself is never dropped; taking a
    // droppable tuple local apart consumes it (its drop flag is cleared).
    hold.value = moveOut(std::move(init));
    if (!hold.value) {
        return;
    }
    out.push_back(std::move(hold));

    // Bind the names only after the value is computed: `local (a, b) = f(a)` reads the outer `a`.
    for (std::size_t i = 0; i < s.names.size(); ++i) {
        EP field = mk(hir::ExprKind::Field, tuple_type.args[i], s.span);
        field->index = static_cast<std::uint32_t>(i);
        field->lvalue = true;
        field->lhs = localRef(tmp, tuple_type, s.span);

        const Type t = tuple_type.args[i];
        hir::Stmt decl;
        decl.kind = hir::StmtKind::Local;
        decl.span = s.span;
        decl.local = addLocal(s.names[i], t, false, s.span);
        rejectDroppablePlace(t, s.span, "a local");
        if (needsDrop(t)) {
            registerDrop(decl.local, t, true, s.span, out);
        }
        decl.value = std::move(field);
        out.push_back(std::move(decl));
        bindLocal(s.names[i], out.back().local, t, s.span);
    }
}

} // namespace xlang::lower
