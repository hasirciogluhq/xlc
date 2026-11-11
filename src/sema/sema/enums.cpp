#include "sema/sema/sema.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <numeric>
#include <random>

namespace xlang {

// Enums (plan 2.6): member values with auto-numbering, and the checks on the underlying
// type and on duplicate or mistyped members (E0705).


// ---------------------------------------------------------------------------------------------
// Enums

const Item& Sema::genericEnumOf(const std::string& name) const {
    const std::size_t cut = name.find('$');
    if (cut != std::string::npos) {
        if (const Symbol* whole = find(name.substr(0, cut));
            whole != nullptr && whole->item != nullptr &&
            std::holds_alternative<StructDecl>(whole->item->decl)) {
            return *whole->item; // the enum itself; its parameters are what the part needs
        }
    }
    // Not a part: the declaration whose own parameters decide.
    if (const Symbol* self = find(name); self != nullptr && self->item != nullptr) {
        return *self->item;
    }
    return memory_order_item_;
}

std::optional<ConstValue>
Sema::enumMember(const std::string& enum_name, const std::string& member, const Span& at) {
    const Symbol* sym = find(enum_name);
    if (sym == nullptr || sym->kind != Symbol::Kind::Enum) {
        error("E0706", at, std::format("unknown enum `{}`", enum_name), "not an enum");
        return std::nullopt;
    }
    const auto& decl = std::get<EnumDecl>(sym->item->decl);
    auto& values = enum_values_[enum_name];
    if (values.empty() && !decl.members.empty()) {
        if (!enum_in_progress_.insert(enum_name).second) {
            return std::nullopt;
        }
        const auto under = resolve(decl.underlying);
        const bool integral = under && under->kind == TypeKind::Int;
        Int128 next = 0;
        for (const EnumMember& m : decl.members) {
            std::optional<ConstValue> v;
            if (m.value) {
                v = evalConst(*m.value);
                if (v && v->kind == ConstValue::Kind::Int) {
                    next = v->int_value + 1;
                }
            } else if (integral) {
                v = ConstValue::makeInt(next);
                ++next;
            }
            values.push_back(std::move(v));
        }
        enum_in_progress_.erase(enum_name);
    }
    for (std::size_t i = 0; i < decl.members.size() && i < values.size(); ++i) {
        if (decl.members[i].name == member) {
            return values[i];
        }
    }
    error("E0706",
          at,
          std::format("enum `{}` has no member `{}`", enum_name, member),
          "unknown member");
    return std::nullopt;
}

void Sema::checkEnum(const EnumDecl& decl) {
    const auto under = resolve(decl.underlying);
    if (!under) {
        return;
    }
    const bool integral = under->kind == TypeKind::Int;
    const bool constant_capable =
        integral || under->kind == TypeKind::Float || under->kind == TypeKind::Bool ||
        (under->kind == TypeKind::Pointer && under->elem && under->elem->kind == TypeKind::Int &&
         under->elem->bits == 8 && !under->elem->is_signed);
    if (!constant_capable) {
        error("E0705",
              decl.underlying.span.valid() ? decl.underlying.span : decl.name_span,
              std::format("`{}` cannot be the underlying type of an enum", typeToString(*under)),
              "must hold a constant: integer, float, bool or *ui8");
        return;
    }
    std::set<std::string> names;
    std::vector<std::pair<Int128, std::string>> seen;
    for (const EnumMember& m : decl.members) {
        if (!names.insert(m.name).second) {
            error("E0705",
                  m.span,
                  std::format("enum member `{}` is defined twice", m.name),
                  "duplicate member");
        }
        if (!m.value && !integral) {
            error("E0705",
                  m.span,
                  std::format("member `{}` needs an explicit value", m.name),
                  "only integer enums are auto-numbered");
            continue;
        }
        const auto value = enumMember(decl.name, m.name, m.span);
        if (!value) {
            continue;
        }
        if (m.value) {
            const bool ok =
                (integral && value->kind == ConstValue::Kind::Int) ||
                (under->kind == TypeKind::Float && value->kind != ConstValue::Kind::String &&
                 value->kind != ConstValue::Kind::Bool) ||
                (under->kind == TypeKind::Bool && value->kind == ConstValue::Kind::Bool) ||
                (under->kind == TypeKind::Pointer && value->kind == ConstValue::Kind::String);
            if (!ok) {
                error("E0705",
                      m.value->span,
                      std::format("value of `{}` does not match the underlying type `{}`",
                                  m.name,
                                  typeToString(*under)),
                      "wrong kind of constant");
            }
        }
    }
}


}  // namespace xlang
