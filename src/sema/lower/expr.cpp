#include "sema/lower/lower.hpp"

namespace xlang::lower {

// Expressions, calls, overload resolution and conversions.

// ---------------------------------------------------------------------------------------------
// Conversions

bool Lowerer::castable(const Type& from, const Type& to) const {
    if (lower::isUnknown(from) || lower::isUnknown(to) || lower::sameType(from, to)) {
        return true;
    }
    const auto scalar = [](const Type& t) {
        return t.kind == TypeKind::Int || t.kind == TypeKind::Float || t.kind == TypeKind::Bool;
    };
    const auto addr = [](const Type& t) {
        return t.kind == TypeKind::Pointer || t.kind == TypeKind::FnPtr;
    };
    if (from.kind == TypeKind::Enum && from.elem) {
        return castable(*from.elem, to);
    }
    if (to.kind == TypeKind::Enum && to.elem) {
        return castable(from, *to.elem);
    }
    if (scalar(from) && scalar(to)) {
        return true;
    }
    if (addr(from) && addr(to)) {
        return true;
    }
    if ((addr(from) && to.kind == TypeKind::Int) || (from.kind == TypeKind::Int && addr(to))) {
        return true;
    }
    return false;
}

Lowerer::EP Lowerer::castTo(EP value, const Type& to, bool implicit) {
    if (!value || lower::sameType(value->type, to)) {
        return value;
    }
    EP c = mk(hir::ExprKind::Cast, to, value->span);
    c->implicit = implicit;
    c->lhs = std::move(value);
    return c;
}

Lowerer::EP Lowerer::toInterface(EP value, const Type& iface, const Span& at) {
    // `value` is a struct/class value (an lvalue) or a pointer to one.
    const std::string iface_q = iface.path.empty() ? std::string{} : iface.path.front();
    EP pointer;
    std::string type_q;
    if (value->type.kind == TypeKind::Pointer && value->type.elem &&
        lower::isAggregate(*value->type.elem)) {
        type_q = value->type.elem->path.front();
        pointer = std::move(value);
    } else {
        type_q = value->type.path.front();
        if (!value->lvalue) {
            err("E0709",
                at,
                "cannot convert a temporary to an interface",
                "the interface value points at the object; store it in a variable first");
            return nullptr;
        }
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(value->type), at);
        addr->lhs = std::move(value);
        pointer = std::move(addr);
    }
    if (findImpl(type_q, iface_q) == nullptr) {
        Diagnostic d;
        d.code = "E0504";
        d.severity = Severity::Error;
        d.message = std::format("`{}` does not implement `{}`", type_q, iface_q);
        d.labels.push_back({at, std::format("`{}` -> `{}` needs an `impl`", type_q, iface_q)});
        d.detail = std::format("No `impl {} for {}` was found.", iface_q, type_q);
        d.reason = "Interfaces are nominal; matching method names never count.";
        d.fix = std::format("add `impl {} for {} {{ ... }}`", iface_q, type_q);
        s_.diag_.report(std::move(d));
        return nullptr;
    }
    EP out = mk(hir::ExprKind::ToInterface, iface, at);
    out->text = type_q;
    out->lhs = std::move(pointer);
    return out;
}

namespace {

/// True when the place `e` lives in the current frame: a local or parameter, a temporary
/// materialized for a droppable value, or a field/element of one (not reached through a pointer).
[[nodiscard]] bool onStack(const hir::Expr& e) {
    switch (e.kind) {
    case hir::ExprKind::LocalRef:
    case hir::ExprKind::Materialize: return true;
    case hir::ExprKind::Field: return !e.through_pointer && e.lhs && onStack(*e.lhs);
    case hir::ExprKind::Index:
        return e.lhs && e.lhs->type.kind == TypeKind::Array && onStack(*e.lhs);
    default: return false;
    }
}

/// The first `ToInterface` under `e` (through casts, selects, tuples and struct literals) whose
/// object is on the stack, or null.
[[nodiscard]] const hir::Expr* stackInterface(const hir::Expr& e) {
    const auto first = [](const auto& range, auto&& proj) -> const hir::Expr* {
        for (const auto& item : range) {
            const hir::Expr* part = proj(item);
            if (const hir::Expr* sub = part ? stackInterface(*part) : nullptr) {
                return sub;
            }
        }
        return nullptr;
    };
    const auto id = [](const hir::ExprPtr& p) -> const hir::Expr* { return p.get(); };
    switch (e.kind) {
    case hir::ExprKind::ToInterface: {
        const hir::Expr* obj = e.lhs.get();
        return obj && obj->kind == hir::ExprKind::AddrOf && obj->lhs && onStack(*obj->lhs) ? &e
                                                                                          : nullptr;
    }
    case hir::ExprKind::Cast: return e.lhs ? stackInterface(*e.lhs) : nullptr;
    case hir::ExprKind::Select:
        if (const hir::Expr* sub = e.rhs ? stackInterface(*e.rhs) : nullptr) {
            return sub;
        }
        return first(e.args, id);
    case hir::ExprKind::TupleLit: return first(e.args, id);
    case hir::ExprKind::StructLit:
        return first(e.fields, [](const hir::FieldInit& f) { return f.value.get(); });
    default: return nullptr;
    }
}

} // namespace

bool Lowerer::rejectStackInterface(const hir::Expr& value) {
    const hir::Expr* bad = stackInterface(value);
    if (bad == nullptr) {
        return false;
    }
    err("E0760",
        bad->span,
        std::format("cannot return a stack `{}` as `{}`: the fat pointer would dangle",
                    bad->text,
                    typeToString(bad->type)),
        "allocate the object on the heap with `new` and return that pointer");
    return true;
}

