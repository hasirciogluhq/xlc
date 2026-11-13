// Lowering: field access `a.b` on structs, classes, tuples, enums and modules.

#include "sema/lower/lower.hpp"

namespace xlang::lower {

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

} // namespace xlang::lower
