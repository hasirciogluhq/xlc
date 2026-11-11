#include "sema/lower/lower.hpp"

namespace xlang::lower {

// User-defined operators. An operator is a function named `operator<symbol>` (the parser makes it
// out of `fn +(...)`, in an inherent `impl T { }` block, at module level or in a class body), so
// it overloads, is generic and is exported like any function. The lowering rewrites an operator
// expression into the call of that function when an operand is a struct or a class:
//
//   a + b   a - b ... a >> b   operator<sym>(a, b)            all binary arithmetic and bit operators
//   a == b                     operator==(a, b)               `!=` is `!(a == b)` when not defined
//   a < b                      operator<(a, b)                `>` `<=` `>=` follow from `<` when not defined
//   -a  !a  ~a                 operator-(a)  operator!(a)  operator~(a)
//   *a                         *operator*(a)                  the result is a pointer, `*a` is its place
//   a[i]                       operator[](a, i)
//   a[i] = v                   operator[]=(&a, i, v)
//   a += b  a <<= b ...        operator+=(&a, b)              else `a = a + b`
//   a++  a--                   operator++(&a)                 else `operator+=(&a, 1)`, else `a = a + 1`
//   a(x, y)                    operator()(&a, x, y)
//
// Operands are looked up by name in the scope of the expression and in the module of every
// struct or class operand (no import is needed for an operator next to its type). A class with a
// method of that name calls the method, with the class as `self`.

namespace {

[[nodiscard]] ExprPtr makeCall(ExprPtr callee, std::vector<ExprPtr> args, const Span& span) {
    auto c = std::make_unique<Expr>();
    c->kind = Expr::Kind::Call;
    c->span = span;
    c->lhs = std::move(callee);
    c->args = std::move(args);
    return c;
}

[[nodiscard]] ExprPtr makeField(ExprPtr base, std::string name, const Span& span) {
    auto f = std::make_unique<Expr>();
    f->kind = Expr::Kind::Field;
    f->span = span;
    f->text = std::move(name);
    f->lhs = std::move(base);
    return f;
}

[[nodiscard]] std::string binaryName(BinaryOp op) {
    return std::string("operator") + binaryOpName(op);
}

/// The type an operator argument has as far as lookup is concerned: a pointer to a struct names
/// the struct.
[[nodiscard]] const Type& lookupType(const Type& t) {
    return t.kind == TypeKind::Pointer && t.elem ? *t.elem : t;
}

} // namespace

std::vector<ScopeEntry>
Lowerer::operatorEntries(const std::string& name, const std::vector<Type>& types, const Span& at) {
    std::vector<ScopeEntry> out;
    const auto add = [&out](ScopeEntry e) {
        if (e.kind != ScopeEntry::Kind::Function) {
            return;
        }
        const bool known = std::ranges::any_of(out, [&e](const ScopeEntry& o) {
            return o.owner == e.owner && o.item == e.item;
        });
        if (!known) {
            out.push_back(std::move(e));
        }
    };
    if (const ScopeEntry* here = lookupName(name)) {
        add(*here);
    }
    if (name == "operator()") {
        // The call of a closure that captures variables (see closure.cpp).
        for (const Type& t : types) {
            const auto it = lookupType(t).path.empty() ? sh_.closures.end()
                                                       : sh_.closures.find(lookupType(t).path.front());
            if (it != sh_.closures.end()) {
                ScopeEntry e;
                e.kind = ScopeEntry::Kind::Function;
                e.name = name;
                e.item = it->second.item;
                e.owner = it->second.owner;
                e.qname = it->second.qname;
                add(std::move(e));
            }
        }
    }
    for (const Type& t : types) {
        if (!isAggregate(lookupType(t))) {
            continue;
        }
        if (const auto owner = entityOfType(lookupType(t))) {
            if (const auto ref = owner->owner->lookupEntity(name)) {
                add(entryOf(*ref, name, at));
            }
        }
    }
    return out;
}

bool Lowerer::hasOperator(const std::string& name,
                          std::size_t arity,
                          const std::vector<Type>& types) {
    if (!types.empty() && lookupType(types.front()).kind == TypeKind::Class &&
        findMethod(lookupType(types.front()), name)) {
        return true;
    }
    return std::ranges::any_of(operatorEntries(name, types, {}), [&](const ScopeEntry& entry) {
        const std::vector<const Item*>& items =
            entry.overloads.empty() ? std::vector<const Item*>{entry.item} : entry.overloads;
        return std::ranges::any_of(items, [&](const Item* item) {
            const auto* f = std::get_if<FnDecl>(&item->decl);
            return f != nullptr && f->params.size() == arity;
        });
    });
}

Lowerer::EP Lowerer::operatorCall(const std::string& name,
                                  ExprPtr receiver,
                                  bool by_pointer,
                                  std::vector<ExprPtr> rest,
                                  const std::vector<Type>& types,
                                  const Span& span,
                                  bool& found) {
    found = true;
    // A class method: `receiver.operator+(rest...)`; the class is `self`.
    if (!types.empty() && lookupType(types.front()).kind == TypeKind::Class &&
        findMethod(lookupType(types.front()), name)) {
        return expr(*makeCall(makeField(std::move(receiver), name, span), std::move(rest), span),
                    nullptr);
    }
    // The functions: the receiver is the first argument.
    const std::vector<ScopeEntry> entries = operatorEntries(name, types, span);
    const std::size_t arity = rest.size() + 1;
    const ScopeEntry* pick = nullptr;
    for (const ScopeEntry& entry : entries) {
        const std::vector<const Item*>& items =
            entry.overloads.empty() ? std::vector<const Item*>{entry.item} : entry.overloads;
        const bool fits = std::ranges::any_of(items, [&](const Item* item) {
            const auto* f = std::get_if<FnDecl>(&item->decl);
            return f != nullptr && f->params.size() == arity;
        });
        if (!fits) {
            continue;
        }
        // Prefer the entry that has an overload with exactly these operand types.
        const bool exact = std::ranges::any_of(candidatesOf(entry), [&](const OverloadCandidate& c) {
            return c.params.size() == types.size() &&
                   std::ranges::equal(c.params, types, [](const Type& p, const Type& t) {
                       return sameType(p, t);
                   });
        });
        if (exact) {
            pick = &entry;
            break;
        }
        if (pick == nullptr) {
            pick = &entry;
        }
    }
    if (pick == nullptr && name == "operator()" && !entries.empty()) {
        pick = &entries.front(); // a call with the wrong argument count: the call reports it
    }
    if (pick == nullptr) {
        found = false;
        return nullptr;
    }
    // An operator that takes its first operand by pointer (`fn [](self: *Grid, ...)`) is passed the
    // operand's address when no overload takes it by value.
    if (!by_pointer && !types.empty() && isAggregate(types.front())) {
        const auto cands = candidatesOf(*pick);
        const auto takes = [&](auto&& param_fits) {
            return std::ranges::any_of(cands, [&](const OverloadCandidate& c) {
                return c.params.size() == arity && param_fits(c.params.front());
            });
        };
        by_pointer = !takes([&](const Type& p) { return sameType(p, types.front()); }) &&
                     takes([&](const Type& p) {
                         return p.kind == TypeKind::Pointer && p.elem && sameType(*p.elem, types.front());
                     });
    }
    std::vector<ExprPtr> args;
    args.push_back(by_pointer ? astUnary(UnaryOp::AddrOf, std::move(receiver), span)
                              : std::move(receiver));
    for (ExprPtr& a : rest) {
        args.push_back(std::move(a));
    }
    const ExprPtr call = makeCall(astName(name, span), std::move(args), span);
    return callEntry(*call, *pick, name);
}

Lowerer::EP Lowerer::userBinary(const Expr& e, const Type& lt, const Type& rt, std::size_t mark) {
    cutPrelude(mark);
    const std::vector<Type> types{lt, rt};
    const BinaryOp op = e.binary_op;
    const auto has = [&](BinaryOp o) { return hasOperator(binaryName(o), 2, types); };
    const auto bin = [&](BinaryOp o, const Expr& a, const Expr& b) {
        return astBinary(o, cloneAst(a), cloneAst(b), e.span);
    };
    const auto negated = [&](ExprPtr x) { return astUnary(UnaryOp::Not, std::move(x), e.span); };
    // `!=`, `>`, `<=` and `>=` that the type does not define follow from `==` and `<`.
    ExprPtr derived;
    if (op == BinaryOp::Ne && !has(op) && has(BinaryOp::Eq)) {
        derived = negated(bin(BinaryOp::Eq, *e.lhs, *e.rhs));
    } else if (op == BinaryOp::Gt && !has(op) && has(BinaryOp::Lt)) {
        derived = bin(BinaryOp::Lt, *e.rhs, *e.lhs);
    } else if (op == BinaryOp::Le && !has(op) && has(BinaryOp::Lt)) {
        derived = negated(bin(BinaryOp::Lt, *e.rhs, *e.lhs));
    } else if (op == BinaryOp::Ge && !has(op) && has(BinaryOp::Lt)) {
        derived = negated(bin(BinaryOp::Lt, *e.lhs, *e.rhs));
    }
    if (derived) {
        return expr(*derived, nullptr);
    }
    std::vector<ExprPtr> rest;
    rest.push_back(cloneAst(*e.rhs));
    bool found = false;
    EP call = operatorCall(binaryName(op), cloneAst(*e.lhs), false, std::move(rest), types, e.span, found);
    if (!found) {
        err("E0709",
            e.span,
            std::format("operator `{}` is not defined for `{}` and `{}`",
                        binaryOpName(op),
                        typeToString(lt),
                        typeToString(rt)),
            std::format("write `fn {}(self: {}, other: {})` in an `impl {} {{ }}` block",
                        binaryOpName(op),
                        typeToString(lt),
                        typeToString(rt),
                        typeToString(isAggregate(lt) ? lt : rt)));
    }
    return call;
}

Lowerer::EP Lowerer::userUnary(const Expr& e, const Type& t, std::size_t mark) {
    cutPrelude(mark);
    const std::string name = std::string("operator") + unaryOpName(e.unary_op);
    bool found = false;
    EP call = operatorCall(name, cloneAst(*e.lhs), false, {}, {t}, e.span, found);
    if (!found) {
        err("E0709",
            e.span,
            std::format("operator `{}` is not defined for `{}`", unaryOpName(e.unary_op), typeToString(t)),
            std::format("write `fn {}(self: {}) -> ...` in an `impl {} {{ }}` block",
                        unaryOpName(e.unary_op),
                        typeToString(t),
                        typeToString(t)));
        return nullptr;
    }
    if (!call || e.unary_op != UnaryOp::Deref || lower::isUnknown(call->type)) {
        return call;
    }
    // `*a` is the place the returned pointer addresses.
    if (call->type.kind != TypeKind::Pointer || !call->type.elem || call->type.elem->isVoid()) {
        err("E0709",
            e.span,
            std::format("`operator*` of `{}` must return a pointer, found `{}`",
                        typeToString(t),
                        typeToString(call->type)),
            "`*a` reads and writes the place the pointer addresses");
        return nullptr;
    }
    EP d = mk(hir::ExprKind::Deref, *call->type.elem, e.span);
    d->lvalue = true;
    d->lhs = std::move(call);
    return d;
}

Lowerer::EP Lowerer::userIndex(const Expr& e, const Type& base, std::size_t mark) {
    cutPrelude(mark);
    std::vector<ExprPtr> rest;
    rest.push_back(cloneAst(*e.rhs));
    bool found = false;
    EP call = operatorCall("operator[]", cloneAst(*e.lhs), false, std::move(rest), {base}, e.span, found);
    if (!found) {
        err("E0708",
            e.span,
            std::format("`{}` cannot be indexed", typeToString(base)),
            std::format("write `fn [](self: {}, i: usize) -> ...` in an `impl {} {{ }}` block",
                        typeToString(base),
                        typeToString(base)));
    }
    return call;
}

Lowerer::EP Lowerer::userCall(const Expr& e, const Type& callee) {
    std::vector<ExprPtr> rest;
    for (const ExprPtr& a : e.args) {
        rest.push_back(cloneAst(*a));
    }
    bool found = false;
    EP call = operatorCall("operator()", cloneAst(*e.lhs), true, std::move(rest), {callee}, e.span, found);
    if (!found) {
        err("E0709",
            e.span,
            std::format("`{}` is not callable", typeToString(callee)),
            std::format("write `fn ()(self: *{}, ...) -> ...` in an `impl {} {{ }}` block",
                        typeToString(callee),
                        typeToString(callee)));
    }
    return call;
}

bool Lowerer::userCompound(const Stmt& s, const Expr& place, std::vector<hir::Stmt>& out) {
    const std::size_t mark = preludeMark();
    const EP probe = expr(place, nullptr);
    cutPrelude(mark);
    if (!probe || !probe->lvalue || !isAggregate(probe->type)) {
        return false;
    }
    const Type self_type = Type::makePointer(probe->type);
    const std::string symbol = binaryName(s.compound_op); // `operator+`
    std::string name;
    std::vector<ExprPtr> rest;
    if (s.is_incdec && hasOperator(symbol + symbol.back(), 1, {self_type})) {
        name = symbol + symbol.back(); // `operator++` / `operator--`
    } else if (hasOperator(symbol + "=", 2, {self_type, Type{}})) {
        name = symbol + "=";
        rest.push_back(cloneAst(*s.expr));
    } else {
        return false; // `place = place op value`
    }
    bool found = false;
    EP call = operatorCall(name, cloneAst(place), true, std::move(rest), {self_type}, s.span, found);
    if (call) {
        hir::Stmt st;
        st.kind = hir::StmtKind::Expr;
        st.span = s.span;
        st.value = std::move(call);
        out.push_back(std::move(st));
    }
    return true;
}

bool Lowerer::userIndexAssign(const Expr& target,
                              const Expr& value,
                              const Span& span,
                              std::vector<hir::Stmt>& out) {
    const std::size_t mark = preludeMark();
    const EP probe = expr(*target.lhs, nullptr);
    cutPrelude(mark);
    if (!probe || !probe->lvalue || !isAggregate(probe->type)) {
        return false;
    }
    const Type self_type = Type::makePointer(probe->type);
    if (!hasOperator("operator[]=", 3, {self_type})) {
        return false;
    }
    std::vector<ExprPtr> rest;
    rest.push_back(cloneAst(*target.rhs));
    rest.push_back(cloneAst(value));
    bool found = false;
    EP call =
        operatorCall("operator[]=", cloneAst(*target.lhs), true, std::move(rest), {self_type}, span, found);
    if (call) {
        hir::Stmt st;
        st.kind = hir::StmtKind::Expr;
        st.span = span;
        st.value = std::move(call);
        out.push_back(std::move(st));
    }
    return true;
}

} // namespace xlang::lower