Lowerer::EP Lowerer::coerce(EP value, const Type& to, const Span& at) {
    if (!value) {
        return nullptr;
    }
    if (lower::isUnknown(value->type) || lower::isUnknown(to)) {
        return value;
    }
    // `[]mut T` degrades to `[]T` with no code: the two are the same two-word layout, only the
    // permission differs. The reverse (`[]T` to `[]mut T`) is not implicit and needs `as`.
    if (value->type.kind == TypeKind::Slice && to.kind == TypeKind::Slice &&
        value->type.is_mutable && !to.is_mutable && value->type.elem && to.elem &&
        lower::sameType(*value->type.elem, *to.elem)) {
        EP v = std::move(value);
        v->type = to;
        return v;
    }
    if (value->kind == hir::ExprKind::Nil) {
        if (to.kind == TypeKind::Pointer || to.kind == TypeKind::FnPtr) {
            value->type = to;
            return value;
        }
        err("E0708",
            at,
            std::format("`nil` cannot be used as `{}`", typeToString(to)),
            "only pointers can be `nil`");
        return nullptr;
    }
    if (to.kind == TypeKind::Interface) {
        if (value->type.kind == TypeKind::Interface) {
            if (lower::sameType(value->type, to)) {
                return value;
            }
            err("E0708",
                at,
                std::format(
                    "expected `{}`, found `{}`", typeToString(to), typeToString(value->type)),
                "different interfaces");
            return nullptr;
        }
        const bool agg = lower::isAggregate(value->type) ||
                         (value->type.kind == TypeKind::Pointer && value->type.elem &&
                          lower::isAggregate(*value->type.elem));
        if (agg) {
            return toInterface(std::move(value), to, at);
        }
    }
    const ConversionResult r = implicitConversion(value->type, to, inUnsafe(), ptrBits());
    switch (r.status) {
    case ConversionStatus::Ok:
        return castTo(std::move(value), to, true);
    case ConversionStatus::Warn:
        s_.diag_.warning("W0601",
                         at,
                         std::format("`{}` is implicitly converted to `{}`, precision may be lost",
                                     typeToString(value->type),
                                     typeToString(to)),
                         "write `as` to silence");
        return castTo(std::move(value), to, true);
    case ConversionStatus::Signedness:
        err("E0601",
            at,
            std::format("implicit conversion from `{}` to `{}` changes signedness",
                        typeToString(value->type),
                        typeToString(to)),
            "write `as`, or use `unsafe { }`");
        return nullptr;
    case ConversionStatus::NeedsCast:
        err("E0708",
            at,
            std::format("expected `{}`, found `{}`", typeToString(to), typeToString(value->type)),
            r.note.empty() ? "an explicit `as` is required" : r.note);
        return nullptr;
    case ConversionStatus::Invalid:
        err("E0708",
            at,
            std::format("expected `{}`, found `{}`", typeToString(to), typeToString(value->type)),
            "no conversion exists");
        return nullptr;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Expressions

Lowerer::EP Lowerer::fromConst(const ConstValue& v, const Type* expected, const Span& span) {
    Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
    switch (v.kind) {
    case ConstValue::Kind::Int: {
        if (want.kind == TypeKind::Float) {
            EP e = mk(hir::ExprKind::FloatConst, want, span);
            e->float_value = static_cast<double>(v.int_value);
            return e;
        }
        Type t = (want.kind == TypeKind::Int || want.kind == TypeKind::Enum) ? want
                 : (v.int_value > 2147483647 || v.int_value < -2147483648LL)
                     ? Type::makeInt(64, true)
                     : Type::makeInt(32, true);
        return intConst(static_cast<std::uint64_t>(v.int_value), std::move(t), span);
    }
    case ConstValue::Kind::Float: {
        EP e = mk(hir::ExprKind::FloatConst,
                  want.kind == TypeKind::Float ? want : Type::makeFloat(64),
                  span);
        e->float_value = v.float_value;
        return e;
    }
    case ConstValue::Kind::Bool: {
        EP e = mk(hir::ExprKind::BoolConst, Type::makeBool(), span);
        e->bool_value = v.bool_value;
        return e;
    }
    case ConstValue::Kind::String:
        return stringLiteral(v.string_value, expected, span);
    }
    return nullptr;
}

Lowerer::EP Lowerer::constExpr(const Expr& e, const Type* expected) {
    const auto v = s_.evalConst(e);
    return v ? fromConst(*v, expected, e.span) : nullptr;
}

/// A string literal takes its type from the context (see xlang-docs/language/index.md, "Strings"):
/// `*u8` (raw NUL-terminated bytes), `[u8; N]` (a mutable zero-padded copy) or `str`.
Lowerer::EP
Lowerer::stringLiteral(const std::string& text, const Type* expected, const Span& span) {
    const Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
    const Type byte = Type::makeInt(8, false);
    const bool raw = want.kind == TypeKind::Pointer || (want.kind == TypeKind::Enum && want.elem &&
                                                        want.elem->kind == TypeKind::Pointer);
    if (raw) {
        EP p = mk(hir::ExprKind::StrConst, Type::makePointer(byte), span);
        p->text = text;
        return p;
    }
    if (want.kind == TypeKind::Array && want.elem && want.elem->kind == TypeKind::Int &&
        want.elem->bits == 8 && !want.elem->is_signed) {
        if (const auto n = arrayLength(s_, want)) {
            if (text.size() > *n) {
                err("E0717",
                    span,
                    std::format("string literal of {} bytes does not fit in `{}`",
                                text.size(),
                                typeToString(want)),
                    "use a longer array or a `str`");
                return nullptr;
            }
            EP list = mk(hir::ExprKind::ArrayList, want, span);
            for (std::uint64_t i = 0; i < *n; ++i) {
                const std::uint64_t b = i < text.size() ? static_cast<unsigned char>(text[i]) : 0;
                list->args.push_back(intConst(b, byte, span));
            }
            return list;
        }
    }
    EP s = mk(hir::ExprKind::StrConst, Type::makeStr(), span);
    s->text = text;
    return s;
}

Lowerer::EP Lowerer::expr(const Expr& e, const Type* expected) {
    switch (e.kind) {
    case Expr::Kind::IntLiteral: {
        Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
        if (want.kind == TypeKind::Float) {
            EP f = mk(hir::ExprKind::FloatConst, want, e.span);
            f->float_value = static_cast<double>(e.int_value);
            return f;
        }
        Type t = want.kind == TypeKind::Int    ? want
                 : e.int_value > 2147483647ULL ? Type::makeInt(64, true)
                                               : Type::makeInt(32, true);
        return intConst(e.int_value, std::move(t), e.span);
    }
    case Expr::Kind::FloatLiteral: {
        Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
        EP f = mk(hir::ExprKind::FloatConst,
                  want.kind == TypeKind::Float ? want : Type::makeFloat(64),
                  e.span);
        f->float_value = e.float_value;
        return f;
    }
    case Expr::Kind::BoolLiteral: {
        EP b = mk(hir::ExprKind::BoolConst, Type::makeBool(), e.span);
        b->bool_value = e.bool_value;
        return b;
    }
    case Expr::Kind::StringLiteral:
        return stringLiteral(e.text, expected, e.span);
    case Expr::Kind::ByteLiteral:
        return intConst(e.int_value, Type::makeInt(8, false), e.span);
    case Expr::Kind::Name:
        return nameExpr(e, expected);
    case Expr::Kind::Field:
        return fieldExpr(e, expected);
    case Expr::Kind::Unary:
        return unaryExpr(e, expected);
    case Expr::Kind::Binary:
        return binaryExpr(e, expected);
    case Expr::Kind::Ternary:
        return ternaryExpr(e, expected);
    case Expr::Kind::Try:
        return tryExpr(e);
    case Expr::Kind::Cast:
        return castExpr(e);
    case Expr::Kind::Call:
        return callExpr(e, expected);
    case Expr::Kind::Index:
        return indexExpr(e);
    case Expr::Kind::Slice:
        return sliceExpr(e);
    case Expr::Kind::ArrayList:
    case Expr::Kind::ArrayRepeat:
        return arrayExpr(e, expected);
    case Expr::Kind::TupleLiteral:
        return tupleExpr(e, expected);
    case Expr::Kind::Closure:
        return closureExpr(e, expected);
    case Expr::Kind::StructLiteral:
        return structLiteral(e);
    case Expr::Kind::New:
        return newExpr(e);
    case Expr::Kind::MacroCall:
        err("E0004",
            e.span,
            std::format("macro `{}!` was not expanded", e.text),
            "no macro with this name is visible; import it");
        return nullptr;
    case Expr::Kind::AsmOperand:
        err("E0709",
            e.span,
            "inline asm operands are only valid inside `__builtin_asm`",
            "misplaced operand");
        return nullptr;
    case Expr::Kind::TypeOperand:
        err("E0709",
            e.span,
            "a type is only valid as the argument of `sizeOf`, `alignOf` and `offsetOf`",
            "a type is not a value");
        return nullptr;
    case Expr::Kind::Spread:
        // Expanded in the call of a pack function's instance; anywhere else it has no meaning.
        err("E0737",
            e.span,
            std::format("`..{}` is not a spread of a parameter pack here", e.text),
            "`..name` is valid only as a call argument, where `name` is the pack");
        return nullptr;
    }
    return nullptr;
}

Lowerer::EP Lowerer::nameExpr(const Expr& e, const Type* expected) {
    const ScopeEntry* entry = lookupName(e.text);
    if (entry == nullptr) {
        if (e.text == "nil") {
            return mk(hir::ExprKind::Nil, Type::makePointer(Type::makeVoid()), e.span);
        }
        if (s_.config_.lookup(e.text)) {
            return constExpr(e, expected);
        }
        if (star_import_) {
            EP x = mk(hir::ExprKind::External, lower::unknownType(), e.span);
            x->text = "*";
            x->member = e.text;
            return x;
        }
        err("E0706",
            e.span,
            std::format("unknown name `{}`", e.text),
            "not declared in this scope");
        return nullptr;
    }
    return fromEntry(*entry, e, expected);
}

Lowerer::EP Lowerer::genericFnValue(const ScopeEntry& entry, const Expr& at, const Type* expected) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    const Item* item = entry.overloads.empty() ? entry.item : entry.overloads.front();
    const std::vector<std::string> names = owner.genericParamsOf(*item);
    if (entry.overloads.size() > 1 || names.empty()) {
        err("E0724",
            at.span,
            std::format("`{}` is generic; a function value needs its type arguments", entry.name),
            "call it, or write the instance name explicitly");
        return nullptr;
    }
    const std::string local = declBase(*item).name;
    const Sema::Symbol* sym = owner.find(local);
    if (sym == nullptr) {
        return nullptr;
    }
    const auto& decl = std::get<FnDecl>(item->decl);
    std::vector<Type> targs;
    if (!at.generic_args.empty()) {
        for (const Type& a : at.generic_args) {
            targs.push_back(resolved(a));
        }
    } else {
        // Infer from the expected `fn(...)` type, matching each parameter and the return type.
        const Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
        std::map<std::string, Type> bound;
        bool ok = want.kind == TypeKind::FnPtr && want.params.size() == decl.params.size();
        for (std::size_t i = 0; ok && i < decl.params.size(); ++i) {
            ok = unifyParam(decl.params[i].type, want.params[i], names, bound, owner);
        }
        if (ok && want.ret) {
            ok = unifyParam(decl.return_type, *want.ret, names, bound, owner);
        }
        for (const std::string& n : names) {
            const auto it = bound.find(n);
            if (!ok || it == bound.end()) {
                err("E0724",
                    at.span,
                    std::format("cannot infer the type arguments of `{}`", entry.name),
                    std::format("write them: `{}<...>` as a value", entry.name));
                return nullptr;
            }
            targs.push_back(it->second);
        }
    }
    const auto inst = owner.instantiate(Sema::EntityRef{&owner, sym, local}, targs, at.span);
    if (!inst) {
        return nullptr;
    }
    const auto& idecl = std::get<FnDecl>(inst->sym->item->decl);
    EP f = mk(hir::ExprKind::FuncRef, fnTypeOf(owner, idecl), at.span);
    f->text = inst->qname();
    return f;
}

Lowerer::EP Lowerer::fromEntry(const ScopeEntry& entry, const Expr& at, const Type* expected) {
    switch (entry.kind) {
    case ScopeEntry::Kind::Local: {
        EP l = mk(hir::ExprKind::LocalRef, entry.type, at.span);
        l->local = entry.local;
        l->text = entry.name;
        l->lvalue = true;
        return l;
    }
    case ScopeEntry::Kind::Global: {
        EP g = mk(hir::ExprKind::GlobalRef, entry.type, at.span);
        g->text = entry.qname.empty() ? entry.name : entry.qname;
        g->lvalue = true;
        g->readonly = entry.readonly;
        return g;
    }
    case ScopeEntry::Kind::Function: {
        Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
        const auto cands = candidatesOf(entry);
        if (cands.empty()) {
            return genericFnValue(entry, at, expected);
        }
        std::size_t pick = 0;
        if (cands.size() > 1) {
            // An overloaded name used as a value is chosen by the expected `fn(...)` type.
            const Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
            std::vector<std::size_t> match;
            if (want.kind == TypeKind::FnPtr) {
                for (std::size_t i = 0; i < cands.size(); ++i) {
                    if (typeToString(Type::makeFnPtr(cands[i].params, cands[i].ret)) ==
                        typeToString(want)) {
                        match.push_back(i);
                    }
                }
            }
            if (match.size() != 1) {
                if (want.kind != TypeKind::FnPtr || match.size() > 1) {
                    err("E0403",
                        at.span,
                        std::format("`{}` used as a value is ambiguous", entry.name),
                        "write a `fn(...)` type that matches exactly one overload");
                } else {
                    err("E0404",
                        at.span,
                        std::format(
                            "no overload of `{}` has type `{}`", entry.name, typeToString(want)),
                        "the expected type must match one overload");
                }
                return nullptr;
            }
            pick = match.front();
        }
        if (sh_.isHook(cands[pick].key)) {
            err("E0727",
                at.span,
                std::format("`{}` is an `@init`, `@fini` or `@entry` function and cannot be used "
                            "as a value",
                            entry.name),
                "hook functions are started by the generated code; take a normal function instead");
            return nullptr;
        }
        EP f = mk(hir::ExprKind::FuncRef, fnTypeOf(owner, *cands[pick].decl), at.span);
        f->text = cands[pick].key;
        return f;
    }
    case ScopeEntry::Kind::Constant: {
        // Evaluated in the module that defines it.
        Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
        Expr name;
        name.kind = Expr::Kind::Name;
        name.text = declBase(*entry.item).name;
        name.span = at.span;
        const auto v = owner.evalConst(name);
        return v ? fromConst(*v, expected, at.span) : nullptr;
    }
    case ScopeEntry::Kind::Type:
        err("E0706",
            at.span,
            std::format("`{}` is a type, not a value", entry.name),
            "a value is expected here");
        return nullptr;
    case ScopeEntry::Kind::Namespace:
        err("E0706",
            at.span,
            std::format("`{}` is a module, not a value", entry.name),
            std::format("use `{}.name`", entry.name));
        return nullptr;
    case ScopeEntry::Kind::External: {
        EP x = mk(hir::ExprKind::External, lower::unknownType(), at.span);
        x->text = entry.module;
        x->member = entry.member;
        return x;
    }
    }
    return nullptr;
}

std::optional<Type> Lowerer::instancesOf(const Type* expected, const std::string& qname) {
    if (expected == nullptr) {
        return std::nullopt;
    }
    const Type t = resolved(*expected);
    // A payload enum's instance is a `Struct`; a value enum's is an `Enum`. Both are named
    // `Result<i32, E>`, so the name of the expected type is what is compared.
    if (t.kind != TypeKind::Struct && t.kind != TypeKind::Enum) {
        return std::nullopt;
    }
    const std::string base = t.path.front().substr(0, t.path.front().find('<'));
    return base == qname ? std::optional<Type>(t) : std::nullopt;
}

std::optional<Lowerer::Sum>
Lowerer::sumFor(const ScopeEntry& head, const Type* expected, const Span& at) {
    // `Result.Ok(v)` of a generic enum names a member of one instance, chosen by the type the
    // surrounding expression expects; the declaration's own variant list has parameter types and
    // no instance to build.
    Sema& owner = head.owner != nullptr ? *head.owner : s_;
    if (head.kind == ScopeEntry::Kind::Type && head.item != nullptr &&
        !owner.genericParamsOf(*head.item).empty()) {
        const std::string name = declBase(*head.item).name;
        const auto instance = instancesOf(expected, owner.qualify(name));
        if (!instance) {
            err("E0756",
                at,
                std::format("cannot infer the type arguments of enum `{}`", name),
                std::format("write the target type, e.g.\n"
                            "    local r: {}<...> = {}.Variant",
                            name,
                            name));
            return std::nullopt;
        }
        return sumOfType(*instance);
    }
    return sumOfEntry(head);
}

bool Lowerer::isGenericPayloadEnum(const ScopeEntry& head) const {
    if (head.kind != ScopeEntry::Kind::Type || head.item == nullptr ||
        sumDecl(head.item) == nullptr) {
        return false;
    }
    const Sema& owner = head.owner != nullptr ? *head.owner : s_;
    return !owner.genericParamsOf(*head.item).empty();
}

Lowerer::EP Lowerer::fieldExpr(const Expr& e, const Type* expected) {
    if (const ScopeEntry* head = chainEntry(*e.lhs)) {
        if (head->kind == ScopeEntry::Kind::Namespace) {
            const ScopeEntry* member = namespaceMember(*head, e.text, e.span, true);
            return member != nullptr ? fromEntry(*member, e, nullptr) : nullptr;
        }
        if (const auto sum = sumFor(*head, expected, e.span)) {
            return sumVariantValue(*sum, e);
        }
        if (isGenericPayloadEnum(*head)) {
            return nullptr; // diagnosed by `sumFor`
        }
        if (head->kind == ScopeEntry::Kind::Type && head->item != nullptr &&
            std::holds_alternative<EnumDecl>(head->item->decl)) {
            Sema& owner = head->owner != nullptr ? *head->owner : s_;
            const std::string name = declBase(*head->item).name;
            // `Result.Ok` of a generic enum names a member of the instance the surrounding
            // expression expects (`local r: Result<i32, E> = Result.Ok(1)`); without that type the
            // instance is unknown (E0756).
            if (!owner.genericParamsOf(*head->item).empty()) {
                const std::optional<Type> instance = instancesOf(expected, owner.qualify(name));
                if (!instance) {
                    err("E0756",
                        e.span,
                        std::format("cannot infer the type arguments of enum `{}`", name),
                        std::format("write the target type, e.g.\n"
                                    "    local r: {}<...> = {}.{}",
                                    name,
                                    name,
                                    e.text));
                    return nullptr;
                }
                // `enumMember` finds a type by its local name, so the module qualifier of the
                // instance path is dropped before the lookup.
                const std::string local = instance->path.front().substr(
                    instance->path.front().rfind("::") == std::string::npos
                        ? 0
                        : instance->path.front().rfind("::") + 2);
                const auto v = owner.enumMember(local, e.text, e.span);
                if (!v) {
                    return nullptr;
                }
                EP out = fromConst(*v, nullptr, e.span);
                if (out && out->kind != hir::ExprKind::FloatConst &&
                    out->kind != hir::ExprKind::BoolConst) {
                    out->type = *instance;
                }
                return out;
            }
            const auto v = owner.enumMember(name, e.text, e.span);
            if (!v) {
                return nullptr;
            }
            Type et = Type::makeNamed({name});
            et.span = e.span;
            const Type enum_type = resolvedIn(owner, et);
            EP out = fromConst(*v, nullptr, e.span);
            if (out && out->kind != hir::ExprKind::FloatConst &&
                out->kind != hir::ExprKind::BoolConst) {
                out->type = enum_type;
            }
            return out;
        }
    }
    EP base = expr(*e.lhs, nullptr);
    if (!base) {
        return nullptr;
    }
    if (lower::isUnknown(base->type)) {
        EP f = mk(hir::ExprKind::Field, lower::unknownType(), e.span);
        f->text = e.text;
        f->lhs = std::move(base);
        return f;
    }
    bool through = false;
    Type agg = base->type;
    if (agg.kind == TypeKind::Pointer && agg.elem) {
        through = true;
        agg = *agg.elem;
    }
    if (agg.kind == TypeKind::Str || agg.kind == TypeKind::Slice) {
        // `str` and a slice both have two read-only fields: `ptr` (index 0) and `len` (index 1).
        // `str.ptr` narrows to `*u8`; a slice's `ptr` carries the element type.
        Type field_type;
        std::uint32_t index = 0;
        if (e.text == "ptr") {
            field_type = agg.kind == TypeKind::Slice && agg.elem
                             ? Type::makePointer(*agg.elem)
                             : Type::makePointer(Type::makeInt(8, false));
        } else if (e.text == "len") {
            field_type = Type::makeInt(0, false);
            index = 1;
        } else {
            err("E0709",
                e.span,
                std::format("`{}` has no field `{}`", typeToString(agg), e.text),
                "the fields are `ptr` and `len`");
            return nullptr;
        }
        EP f = mk(hir::ExprKind::Field, field_type, e.span);
        f->text = e.text;
        f->index = index;
        f->through_pointer = through;
        f->lvalue = false;
        f->lhs = std::move(base);
        return f;
    }
    if (agg.kind == TypeKind::Array && e.text == "len") {
        // `[T; N].len` is the compile-time constant `N` as `usize`; the array itself is not read,
        // so a base that would run code (`make().len`) is refused instead of being skipped.
        const std::optional<std::uint64_t> n = arrayLength(s_, agg);
        if (!n) {
            return nullptr;
        }
        if (lower::astHasSideEffects(*e.lhs)) {
            err("E0709",
                e.span,
                "`.len` of an array does not evaluate the array",
                std::format("the length is the constant {}; bind the array to a local first, or "
                            "write the constant",
                            *n));
            return nullptr;
        }
        return intConst(*n, Type::makeInt(0, false), e.span);
    }
    if (!lower::isAggregate(agg) && agg.kind != TypeKind::Tuple) {
        err("E0709",
            e.span,
            std::format("`{}` has no field `{}`", typeToString(base->type), e.text),
            "only structs, classes and tuples have fields");
        return nullptr;
    }
    if (agg.kind == TypeKind::Tuple) {
        // `t.0`, `t.1`, ...: the element at that position (`text` is empty for a tuple access).
        if (e.int_value >= agg.args.size()) {
            err("E0709",
                e.span,
                std::format("`{}` has {} element(s), so `.{}` is out of range",
                            typeToString(agg),
                            agg.args.size(),
                            e.int_value),
                "write an element index below the element count");
            return nullptr;
        }
        EP f = mk(hir::ExprKind::Field, agg.args[e.int_value], e.span);
        f->index = static_cast<std::uint32_t>(e.int_value);
        f->through_pointer = through;
        f->lvalue = through || base->lvalue;
        f->readonly = !through && base->readonly;
        f->lhs = std::move(base);
        return f;
    }
    std::vector<FieldInfo> fields;
    collectFields(agg.path.front(), fields);
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (fields[i].decl->name == e.text) {
            EP f = mk(
                hir::ExprKind::Field, resolvedIn(*fields[i].owner, fields[i].decl->type), e.span);
            f->text = e.text;
            f->index = static_cast<std::uint32_t>(i);
            f->through_pointer = through;
            f->lvalue = through || base->lvalue;
            f->readonly = !through && base->readonly;
            f->lhs = std::move(base);
            return f;
        }
    }
    err("E0709",
        e.span,
        std::format("`{}` has no field `{}`", typeToString(agg), e.text),
        "unknown field");
    return nullptr;
}

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
            if (prelude_ == nullptr || in_defer_ != 0 || v->type.isVoid()) {
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

bool Lowerer::lowerArgs(const Expr& call,
                        const std::vector<Type>& params,
                        std::vector<EP>& out,
                        const std::string& callee) {
    if (call.args.size() != params.size()) {
        err("E0710",
            call.span,
            std::format(
                "`{}` takes {} argument(s), {} given", callee, params.size(), call.args.size()),
            "wrong number of arguments");
        for (const ExprPtr& a : call.args) {
            (void)expr(*a, nullptr);
        }
        return false;
    }
    bool ok = true;
    for (std::size_t i = 0; i < params.size(); ++i) {
        EP a = moveOut(coerce(expr(*call.args[i], &params[i]), params[i], call.args[i]->span));
        ok = ok && a != nullptr;
        out.push_back(std::move(a));
    }
    return ok;
}

Lowerer::EP Lowerer::syscallExpr(const Expr& e) {
    if (e.args.empty()) {
        err("E0710", e.span, "`syscall` needs the syscall number", "wrong number of arguments");
        return nullptr;
    }
    const auto limit = lower::syscallMaxArgs(s_.config_.target);
    if (limit && e.args.size() - 1 > *limit) {
        err("E0710",
            e.span,
            std::format("`syscall` has {} argument(s) after the number, `{}` allows at most {}",
                        e.args.size() - 1,
                        s_.config_.target.triple.empty()
                            ? s_.config_.target.os + "/" + s_.config_.target.arch
                            : s_.config_.target.triple,
                        *limit),
            "too many arguments for this target");
        return nullptr;
    }
    EP out = mk(hir::ExprKind::Syscall, Type::makeInt(64, true), e.span);
    bool ok = true;
    for (const ExprPtr& a : e.args) {
        EP v = expr(*a, nullptr);
        if (!v) {
            ok = false;
            continue;
        }
        if (!lower::isUnknown(v->type) && v->type.kind != TypeKind::Int &&
            v->type.kind != TypeKind::Pointer) {
            err("E0708",
                a->span,
                std::format("`syscall` arguments are integers or pointers, found `{}`",
                            typeToString(v->type)),
                "integer or `*T` expected");
            ok = false;
        }
        out->args.push_back(std::move(v));
    }
    if (!ok) {
        return nullptr;
    }
    return out;
}

/// A compile-time `MemoryOrder` member (`MemoryOrder.seqCst`): 0 relaxed .. 4 seqCst.
std::optional<std::uint32_t> Lowerer::memoryOrder(const Expr& arg) {
    EP v = expr(arg, nullptr);
    if (!v) {
        return std::nullopt;
    }
    if (v->kind != hir::ExprKind::IntConst || v->type.kind != TypeKind::Enum ||
        v->type.path.empty() || v->type.path.front() != "MemoryOrder") {
        err("E0708",
            arg.span,
            "the memory order must be a `MemoryOrder` member",
            "write `MemoryOrder.seqCst` (a compile-time constant)");
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(v->int_value);
}

Lowerer::EP Lowerer::builtinCall(const Expr& e, const std::string& name) {
    const bool is_load = name == "__builtin_atomicLoad";
    const bool is_store = name == "__builtin_atomicStore";
    const bool is_xchg = name == "__builtin_atomicXchg";
    const bool is_cas = name == "__builtin_atomicCas";
    const bool is_add = name == "__builtin_atomicAdd";
    if (name == "__builtin_asm") {
        return asmCall(e);
    }
    if (name == "__builtin_write") {
        err("E0709",
            e.span,
            "`__builtin_write` is a statement, not a value",
            "call it as a statement: `__builtin_write(p, v)`");
        return nullptr;
    }
    if (name == "__builtin_read") {
        // `__builtin_read(p)`: the value at `p`, copied out. Nothing is dropped or moved; the
        // caller owns the copy.
        if (e.args.size() != 1) {
            err("E0710", e.span, "`__builtin_read` takes one pointer", "wrong number of arguments");
            return nullptr;
        }
        EP p = expr(*e.args[0], nullptr);
        if (!p) {
            return nullptr;
        }
        if (p->type.kind != TypeKind::Pointer || !p->type.elem) {
            err("E0709",
                e.args[0]->span,
                std::format("`__builtin_read` needs a pointer, found `{}`", typeToString(p->type)),
                "pass `&x`");
            return nullptr;
        }
        if (p->type.elem->isVoid()) {
            err("E0709",
                e.args[0]->span,
                "`__builtin_read` cannot read a `void`",
                "pass a pointer to a typed value");
            return nullptr;
        }
        EP r = mk(hir::ExprKind::RawRead, *p->type.elem, e.span);
        r->lhs = std::move(p);
        return r;
    }
    if (name == "__builtin_needsDrop") {
        // `__builtin_needsDrop<T>()`: a compile-time bool.
        static const std::vector<Type> none;
        const auto& written = e.lhs ? e.lhs->generic_args : none;
        if (written.size() != 1 || !e.args.empty()) {
            err("E0710",
                e.span,
                "`__builtin_needsDrop` takes one type: `__builtin_needsDrop<T>()`",
                "wrong number of arguments");
            return nullptr;
        }
        return boolConst(needsDrop(resolved(written[0])), e.span);
    }
    if (name == "__builtin_drop") {
        // `__builtin_drop(p)`: runs the drop of the pointee of `p` (nothing when it needs none).
        if (e.args.size() != 1) {
            err("E0710", e.span, "`__builtin_drop` takes one pointer", "wrong number of arguments");
            return nullptr;
        }
        EP p = expr(*e.args[0], nullptr);
        if (!p) {
            return nullptr;
        }
        if (p->type.kind != TypeKind::Pointer || !p->type.elem) {
            err("E0709",
                e.args[0]->span,
                std::format("`__builtin_drop` needs a pointer, found `{}`", typeToString(p->type)),
                "pass `&x`");
            return nullptr;
        }
        const Type t = *p->type.elem;
        if (!needsDrop(t)) {
            EP nop = mk(hir::ExprKind::Asm, Type::makeVoid(), e.span);
            return nop;
        }
        return dropCallOn(std::move(p), t, e.span);
    }
    if (name == "__builtin_unwindPads" || name == "__builtin_unwindPadCount") {
        if (!e.args.empty()) {
            err("E0710",
                e.span,
                std::format("`{}` takes no arguments", name),
                "wrong number of arguments");
            return nullptr;
        }
        const bool pads = name == "__builtin_unwindPads";
        return mk(pads ? hir::ExprKind::UnwindPads : hir::ExprKind::UnwindPadCount,
                  pads ? Type::makePointer(Type::makeInt(0, false)) : Type::makeInt(0, false),
                  e.span);
    }
    if (name == "__builtin_fence") {
        if (e.args.size() != 1) {
            err("E0710",
                e.span,
                "`__builtin_fence` takes the memory order",
                "wrong number of arguments");
            return nullptr;
        }
        const auto order = memoryOrder(*e.args[0]);
        if (!order) {
            return nullptr;
        }
        if (*order == 0) {
            err("E0708",
                e.args[0]->span,
                "a fence cannot be `relaxed`",
                "use acquire, release, acqRel or seqCst");
            return nullptr;
        }
        EP f = mk(hir::ExprKind::Fence, Type::makeVoid(), e.span);
        f->index = *order;
        return f;
    }
    if (!(is_load || is_store || is_xchg || is_cas || is_add)) {
        err("E0706",
            e.lhs->span,
            std::format("unknown builtin `{}`", name),
            "not in the closed builtin list");
        return nullptr;
    }
    const std::size_t want_args = is_load ? 2 : (is_cas ? 4 : 3);
    if (e.args.size() != want_args) {
        err("E0710",
            e.span,
            std::format("`{}` takes {} arguments, {} given", name, want_args, e.args.size()),
            "wrong number of arguments");
        for (const ExprPtr& a : e.args) {
            (void)expr(*a, nullptr);
        }
        return nullptr;
    }
    EP ptr = expr(*e.args[0], nullptr);
    if (!ptr) {
        return nullptr;
    }
    if (lower::isUnknown(ptr->type)) {
        return mk(hir::ExprKind::AtomicLoad, lower::unknownType(), e.span);
    }
    if (ptr->type.kind != TypeKind::Pointer || !ptr->type.elem) {
        err("E0708",
            e.args[0]->span,
            std::format("atomics take a pointer, found `{}`", typeToString(ptr->type)),
            "pointer expected");
        return nullptr;
    }
    const Type elem = *ptr->type.elem;
    const bool ok_type =
        elem.kind == TypeKind::Int || elem.kind == TypeKind::Bool || elem.kind == TypeKind::Pointer;
    if (!ok_type) {
        err("E0708",
            e.args[0]->span,
            std::format("atomics work on integers, `bool` and pointers, not `{}`",
                        typeToString(elem)),
            "unsupported atomic type");
        return nullptr;
    }
    if (is_add && elem.kind != TypeKind::Int) {
        err("E0708", e.args[0]->span, "`__builtin_atomicAdd` needs an integer", "not an integer");
        return nullptr;
    }
    const auto order = memoryOrder(*e.args[want_args - 1]);
    if (!order) {
        return nullptr;
    }
    if (is_load && (*order == 2 || *order == 3)) {
        err("E0708",
            e.args[want_args - 1]->span,
            "a load cannot be `release` or `acqRel`",
            "use relaxed, acquire or seqCst");
        return nullptr;
    }
    if (is_store && (*order == 1 || *order == 3)) {
        err("E0708",
            e.args[want_args - 1]->span,
            "a store cannot be `acquire` or `acqRel`",
            "use relaxed, release or seqCst");
        return nullptr;
    }
    const auto valueArg = [&](std::size_t i) {
        return coerce(expr(*e.args[i], &elem), elem, e.args[i]->span);
    };
    hir::ExprKind kind = hir::ExprKind::AtomicLoad;
    Type result = elem;
    if (is_store) {
        kind = hir::ExprKind::AtomicStore;
        result = Type::makeVoid();
    } else if (is_xchg) {
        kind = hir::ExprKind::AtomicXchg;
    } else if (is_cas) {
        kind = hir::ExprKind::AtomicCas;
    } else if (is_add) {
        kind = hir::ExprKind::AtomicAdd;
    }
    EP out = mk(kind, result, e.span);
    out->index = *order;
    out->lhs = std::move(ptr);
    if (is_store || is_xchg || is_add) {
        out->rhs = valueArg(1);
        if (!out->rhs) {
            return nullptr;
        }
    } else if (is_cas) {
        out->rhs = valueArg(1);
        EP desired = valueArg(2);
        if (!out->rhs || !desired) {
            return nullptr;
        }
        out->args.push_back(std::move(desired));
    }
    return out;
}

Lowerer::EP Lowerer::asmCall(const Expr& e) {
    if (e.args.empty() || e.args[0]->kind != Expr::Kind::StringLiteral) {
        err("E0709",
            e.span,
            "`__builtin_asm` needs a template string first",
            "write `__builtin_asm(\"nop\")`");
        return nullptr;
    }
    EP out = mk(hir::ExprKind::Asm, Type::makeVoid(), e.span);
    out->text = e.args[0]->text;
    bool ok = true;
    for (std::size_t i = 1; i < e.args.size(); ++i) {
        const Expr& a = *e.args[i];
        if (a.kind == Expr::Kind::AsmOperand) {
            hir::AsmOperand op;
            op.kind = a.text == "in"    ? hir::AsmOperand::Kind::In
                      : a.text == "out" ? hir::AsmOperand::Kind::Out
                                        : hir::AsmOperand::Kind::InOut;
            if (a.lhs->kind == Expr::Kind::StringLiteral) {
                op.reg = a.lhs->text;
            } else if (a.lhs->kind == Expr::Kind::Name && a.lhs->text == "reg") {
                op.reg = "reg";
            } else {
                err("E0709",
                    a.lhs->span,
                    "register must be `reg` or a register name string",
                    "invalid register");
                ok = false;
                continue;
            }
            EP v = expr(*a.rhs, nullptr);
            if (!v) {
                ok = false;
                continue;
            }
            if (!lower::isUnknown(v->type) && v->type.kind != TypeKind::Int &&
                v->type.kind != TypeKind::Pointer && v->type.kind != TypeKind::Float &&
                v->type.kind != TypeKind::FnPtr) {
                err("E0708",
                    a.rhs->span,
                    std::format("asm operands are scalars, found `{}`", typeToString(v->type)),
                    "integer, float or pointer expected");
                ok = false;
                continue;
            }
            if (op.kind != hir::AsmOperand::Kind::In && !v->lvalue) {
                err("E0709",
                    a.rhs->span,
                    "an asm output needs a place",
                    "write a variable or `*p`");
                ok = false;
                continue;
            }
            op.value = std::move(v);
            out->asm_operands.push_back(std::move(op));
        } else if (a.kind == Expr::Kind::Call && a.lhs->kind == Expr::Kind::Name &&
                   (a.lhs->text == "clobber" || a.lhs->text == "options")) {
            for (const ExprPtr& arg : a.args) {
                if (arg->kind != Expr::Kind::StringLiteral) {
                    err("E0709",
                        arg->span,
                        std::format("`{}` takes strings", a.lhs->text),
                        "string expected");
                    ok = false;
                    continue;
                }
                if (a.lhs->text == "clobber") {
                    out->clobbers.push_back(arg->text);
                } else if (arg->text == "nomem") {
                    out->nomem = true;
                } else if (arg->text == "noreturn") {
                    out->noreturn = true;
                } else {
                    err("E0709",
                        arg->span,
                        std::format("unknown asm option `{}`", arg->text),
                        "known options: nomem, noreturn");
                    ok = false;
                }
            }
        } else {
            err("E0709",
                a.span,
                "unexpected `__builtin_asm` argument",
                "operands are `in(..)`, `out(..)`, `inout(..)`, `clobber(..)`, `options(..)`");
            ok = false;
        }
    }
    if (!ok) {
        return nullptr;
    }
    return out;
}

std::optional<Lowerer::MethodRef> Lowerer::findMethod(const Type& agg, const std::string& name) {
    if (agg.kind == TypeKind::Interface) {
        const auto ref = entityOfType(agg);
        if (!ref || !std::holds_alternative<InterfaceDecl>(ref->sym->item->decl)) {
            return std::nullopt;
        }
        const auto& decl = std::get<InterfaceDecl>(ref->sym->item->decl);
        for (std::size_t i = 0; i < decl.methods.size(); ++i) {
            if (decl.methods[i].name == name) {
                MethodRef m;
                m.kind = MethodRef::Kind::Itable;
                m.decl = &decl.methods[i];
                m.owner = ref->owner;
                m.index = static_cast<std::uint32_t>(i);
                return m;
            }
        }
        return std::nullopt;
    }
    std::string cur = agg.path.front();
    std::set<std::string> seen;
    while (!cur.empty() && seen.insert(cur).second) {
        const auto ref = s_.entityByQName(cur);
        const ClassDecl* c = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
        if (c != nullptr) {
            for (const FnDecl& m : c->methods) {
                if (m.name == name) {
                    MethodRef r;
                    r.kind = (m.is_virtual || m.is_override) ? MethodRef::Kind::Virtual
                                                             : MethodRef::Kind::Direct;
                    r.fn_name = cur + "." + name;
                    r.decl = &m;
                    r.owner = ref->owner;
                    r.cls = c;
                    r.cls_q = cur;
                    if (r.kind == MethodRef::Kind::Virtual) {
                        const auto& slots = vtableOf(agg.path.front());
                        for (std::size_t i = 0; i < slots.size(); ++i) {
                            if (slots[i].slot.method == name) {
                                r.index = static_cast<std::uint32_t>(i);
                                r.fn_name = slots[i].slot.function;
                            }
                        }
                    }
                    return r;
                }
            }
        }
        instantiateImpls();
        for (const ImplInfo& i : sh_.impls) {
            if (i.type_q != cur) {
                continue;
            }
            for (const FnDecl& m : i.decl->methods) {
                if (m.name == name) {
                    MethodRef r;
                    r.fn_name = i.prefix + name;
                    r.decl = &m;
                    r.owner = i.owner;
                    r.from_impl = true;
                    return r;
                }
            }
        }
        cur.clear();
        if (c != nullptr && c->has_base) {
            Type base = Type::makeNamed(c->base);
            if (const auto r = ref->owner->resolve(base); r && r->kind == TypeKind::Class) {
                cur = r->path.front();
            }
        }
    }
    return std::nullopt;
}

Lowerer::EP Lowerer::methodCall(const Expr& e, const Expr& recv_ast) {
    const Expr& callee = *e.lhs;
    const std::size_t mark = preludeMark();
    EP recv = expr(recv_ast, nullptr);
    if (!recv) {
        for (const ExprPtr& a : e.args) {
            (void)expr(*a, nullptr);
        }
        return nullptr;
    }
    if (lower::isUnknown(recv->type)) {
        EP c = mk(hir::ExprKind::CallIndirect, lower::unknownType(), e.span);
        c->lhs = std::move(recv);
        for (const ExprPtr& a : e.args) {
            c->args.push_back(expr(*a, nullptr));
        }
        return c;
    }
    if (!recv->lvalue && needsDrop(recv->type)) {
        recv = materializeTemp(std::move(recv), recv_ast.span);
    }
    bool through = false;
    Type agg = recv->type;
    if (agg.kind == TypeKind::Pointer && agg.elem) {
        through = true;
        agg = *agg.elem;
    }
    if (agg.kind != TypeKind::Interface && !lower::isAggregate(agg)) {
        err("E0709",
            callee.span,
            std::format("`{}` has no method `{}`", typeToString(recv->type), callee.text),
            "only structs, classes and interfaces have methods");
        return nullptr;
    }
    // `value.field(args)` where `field` holds a function pointer: an indirect call.
    if (lower::isAggregate(agg)) {
        std::vector<FieldInfo> fields;
        collectFields(agg.path.front(), fields);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].decl->name != callee.text) {
                continue;
            }
            const Type ft = resolvedIn(*fields[i].owner, fields[i].decl->type);
            if (ft.kind != TypeKind::FnPtr || !ft.ret) {
                break;
            }
            EP f = mk(hir::ExprKind::Field, ft, callee.span);
            f->text = callee.text;
            f->index = static_cast<std::uint32_t>(i);
            f->through_pointer = through;
            f->lvalue = true;
            f->lhs = std::move(recv);
            std::vector<EP> args;
            if (!lowerArgs(e, ft.params, args, callee.text)) {
                return nullptr;
            }
            EP c = mk(hir::ExprKind::CallIndirect, *ft.ret, e.span);
            c->lhs = std::move(f);
            c->args = std::move(args);
            return c;
        }
    }
    auto m = findMethod(agg, callee.text);
    if (!m && lower::isAggregate(agg)) {
        // A method of a struct (or a class without it) from an inherent `impl`: `T.name(self, ...)`.
        cutPrelude(mark);
        ExprPtr receiver = cloneAst(recv_ast);
        if (through) {
            receiver = lower::astUnary(UnaryOp::Deref, std::move(receiver), recv_ast.span);
        }
        std::vector<ExprPtr> rest;
        for (const ExprPtr& a : e.args) {
            rest.push_back(cloneAst(*a));
        }
        bool found = false;
        EP c = callMethod(std::move(receiver), agg, callee.text, std::move(rest), e.span, found);
        if (found) {
            return c;
        }
    }
    if (!m) {
        err("E0709",
            callee.span,
            std::format("`{}` has no method `{}`", typeToString(agg), callee.text),
            "unknown method");
        for (const ExprPtr& a : e.args) {
            (void)expr(*a, nullptr);
        }
        return nullptr;
    }
    std::vector<EP> args;
    bool overloaded_args = false;
    if (!m->decl->generic_params.empty()) {
        // A generic method: its instance is chosen by the written or inferred type arguments.
        if (m->kind != MethodRef::Kind::Direct || m->cls == nullptr) {
            err("E0733",
                callee.span,
                std::format("generic method `{}` is only supported in a class", callee.text),
                "move the method into the class");
            return nullptr;
        }
        const std::vector<std::string>& names = m->decl->generic_params;
        std::vector<PreArg> pre = preLowerArgs(e);
        std::vector<Type> targs;
        if (!callee.generic_args.empty()) {
            if (callee.generic_args.size() != names.size()) {
                err("E0724",
                    callee.span,
                    std::format("`{}` takes {} type argument(s), {} given",
                                callee.text,
                                names.size(),
                                callee.generic_args.size()),
                    "write one type per type parameter");
                return nullptr;
            }
            for (const Type& a : callee.generic_args) {
                targs.push_back(resolved(a));
            }
        } else {
            std::map<std::string, Type> bound;
            bool ok = true;
            std::size_t i = 0;
            for (const Param& p : m->decl->params) {
                if (p.is_self) {
                    continue;
                }
                if (i >= pre.size() || lower::isUnknown(pre[i].type) ||
                    !unifyParam(p.type, pre[i].type, names, bound, *m->owner)) {
                    ok = false;
                    break;
                }
                ++i;
            }
            for (const std::string& n : names) {
                if (!ok || !bound.contains(n)) {
                    err("E0724",
                        callee.span,
                        std::format("cannot infer `{}` of `{}`", n, callee.text),
                        std::format("write the type arguments: `.{}<...>(...)`", callee.text));
                    return nullptr;
                }
                targs.push_back(bound.at(n));
            }
        }
        const Sema::MethodInstance* inst =
            m->owner->instantiateMethod(*m->decl, m->cls->name, m->cls_q, targs);
        std::vector<Type> params;
        for (const Param& p : inst->decl.params) {
            if (!p.is_self) {
                params.push_back(resolvedIn(*m->owner, p.type));
            }
        }
        m->decl = &inst->decl;
        m->fn_name = inst->key;
        if (!lowerArgsPre(e, params, pre, args, callee.text)) {
            return nullptr;
        }
        overloaded_args = true;
    }
    if (m->kind == MethodRef::Kind::Direct && m->cls != nullptr && !overloaded_args) {
        // Non-virtual methods of one class may overload each other.
        const auto cands = methodCandidates(*m, callee.text);
        if (cands.size() > 1) {
            std::vector<PreArg> pre = preLowerArgs(e);
            const auto chosen = resolveOverload(e, callee.text, cands, pre);
            if (!chosen) {
                return nullptr;
            }
            m->decl = cands[*chosen].decl;
            m->fn_name = cands[*chosen].key;
            overloaded_args = true;
            if (!lowerArgsPre(e, cands[*chosen].params, pre, args, callee.text)) {
                return nullptr;
            }
        }
    }
    std::vector<Type> params;
    for (const Param& p : m->decl->params) {
        if (!p.is_self) {
            params.push_back(resolvedIn(*m->owner, p.type));
        }
    }
    const Type ret = resolvedIn(*m->owner, m->decl->return_type);
    if (!overloaded_args && !lowerArgs(e, params, args, callee.text)) {
        return nullptr;
    }

    if (m->kind == MethodRef::Kind::Itable) {
        if (through) { // pointer to an interface value: call through the value
            EP d = mk(hir::ExprKind::Deref, agg, recv->span);
            d->lvalue = true;
            d->lhs = std::move(recv);
            recv = std::move(d);
        }
        EP c = mk(hir::ExprKind::CallItable, ret, e.span);
        c->text = callee.text;
        c->index = m->index;
        c->lhs = std::move(recv);
        c->args = std::move(args);
        return c;
    }

    EP self;
    if (through) {
        self = std::move(recv);
    } else {
        if (!recv->lvalue) {
            err("E0709",
                recv_ast.span,
                "a method needs an addressable receiver",
                "store the value in a variable first");
            return nullptr;
        }
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(agg), recv->span);
        addr->lhs = std::move(recv);
        self = std::move(addr);
    }
    EP c =
        mk(m->kind == MethodRef::Kind::Virtual ? hir::ExprKind::CallVirtual : hir::ExprKind::Call,
           ret,
           e.span);
    c->text = m->fn_name;
    c->index = m->index;
    c->member = callee.text;
    c->args.push_back(std::move(self));
    for (EP& a : args) {
        c->args.push_back(std::move(a));
    }
    return c;
}

