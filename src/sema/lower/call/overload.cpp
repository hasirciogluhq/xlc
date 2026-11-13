// Lowering: overload keys and signatures, argument pre-lowering, candidate ranking and overload resolution.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

std::string Lowerer::overloadKey(const std::string& qname, const std::vector<Type>& params) {
    std::string key = qname;
    for (const Type& p : params) {
        key += "$" + mangleType(p);
    }
    return key;
}

std::string Lowerer::signatureOf(const std::string& name, const std::vector<Type>& params) {
    std::string out = name + "(";
    for (std::size_t i = 0; i < params.size(); ++i) {
        out += (i != 0 ? ", " : "") + typeToString(params[i]);
    }
    return out + ")";
}

std::vector<Lowerer::OverloadCandidate> Lowerer::methodCandidates(const MethodRef& m,
                                                                  const std::string& name) {
    const std::string qname = m.cls_q + "." + name;
    std::vector<OverloadCandidate> out;
    for (const FnDecl& fm : m.cls->methods) {
        if (fm.name != name) {
            continue;
        }
        OverloadCandidate c;
        c.decl = &fm;
        for (const Param& p : fm.params) {
            if (!p.is_self) {
                c.params.push_back(resolvedIn(*m.owner, p.type));
            }
        }
        c.ret = resolvedIn(*m.owner, fm.return_type);
        out.push_back(std::move(c));
    }
    const bool overloaded = out.size() > 1;
    for (OverloadCandidate& c : out) {
        c.key = overloaded ? overloadKey(qname, c.params) : qname;
    }
    return out;
}

bool Lowerer::unifyParam(const Type& pattern,
                         const Type& actual,
                         const std::vector<std::string>& names,
                         std::map<std::string, Type>& bound,
                         Sema& owner) {
    if (pattern.kind == TypeKind::Named && pattern.path.size() == 1 && pattern.args.empty() &&
        std::ranges::find(names, pattern.path.front()) != names.end()) {
        const auto it = bound.find(pattern.path.front());
        if (it != bound.end()) {
            return lower::sameType(it->second, actual);
        }
        bound.emplace(pattern.path.front(), actual);
        return true;
    }
    if (pattern.kind == TypeKind::Pointer || pattern.kind == TypeKind::Array) {
        return actual.kind == pattern.kind && pattern.elem && actual.elem &&
               unifyParam(*pattern.elem, *actual.elem, names, bound, owner);
    }
    if (pattern.kind == TypeKind::FnPtr) {
        if (actual.kind != TypeKind::FnPtr || pattern.params.size() != actual.params.size() ||
            !pattern.ret || !actual.ret) {
            return false;
        }
        for (std::size_t i = 0; i < pattern.params.size(); ++i) {
            if (!unifyParam(pattern.params[i], actual.params[i], names, bound, owner)) {
                return false;
            }
        }
        return unifyParam(*pattern.ret, *actual.ret, names, bound, owner);
    }
    if (pattern.kind == TypeKind::Named && !pattern.args.empty()) {
        // `Vec<T>` against an instance of a generic class: the instance must come from `Vec`. The
        // generic name is looked up rather than resolved, because a bare generic name is E0724.
        std::optional<Sema::EntityRef> base;
        if (pattern.path.size() == 1) {
            base = pattern.path.front().find("::") != std::string::npos
                       ? owner.entityByQName(pattern.path.front())
                       : owner.lookupEntity(pattern.path.front());
        } else if (pattern.path.size() == 2) {
            base = owner.lookupQualified(pattern.path);
        }
        if (!base || (actual.kind != TypeKind::Class && actual.kind != TypeKind::Struct) ||
            actual.path.empty()) {
            return false;
        }
        const auto ref = s_.entityByQName(actual.path.front());
        const Sema::InstanceInfo* info = ref ? ref->owner->instanceInfo(ref->local) : nullptr;
        if (info == nullptr || info->generic != base->qname() ||
            info->args.size() != pattern.args.size()) {
            return false;
        }
        for (std::size_t i = 0; i < pattern.args.size(); ++i) {
            if (!unifyParam(pattern.args[i], info->args[i], names, bound, owner)) {
                return false;
            }
        }
        return true;
    }
    const auto concrete = owner.resolve(pattern);
    if (!concrete) {
        return false;
    }
    if (lower::sameType(*concrete, actual)) {
        return true;
    }
    // A parameter that names no type parameter takes any argument that converts to it (`&obj` to
    // an interface, an integer widening); the call coerces and checks it after inference.
    hir::Expr probe;
    probe.type = actual;
    probe.lvalue = true;
    return convertible(probe, *concrete);
}

