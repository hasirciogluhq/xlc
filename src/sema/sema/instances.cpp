#include "sema/sema/sema.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <numeric>
#include <random>
#include <ranges>

namespace xlang {

// Generic instances (plan 6): `instantiate`, method instances and the queues the lowering
// pass drains. Each instance is a cloned `Item` interned in `instances_` and named
// `f<i32, str>`. A generic payload enum is several items (`E`, `E$Tag`, `E$Data`, `E$V`), so
// instantiating `E` also interns the instance of each part, named `E<i32>$V` after it.

using sema_detail::typeNesting;

std::optional<Sema::EntityRef>
Sema::instantiate(const EntityRef& generic, const std::vector<Type>& args, const Span& at) {
    const Item& gitem = *generic.sym->item;
    const std::vector<std::string> params = genericParamsOf(gitem);
    Sema& owner = *generic.owner;
    if (params.size() != args.size()) {
        error("E0724",
              at,
              std::format("`{}` takes {} type argument(s), {} given",
                          generic.local,
                          params.size(),
                          args.size()),
              "write one type per type parameter");
        return std::nullopt;
    }
    int nesting = 0;
    for (const Type& a : args) {
        nesting = std::max(nesting, typeNesting(a));
    }
    if (nesting > 64) {
        error("E0725",
              at,
              "generic instantiation too deep",
              "the type arguments nest more than 64 levels");
        return std::nullopt;
    }
    // Instance names use `.` for qualified type names: `::` separates the module (see qualify).
    std::string shown;
    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string spelled = typeToString(args[i]);
        for (std::size_t p = spelled.find("::"); p != std::string::npos;
             p = spelled.find("::", p)) {
            spelled.replace(p, 2, ".");
        }
        shown += (i != 0 ? ", " : "") + spelled;
    }
    const std::string local = generic.local + "<" + shown + ">";
    if (owner.symbols_.find(local) != owner.symbols_.end()) {
        return EntityRef{&owner, &owner.symbols_.at(local), local};
    }
    TypeEnv env;
    for (std::size_t i = 0; i < params.size(); ++i) {
        env[params[i]] = args[i];
    }
    // The parts of an enum instance keep the declaration's naming scheme with the arguments after
    // the `$` suffix: `Result$Ok<T, E>` becomes `Result$Ok<i32, MyErr>`, which is exactly what
    // `Lowerer::sumPart` builds from the instance name `Result<i32, MyErr>`.
    const auto partName = [&](std::string_view suffix) {
        return generic.local + std::string(suffix) + "<" + shown + ">";
    };
    const auto make = [&](const Item& from, const std::string& instance_name) {
        owner.instances_.push_back(instantiateItem(from, env, instance_name));
        return &owner.instances_.back();
    };
    const Item* item = make(gitem, local);
    const Symbol* sym = owner.internInstance(*item, local, generic.qname(), args, at);
    if (const auto* sum = std::get_if<StructDecl>(&item->decl);
        sum != nullptr && !sum->variants.empty()) {
        // A payload enum is one type made of several items: `E`, `E$Tag`, `E$Data` and one
        // `E$Variant` per variant with data. An instance of `E` is built out of the instances of
        // the others (its fields are typed with them), so they are made here, in the module that
        // owns `E`, with the same arguments: the instance of `E` is never half-made.
        if (const std::string data = partName("$Data");
            owner.symbols_.find(data) == owner.symbols_.end()) {
            // The union is rebuilt from the declaration with the instance name: it has no item of
            // the enum to be cloned from without its payload structs.
            Item union_of;
            union_of.modifiers = gitem.modifiers;
            StructDecl union_decl;
            static_cast<DeclBase&>(union_decl) = declBase(gitem);
            union_decl.name = data;
            union_decl.is_payload = false;
            union_decl.is_union = true;
            for (const SumVariant& v : sum->variants) {
                if (!v.payload.empty()) {
                    union_decl.fields.push_back({v.name,
                                                 Type::makeNamed({partName("$" + v.name)}),
                                                 {},
                                                 declBase(gitem).span});
                }
            }
            union_of.decl = std::move(union_decl);
            (void)owner.internInstance(*make(union_of, data), data, generic.qname(), args, at);
        }
        for (const SumVariant& v : sum->variants) {
            if (v.payload.empty()) {
                continue;
            }
            const std::string part = partName("$" + v.name);
            if (owner.symbols_.find(part) != owner.symbols_.end()) {
                continue;
            }
            // The payload struct is cloned from the declaration item itself: its fields are the
            // enum's payload types, which the substitution makes concrete.
            Item payload_of;
            payload_of.modifiers = gitem.modifiers;
            StructDecl payload_decl;
            static_cast<DeclBase&>(payload_decl) = declBase(gitem);
            payload_decl.name = part;
            payload_decl.is_union = false;
            payload_decl.is_payload = true;
            for (const std::size_t i : std::views::iota(std::size_t{0}, v.payload.size())) {
                payload_decl.fields.push_back(
                    {std::format("_{}", i), v.payload[i], {}, declBase(gitem).span});
            }
            payload_of.decl = std::move(payload_decl);
            (void)owner.internInstance(*make(payload_of, part), part, generic.qname(), args, at);
        }
    }
    return EntityRef{&owner, sym, local};
}

