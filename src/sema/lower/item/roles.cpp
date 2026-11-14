// Lowering: `register(role)` checks (panic, alloc, free, unwind, args) and `@init`/`@fini`/`@entry` checks.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

namespace {

[[nodiscard]] bool isVoidPointer(const Type& t) {
    return t.kind == TypeKind::Pointer && t.elem && t.elem->kind == TypeKind::Void;
}

} // namespace

/// @grammar `register(panic)`, `register(alloc)`, `register(free)`, `register(unwind)`, `register(args)`: one symbol per role per group, checked structurally against the shape the role needs.
/// @invariant group 1 is the stdlib default and a group 2 (user) registration replaces it per role, so user code can override the allocator without changing the stdlib.
void Lowerer::checkRegistrations() {
    for (const Item& item : s_.program_.items) {
        if (!item.modifiers.registered || item.modifiers.register_role.empty()) {
            continue;
        }
        const DeclBase& base = declBase(item);
        const std::string& role = item.modifiers.register_role;
        const auto* fn = std::get_if<FnDecl>(&item.decl);
        bool ok = false;
        std::string why;
        if (role == "panic") {
            ok = fn != nullptr && fn->params.size() == 1 && !fn->params[0].is_self &&
                 fn->return_type.isNever();
            if (ok) {
                const Type p = resolved(fn->params[0].type);
                ok = p.kind == TypeKind::Pointer && p.elem && p.elem->kind == TypeKind::Int &&
                     p.elem->bits == 8 && !p.elem->is_signed;
            }
            why = "`register(panic)` needs `fn(msg: *u8) -> never`";
        } else if (role == "alloc") {
            ok = fn != nullptr && fn->params.size() == 2 && !fn->params[0].is_self &&
                 !fn->params[1].is_self && isVoidPointer(resolved(fn->return_type));
            if (ok) {
                const Type a = resolved(fn->params[0].type);
                const Type b = resolved(fn->params[1].type);
                ok = a.kind == TypeKind::Int && !a.is_signed && b.kind == TypeKind::Int &&
                     !b.is_signed && a.bits == 0 && b.bits == 0; // `usize`
            }
            why = "`register(alloc)` needs `fn(size: usize, align: usize) -> *void`";
        } else if (role == "free") {
            ok = fn != nullptr && fn->params.size() == 1 && !fn->params[0].is_self &&
                 fn->return_type.isVoid() && isVoidPointer(resolved(fn->params[0].type));
            why = "`register(free)` needs `fn(p: *void)` with no return type";
        } else if (role == "unwind") {
            ok = fn != nullptr && fn->params.size() == 3 && fn->return_type.isVoid();
            if (ok) {
                const Type a = resolved(fn->params[0].type);
                const Type b = resolved(fn->params[1].type);
                const Type c = resolved(fn->params[2].type);
                const auto bytes = [](const Type& t) {
                    return t.kind == TypeKind::Pointer && t.elem && t.elem->kind == TypeKind::Int &&
                           t.elem->bits == 8 && !t.elem->is_signed;
                };
                ok = a.kind == TypeKind::Int && a.bits == 32 && a.is_signed &&
                     b.kind == TypeKind::Pointer && b.elem && b.elem->kind == TypeKind::Int &&
                     b.elem->bits == 0 && !b.elem->is_signed && bytes(c); // `*usize`, `*u8`
            }
            why = "`register(unwind)` needs `fn(op: i32, snapshot: *usize, message: *u8)` with no "
                  "return type";
        } else if (role == "args") {
            // `fn(argc: *i32) -> **u8`: stores the argument count and returns the argument vector.
            ok = fn != nullptr && fn->params.size() == 1 && !fn->params[0].is_self;
            if (ok) {
                const Type a = resolved(fn->params[0].type);
                const Type r = resolved(fn->return_type);
                ok = a.kind == TypeKind::Pointer && a.elem && a.elem->kind == TypeKind::Int &&
                     a.elem->bits == 32 && a.elem->is_signed && r.kind == TypeKind::Pointer &&
                     r.elem && r.elem->kind == TypeKind::Pointer && r.elem->elem &&
                     r.elem->elem->kind == TypeKind::Int && r.elem->elem->bits == 8 &&
                     !r.elem->elem->is_signed; // `*i32`, `**u8`
            }
            why = "`register(args)` needs `fn(argc: *i32) -> **u8`";
        } else {
            why = std::format("unknown role `{}`; known roles: panic, alloc, free, unwind, args",
                              role);
        }
        if (!ok) {
            err("E0503",
                base.name_span,
                std::format("`{}` does not fit role `{}`", base.name, role),
                why);
            continue;
        }
        const std::string qname = s_.qualify(base.name);
        const LowerShared::Role info{
            base.name_span, base.name, qname, is_library_, &s_, s_.findLocal(base.name)};
        const auto [it, inserted] = sh_.roles.emplace(role, info);
        if (!inserted) {
            if (it->second.from_library == is_library_) {
                Diagnostic d;
                d.code = "E0502";
                d.severity = Severity::Error;
                d.message = std::format("role `{}` is registered twice", role);
                d.labels.push_back({it->second.span, "first registration"});
                d.labels.push_back({base.name_span, "second registration"});
                d.fix = "keep one registration per role in your program; a library default is "
                        "replaced by registering the role in your own code";
                s_.diag_.report(std::move(d));
                continue;
            }
            // A library default yields to the program; a library registration never replaces one.
            if (is_library_) {
                continue;
            }
            it->second = info;
        }
        if (role == "panic") {
            mod_.panic_function = qname;
        } else if (role == "alloc") {
            mod_.alloc_function = qname;
        } else if (role == "free") {
            mod_.free_function = qname;
        } else if (role == "args") {
            mod_.args_function = qname;
        } else {
            mod_.unwind_function = qname;
        }
    }
}

