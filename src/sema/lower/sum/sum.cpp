// Lowering: payload enums: variant lookup, construction and payload reads.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

// ---------------------------------------------------------------------------------------------
// Payload enums. The parser turns `enum E { A(i64), B }` into the struct `E { tag: E$Tag, data:
// E$Data }`, the tag enum `E$Tag`, the union `E$Data { A: E$A }` and one struct `E$A` per variant
// with data. Here the variants are built (`E.A(1)`, `E.B`) and matched (`switch`). A generic enum
// is the same shape inside its instance: `E<i32>` is the instance of `E`, and its parts are the
// instances `E<i32>$Tag`, `E<i32>$Data` and `E<i32>$A` (made by `Sema::instantiate`).

const StructDecl* Lowerer::sumDecl(const Item* item) {
    if (item == nullptr) {
        return nullptr;
    }
    const auto* decl = std::get_if<StructDecl>(&item->decl);
    return decl != nullptr && !decl->variants.empty() ? decl : nullptr;
}

bool Lowerer::sameVariants(const StructDecl& named, const StructDecl& scrutinee) {
    if (&named == &scrutinee) {
        return true;
    }
    // A case names the declaration (`Result.Ok`), the scrutinized value is one instance of it
    // (`Result<i32, E>`): same enum name before the arguments, same variants.
    const std::string base = scrutinee.name.substr(0, scrutinee.name.find('<'));
    return named.name == base && std::ranges::equal(named.variants,
                                                    scrutinee.variants,
                                                    std::equal_to<>(),
                                                    &SumVariant::name,
                                                    &SumVariant::name);
}

std::optional<Lowerer::Sum> Lowerer::sumOfEntry(const ScopeEntry& head) {
    if (head.kind != ScopeEntry::Kind::Type || head.item == nullptr) {
        return std::nullopt;
    }
    const StructDecl* decl = sumDecl(head.item);
    if (decl == nullptr) {
        return std::nullopt;
    }
    return Sum{head.owner != nullptr ? head.owner : &s_, decl};
}

std::optional<Lowerer::Sum> Lowerer::sumOfType(const Type& t) {
    // A generic enum: `Result<i32, E>` is a `Struct` whose name is the instance of `Result`; its
    // variant list came with the instance (see `instantiateItem`).
    if (t.kind != TypeKind::Struct && t.kind != TypeKind::Named) {
        return std::nullopt;
    }
    const auto ref = entityOfType(t);
    if (!ref) {
        return std::nullopt;
    }
    const StructDecl* decl = sumDecl(ref->sym->item);
    if (decl == nullptr) {
        return std::nullopt;
    }
    return Sum{ref->owner, decl};
}

Type Lowerer::sumPart(const Sum& sum, std::string_view suffix, const Span& span) {
    // An instance is named `Result<i32, E>`; its union and its payload structs keep the arguments
    // after the suffix (`Result$Data<i32, E>`, `Result$Ok<i32, E>`), while the tag enum is the same
    // value enum for every instance of the declaration (`Result$Tag`).
    const std::string base = sum.decl->name;
    const std::size_t cut = base.find('<');
    const bool per_instance = cut != std::string::npos && suffix != "$Tag";
    Type named =
        Type::makeNamed({per_instance ? base.substr(0, cut) + std::string(suffix) + base.substr(cut)
                                      : base.substr(0, cut) + std::string(suffix)});
    named.span = span;
    return resolvedIn(*sum.owner, named);
}