Lowerer::EP Lowerer::callEntry(const Expr& e, const ScopeEntry& entry, const std::string& name) {
    if (entry.kind == ScopeEntry::Kind::Function) {
        // Overloaded or generic calls lower their arguments once here; the choice is made from
        // their cached types.
        std::vector<const Item*> items = entry.overloads;
        if (items.empty()) {
            items.push_back(entry.item);
        }
        bool generic = false;
        bool pack = false;
        for (const Item* item : items) {
            generic = generic || !s_.genericParamsOf(*item).empty();
            pack = pack || Sema::isPackFn(*item);
        }
        const bool many = items.size() > 1 || generic || pack;
        std::vector<PreArg> pre;
        if (many) {
            pre = preLowerArgs(e);
        }
        auto cands = candidatesOf(entry);
        for (OverloadCandidate& g : genericCandidates(entry, e, pre)) {
            cands.push_back(std::move(g));
        }
        // A pack instance is made only when no fixed-arity overload has the call's argument count,
        // so an exact overload is always preferred over the pack.
        const bool fixed_arity = std::ranges::any_of(
            cands, [&e](const OverloadCandidate& c) { return c.params.size() == e.args.size(); });
        if (pack && !fixed_arity) {
            for (OverloadCandidate& p : packCandidates(entry, e, pre)) {
                cands.push_back(std::move(p));
            }
        }
        if (cands.empty()) {
            err("E0404",
                e.span,
                std::format("no `{}` fits this call", name),
                "check the arguments and the type arguments");
            return nullptr;
        }
        std::size_t pick = 0;
        if (many) {
            const auto chosen = resolveOverload(e, name, cands, pre);
            if (!chosen) {
                return nullptr;
            }
            pick = *chosen;
        }
        const OverloadCandidate& cand = cands[pick];
        if (sh_.isHook(cand.key)) {
            err("E0727",
                e.span,
                std::format("`{}` is an `@init`, `@fini` or `@entry` function and cannot be called",
                            name),
                "hook functions are started by the generated code; move the shared code into a "
                "normal "
                "function and call that");
            return nullptr;
        }
        std::vector<EP> args;
        const bool lowered = many ? lowerArgsPre(e, cand.params, pre, args, name)
                                  : lowerArgs(e, cand.params, args, name);
        if (!lowered) {
            return nullptr;
        }
        EP c = mk(hir::ExprKind::Call, cand.ret, e.span);
        c->text = cand.key;
        c->args = std::move(args);
        return c;
    }
    if (entry.kind == ScopeEntry::Kind::External) {
        EP c = mk(hir::ExprKind::ExternalCall, lower::unknownType(), e.span);
        c->text = entry.module;
        c->member = entry.member;
        for (const ExprPtr& a : e.args) {
            c->args.push_back(expr(*a, nullptr));
        }
        return c;
    }
    return nullptr;
}

