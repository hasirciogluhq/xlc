// Lowering: function bodies: parameters, prologue, return checks and drops at the end.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

void Lowerer::lowerFunction(const FnDecl& f,
                            const Modifiers& mods,
                            const std::string& qname,
                            const std::string& local_name,
                            const std::vector<Attribute>& attrs,
                            const std::string& owner,
                            const Type* self_type,
                            bool overloaded) {
    hir::Function out;
    out.name = qname;
    out.module = s_.key_;
    out.owner = owner;
    out.is_init = sh_.init_fns.contains(qname);
    out.is_fini = sh_.fini_fns.contains(qname);
    out.is_entry = sh_.entry_fn == qname;
    out.exported = mods.exported || mods.export_c || mods.registered;
    out.export_c = mods.export_c;
    out.declared = mods.declared;
    if (const Attribute* lib = lower::findAttribute(attrs, "library");
        lib != nullptr && mods.declared) {
        if (lib->args.size() == 1 && lib->args[0].kind == AttributeArg::Kind::String) {
            out.import_library = lib->args[0].text;
        } else {
            err("E0705",
                f.name_span,
                "`@library` takes one string (the OS library name)",
                "invalid argument");
        }
    }
    out.registered = mods.registered;
    out.register_role = mods.register_role;
    out.is_virtual = f.is_virtual;
    out.is_override = f.is_override;
    out.has_body = f.has_body;
    out.span = f.span;
    out.ret = resolved(f.return_type);
    // A closure whose return type is not written takes it from its first `return` (see closure.cpp).
    inferring_ = std::exchange(infer_request_, false);
    if (inferring_) {
        out.ret = lower::unknownType();
    }

    fn_ = &out;
    in_ctor_ = !owner.empty() && self_type != nullptr && f.name == "init";
    ret_ = out.ret;
    unsafe_ = 0;
    loops_.clear();
    defers_.clear();
    scopes_.push(ScopeKind::Function);
    // Parameters that need drop are owned by the callee: their drops run on every exit path.
    std::vector<hir::Stmt> prologue;
    struct OwnedParam {
        hir::LocalId id;
        Type type;
        Span span;
    };
    std::vector<OwnedParam> owned_params;
    const std::string base_call = std::exchange(base_init_call_, {});
    entry_temps_.clear();
    entry_flags_.clear();
    unwind_entries_.clear();
    sum_views_.clear();
    has_try_ = false;
    defers_.emplace_back();
    std::vector<Type> param_types;
    for (const Param& p : f.params) {
        Type t;
        if (p.is_self) {
            if (self_type == nullptr) {
                err("E0709",
                    p.span,
                    "`self` outside of a method",
                    "only class and `impl` methods have `self`");
                t = lower::unknownType();
            } else {
                t = *self_type;
            }
        } else {
            t = resolved(p.type);
        }
        param_types.push_back(t);
        const hir::LocalId id = addLocal(p.is_self ? "self" : p.name, t, true, p.span);
        bindLocal(p.is_self ? "self" : p.name, id, t, p.span);
        if (!p.is_self) {
            rejectDroppablePlace(t, p.span, "a parameter");
        }
        if (!p.is_self && needsDrop(t)) {
            owned_params.push_back({id, t, p.span});
        }
    }
    if (out.is_entry) {
        checkEntrySignature(f, param_types, out.ret);
    }
    out.param_count = static_cast<std::uint32_t>(out.locals.size());
    // The drop flags follow the parameters (the first `param_count` locals).
    for (const OwnedParam& p : owned_params) {
        registerDrop(p.id, p.type, true, p.span, prologue);
    }
    if (mods.export_c) {
        bool ok = cCompatible(s_, out.ret);
        for (const Type& t : param_types) {
            ok = ok && cCompatible(s_, t);
        }
        if (!ok) {
            err("E0302",
                f.name_span,
                "`export(c)` signature uses a type that C cannot express",
                "integers, floats, bool, pointers, function pointers, `@repr(c)` structs and "
                "integer enums only");
        }
    }
    assignSymbol(attrs,
                 mods.export_c,
                 mods.declared,
                 local_name,
                 param_types,
                 overloaded,
                 out.symbol,
                 f.name_span);
    if (f.has_body) {
        if (!base_call.empty()) {
            // A derived `init` first runs the base class constructor on `self`.
            EP self = localRef(0, out.locals.front().type, f.span);
            EP call = mk(hir::ExprKind::Call, Type::makeVoid(), f.span);
            call->text = base_call;
            call->args.push_back(std::move(self));
            hir::Stmt s;
            s.kind = hir::StmtKind::Expr;
            s.span = f.span;
            s.value = std::move(call);
            prologue.push_back(std::move(s));
        }
        lowerBlock(f.body, out.body, false);
        if (!out.ret.isVoid() && !lower::isUnknown(out.ret) && !lower::terminates(out.body)) {
            err("E0764",
                f.name_span,
                out.ret.isNever()
                    ? std::format("`{}` is `-> never` but can reach its end", f.name)
                    : std::format("`{}` can reach the end without returning a value", f.name),
                out.ret.isNever() ? "end every path with an endless loop or a `-> never` call"
                                  : "not every path returns");
        }
    }
    // Falling off the end runs the parameter drops (a `return` already ran them); the temporaries
    // and their flags are declared first, so every path sees them initialized.
    const bool jumps = !out.body.empty() && out.body.back().kind == hir::StmtKind::Return;
    if (f.has_body && !jumps) {
        emitDefers(defers_.size() - 1, out.body);
    }
    defers_.pop_back();
    // Flags first: the prologue and the body set them.
    std::vector<hir::Stmt> head;
    for (const hir::LocalId flag : entry_flags_) {
        hir::Stmt live;
        live.kind = hir::StmtKind::Local;
        live.span = f.span;
        live.local = flag;
        live.value = boolConst(false, f.span);
        head.push_back(std::move(live));
    }
    entry_flags_.clear();
    for (const auto& [tmp, flag] : entry_temps_) {
        hir::Stmt decl;
        decl.kind = hir::StmtKind::Local;
        decl.span = f.span;
        decl.local = tmp;
        head.push_back(std::move(decl));
        hir::Stmt live;
        live.kind = hir::StmtKind::Local;
        live.span = f.span;
        live.local = flag;
        live.value = boolConst(false, f.span);
        head.push_back(std::move(live));
    }
    entry_temps_.clear();
    for (hir::Stmt& p : prologue) {
        head.push_back(std::move(p));
    }
    // The landing pad: every registered cleanup, newest first (needs the registered `unwind`).
    if (!mod_.unwind_function.empty() && f.has_body) {
        for (std::size_t i = unwind_entries_.size(); i-- > 0;) {
            out.unwind_body.push_back(lower::cloneHirStmt(unwind_entries_[i]));
        }
    }
    out.has_try = has_try_;
    unwind_entries_.clear();
    has_try_ = false;
    out.body.insert(out.body.begin(),
                    std::make_move_iterator(head.begin()),
                    std::make_move_iterator(head.end()));
    scopes_.pop();
    fn_ = nullptr;
    in_ctor_ = false;
    if (inferring_) {
        // No `return` gave a value: the closure returns nothing.
        out.ret = lower::isUnknown(ret_) ? Type::makeVoid() : ret_;
        inferred_ret_ = out.ret;
        inferring_ = false;
    }
    mod_.functions.push_back(std::move(out));
}

} // namespace xlang::lower
