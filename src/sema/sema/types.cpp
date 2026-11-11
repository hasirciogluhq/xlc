#include "sema/sema/sema.hpp"

#include "xlang/sema/compilation.h"

#include <algorithm>
#include <format>
#include <functional>
#include <numeric>
#include <random>

namespace xlang {

using sema_detail::joinDotted;

// Type resolution (plan 2.5): `Named` -> Struct/Class/Interface/Enum, alias expansion
// (E0704 on a cycle), generic arguments (E0724) and the alias-substitution path.

// ---------------------------------------------------------------------------------------------
// Types

std::optional<Type> Sema::resolve(const Type& type) {
    switch (type.kind) {
    case TypeKind::Pointer: {
        auto inner = resolve(*type.elem);
        if (!inner) {
            return std::nullopt;
        }
        Type out = type;
        out.elem = std::make_shared<const Type>(std::move(*inner));
        return out;
    }
    case TypeKind::Array: {
        auto inner = resolve(*type.elem);
        if (!inner) {
            return std::nullopt;
        }
        Type out = type;
        out.elem = std::make_shared<const Type>(std::move(*inner));
        return out;
    }
    case TypeKind::Slice: {
        auto inner = resolve(*type.elem);
        if (!inner) {
            return std::nullopt;
        }
        Type out = type;
        out.elem = std::make_shared<const Type>(std::move(*inner));
        return out;
    }
    case TypeKind::Tuple: {
        Type out = type;
        out.args.clear();
        for (const Type& a : type.args) {
            auto r = resolve(a);
            if (!r) {
                return std::nullopt;
            }
            out.args.push_back(std::move(*r));
        }
        return out;
    }
    case TypeKind::FnPtr: {
        Type out = type;
        out.params.clear();
        for (const Type& p : type.params) {
            auto r = resolve(p);
            if (!r) {
                return std::nullopt;
            }
            out.params.push_back(std::move(*r));
        }
        auto ret = resolve(*type.ret);
        if (!ret) {
            return std::nullopt;
        }
        out.ret = std::make_shared<const Type>(std::move(*ret));
        return out;
    }
    case TypeKind::Named:
        break;
    default:
        return type;
    }

    std::optional<EntityRef> ref;
    if (type.path.size() == 1) {
        // A name already qualified by `qualify` (a substituted generic argument) names its module.
        ref = type.path.front().find("::") != std::string::npos ? entityByQName(type.path.front())
                                                                : lookupEntity(type.path.front());
    } else if (type.path.size() == 2) {
        ref = lookupQualified(type.path);
        if (!ref && comp_ == nullptr) {
            return type; // standalone: `alias.Name` stays unresolved
        }
    } else {
        return type;
    }
    const std::string shown = joinDotted(type.path);
    if (!ref) {
        error("E0706",
              type.span,
              std::format("unknown type `{}`", shown),
              "not declared in this module");
        return std::nullopt;
    }
    if (!type.args.empty() && ref->sym->kind == Symbol::Kind::Alias && ref->sym->item != nullptr &&
        !genericParamsOf(*ref->sym->item).empty()) {
        // `Alias<A, B>`: the target with its parameters replaced, resolved in the alias's module.
        const std::vector<std::string> params = genericParamsOf(*ref->sym->item);
        if (params.size() != type.args.size()) {
            error("E0724",
                  type.span,
                  std::format("`{}` takes {} type argument(s), {} given",
                              shown,
                              params.size(),
                              type.args.size()),
                  "write one type per type parameter");
            return std::nullopt;
        }
        TypeEnv env;
        for (std::size_t i = 0; i < params.size(); ++i) {
            auto r = resolve(type.args[i]);
            if (!r) {
                return std::nullopt;
            }
            env[params[i]] = std::move(*r);
        }
        Sema& owner = *ref->owner;
        if (!owner.alias_in_progress_.insert(ref->local).second) {
            error("E0704",
                  type.span,
                  std::format("type alias `{}` refers to itself", shown),
                  "cyclic alias");
            return std::nullopt;
        }
        auto target = owner.resolve(
            substituteType(std::get<TypeAliasDecl>(ref->sym->item->decl).target, env));
        owner.alias_in_progress_.erase(ref->local);
        if (!target) {
            return std::nullopt;
        }
        target->span = type.span;
        return target;
    }
    if (!type.args.empty()) {
        // `Name<A, B>`: an instance of a generic struct or class.
        if (ref->sym->item == nullptr || genericParamsOf(*ref->sym->item).empty()) {
            error("E0724",
                  type.span,
                  std::format("`{}` takes no type arguments", shown),
                  "remove the `<...>`");
            return std::nullopt;
        }
        std::vector<Type> args;
        for (const Type& a : type.args) {
            auto r = resolve(a);
            if (!r) {
                return std::nullopt;
            }
            args.push_back(std::move(*r));
        }
        const auto inst = ref->owner->instantiate(*ref, args, type.span);
        if (!inst) {
            return std::nullopt;
        }
        Type out = type;
        out.path = {inst->qname()};
        out.args = std::move(args);
        switch (inst->sym->kind) {
        case Symbol::Kind::Class:
            out.kind = TypeKind::Class;
            break;
        case Symbol::Kind::Interface:
            out.kind = TypeKind::Interface;
            break;
        case Symbol::Kind::Enum: {
            // A generic value enum keeps its underlying type, computed once for the instance.
            const auto& decl = std::get<EnumDecl>(inst->sym->item->decl);
            out.kind = TypeKind::Enum;
            auto under = inst->owner->resolve(decl.underlying);
            if (!under) {
                return std::nullopt;
            }
            out.elem = std::make_shared<const Type>(std::move(*under));
            break;
        }
        default:
            out.kind = TypeKind::Struct;
            break;
        }
        return out;
    }
    if (ref->sym->item != nullptr && !genericParamsOf(*ref->sym->item).empty()) {
        error("E0724",
              type.span,
              std::format("`{}` is generic and takes {} type argument(s), none given",
                          shown,
                          genericParamsOf(*ref->sym->item).size()),
              std::format("write the arguments: `{}<...>`", shown));
        return std::nullopt;
    }
    Sema& owner = *ref->owner;
    Type out = type;
    out.path = {ref->qname()};
    switch (ref->sym->kind) {
    case Symbol::Kind::Struct:
        out.kind = TypeKind::Struct;
        return out;
    case Symbol::Kind::Class:
        out.kind = TypeKind::Class;
        return out;
    case Symbol::Kind::Interface:
        out.kind = TypeKind::Interface;
        return out;
    case Symbol::Kind::Enum: {
        const auto& decl = std::get<EnumDecl>(ref->sym->item->decl);
        out.kind = TypeKind::Enum;
        auto under = owner.resolve(decl.underlying);
        if (!under) {
            return std::nullopt;
        }
        out.elem = std::make_shared<const Type>(std::move(*under));
        return out;
    }
    case Symbol::Kind::Alias: {
        if (const auto params = genericParamsOf(*ref->sym->item); !params.empty()) {
            error("E0724",
                  type.span,
                  std::format("`{}` takes {} type argument(s), none given", shown, params.size()),
                  "write `<...>` with one type per type parameter");
            return std::nullopt;
        }
        if (!owner.alias_in_progress_.insert(ref->local).second) {
            error("E0704",
                  type.span,
                  std::format("type alias `{}` refers to itself", shown),
                  "cyclic alias");
            return std::nullopt;
        }
        auto target = owner.resolve(std::get<TypeAliasDecl>(ref->sym->item->decl).target);
        owner.alias_in_progress_.erase(ref->local);
        if (!target) {
            return std::nullopt;
        }
        target->span = type.span;
        return target;
    }
    default:
        error("E0706",
              type.span,
              std::format("`{}` is not a type", shown),
              "this name is not a type");
        return std::nullopt;
    }
}

} // namespace xlang