Lowerer::EP Lowerer::callExpr(const Expr& e, const Type* expected) {
    const Expr& callee = *e.lhs;
    (void)expected;
    if (callee.kind == Expr::Kind::Name) {
        const std::string& name = callee.text;
        const ScopeEntry* entry = lookupName(name);
        const bool shadowed = entry != nullptr && entry->kind != ScopeEntry::Kind::Type;
        if (!shadowed) {
            if (name == "syscall") {
                return syscallExpr(e);
            }
            if (name == "sizeOf" || name == "alignOf" || name == "offsetOf") {
                const Type size_t_type = Type::makeInt(0, false);
                return constExpr(e, &size_t_type);
            }
            if (name == "staticAssert") {
                err("E0709", e.span, "`staticAssert` is a statement", "it has no value");
                return nullptr;
            }
            if (name.starts_with("__builtin_")) {
                return builtinCall(e, name);
            }
        }
        if (entry != nullptr && entry->kind == ScopeEntry::Kind::Type && entry->item != nullptr &&
            std::holds_alternative<ClassDecl>(entry->item->decl)) {
            const Sema& holder = entry->owner != nullptr ? *entry->owner : s_;
            if (!holder.genericParamsOf(*entry->item).empty()) {
                return genericConstruct(e, callee, *entry); // `Vec<i32>(args)`
            }
            return constructCall(e, *entry); // `T(args)` builds a class
        }
        if (entry != nullptr) {
            if (EP c = callEntry(e, *entry, name)) {
                return c;
            }
            if (entry->kind == ScopeEntry::Kind::Function ||
                entry->kind == ScopeEntry::Kind::External) {
                return nullptr; // diagnosed
            }
        } else if (star_import_) {
            EP c = mk(hir::ExprKind::ExternalCall, lower::unknownType(), e.span);
            c->text = "*";
            c->member = name;
            for (const ExprPtr& a : e.args) {
                c->args.push_back(expr(*a, nullptr));
            }
            return c;
        }
    } else if (callee.kind == Expr::Kind::Field) {
        // `ns.fn(...)` (module namespace), `Enum.Variant(...)` or `value.method(...)`.
        if (const ScopeEntry* head = chainEntry(*callee.lhs); head != nullptr) {
            if (const auto sum = sumFor(*head, expected, e.span)) {
                return sumVariantCall(*sum, e);
            }
            if (isGenericPayloadEnum(*head)) {
                return nullptr; // diagnosed by `sumFor`
            }
        }
        if (const ScopeEntry* head = chainEntry(*callee.lhs);
            head != nullptr && head->kind == ScopeEntry::Kind::Namespace) {
            const ScopeEntry* member = namespaceMember(*head, callee.text, callee.span, true);
            if (member == nullptr) {
                return nullptr;
            }
            if (EP c = callEntry(e, *member, callee.text)) {
                return c;
            }
            if (member->kind == ScopeEntry::Kind::Function ||
                member->kind == ScopeEntry::Kind::External) {
                return nullptr;
            }
        }
        if (const ScopeEntry* ent = chainEntry(callee); ent == nullptr ||
                                                        ent->kind == ScopeEntry::Kind::Local ||
                                                        ent->kind == ScopeEntry::Kind::Global) {
            return methodCall(e, *callee.lhs);
        }
    }
    // Anything else: the callee must be a function pointer value.
    EP f = expr(callee, nullptr);
    if (!f) {
        return nullptr;
    }
    if (lower::isUnknown(f->type)) {
        EP c = mk(hir::ExprKind::CallIndirect, lower::unknownType(), e.span);
        c->lhs = std::move(f);
        for (const ExprPtr& a : e.args) {
            c->args.push_back(expr(*a, nullptr));
        }
        return c;
    }
    if (lower::isAggregate(f->type)) {
        return userCall(e, f->type); // `operator()`, and every closure that captures
    }
    if (f->type.kind != TypeKind::FnPtr || !f->type.ret) {
        err("E0709",
            e.span,
            std::format("`{}` is not callable", typeToString(f->type)),
            "expected a function or function pointer");
        return nullptr;
    }
    std::vector<EP> args;
    const std::string label = f->kind == hir::ExprKind::LocalRef ? f->text : "function pointer";
    if (!lowerArgs(e, f->type.params, args, label)) {
        return nullptr;
    }
    EP c = mk(f->kind == hir::ExprKind::FuncRef ? hir::ExprKind::Call : hir::ExprKind::CallIndirect,
              *f->type.ret,
              e.span);
    if (f->kind == hir::ExprKind::FuncRef) {
        c->text = f->text;
    } else {
        c->lhs = std::move(f);
    }
    c->args = std::move(args);
    return c;
}

