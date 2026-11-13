// Lowering: plain `enum` declarations.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

void Lowerer::lowerEnum(const EnumDecl& decl) {
    hir::Enum out;
    out.name = s_.qualify(decl.name);
    out.underlying = resolved(decl.underlying);
    // The member values of an instance are computed by the module that owns the generic enum.
    Sema* owner_ref =
        s_.compilation() != nullptr ? s_.compilation()->semaOf(s_.genericOwnerKey(decl.name)) : &s_;
    Sema& owner = owner_ref != nullptr ? *owner_ref : s_;
    for (const EnumMember& m : decl.members) {
        hir::EnumMemberValue v;
        v.name = m.name;
        if (const auto c = owner.enumMember(decl.name, m.name, m.span)) {
            if (c->kind == ConstValue::Kind::Int) {
                v.int_value = static_cast<std::int64_t>(c->int_value);
            } else {
                v.text = c->toString();
            }
        }
        out.members.push_back(std::move(v));
    }
    mod_.enums.push_back(std::move(out));
}

} // namespace xlang::lower