std::vector<Lowerer::PreArg> Lowerer::preLowerArgs(const Expr& call) {
    std::vector<PreArg> out;
    for (const ExprPtr& a : call.args) {
        PreArg p;
        switch (a->kind) {
        case Expr::Kind::IntLiteral:
            p.literal = true;
            p.type =
                a->int_value > 2147483647ULL ? Type::makeInt(64, true) : Type::makeInt(32, true);
            break;
        case Expr::Kind::FloatLiteral:
            p.literal = true;
            p.type = Type::makeFloat(64);
            break;
        case Expr::Kind::StringLiteral:
            p.literal = true;
            p.type = Type::makeStr();
            break;
        case Expr::Kind::Closure:
            // A closure with untyped parameters takes them from the parameter type of the
            // overload that is chosen, so it is lowered with that type, like a literal.
            if (std::ranges::any_of(a->closure->params,
                                    [](const Param& param) { return !param.has_type; })) {
                p.literal = true;
                p.type = lower::unknownType();
                break;
            }
            p.value = expr(*a, nullptr);
            p.type = p.value ? p.value->type : lower::unknownType();
            break;
        case Expr::Kind::Name:
            if (a->text == "nil") {
                p.literal = true;
                p.type = lower::unknownType();
                break;
            }
            [[fallthrough]];
        default:
            p.value = expr(*a, nullptr);
            p.type = p.value ? p.value->type : lower::unknownType();
            break;
        }
        out.push_back(std::move(p));
    }
    return out;
}