/// @grammar `@init fn`, `@fini fn`, `@entry fn`: module-level hooks that are not called by any code and are kept by reachability for that reason.
/// @invariant `@init` runs for the stdlib group before user code and for an import before its importer; `_xl_initAll` is the only caller and it is compiler-owned.
void Lowerer::checkHookFunctions() {
    // Attribute name, and the code reported when it is put on something that cannot be that hook.
    struct Hook {
        std::string_view attr;
        const char* code;
    };
    constexpr std::array<Hook, 3> hooks{{{"init", "E0729"}, {"fini", "E0729"}, {"entry", "E0752"}}};
    for (const Item& item : s_.program_.items) {
        const DeclBase& base = declBase(item);
        if (const auto* cls = std::get_if<ClassDecl>(&item.decl)) {
            for (const FnDecl& m : cls->methods) {
                for (const Hook& h : hooks) {
                    if (const Attribute* at = lower::findAttribute(m.attributes, h.attr)) {
                        err(h.code,
                            at->span,
                            std::format("`@{}` on method `{}.{}` is not allowed",
                                        h.attr,
                                        base.name,
                                        m.name),
                            std::format("`@{}` is for module-level functions", h.attr));
                    }
                }
            }
        }
        const auto* fn = std::get_if<FnDecl>(&item.decl);
        for (const Hook& h : hooks) {
            const Attribute* at = lower::findAttribute(base.attributes, h.attr);
            if (at == nullptr) {
                continue;
            }
            // Init and fini run without arguments; the entry's signature is checked with its types.
            const bool no_params = h.attr != "entry";
            if (fn == nullptr || !fn->generic_params.empty() || !fn->has_body ||
                (no_params && (!fn->params.empty() || !fn->return_type.isVoid()))) {
                err(h.code,
                    at->span,
                    std::format("`@{}` on `{}` needs a plain function", h.attr, base.name),
                    no_params
                        ? std::format("write `@{} fn name() {{ ... }}`: no generics, parameters, "
                                      "return type or declaration without a body",
                                      h.attr)
                        : "write `@entry fn main()`: no generics, methods or declaration without a "
                          "body");
                continue;
            }
            const std::string qname = s_.qualify(base.name);
            if (h.attr == "init") {
                sh_.init_fns.insert(qname);
                init_order_.push_back(qname);
            } else if (h.attr == "fini") {
                sh_.fini_fns.insert(qname);
                fini_order_.push_back(qname);
            } else if (!sh_.entry_fn.empty()) {
                err("E0753",
                    at->span,
                    std::format("`@entry` on `{}` but the program already has `@entry` on `{}`",
                                base.name,
                                sh_.entry_fn),
                    "a program has exactly one `@entry` function: remove the second one");
            } else {
                sh_.entry_fn = qname;
            }
        }
    }
}

void Lowerer::checkEntrySignature(const FnDecl& f,
                                  const std::vector<Type>& params,
                                  const Type& ret) {
    const auto isUi8 = [](const Type& t) {
        return t.kind == TypeKind::Int && t.bits == 8 && !t.is_signed;
    };
    const auto isPtrTo = [](const Type& t, const auto& pred) {
        return t.kind == TypeKind::Pointer && t.elem != nullptr && pred(*t.elem);
    };
    const bool ret_ok =
        ret.isVoid() || (ret.kind == TypeKind::Int && ret.bits == 32 && ret.is_signed);
    const bool argc_ok = params.size() >= 1 && params[0].kind == TypeKind::Int &&
                         params[0].bits == 32 && params[0].is_signed;
    const bool argv_ok =
        params.size() >= 2 && isPtrTo(params[1], [&](const Type& t) { return isPtrTo(t, isUi8); });
    const bool params_ok = params.empty() || (params.size() == 2 && argc_ok && argv_ok);
    if (!ret_ok || !params_ok) {
        err("E0752",
            f.name_span,
            std::format("`@entry` function `{}` has an invalid signature", f.name),
            "write `@entry fn name()` or `@entry fn name(argc: i32, argv: **u8) -> i32`: "
            "the return type is void or i32");
    }
}

} // namespace xlang::lower