std::optional<std::size_t> Lowerer::sumVariantIndex(const Sum& sum, std::string_view name) {
    const auto it = std::ranges::find(sum.decl->variants, name, &SumVariant::name);
    if (it == sum.decl->variants.end()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(it - sum.decl->variants.begin());
}

/// Index of the variant's field inside `E$Data`: only variants with data have one.
std::size_t Lowerer::sumDataIndex(const Sum& sum, std::size_t variant) {
    return static_cast<std::size_t>(
        std::ranges::count_if(sum.decl->variants | std::views::take(variant),
                              [](const SumVariant& v) { return !v.payload.empty(); }));
}

Lowerer::EP Lowerer::sumConstruct(const Sum& sum,
                                  std::size_t variant,
                                  std::vector<EP> values,
                                  const Span& span) {
    const SumVariant& v = sum.decl->variants[variant];
    // The literal is the enum itself (`Result<i32, E>`, a struct), built from its two parts.
    Type whole = Type::makeNamed({sum.owner->qualify(sum.decl->name)});
    whole.kind = TypeKind::Struct;
    whole.span = span;
    EP lit = mk(hir::ExprKind::StructLit, whole, span);
    lit->fields.push_back({"tag", 0, intConst(variant, sumPart(sum, "$Tag", span), span)});
    if (v.payload.empty()) {
        return lit;
    }
    EP payload = mk(hir::ExprKind::StructLit, sumPart(sum, "$" + v.name, span), span);
    for (const std::size_t i : std::views::iota(std::size_t{0}, values.size())) {
        payload->fields.push_back(
            {std::format("_{}", i), static_cast<std::uint32_t>(i), std::move(values[i])});
    }
    EP data = mk(hir::ExprKind::StructLit, sumPart(sum, "$Data", span), span);
    data->fields.push_back(
        {v.name, static_cast<std::uint32_t>(sumDataIndex(sum, variant)), std::move(payload)});
    lit->fields.push_back({"data", 1, std::move(data)});
    return lit;
}

/// `E.B` used as a value: only a variant without data can stand alone.
Lowerer::EP Lowerer::sumVariantValue(const Sum& sum, const Expr& e) {
    const auto variant = sumVariantIndex(sum, e.text);
    if (!variant) {
        err("E0706",
            e.span,
            std::format("enum `{}` has no member `{}`", sum.decl->name, e.text),
            "unknown member");
        return nullptr;
    }
    const SumVariant& v = sum.decl->variants[*variant];
    if (!v.payload.empty()) {
        err("E0751",
            e.span,
            std::format(
                "variant `{}.{}` carries {} value(s)", sum.decl->name, v.name, v.payload.size()),
            std::format("write `{}.{}(...)` with its values", sum.decl->name, v.name));
        return nullptr;
    }
    return sumConstruct(sum, *variant, {}, e.span);
}

/// `E.A(x, y)`: every argument converts to the payload type at its position.
Lowerer::EP Lowerer::sumVariantCall(const Sum& sum, const Expr& call) {
    const Expr& callee = *call.lhs;
    const auto variant = sumVariantIndex(sum, callee.text);
    if (!variant) {
        err("E0706",
            callee.span,
            std::format("enum `{}` has no member `{}`", sum.decl->name, callee.text),
            "unknown member");
        return nullptr;
    }
    const SumVariant& v = sum.decl->variants[*variant];
    if (call.args.size() != v.payload.size()) {
        err("E0751",
            call.span,
            std::format("variant `{}.{}` takes {} value(s), {} given",
                        sum.decl->name,
                        v.name,
                        v.payload.size(),
                        call.args.size()),
            "the number of values must match the variant's payload");
        return nullptr;
    }
    std::vector<EP> values;
    bool ok = true;
    for (const auto& [arg, type] : std::views::zip(call.args, v.payload)) {
        const Type want = resolvedIn(*sum.owner, type);
        EP value = expr(*arg, &want);
        value = value ? coerce(std::move(value), want, arg->span) : nullptr;
        // The payload owns the value: a droppable argument moves into it (a copy would drop it
        // twice: once through the enum, once through the local).
        value = value ? moveOut(std::move(value)) : nullptr;
        ok = ok && value != nullptr;
        values.push_back(std::move(value));
    }
    return ok ? sumConstruct(sum, *variant, std::move(values), call.span) : nullptr;
}

/// The place `scrutinee.data.<Variant>._<index>`.
Lowerer::EP Lowerer::sumPayloadRead(const Sum& sum,
                                    hir::LocalId scrutinee,
                                    const Type& self,
                                    std::size_t variant,
                                    std::size_t index,
                                    const Span& span) {
    const SumVariant& v = sum.decl->variants[variant];
    const auto field = [&](EP base, std::string name, std::uint32_t at, Type type) {
        EP f = mk(hir::ExprKind::Field, std::move(type), span);
        f->text = std::move(name);
        f->index = at;
        f->lvalue = true;
        f->lhs = std::move(base);
        return f;
    };
    EP root = mk(hir::ExprKind::LocalRef, self, span);
    root->local = scrutinee;
    root->lvalue = true;
    EP data = field(std::move(root), "data", 1, sumPart(sum, "$Data", span));
    EP payload = field(std::move(data),
                       v.name,
                       static_cast<std::uint32_t>(sumDataIndex(sum, variant)),
                       sumPart(sum, "$" + v.name, span));
    return field(std::move(payload),
                 std::format("_{}", index),
                 static_cast<std::uint32_t>(index),
                 resolvedIn(*sum.owner, v.payload[index]));
}

} // namespace xlang::lower
