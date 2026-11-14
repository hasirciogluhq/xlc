// Lowering: expression dispatch, constants, string literals, names and generic function values.

#include "sema/lower/lower.hpp"

namespace xlang::lower {

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
/// @invariant an unsuffixed integer literal is `i32` unless the context asks for another type, and it widens to `i64` only when the value does not fit in 32 bits.
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
/// @invariant an unsuffixed float literal is `f64` unless the context asks for `f32`, so `local x: f32 = 1.5` narrows at the literal, not at a cast.
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
/// @grammar a string literal is a `str`, or byte by byte into a `[u8; N]` when the context names one; a literal longer than N is E0717.
    case Expr::Kind::StringLiteral:
        return stringLiteral(e.text, expected, e.span);
/// @invariant `b'x'` is always `u8`, never widened by the context, because the byte is the whole value.
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

} // namespace xlang::lower
