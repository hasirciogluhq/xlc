// Lowering: class vtables: virtual slots, overrides and their signature checks.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

// ---------------------------------------------------------------------------------------------
// Vtables (plan 4.4)

const std::vector<LowerShared::SlotInfo>& Lowerer::vtableOf(const std::string& qname) {
    static const std::vector<SlotInfo> empty;
    if (const auto it = sh_.vtables.find(qname); it != sh_.vtables.end()) {
        return it->second;
    }
    const auto ref = s_.entityByQName(qname);
    const ClassDecl* cd = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
    if (cd == nullptr || !sh_.vtables_in_progress.insert(qname).second) {
        return empty;
    }
    Sema& owner = *ref->owner;
    std::vector<SlotInfo> slots;
    if (cd->has_base) {
        Type base = Type::makeNamed(cd->base);
        base.span = cd->name_span;
        if (const auto r = owner.resolve(base); r && r->kind == TypeKind::Class) {
            slots = vtableOf(r->path.front());
        }
    }
    const auto signature = [&](Sema& sema, const FnDecl& f) {
        std::string s;
        for (const Param& p : f.params) {
            if (!p.is_self) {
                s += typeToString(resolvedIn(sema, p.type)) + ",";
            }
        }
        return s + "->" + typeToString(resolvedIn(sema, f.return_type));
    };
    for (const FnDecl& m : cd->methods) {
        const std::string fname = qname + "." + m.name;
        std::size_t idx = slots.size();
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].slot.method == m.name) {
                idx = i;
            }
        }
        const bool found = idx != slots.size();
        if (m.is_override) {
            if (!found) {
                owner.error("E0712",
                            m.name_span,
                            std::format("`{}` overrides nothing", m.name),
                            "no virtual method with this name in a base class");
                continue;
            }
            if (signature(owner, m) != signature(*slots[idx].owner, *slots[idx].decl)) {
                owner.error("E0712",
                            m.name_span,
                            std::format("override `{}` has a different signature", m.name),
                            "parameters and return type must match the base method");
            }
            slots[idx] = SlotInfo{{m.name, fname}, &m, &owner};
        } else if (m.is_virtual) {
            if (found) {
                owner.error("E0712",
                            m.name_span,
                            std::format("`{}` is already virtual in a base class", m.name),
                            "write `override fn` to replace it");
                continue;
            }
            slots.push_back(SlotInfo{{m.name, fname}, &m, &owner});
        } else if (found) {
            owner.error("E0712",
                        m.name_span,
                        std::format("`{}` hides a virtual method of a base class", m.name),
                        "write `override fn` to replace it");
        }
    }
    // A polymorphic class drops through its vtable, like a C++ virtual destructor: slot 0 is the
    // drop glue of the dynamic class, so `delete p` / `__builtin_drop(p)` through `*Base` runs the
    // drop of the object's own class. A root `@repr(c)` class gets no such slot: its vtable mirrors
    // a C++ layout the program does not control.
    const Attribute* repr = lower::findAttribute(cd->attributes, "repr");
    const bool repr_c = repr != nullptr && std::ranges::any_of(repr->args, [](const AttributeArg& a) {
                            return a.text == "c";
                        });
    const SlotInfo drop{{std::string(kDropSlot), dropGlueName(qname)}, nullptr, &owner};
    if (!slots.empty() && slots.front().slot.method == kDropSlot) {
        slots.front() = drop;
    } else if (!slots.empty() && !repr_c) {
        slots.insert(slots.begin(), drop);
    }
    sh_.vtables_in_progress.erase(qname);
    return sh_.vtables[qname] = std::move(slots);
}

} // namespace xlang::lower