const Sema::Symbol* Sema::internInstance(const Item& item,
                                         const std::string& local,
                                         const std::string& origin,
                                         const std::vector<Type>& args,
                                         const Span& at) {
    Symbol sym;
    sym.item = &item;
    if (std::holds_alternative<ClassDecl>(item.decl)) {
        sym.kind = Symbol::Kind::Class;
        aggregate_order_.push_back(local);
    } else if (std::holds_alternative<StructDecl>(item.decl)) {
        sym.kind = Symbol::Kind::Struct;
        aggregate_order_.push_back(local);
    } else if (std::holds_alternative<InterfaceDecl>(item.decl)) {
        sym.kind = Symbol::Kind::Interface;
    } else if (std::holds_alternative<EnumDecl>(item.decl)) {
        // `enum E<T>`: the tag and the union of a generic payload enum are enums too, and a
        // generic value enum keeps being an enum. None of them owns a layout.
        sym.kind = Symbol::Kind::Enum;
    } else {
        sym.kind = Symbol::Kind::Fn;
        sym.overloads = {&item};
    }
    const auto [it, inserted] = symbols_.emplace(local, std::move(sym));
    if (inserted) {
        instance_info_[local] = InstanceInfo{origin, args, at, current_instance_, this};
        item_instance_[&item] = local;
        pending_instances_.push_back(&item);
    }
    return &it->second;
}

// The module key of the generic declaration an instance comes from: the instance's origin is
// `key::name` (`name` alone for the root module), so everything before the last `::`.
std::string Sema::genericOwnerKey(const std::string& local) const {
    const InstanceInfo* info = instanceInfo(local);
    if (info == nullptr) {
        return key_;
    }
    const std::size_t cut = info->generic.rfind("::");
    return cut == std::string::npos ? std::string{} : info->generic.substr(0, cut);
}

std::vector<const Item*> Sema::takePendingInstances() {
    std::vector<const Item*> out;
    out.swap(pending_instances_);
    return out;
}

const Sema::InstanceInfo* Sema::instanceInfo(const std::string& local) const {
    const auto it = instance_info_.find(local);
    return it == instance_info_.end() ? nullptr : &it->second;
}

const Item* Sema::addImplInstance(Item item) {
    instances_.push_back(std::move(item));
    const Item* out = &instances_.back();
    pending_instances_.push_back(out);
    return out;
}

const Sema::MethodInstance* Sema::instantiateMethod(const FnDecl& generic,
                                                    const std::string& cls,
                                                    const std::string& cls_q,
                                                    const std::vector<Type>& args) {
    std::string key = cls_q + "." + generic.name + "<";
    for (std::size_t i = 0; i < args.size(); ++i) {
        key += (i == 0 ? "" : ", ") + typeToString(args[i]);
    }
    key += ">";
    if (const auto it = method_index_.find(key); it != method_index_.end()) {
        return it->second;
    }
    TypeEnv env;
    for (std::size_t i = 0; i < generic.generic_params.size() && i < args.size(); ++i) {
        env[generic.generic_params[i]] = args[i];
    }
    method_instances_.push_back(MethodInstance{instantiateFn(generic, env), key, cls, cls_q});
    const MethodInstance* out = &method_instances_.back();
    method_index_.emplace(key, out);
    pending_methods_.push_back(out);
    return out;
}

std::vector<const Sema::MethodInstance*> Sema::takePendingMethods() {
    std::vector<const MethodInstance*> out;
    out.swap(pending_methods_);
    return out;
}

const Sema::Symbol* Sema::find(const std::string& name) const {
    const auto it = symbols_.find(name);
    return it == symbols_.end() ? nullptr : &it->second;
}

std::vector<std::string> Sema::aggregateNames() const {
    return aggregate_order_;
}

} // namespace xlang