Lowerer::EP Lowerer::indexExpr(const Expr& e) {
    const std::size_t mark = preludeMark();
    EP base = expr(*e.lhs, nullptr);
    EP idx = expr(*e.rhs, nullptr);
    if (!base || !idx) {
        return nullptr;
    }
    if (lower::isUnknown(base->type)) {
        EP i = mk(hir::ExprKind::Index, lower::unknownType(), e.span);
        i->lhs = std::move(base);
        i->rhs = std::move(idx);
        return i;
    }
    if (lower::isAggregate(base->type)) {
        return userIndex(e, base->type, mark); // `operator[]`
    }
    if (!lower::isUnknown(idx->type) && idx->type.kind != TypeKind::Int) {
        err("E0708",
            e.rhs->span,
            std::format("index must be an integer, found `{}`", typeToString(idx->type)),
            "integer expected");
        return nullptr;
    }
    if (base->type.kind == TypeKind::Str) {
        // Reads one byte; checked against `len` at run time under --safety=checked. Never a place.
        EP i = mk(hir::ExprKind::Index, Type::makeInt(8, false), e.span);
        i->checked = s_.config_.safety == "checked";
        i->lvalue = false;
        i->lhs = std::move(base);
        i->rhs = std::move(idx);
        return i;
    }
    if (base->type.kind == TypeKind::Slice) {
        // A slice is `{ ptr, len }` over `elem`: check against `len`, then index `elem` at `ptr`.
        // A `[]mut T` element is a place (a write lands in the owner's storage); a `[]T` element is
        // a copy out of the view, like a `str` byte, so it is not a place.
        if (!base->type.elem) {
            return nullptr;
        }
        EP i = mk(hir::ExprKind::Index, *base->type.elem, e.span);
        i->checked = s_.config_.safety == "checked";
        i->lvalue = base->type.is_mutable && base->lvalue;
        i->readonly = base->readonly;
        i->lhs = std::move(base);
        i->rhs = std::move(idx);
        return i;
    }
    if (base->type.kind != TypeKind::Array && base->type.kind != TypeKind::Pointer) {
        err("E0709",
            e.span,
            std::format("`{}` cannot be indexed", typeToString(base->type)),
            "only arrays and pointers can be indexed");
        return nullptr;
    }
    const bool is_array = base->type.kind == TypeKind::Array;
    if (!base->type.elem) {
        return nullptr;
    }
    if (!is_array && base->type.elem->isVoid()) {
        err("E0709", e.span, "cannot index `*void`", "cast it to a typed pointer first");
        return nullptr;
    }
    if (is_array && e.rhs->kind == Expr::Kind::IntLiteral) {
        if (const auto len = arrayLength(s_, base->type); len && e.rhs->int_value >= *len) {
            err("E0701",
                e.rhs->span,
                std::format("index {} is out of bounds for `{}` (length {})",
                            e.rhs->int_value,
                            typeToString(base->type),
                            *len),
                "constant index out of range");
            return nullptr;
        }
    }
    EP i = mk(hir::ExprKind::Index, *base->type.elem, e.span);
    i->checked = is_array && s_.config_.safety == "checked";
    i->lvalue = is_array ? base->lvalue : true;
    i->readonly = is_array && base->readonly;
    i->lhs = std::move(base);
    i->rhs = std::move(idx);
    return i;
}

