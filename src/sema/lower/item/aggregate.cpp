// Lowering: struct and class declarations: fields, methods, `init`/`drop` checks.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

void Lowerer::lowerAggregate(const Item& item) {
    if (const auto* st = std::get_if<StructDecl>(&item.decl)) {
        hir::Aggregate a;
        a.name = s_.qualify(st->name);
        a.opaque = st->opaque;
        a.is_union = st->is_union;
        a.span = st->span;
        const Attribute* repr = lower::findAttribute(st->attributes, "repr");
        a.repr_c = repr != nullptr && std::ranges::any_of(repr->args, [](const AttributeArg& x) {
                       return x.text == "c";
                   });
        // A payload enum owns its payload: its glue drops only the live variant's values.
        if (!st->variants.empty()) {
            emitSumDropGlue(a.name, *st, st->span);
        }
        for (const FieldDecl& f : st->fields) {
            const Type ft = resolved(f.type);
            // A payload struct and the union that holds it are dropped by the enum's own glue,
            // which drops the live variant only, so a droppable payload is allowed there. Such a
            // struct is never a field of anything else and has no name a program can write.
            // A sum (`E` itself) and its parts are dropped by `E`'s glue, which drops the live
            // variant only, so a droppable payload is allowed there.
            const bool in_sum = !st->variants.empty() || st->is_payload || st->is_union;
            if (droppableSum(ft) && !in_sum) {
                err("E0758",
                    f.span,
                    std::format("`{}` is a payload enum that needs drop and cannot be a struct "
                                "field",
                                typeToString(ft)),
                    "use a class field instead; structs are plain data and drop nothing");
            } else if (needsDrop(ft) && !in_sum) {
                err("E0721",
                    f.span,
                    std::format("`{}` needs drop and cannot be a struct field", typeToString(ft)),
                    "use a class field instead; structs are plain data");
            }
            rejectDroppablePlace(ft, f.span, "a struct field");
            a.fields.push_back({f.name, ft});
        }
        mod_.aggregates.push_back(std::move(a));
    } else if (const auto* cl = std::get_if<ClassDecl>(&item.decl)) {
        hir::Aggregate a;
        a.name = s_.qualify(cl->name);
        a.is_class = true;
        for (const FnDecl& m : cl->methods) {
            const bool only_self =
                std::ranges::all_of(m.params, [](const Param& p) { return p.is_self; });
            if (m.name == "init" && m.has_return_type) {
                err("E0722", m.name_span, "`init` takes no return type", "remove the `-> T`");
            }
            if (m.name == "drop" &&
                (!only_self || m.has_return_type || m.is_virtual || m.is_override)) {
                err("E0722",
                    m.name_span,
                    "`drop` takes only `self` and returns nothing, and cannot be virtual",
                    "write `fn drop(self) { ... }`");
            }
        }
        if (cl->has_base && hasInit(s_.qualify(cl->name)) && baseInitKey(*cl, s_).empty()) {
            if (const auto base = s_.resolve(Type::makeNamed(cl->base));
                base && base->kind == TypeKind::Class && hasInit(base->path.front())) {
                err("E0722",
                    cl->name_span,
                    std::format("`{}` has an `init` but its base has none without arguments",
                                cl->name),
                    "give the base class an `init` that takes no arguments");
            }
        }
        emitDropGlue(a.name, *cl, cl->span);
        a.opaque = cl->opaque;
        a.span = cl->span;
        const Attribute* repr = lower::findAttribute(cl->attributes, "repr");
        a.repr_c = repr != nullptr && std::ranges::any_of(repr->args, [](const AttributeArg& x) {
                       return x.text == "c";
                   });
        if (cl->has_base) {
            Type base = Type::makeNamed(cl->base);
            base.span = cl->name_span;
            if (const auto r = s_.resolve(base); r && r->kind == TypeKind::Class) {
                a.base = r->path.front();
            }
        }
        for (const FieldDecl& f : cl->fields) {
            const Type ft = resolved(f.type);
            rejectDroppablePlace(ft, f.span, "a class field");
            a.fields.push_back({f.name, ft});
        }
        for (const FnDecl& m : cl->methods) {
            a.methods.push_back(a.name + "." + m.name);
        }
        for (const SlotInfo& slot : vtableOf(a.name)) {
            a.vtable.push_back(slot.slot);
        }
        mod_.aggregates.push_back(std::move(a));
    }
}

} // namespace xlang::lower