std::vector<Lowerer::OverloadCandidate> Lowerer::genericCandidates(const ScopeEntry& entry,
                                                                   const Expr& call,
                                                                   const std::vector<PreArg>& pre) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    std::vector<const Item*> items = entry.overloads;
    if (items.empty()) {
        items.push_back(entry.item);
    }
    const std::vector<Type> no_args;
    std::vector<OverloadCandidate> out;
    for (const Item* item : items) {
        const std::vector<std::string> names = s_.genericParamsOf(*item);
        if (names.empty()) {
            continue;
        }
        const std::string local = declBase(*item).name;
        if (owner.find(local) == nullptr) {
            continue;
        }
        // The symbol of this overload (the name's own symbol shows its first declaration only).
        Sema::Symbol this_overload;
        this_overload.kind = Sema::Symbol::Kind::Fn;
        this_overload.item = item;
        const Sema::EntityRef gref{&owner, &this_overload, local};
        const auto& decl = std::get<FnDecl>(item->decl);
        std::size_t arity = 0;
        for (const Param& p : decl.params) {
            arity += p.is_self ? 0 : 1;
        }
        if (arity != call.args.size()) {
            continue;
        }
        std::vector<Type> targs;
        const std::vector<Type>& written = call.lhs ? call.lhs->generic_args : no_args;
        if (!written.empty()) {
            if (written.size() != names.size()) {
                continue;
            }
            for (const Type& a : written) {
                targs.push_back(resolved(a));
            }
        } else {
            std::map<std::string, Type> bound;
            bool ok = true;
            std::size_t i = 0;
            for (const Param& p : decl.params) {
                if (p.is_self) {
                    continue;
                }
                if (lower::isUnknown(pre[i].type) ||
                    !unifyParam(p.type, pre[i].type, names, bound, owner)) {
                    ok = false;
                    break;
                }
                ++i;
            }
            if (!ok) {
                continue;
            }
            for (const std::string& n : names) {
                const auto it = bound.find(n);
                if (it == bound.end()) {
                    err("E0724",
                        call.span,
                        std::format("cannot infer `{}`", n),
                        std::format("write the type arguments: `{}<...>(...)`", local));
                    return out;
                }
                targs.push_back(it->second);
            }
        }
        const auto inst = owner.instantiate(gref, targs, call.span);
        if (!inst) {
            continue;
        }
        const auto& idecl = std::get<FnDecl>(inst->sym->item->decl);
        OverloadCandidate c;
        c.decl = &idecl;
        for (const Param& p : idecl.params) {
            if (!p.is_self) {
                c.params.push_back(resolvedIn(owner, p.type));
            }
        }
        c.ret = resolvedIn(owner, idecl.return_type);
        c.key = inst->qname();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Lowerer::OverloadCandidate>
Lowerer::packCandidates(const ScopeEntry& entry, const Expr& call, const std::vector<PreArg>& pre) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    std::vector<const Item*> items = entry.overloads;
    if (items.empty()) {
        items.push_back(entry.item);
    }
    std::vector<OverloadCandidate> out;
    for (const Item* item : items) {
        if (!Sema::isPackFn(*item)) {
            continue;
        }
        const auto& decl = std::get<FnDecl>(item->decl);
        std::size_t fixed = 0;
        for (const Param& p : decl.params) {
            fixed += (p.is_self || p.is_pack) ? 0 : 1;
        }
        if (call.args.size() < fixed) {
            continue;
        }
        std::vector<Type> elements;
        bool known = true;
        for (std::size_t i = fixed; i < pre.size(); ++i) {
            known = known && !lower::isUnknown(pre[i].type);
            elements.push_back(pre[i].type);
        }
        if (!known) {
            continue; // an argument of unknown type (`nil`): no instance can be made for it
        }
        const std::string local = declBase(*item).name;
        const auto inst = owner.instantiatePack(*item, local, elements, call.span);
        if (!inst) {
            continue;
        }
        const auto& idecl = std::get<FnDecl>(inst->sym->item->decl);
        OverloadCandidate c;
        c.decl = &idecl;
        for (const Param& p : idecl.params) {
            if (!p.is_self) {
                c.params.push_back(resolvedIn(owner, p.type));
            }
        }
        c.ret = resolvedIn(owner, idecl.return_type);
        c.key = inst->qname();
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<Lowerer::OverloadCandidate> Lowerer::candidatesOf(const ScopeEntry& entry) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    std::vector<const Item*> items = entry.overloads;
    if (items.empty()) {
        items.push_back(entry.item);
    }
    const bool overloaded = items.size() > 1;
    const std::string qname = entry.qname.empty() ? entry.name : entry.qname;
    std::vector<OverloadCandidate> out;
    for (const Item* item : items) {
        if (!s_.genericParamsOf(*item).empty() || Sema::isPackFn(*item)) {
            continue; // generic and pack overloads are instantiated per call (genericCandidates,
                      // packCandidates)
        }
        OverloadCandidate c;
        c.decl = &std::get<FnDecl>(item->decl);
        for (const Param& p : c.decl->params) {
            c.params.push_back(resolvedIn(owner, p.type));
        }
        c.ret = resolvedIn(owner, c.decl->return_type);
        c.key = overloaded ? overloadKey(qname, c.params) : qname;
        out.push_back(std::move(c));
    }
    return out;
}

namespace {

/// Cost of passing `arg`, whose type is `from`, to a parameter of type `param` (lower is better).
/// Untyped literals use their own rules; a typed argument costs 0 when the types match, 1 for
/// integer widening or wrapping in an interface, 2 for an integer to float conversion and 3 for
/// other conversions.
[[nodiscard]] int argumentCost(const Expr& arg, const Type& from, const Type& param) {
    switch (arg.kind) {
    case Expr::Kind::IntLiteral:
        if (param.kind == TypeKind::Int) {
            return param.bits == 64 && param.is_signed ? 1 : 2;
        }
        return 3;
    case Expr::Kind::FloatLiteral:
        if (param.kind == TypeKind::Float) {
            return param.bits == 64 ? 1 : 2;
        }
        return 3;
    case Expr::Kind::StringLiteral:
        if (param.kind == TypeKind::Str) {
            return 0;
        }
        if (param.kind == TypeKind::Pointer) {
            return 1;
        }
        return param.kind == TypeKind::Array ? 2 : 3;
    case Expr::Kind::Name:
        if (arg.text == "nil") {
            return 1;
        }
        break;
    default:
        break;
    }
    if (lower::isUnknown(from) || lower::sameType(from, param)) {
        return 0;
    }
    if (param.kind == TypeKind::Int && from.kind == TypeKind::Int) {
        return 1;
    }
    if (param.kind == TypeKind::Interface) {
        return 1;
    }
    if (param.kind == TypeKind::Float &&
        (from.kind == TypeKind::Int || from.kind == TypeKind::Float)) {
        return 2;
    }
    return 3;
}

/// What an argument looks like in a diagnostic.
[[nodiscard]] std::string argumentText(const Expr& arg, const Type& from) {
    switch (arg.kind) {
    case Expr::Kind::IntLiteral:
        return "integer literal";
    case Expr::Kind::FloatLiteral:
        return "float literal";
    case Expr::Kind::StringLiteral:
        return "string literal";
    default:
        break;
    }
    return lower::isUnknown(from) ? "unknown" : typeToString(from);
}

} // namespace

bool Lowerer::convertible(const hir::Expr& value, const Type& to) {
    if (lower::isUnknown(value.type) || lower::isUnknown(to)) {
        return true;
    }
    if (to.kind == TypeKind::Interface) {
        if (value.type.kind == TypeKind::Interface) {
            return lower::sameType(value.type, to);
        }
        const bool pointer = value.type.kind == TypeKind::Pointer && value.type.elem &&
                             lower::isAggregate(*value.type.elem);
        if (pointer || lower::isAggregate(value.type)) {
            if (!pointer && !value.lvalue) {
                return false; // `coerce` reports a temporary (E0709)
            }
            const Type& object = pointer ? *value.type.elem : value.type;
            const std::string iface_q = to.path.empty() ? std::string{} : to.path.front();
            return findImpl(object.path.front(), iface_q) != nullptr;
        }
    }
    const ConversionResult r = implicitConversion(value.type, to, inUnsafe(), ptrBits());
    return r.status == ConversionStatus::Ok || r.status == ConversionStatus::Warn;
}

bool Lowerer::literalFits(const Expr& arg, const Type& param) {
    if (lower::isUnknown(param)) {
        return true;
    }
    switch (arg.kind) {
    case Expr::Kind::IntLiteral:
        return param.kind == TypeKind::Int || param.kind == TypeKind::Float ||
               param.kind == TypeKind::Enum;
    case Expr::Kind::FloatLiteral:
        return param.kind == TypeKind::Float;
    case Expr::Kind::StringLiteral:
        if (param.kind == TypeKind::Str || param.kind == TypeKind::Pointer) {
            return true;
        }
        if (param.kind == TypeKind::Enum) {
            return param.elem && param.elem->kind == TypeKind::Pointer;
        }
        if (param.kind == TypeKind::Array && param.elem && param.elem->kind == TypeKind::Int &&
            param.elem->bits == 8 && !param.elem->is_signed) {
            const auto n = arrayLength(s_, param);
            return n && arg.text.size() <= *n;
        }
        return false;
    case Expr::Kind::Name:
        return arg.text == "nil" &&
               (param.kind == TypeKind::Pointer || param.kind == TypeKind::FnPtr);
    case Expr::Kind::Closure:
        return param.kind == TypeKind::FnPtr && param.params.size() == arg.closure->params.size();
    default:
        return false;
    }
}

bool Lowerer::lowerArgsPre(const Expr& call,
                           const std::vector<Type>& params,
                           std::vector<PreArg>& pre,
                           std::vector<EP>& out,
                           const std::string& callee) {
    if (call.args.size() != params.size()) {
        err("E0710",
            call.span,
            std::format(
                "`{}` takes {} argument(s), {} given", callee, params.size(), call.args.size()),
            "wrong number of arguments");
        return false;
    }
    bool ok = true;
    for (std::size_t i = 0; i < params.size(); ++i) {
        EP v = pre[i].literal ? expr(*call.args[i], &params[i]) : std::move(pre[i].value);
        EP a = moveOut(coerce(std::move(v), params[i], call.args[i]->span));
        ok = ok && a != nullptr;
        out.push_back(std::move(a));
    }
    return ok;
}

std::optional<std::size_t> Lowerer::resolveOverload(const Expr& call,
                                                    const std::string& name,
                                                    const std::vector<OverloadCandidate>& cands,
                                                    const std::vector<PreArg>& pre) {
    // A non-literal argument that failed to lower has reported its error already.
    for (const PreArg& p : pre) {
        if (!p.literal && !p.value) {
            return std::nullopt;
        }
    }
    if (cands.size() == 1) {
        return 0; // a single candidate is checked by `lowerArgsPre`
    }
    struct Scored {
        std::size_t index;
        int cost;
    };
    // Costs and viability come from the cached types only: no lowering, no diagnostics.
    std::vector<Scored> viable;
    for (std::size_t c = 0; c < cands.size(); ++c) {
        const OverloadCandidate& cand = cands[c];
        if (cand.params.size() != call.args.size()) {
            continue;
        }
        int cost = 0;
        bool ok = true;
        for (std::size_t i = 0; i < call.args.size() && ok; ++i) {
            const Expr& arg = *call.args[i];
            const PreArg& p = pre[i];
            const Type& param = cand.params[i];
            ok = p.literal ? literalFits(arg, param) : convertible(*p.value, param);
            cost += argumentCost(arg, p.type, param);
        }
        if (ok) {
            viable.push_back({c, cost});
        }
    }
    if (viable.empty()) {
        std::string shown;
        std::string candidates;
        for (const OverloadCandidate& cand : cands) {
            candidates += "\n  " + signatureOf(name, cand.params);
        }
        for (std::size_t i = 0; i < call.args.size(); ++i) {
            shown += (i != 0 ? ", " : "") + argumentText(*call.args[i], pre[i].type);
        }
        Diagnostic d;
        d.code = "E0404";
        d.severity = Severity::Error;
        d.message = std::format("no `{}` takes ({})", name, shown);
        d.labels.push_back({call.span, "no overload matches these arguments"});
        d.fix = "candidates:" + candidates;
        s_.diag_.report(std::move(d));
        return std::nullopt;
    }
    int best = viable.front().cost;
    for (const Scored& s : viable) {
        best = std::min(best, s.cost);
    }
    std::vector<std::size_t> tied;
    for (const Scored& s : viable) {
        if (s.cost == best) {
            tied.push_back(s.index);
        }
    }
    if (tied.size() > 1) {
        std::string list;
        for (const std::size_t t : tied) {
            list += "\n  " + signatureOf(name, cands[t].params);
        }
        Diagnostic d;
        d.code = "E0403";
        d.severity = Severity::Error;
        d.message = std::format("call to `{}` is ambiguous", name);
        d.labels.push_back({call.span, "several overloads match equally well"});
        d.fix = "write the argument types so one overload is closer, e.g. a typed literal:" + list;
        s_.diag_.report(std::move(d));
        return std::nullopt;
    }
    return tied.front();
}

} // namespace xlang::lower