/// `base[lo..hi]` / `base[lo..]` / `base[..hi]` / `base[..]`: a `str` view over the bytes `lo` up
/// to `hi`, never a copy. `str` and `[u8; N]` are the bases: a `str` is already a byte view, and
/// an array of bytes borrows its storage. Both bounds are optional and default to 0 and the length.
/// Under --safety=checked the node carries `checked`, so both bounds are compared against the base
/// length at run time (`E0` at compile time only for constant bounds, below).
Lowerer::EP Lowerer::sliceExpr(const Expr& e) {
    EP base = expr(*e.lhs, nullptr);
    if (!base) {
        return nullptr;
    }
    const auto slicing_is_bytes = [](const Type& t) {
        return t.kind == TypeKind::Str || (t.kind == TypeKind::Array && t.elem &&
                                           t.elem->kind == TypeKind::Int && t.elem->bits == 8);
    };
    // Slicing yields a slice type: `str`/`[u8; N]` keeps its old `str` result for source
    // compatibility, while an array or an existing slice yields `[]T` over its element type.
    bool yields_str = false;
    Type result = Type::makeStr();
    if (!lower::isUnknown(base->type)) {
        if (slicing_is_bytes(base->type)) {
            yields_str = true;
            result = Type::makeStr();
        } else if (base->type.kind == TypeKind::Array && base->type.elem) {
            // A slice is writable only when the owner is a writable place and not `const`.
            result = Type::makeSlice(*base->type.elem, base->lvalue && !base->readonly);
        } else if (base->type.kind == TypeKind::Slice && base->type.elem) {
            result = Type::makeSlice(*base->type.elem,
                                     base->type.is_mutable && base->lvalue && !base->readonly);
        } else {
            err("E0709",
                e.span,
                std::format("`{}` cannot be sliced", typeToString(base->type)),
                "only `str`, arrays and slices can be sliced");
            return nullptr;
        }
    }
    const Type usize = Type::makeInt(0, false);
    EP lo = e.args.empty() ? nullptr : expr(*e.args[0], &usize);
    lo = lo ? coerce(std::move(lo), usize, e.args[0]->span) : nullptr;
    EP hi = e.rhs ? expr(*e.rhs, &usize) : nullptr;
    hi = hi ? coerce(std::move(hi), usize, e.rhs->span) : nullptr;
    if ((!e.args.empty() && !lo) || (e.rhs != nullptr && !hi)) {
        return nullptr;
    }
    if (lower::isUnknown(base->type)) {
        EP s = mk(hir::ExprKind::Slice, lower::unknownType(), e.span);
        s->lhs = std::move(base);
        s->rhs = std::move(hi);
        if (lo) {
            s->args.push_back(std::move(lo));
        }
        return s;
    }
    // Constant bounds on a constant-length array are a compile-time range check, like indexing.
    if (base->type.kind == TypeKind::Array) {
        if (const auto len = arrayLength(s_, base->type); len && e.rhs != nullptr &&
                                                          e.rhs->kind == Expr::Kind::IntLiteral &&
                                                          e.rhs->int_value > *len) {
            err("E0701",
                e.rhs->span,
                std::format("slice end {} is past the length {} of `{}`",
                            e.rhs->int_value,
                            *len,
                            typeToString(base->type)),
                "the end bound counts from the start");
            return nullptr;
        }
    }
    (void)yields_str;
    EP s = mk(hir::ExprKind::Slice, result, e.span);
    s->checked = s_.config_.safety == "checked";
    s->lvalue = false;
    s->rhs = std::move(hi);
    s->lhs = std::move(base);
    if (lo) {
        s->args.push_back(std::move(lo));
    }
    return s;
}

Lowerer::EP Lowerer::arrayExpr(const Expr& e, const Type* expected) {
    const Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
    const Type* elem_hint = want.kind == TypeKind::Array && want.elem ? want.elem.get() : nullptr;
    if (e.kind == Expr::Kind::ArrayRepeat) {
        EP v = expr(*e.lhs, elem_hint);
        const auto n = s_.evalConst(*e.rhs);
        if (!v || !n) {
            return nullptr;
        }
        if (n->kind != ConstValue::Kind::Int || n->int_value < 0) {
            err("E0707",
                e.rhs->span,
                "array length must be a non-negative integer constant",
                "invalid length");
            return nullptr;
        }
        auto len = std::make_shared<Expr>();
        len->kind = Expr::Kind::IntLiteral;
        len->int_value = static_cast<std::uint64_t>(n->int_value);
        len->span = e.rhs->span;
        if (elem_hint != nullptr) {
            v = coerce(std::move(v), *elem_hint, e.lhs->span);
            if (!v) {
                return nullptr;
            }
        }
        EP out = mk(hir::ExprKind::ArrayRepeat, Type::makeArray(v->type, len), e.span);
        out->int_value = len->int_value;
        out->lhs = std::move(v);
        return out;
    }
    std::vector<EP> items;
    Type elem;
    bool have = false;
    bool ok = true;
    for (const ExprPtr& a : e.args) {
        EP v = expr(*a, have ? &elem : elem_hint);
        if (v && elem_hint != nullptr) {
            v = coerce(std::move(v), *elem_hint, a->span);
        } else if (v && have) {
            v = coerce(std::move(v), elem, a->span);
        }
        if (!v) {
            ok = false;
            continue;
        }
        if (!have) {
            elem = elem_hint != nullptr ? *elem_hint : v->type;
            have = true;
        }
        items.push_back(std::move(v));
    }
    if (!ok) {
        return nullptr;
    }
    if (!have) {
        if (elem_hint == nullptr) {
            err("E0708",
                e.span,
                "cannot infer the element type of an empty array",
                "write a type: `local a: [T; 0]`");
            return nullptr;
        }
        elem = *elem_hint;
    }
    auto len = std::make_shared<Expr>();
    len->kind = Expr::Kind::IntLiteral;
    len->int_value = items.size();
    len->span = e.span;
    EP out = mk(hir::ExprKind::ArrayList, Type::makeArray(elem, len), e.span);
    out->args = std::move(items);
    return out;
}

Lowerer::EP Lowerer::tupleExpr(const Expr& e, const Type* expected) {
    // `(a, b)`: every element takes its type from the expected tuple's matching element (so an
    // untyped literal like `0` becomes `f64` when the destination says so) or stands alone.
    const Type want = expected != nullptr ? resolved(*expected) : lower::unknownType();
    const bool hinted = want.kind == TypeKind::Tuple && want.args.size() == e.args.size();
    std::vector<EP> items;
    bool ok = true;
    for (std::size_t i = 0; i < e.args.size(); ++i) {
        EP v = expr(*e.args[i], hinted ? &want.args[i] : nullptr);
        if (v && hinted) {
            v = coerce(std::move(v), want.args[i], e.args[i]->span);
        }
        if (!v) {
            ok = false;
            continue;
        }
        items.push_back(std::move(v));
    }
    if (!ok) {
        return nullptr;
    }
    std::vector<Type> elements;
    for (const EP& v : items) {
        elements.push_back(v->type);
    }
    EP out = mk(hir::ExprKind::TupleLit, Type::makeTuple(std::move(elements)), e.span);
    out->args = std::move(items);
    return out;
}

void Lowerer::lowerFields(const Expr& e, const Type& agg, std::vector<hir::FieldInit>& out) {
    std::vector<FieldInfo> fields;
    collectFields(agg.path.front(), fields);
    std::set<std::string> seen;
    for (const FieldInit& init : e.fields) {
        std::size_t index = fields.size();
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (fields[i].decl->name == init.name) {
                index = i;
            }
        }
        if (index == fields.size()) {
            err("E0709",
                init.span,
                std::format("`{}` has no field `{}`", typeToString(agg), init.name),
                "unknown field");
            if (init.value) {
                (void)expr(*init.value, nullptr);
            }
            continue;
        }
        if (!seen.insert(init.name).second) {
            err("E0709",
                init.span,
                std::format("field `{}` is initialized twice", init.name),
                "duplicate field");
            continue;
        }
        const Type ft = resolvedIn(*fields[index].owner, fields[index].decl->type);
        EP v = init.value ? moveOut(coerce(expr(*init.value, &ft), ft, init.value->span)) : nullptr;
        if (!v) {
            continue;
        }
        hir::FieldInit h;
        h.name = init.name;
        h.index = static_cast<std::uint32_t>(index);
        h.value = std::move(v);
        out.push_back(std::move(h));
    }
}

Lowerer::EP Lowerer::structLiteral(const Expr& e) {
    const Type t = resolved(e.type);
    if (lower::isUnknown(t)) {
        return nullptr;
    }
    if (!lower::isAggregate(t)) {
        err("E0709",
            e.span,
            std::format("`{}` is not a struct or class", typeToString(t)),
            "cannot build it with `{ ... }`");
        return nullptr;
    }
    if (t.kind == TypeKind::Class && hasInit(t.path.front())) {
        err("E0723",
            e.span,
            std::format(
                "`{}` has an `init`; build it with `{}(...)`", typeToString(t), typeToString(t)),
            "a class literal would skip the constructor");
        return nullptr;
    }
    EP out = mk(hir::ExprKind::StructLit, t, e.span);
    lowerFields(e, t, out->fields);
    return out;
}

Lowerer::EP Lowerer::newExpr(const Expr& e) {
    const Type t = resolved(e.type);
    if (lower::isUnknown(t)) {
        return nullptr;
    }
    if (!lower::isAggregate(t)) {
        err("E0709",
            e.span,
            std::format("`new` needs a struct or class, found `{}`", typeToString(t)),
            "not allocatable");
        return nullptr;
    }
    EP out = mk(hir::ExprKind::New, Type::makePointer(t), e.span);
    out->target = t;
    if (t.kind == TypeKind::Class && (e.has_call_args || hasInit(t.path.front()))) {
        if (!e.has_call_args) {
            err("E0723",
                e.span,
                std::format("`{}` has an `init`; allocate it with `new {}(...)`",
                            typeToString(t),
                            typeToString(t)),
                "a field literal would skip the constructor");
            return nullptr;
        }
        if (!lowerInitCall(e, t, out->text, out->args)) {
            return nullptr;
        }
    } else if (e.has_call_args) {
        err("E0709",
            e.span,
            std::format("`new {}(...)` needs a class with an `init`", typeToString(t)),
            "use `new T { ... }` for structs");
        return nullptr;
    }
    lowerFields(e, t, out->fields);
    if (e.lhs) {
        // Placement: `new(p) T { ... }` initializes the memory `p` points at; no allocation
        // happens.
        EP a = expr(*e.lhs, nullptr);
        if (!a) {
            return nullptr;
        }
        if (!lower::isUnknown(a->type) && a->type.kind != TypeKind::Pointer) {
            err("E0709",
                e.lhs->span,
                std::format("`new(p) T` needs a pointer to the memory, found `{}`",
                            typeToString(a->type)),
                "pass the memory as a pointer, for example `new(allocator.alloc(sizeOf(T), "
                "alignOf(T))) T { }`");
            return nullptr;
        }
        out->lhs = std::move(a);
    } else {
        if (mod_.alloc_function.empty()) {
            Diagnostic d;
            d.code = "E0501";
            d.severity = Severity::Error;
            d.message = "`new` has no registered `alloc` function";
            d.labels.push_back({e.span, "no `register(alloc)` function in this compilation"});
            d.reason = "The compiler binds no allocator by name.";
            d.fix = "write `new(p) T { ... }` with your own memory or register an `alloc` function";
            s_.diag_.report(std::move(d));
            return nullptr;
        }
        out->registered = true;
    }
    return out;
}

} // namespace xlang::lower
