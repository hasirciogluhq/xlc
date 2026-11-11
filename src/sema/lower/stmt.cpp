#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

// Statements (control flow, `defer`, `try`) and ownership (drop glue, drop flags, moves).

// ---------------------------------------------------------------------------------------------
// Statements

hir::LocalId
Lowerer::addLocal(const std::string& name, const Type& type, bool is_param, const Span& span) {
    fn_->locals.push_back(hir::Local{name, type, is_param, span});
    return static_cast<hir::LocalId>(fn_->locals.size() - 1);
}

void Lowerer::bindLocal(const std::string& name,
                        hir::LocalId id,
                        const Type& type,
                        const Span& span) {
    if (isRegisteredName(name)) {
        s_.diag_.warning("W0402",
                         span,
                         std::format("`{}` shadows a registered global", name),
                         "a `register`ed name is hidden here; its role binding is unaffected");
    }
    ScopeEntry e;
    e.kind = ScopeEntry::Kind::Local;
    e.name = name;
    e.type = type;
    e.span = span;
    e.local = id;
    if (const ScopeEntry* prev = scopes_.declare(std::move(e))) {
        Diagnostic d;
        d.code = "E0401";
        d.severity = Severity::Error;
        d.message = std::format("`{}` is defined twice in this scope", name);
        d.labels.push_back({prev->span, "first definition"});
        d.labels.push_back({span, "second definition"});
        d.fix = "rename one of them";
        s_.diag_.report(std::move(d));
    }
}

// ---------------------------------------------------------------------------------------------
// Ownership (core-language phase 5). Droppable locals carry a drop flag; the drop of a local is
// a deferred statement `if flag { glue(&x); flag = false }`, so the existing defer machinery runs
// it on every exit path.

bool Lowerer::droppableArray(const Type& t) {
    return t.kind == TypeKind::Array && t.elem && (needsDrop(*t.elem) || droppableArray(*t.elem));
}

bool Lowerer::droppableSum(const Type& t) {
    const Type* cur = &t;
    while (cur->kind == TypeKind::Array && cur->elem) {
        cur = cur->elem.get();
    }
    if (cur->kind != TypeKind::Struct || cur->path.empty() || !needsDrop(*cur)) {
        return false;
    }
    const auto ref = entityOfType(*cur);
    const StructDecl* decl = ref ? sumDecl(ref->sym->item) : nullptr;
    return decl != nullptr && !decl->variants.empty();
}

void Lowerer::rejectDroppableArray(const Type& t, const Span& at, const char* where) {
    if (droppableArray(t)) {
        err("E0721",
            at,
            std::format("`{}` is an array of a type that needs drop and cannot be {}",
                        typeToString(t),
                        where),
            "arrays do not drop their elements; use a class value instead");
    }
}

void Lowerer::rejectDroppablePlace(const Type& t, const Span& at, const char* where) {
    // An array of a payload enum that needs drop is the sum-type case of the array rule, and the
    // message names the enum, not the `$`-named union it is built from.
    if (droppableArray(t) && droppableSum(t)) {
        err("E0759",
            at,
            std::format("`{}` is an array of a payload enum that needs drop and cannot be {}",
                        typeToString(t),
                        where),
            "arrays do not drop their elements; use a `Vec<T>` instead");
        return;
    }
    rejectDroppableArray(t, at, where);
}

bool Lowerer::needsDrop(const Type& t) {
    if ((t.kind != TypeKind::Class && t.kind != TypeKind::Struct) || t.path.empty()) {
        return false;
    }
    const std::string& qname = t.path.front();
    if (const auto it = needs_drop_.find(qname); it != needs_drop_.end()) {
        return it->second;
    }
    needs_drop_[qname] = false; // guards against a cycle
    bool result = false;
    if (const auto ref = s_.entityByQName(qname)) {
        Sema& owner = *ref->owner;
        if (const auto* c = std::get_if<ClassDecl>(&ref->sym->item->decl)) {
            for (const FnDecl& m : c->methods) {
                result = result || m.name == "drop";
            }
            for (const FieldDecl& f : c->fields) {
                result = result || needsDrop(resolvedIn(owner, f.type));
            }
            if (c->has_base) {
                if (const auto base = owner.resolve(Type::makeNamed(c->base))) {
                    result = result || needsDrop(*base);
                }
            }
        } else if (const auto* st = std::get_if<StructDecl>(&ref->sym->item->decl)) {
            if (!st->variants.empty()) {
                // A payload enum owns its payload: it needs drop when one of its variants does.
                for (const SumVariant& v : st->variants) {
                    result = result || std::ranges::any_of(v.payload, [&](const Type& p) {
                                 return needsDrop(resolvedIn(owner, p));
                             });
                }
            } else {
                for (const FieldDecl& f : st->fields) {
                    result = result || needsDrop(resolvedIn(owner, f.type));
                }
            }
        }
    }
    needs_drop_[qname] = result;
    return result;
}

std::string Lowerer::dropGlueName(const std::string& qname) {
    return qname + "$dropGlue";
}

bool Lowerer::hasInit(const std::string& qname) {
    const auto ref = s_.entityByQName(qname);
    const auto* c = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
    return c != nullptr &&
           std::ranges::any_of(c->methods, [](const FnDecl& m) { return m.name == "init"; });
}

std::string Lowerer::zeroArgInitOf(const std::string& qname) {
    const auto ref = s_.entityByQName(qname);
    const auto* c = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
    if (c == nullptr) {
        return {};
    }
    for (const FnDecl& m : c->methods) {
        if (m.name == "init" &&
            std::ranges::all_of(m.params, [](const Param& p) { return p.is_self; })) {
            return qname + ".init";
        }
    }
    return {};
}

std::string Lowerer::baseInitKey(const ClassDecl& cl, Sema& owner) {
    if (!cl.has_base) {
        return {};
    }
    const auto base = owner.resolve(Type::makeNamed(cl.base));
    if (!base || base->kind != TypeKind::Class) {
        return {};
    }
    return zeroArgInitOf(base->path.front());
}

void Lowerer::emitDropGlue(const std::string& qname, const ClassDecl& c, const Span& span) {
    Type cls = Type::makeNamed({qname});
    cls.kind = TypeKind::Class;
    if (!needsDrop(cls)) {
        return;
    }
    const Type ptr = Type::makePointer(cls);
    hir::Function g;
    g.name = dropGlueName(qname);
    g.symbol = g.name;
    g.module = s_.key_;
    g.exported = true;
    g.has_body = true;
    g.span = span;
    g.ret = Type::makeVoid();
    g.locals.push_back(hir::Local{"p", ptr, true, span});
    g.param_count = 1;
    const auto self = [&] { return localRef(0, ptr, span); };
    const auto statement = [&](EP value) {
        hir::Stmt s;
        s.kind = hir::StmtKind::Expr;
        s.span = span;
        s.value = std::move(value);
        return s;
    };
    // The user's `drop` body, then the fields that need drop in reverse order, then the base.
    if (std::ranges::any_of(c.methods, [](const FnDecl& m) { return m.name == "drop"; })) {
        EP call = mk(hir::ExprKind::Call, Type::makeVoid(), span);
        call->text = qname + ".drop";
        call->args.push_back(self());
        g.body.push_back(statement(std::move(call)));
    }
    std::vector<FieldInfo> layout;
    collectFields(qname, layout);
    for (std::size_t i = c.fields.size(); i-- > 0;) {
        const FieldDecl& f = c.fields[i];
        const Type ft = resolved(f.type);
        if (!needsDrop(ft)) {
            continue;
        }
        std::uint32_t index = 0;
        for (std::size_t k = 0; k < layout.size(); ++k) {
            if (layout[k].decl == &f) {
                index = static_cast<std::uint32_t>(k);
            }
        }
        EP field = mk(hir::ExprKind::Field, ft, span);
        field->text = f.name;
        field->index = index;
        field->through_pointer = true;
        field->lvalue = true;
        field->lhs = self();
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(ft), span);
        addr->lhs = std::move(field);
        g.body.push_back(statement(dropCallOn(std::move(addr), ft, span)));
    }
    if (c.has_base) {
        if (const auto base = s_.resolve(Type::makeNamed(c.base)); base && needsDrop(*base)) {
            EP call = mk(hir::ExprKind::Call, Type::makeVoid(), span);
            call->text = dropGlueName(base->path.front());
            call->args.push_back(self());
            g.body.push_back(statement(std::move(call)));
        }
    }
    mod_.functions.push_back(std::move(g));
}

/// The drop glue of a payload enum `E` (it is a struct `{ tag: E$Tag, data: E$Data }`):
///
///   E$dropGlue(p: *E) {
///       switch p.tag { case E.Variant { drop(&p.data.Variant._i) ... } }
///   }
///
/// Only the payload of the live variant is touched, so the bytes of the other variants stay
/// unread. A variant without a droppable payload gets no case; the switch is a chain of `if`s,
/// and the tag is compared against the variant's position, exactly as `switch` on the enum
/// builds it.
void Lowerer::emitSumDropGlue(const std::string& qname, const StructDecl& sum, const Span& span) {
    Type self = Type::makeNamed({qname});
    self.kind = TypeKind::Struct;
    if (!needsDrop(self)) {
        return;
    }
    // The glue is named after the instance (`Result$dropGlue<i32, E>`): two instances of one
    // generic enum have different payloads, so they cannot share a body.
    const std::string glue = dropGlueName(qname);
    const Type ptr = Type::makePointer(self);
    const Sum parts{&s_, &sum};
    const Type tag_type = sumPart(parts, "$Tag", span);
    hir::Function g;
    g.name = glue;
    g.symbol = glue;
    g.module = s_.key_;
    g.exported = true;
    g.has_body = true;
    g.span = span;
    g.ret = Type::makeVoid();
    g.locals.push_back(hir::Local{"p", ptr, true, span});
    g.param_count = 1;
    const auto self_ptr = [&] { return localRef(0, ptr, span); };
    const auto statement = [&](EP value) {
        hir::Stmt s;
        s.kind = hir::StmtKind::Expr;
        s.span = span;
        s.value = std::move(value);
        return s;
    };
    /// `p.<name>`: a field of the enum itself (`tag`, `data`) or, when `through` is set, of the
    /// object `p` points at.
    const auto field = [&](EP base,
                           const std::string& name,
                           std::uint32_t index,
                           Type type,
                           bool through = false) {
        EP f = mk(hir::ExprKind::Field, std::move(type), span);
        f->text = name;
        f->index = index;
        f->through_pointer = through;
        f->lvalue = true;
        f->lhs = std::move(base);
        return f;
    };
    EP tag = field(self_ptr(), "tag", 0, tag_type, true);
    hir::Stmt chain;
    bool has_chain = false;
    for (const std::size_t i : std::views::iota(std::size_t{0}, sum.variants.size())) {
        const SumVariant& v = sum.variants[i];
        std::vector<std::pair<std::size_t, Type>> droppable;
        for (const std::size_t k : std::views::iota(std::size_t{0}, v.payload.size())) {
            Type ft = resolvedIn(s_, v.payload[k]);
            rejectDroppablePlace(ft, span, "a payload value");
            if (needsDrop(ft)) {
                droppable.emplace_back(k, std::move(ft));
            }
        }
        if (droppable.empty()) {
            continue;
        }
        hir::Stmt body;
        body.kind = hir::StmtKind::Block;
        body.span = span;
        for (const auto& [k, ft] : droppable) {
            EP data = field(self_ptr(), "data", 1, sumPart(parts, "$Data", span), true);
            EP payload = field(std::move(data),
                               v.name,
                               static_cast<std::uint32_t>(sumDataIndex(parts, i)),
                               sumPart(parts, "$" + v.name, span));
            EP value =
                field(std::move(payload), std::format("_{}", k), static_cast<std::uint32_t>(k), ft);
            EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(ft), span);
            addr->lhs = std::move(value);
            body.body.push_back(statement(dropCallOn(std::move(addr), ft, span)));
        }
        EP eq = mk(hir::ExprKind::Binary, Type::makeBool(), span);
        eq->binary_op = BinaryOp::Eq;
        eq->lhs = i == 0 ? std::move(tag) : field(self_ptr(), "tag", 0, tag_type, true);
        eq->rhs = intConst(i, tag_type, span);
        hir::Stmt root;
        root.kind = hir::StmtKind::If;
        root.span = span;
        root.value = std::move(eq);
        root.body.push_back(std::move(body));
        if (has_chain) {
            root.else_body.push_back(std::move(chain));
        }
        chain = std::move(root);
        has_chain = true;
    }
    if (has_chain) {
        g.body.push_back(std::move(chain));
    }
    mod_.functions.push_back(std::move(g));
}

Lowerer::EP Lowerer::localRef(hir::LocalId id, const Type& type, const Span& span) {
    EP r = mk(hir::ExprKind::LocalRef, type, span);
    r->local = id;
    r->text = fn_ != nullptr && id < fn_->locals.size() ? fn_->locals[id].name : std::string{};
    r->lvalue = true;
    return r;
}

Lowerer::EP Lowerer::boolConst(bool value, const Span& span) {
    EP b = mk(hir::ExprKind::BoolConst, Type::makeBool(), span);
    b->bool_value = value;
    return b;
}

Lowerer::EP Lowerer::dropCallOn(EP addr, const Type& t, const Span& span) {
    EP call = mk(hir::ExprKind::Call, Type::makeVoid(), span);
    call->text = dropGlueName(t.path.front());
    call->args.push_back(std::move(addr));
    return call;
}

hir::Stmt Lowerer::dropStmt(hir::LocalId var, hir::LocalId flag, const Type& t, const Span& span) {
    hir::Stmt s;
    s.kind = hir::StmtKind::If;
    s.span = span;
    s.value = localRef(flag, Type::makeBool(), span);
    hir::Stmt drop;
    drop.kind = hir::StmtKind::Expr;
    drop.span = span;
    EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(t), span);
    addr->lhs = localRef(var, t, span);
    drop.value = dropCallOn(std::move(addr), t, span);
    hir::Stmt clear;
    clear.kind = hir::StmtKind::Assign;
    clear.span = span;
    clear.target = localRef(flag, Type::makeBool(), span);
    clear.value = boolConst(false, span);
    s.body.push_back(std::move(drop));
    s.body.push_back(std::move(clear));
    return s;
}

void Lowerer::registerDrop(hir::LocalId var,
                           const Type& t,
                           bool initialized,
                           const Span& span,
                           std::vector<hir::Stmt>& decls) {
    const hir::LocalId flag = hiddenLocal("live", Type::makeBool(), span);
    // The flag itself is declared `false` at the function head; here it is set.
    entry_flags_.push_back(flag);
    hir::Stmt decl;
    decl.kind = hir::StmtKind::Assign;
    decl.span = span;
    decl.target = localRef(flag, Type::makeBool(), span);
    decl.value = boolConst(initialized, span);
    decls.push_back(std::move(decl));
    drop_flags_[var] = flag;
    if (!defers_.empty()) {
        hir::Stmt cleanup = dropStmt(var, flag, t, span);
        unwind_entries_.push_back(lower::cloneHirStmt(cleanup));
        defers_.back().push_back(std::move(cleanup));
    }
}

Lowerer::EP Lowerer::moveOut(EP value, bool fromSum) {
    if (!value || !needsDrop(value->type)) {
        return value;
    }
    if (value->kind == hir::ExprKind::LocalRef) {
        const auto it = drop_flags_.find(value->local);
        if (it == drop_flags_.end()) {
            return value;
        }
        EP m = mk(hir::ExprKind::Move, value->type, value->span);
        m->local = it->second;
        m->lhs = std::move(value);
        return m;
    }
    if (value->kind == hir::ExprKind::Field || value->kind == hir::ExprKind::Index ||
        value->kind == hir::ExprKind::Deref) {
        // The payload of a `case` pattern of a payload enum: the switch owns the scrutinee and the
        // glue drops only the live variant, so the binding takes the value and the owner is
        // consumed afterwards.
        if (fromSum) {
            return value;
        }
        err("E0720",
            value->span,
            "cannot move out of a field, element or pointer",
            "take its address (`&x.f`) or copy it through a `clone()` method");
        return nullptr;
    }
    return value;
}
Lowerer::EP Lowerer::materializeTemp(EP value, const Span& span) {
    if (defers_.empty()) {
        return value;
    }
    const Type t = value->type;
    const hir::LocalId tmp = hiddenLocal("tmp", t, span);
    const hir::LocalId flag = hiddenLocal("live", Type::makeBool(), span);
    entry_temps_.emplace_back(tmp, flag);
    hir::Stmt cleanup = dropStmt(tmp, flag, t, span);
    unwind_entries_.push_back(lower::cloneHirStmt(cleanup));
    defers_.back().push_back(std::move(cleanup));
    EP m = mk(hir::ExprKind::Materialize, t, span);
    m->local = tmp;
    m->index = flag;
    m->lhs = std::move(value);
    m->lvalue = true;
    return m;
}

void Lowerer::lowerDropAssign(EP target, EP value, const Span& span, std::vector<hir::Stmt>& out) {
    const Type t = target->type;
    const hir::LocalId tmp = hiddenLocal("asg", t, span);
    hir::Stmt store;
    store.kind = hir::StmtKind::Local;
    store.span = span;
    store.local = tmp;
    store.value = std::move(value);
    out.push_back(std::move(store));
    // The new value is computed first (it may read the old one); then the old one is dropped.
    const auto flag = target->kind == hir::ExprKind::LocalRef ? drop_flags_.find(target->local)
                                                              : drop_flags_.end();
    const bool tracked = flag != drop_flags_.end();
    if (tracked) {
        out.push_back(dropStmt(target->local, flag->second, t, span));
    } else {
        hir::Stmt old;
        old.kind = hir::StmtKind::Expr;
        old.span = span;
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(t), span);
        addr->lhs = lower::cloneHirExpr(*target);
        old.value = dropCallOn(std::move(addr), t, span);
        out.push_back(std::move(old));
    }
    hir::Stmt assign;
    assign.kind = hir::StmtKind::Assign;
    assign.span = span;
    assign.target = std::move(target);
    assign.value = localRef(tmp, t, span);
    out.push_back(std::move(assign));
    if (tracked) {
        hir::Stmt live;
        live.kind = hir::StmtKind::Assign;
        live.span = span;
        live.target = localRef(flag->second, Type::makeBool(), span);
        live.value = boolConst(true, span);
        out.push_back(std::move(live));
    }
}

Lowerer::EP Lowerer::genericConstruct(const Expr& e, const Expr& callee, const ScopeEntry& entry) {
    Sema& owner = entry.owner != nullptr ? *entry.owner : s_;
    const std::string local = declBase(*entry.item).name;
    const Sema::Symbol* sym = owner.find(local);
    if (sym == nullptr || callee.generic_args.empty()) {
        err("E0724",
            e.span,
            std::format("cannot infer the type arguments of `{}`", local),
            std::format("write them: `{}<...>(...)`", local));
        return nullptr;
    }
    std::vector<Type> targs;
    for (const Type& a : callee.generic_args) {
        targs.push_back(resolved(a));
    }
    const auto inst = owner.instantiate(Sema::EntityRef{&owner, sym, local}, targs, e.span);
    if (!inst) {
        return nullptr;
    }
    return constructCall(e, entryOf(*inst, entry.name, e.span));
}

bool Lowerer::lowerInitCall(const Expr& e, const Type& t, std::string& key, std::vector<EP>& args) {
    const auto ref = s_.entityByQName(t.path.front());
    const auto* cls = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
    key.clear();
    args.clear();
    if (cls == nullptr) {
        return false;
    }
    MethodRef m;
    m.kind = MethodRef::Kind::Direct;
    m.cls = cls;
    m.cls_q = t.path.front();
    m.owner = ref->owner;
    const auto cands = methodCandidates(m, "init");
    if (cands.empty()) {
        if (!e.args.empty()) {
            err("E0710",
                e.span,
                std::format("`{}` has no `init`; it takes no arguments", cls->name),
                "write an `init` method to take arguments");
            return false;
        }
        return true;
    }
    std::size_t pick = 0;
    if (cands.size() > 1) {
        std::vector<PreArg> pre = preLowerArgs(e);
        const auto chosen = resolveOverload(e, cls->name, cands, pre);
        if (!chosen) {
            return false;
        }
        pick = *chosen;
        if (!lowerArgsPre(e, cands[pick].params, pre, args, cls->name)) {
            return false;
        }
    } else if (!lowerArgs(e, cands[pick].params, args, cls->name)) {
        return false;
    }
    key = cands[pick].key;
    return true;
}

Lowerer::EP Lowerer::constructCall(const Expr& e, const ScopeEntry& entry) {
    const auto& cls = std::get<ClassDecl>(entry.item->decl);
    const std::string qname = entry.qname.empty() ? cls.name : entry.qname;
    Type t = Type::makeNamed({qname});
    t.kind = TypeKind::Class;
    t.span = e.span;
    std::string key;
    std::vector<EP> args;
    if (!lowerInitCall(e, t, key, args)) {
        return nullptr;
    }
    EP out = mk(hir::ExprKind::Construct, t, e.span);
    out->target = t;
    out->text = key;
    out->args = std::move(args);
    return out;
}

void Lowerer::lowerBlock(const Block& block, std::vector<hir::Stmt>& out, bool new_scope) {
    if (new_scope) {
        scopes_.push(ScopeKind::Block);
    }
    defers_.emplace_back();
    for (const Stmt& s : block.statements) {
        lowerStmt(s, out);
    }
    // Deferred statements run when the block is left by falling off its end; `return`, `break`
    // and `continue` emit them at the jump.
    const bool jumps = !out.empty() && (out.back().kind == hir::StmtKind::Return ||
                                        out.back().kind == hir::StmtKind::Break ||
                                        out.back().kind == hir::StmtKind::Continue);
    if (!jumps) {
        emitDefers(defers_.size() - 1, out);
    }
    defers_.pop_back();
    if (new_scope) {
        scopes_.pop();
    }
}

hir::Stmt Lowerer::leaveCall(hir::LocalId snap, const Span& span) {
    const Type usize_t = Type::makeInt(0, false);
    EP call = mk(hir::ExprKind::Call, Type::makeVoid(), span);
    call->text = mod_.unwind_function;
    call->args.push_back(intConst(0, Type::makeInt(32, true), span));
    EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(fn_->locals[snap].type), span);
    addr->lhs = localRef(snap, fn_->locals[snap].type, span);
    EP cast = mk(hir::ExprKind::Cast, Type::makePointer(usize_t), span);
    cast->lhs = std::move(addr);
    call->args.push_back(std::move(cast));
    call->args.push_back(mk(hir::ExprKind::Nil, Type::makePointer(Type::makeInt(8, false)), span));
    hir::Stmt st;
    st.kind = hir::StmtKind::Expr;
    st.span = span;
    st.value = std::move(call);
    return st;
}

void Lowerer::lowerTry(const Stmt& s, std::vector<hir::Stmt>& out) {
    if (mod_.unwind_function.empty()) {
        err("E0501",
            s.span,
            "`try` has no registered `unwind` function",
            "register one with `register(unwind) fn ...`, or import a library that does");
        return;
    }
    has_try_ = true;
    const Type usize_t = Type::makeInt(0, false);
    const Type byte_t = Type::makeInt(8, false);
    const auto arrayOf = [&](const Type& elem, std::uint64_t n) {
        auto len = std::make_shared<Expr>();
        len->kind = Expr::Kind::IntLiteral;
        len->int_value = n;
        len->span = s.span;
        return Type::makeArray(elem, len);
    };
    const Type snap_t = arrayOf(usize_t, 32);
    const Type msg_t = arrayOf(byte_t, 256);
    const hir::LocalId snap = hiddenLocal("snap", snap_t, s.span);
    const hir::LocalId msg = hiddenLocal("msg", msg_t, s.span);
    for (const hir::LocalId id : {snap, msg}) {
        hir::Stmt decl;
        decl.kind = hir::StmtKind::Local;
        decl.span = s.span;
        decl.local = id;
        out.push_back(std::move(decl));
    }
    hir::Stmt t;
    t.kind = hir::StmtKind::Try;
    t.span = s.span;
    t.local = snap;
    t.msg_local = msg;

    // The body: leaving it by any way runs `leave`; cleanups registered inside it also belong to
    // the handler.
    const std::size_t first = unwind_entries_.size();
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();
    defers_.back().push_back(leaveCall(snap, s.span));
    for (const Stmt& b : s.body->statements) {
        lowerStmt(b, t.body);
    }
    const bool jumps = !t.body.empty() && (t.body.back().kind == hir::StmtKind::Return ||
                                           t.body.back().kind == hir::StmtKind::Break ||
                                           t.body.back().kind == hir::StmtKind::Continue);
    if (!jumps) {
        emitDefers(defers_.size() - 1, t.body);
    }
    defers_.pop_back();
    scopes_.pop();
    const std::size_t last = unwind_entries_.size();

    // The handler runs the body's pending cleanups first, then binds the message.
    scopes_.push(ScopeKind::Block);
    for (std::size_t i = last; i-- > first;) {
        t.else_body.push_back(lower::cloneHirStmt(unwind_entries_[i]));
    }
    if (!s.name.empty()) {
        const Type ptr = Type::makePointer(byte_t);
        const hir::LocalId id = addLocal(s.name, ptr, false, s.span);
        bindLocal(s.name, id, ptr, s.span);
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(msg_t), s.span);
        addr->lhs = localRef(msg, msg_t, s.span);
        EP cast = mk(hir::ExprKind::Cast, ptr, s.span);
        cast->lhs = std::move(addr);
        hir::Stmt bind;
        bind.kind = hir::StmtKind::Local;
        bind.span = s.span;
        bind.local = id;
        bind.value = std::move(cast);
        t.else_body.push_back(std::move(bind));
    }
    lowerBlock(*s.else_body, t.else_body, false);
    scopes_.pop();
    out.push_back(std::move(t));
}

void Lowerer::emitDefers(std::size_t from, std::vector<hir::Stmt>& out) const {
    for (std::size_t f = defers_.size(); f-- > from;) {
        const auto& frame = defers_[f];
        for (std::size_t i = frame.size(); i-- > 0;) {
            out.push_back(lower::cloneHirStmt(frame[i]));
        }
    }
}

hir::LocalId Lowerer::hiddenLocal(const char* prefix, const Type& type, const Span& span) {
    return addLocal(std::format("${}{}", prefix, hidden_counter_++), type, false, span);
}

void Lowerer::lowerRawWrite(const Expr& call, const Span& span, std::vector<hir::Stmt>& out) {
    if (call.args.size() != 2) {
        err("E0710",
            call.span,
            "`__builtin_write` takes a pointer and a value",
            "write it as `__builtin_write(p, v)`");
        return;
    }
    EP p = expr(*call.args[0], nullptr);
    if (!p) {
        return;
    }
    if (p->type.kind != TypeKind::Pointer || !p->type.elem) {
        err("E0709",
            call.args[0]->span,
            std::format("`__builtin_write` needs a pointer, found `{}`", typeToString(p->type)),
            "pass `&x`");
        return;
    }
    const Type t = *p->type.elem;
    EP v = expr(*call.args[1], &t);
    if (v) {
        v = coerce(std::move(v), t, call.args[1]->span);
    }
    v = moveOut(std::move(v));
    if (!v) {
        return;
    }
    EP target = mk(hir::ExprKind::Deref, t, span);
    target->lhs = std::move(p);
    target->lvalue = true;
    hir::Stmt h;
    h.kind = hir::StmtKind::Assign;
    h.span = span;
    h.target = std::move(target);
    h.value = std::move(v);
    out.push_back(std::move(h));
}

void Lowerer::lowerAssign(const Expr& target_ast,
                          const Expr& value_ast,
                          const Span& span,
                          std::vector<hir::Stmt>& out) {
    if (target_ast.kind == Expr::Kind::Index && userIndexAssign(target_ast, value_ast, span, out)) {
        return; // `a[i] = v` through `operator[]=`
    }
    EP target = expr(target_ast, nullptr);
    if (target && lower::isStrPlace(*target)) {
        err("E0716",
            target_ast.span,
            "cannot assign through a `str`; it is read-only",
            "copy the bytes into a `[u8; N]` or a `String`");
        target.reset();
    }
    if (target && target->readonly) {
        err("E0716",
            target_ast.span,
            std::format("cannot assign to `const` `{}`", lower::placeRootName(*target)),
            "a `const` is read-only; copy it into a `local` to change it");
        target.reset();
    }
    if (target && !target->lvalue && !lower::isUnknown(target->type)) {
        // A write through a read-only view is the common case here: say so, instead of the generic
        // "not a place" line, so the fix (`[]mut T`) is obvious.
        if (target->kind == hir::ExprKind::Index && target->lhs &&
            target->lhs->type.kind == TypeKind::Slice && !target->lhs->type.is_mutable) {
            err("E0716",
                target_ast.span,
                "cannot write through a read-only slice `[]T`",
                "a writable view is `[]mut T`; take one from a mutable owner (`buf[..]`)");
        } else {
            err("E0709",
                target_ast.span,
                "cannot assign to this expression",
                "not a place (variable, `*p`, field or element)");
        }
        target.reset();
    }
    // In a class `init`, `self.f = v` initializes a field that holds zeroed memory, so nothing is
    // dropped.
    const bool raw = in_ctor_ && target_ast.kind == Expr::Kind::Field &&
                     target_ast.lhs != nullptr && target_ast.lhs->kind == Expr::Kind::Name &&
                     target_ast.lhs->text == "self";
    assignTo(std::move(target), value_ast, span, out, raw);
}

void Lowerer::assignTo(
    EP target, const Expr& value_ast, const Span& span, std::vector<hir::Stmt>& out, bool raw) {
    const Type want = target ? target->type : lower::unknownType();
    EP value = expr(value_ast, target ? &want : nullptr);
    if (target && value) {
        value = coerce(std::move(value), want, value_ast.span);
    }
    value = moveOut(std::move(value));
    if (!target || !value) {
        return;
    }
    if (!raw && needsDrop(target->type)) {
        lowerDropAssign(std::move(target), std::move(value), span, out);
        return;
    }
    hir::Stmt h;
    h.span = span;
    h.kind = hir::StmtKind::Assign;
    h.target = std::move(target);
    h.value = std::move(value);
    out.push_back(std::move(h));
}

/// `t op= v`  =>  `t = t op v`; a target with side effects is evaluated once through a pointer.
void Lowerer::lowerCompound(const Stmt& s, std::vector<hir::Stmt>& out) {
    if (!lower::astHasSideEffects(*s.target)) {
        if (userCompound(s, *s.target, out)) {
            return; // `operator+=` / `operator++` of a struct or class place
        }
        const ExprPtr bin = lower::astBinary(
            s.compound_op, lower::cloneAst(*s.target), lower::cloneAst(*s.expr), s.span);
        lowerAssign(*s.target, *bin, s.span, out);
        return;
    }
    EP target = expr(*s.target, nullptr);
    if (!target) {
        return;
    }
    if (target->readonly) {
        err("E0716",
            s.target->span,
            std::format("cannot assign to `const` `{}`", lower::placeRootName(*target)),
            "a `const` is read-only; copy it into a `local` to change it");
        return;
    }
    if (!target->lvalue && !lower::isUnknown(target->type)) {
        err("E0709",
            s.target->span,
            "cannot assign to this expression",
            "not a place (variable, `*p`, field or element)");
        return;
    }
    if (lower::isUnknown(target->type)) {
        return;
    }
    const Type ptr = Type::makePointer(target->type);
    const hir::LocalId id = hiddenLocal("lhs", ptr, s.span);
    const std::string name = fn_->locals[id].name;
    hir::Stmt decl;
    decl.kind = hir::StmtKind::Local;
    decl.span = s.span;
    decl.local = id;
    EP addr = mk(hir::ExprKind::AddrOf, ptr, s.span);
    addr->lhs = std::move(target);
    decl.value = std::move(addr);
    out.push_back(std::move(decl));
    bindLocal(name, id, ptr, s.span);
    const ExprPtr place =
        lower::astUnary(UnaryOp::Deref, lower::astName(name, s.target->span), s.target->span);
    if (userCompound(s, *place, out)) {
        return;
    }
    const ExprPtr bin = lower::astBinary(
        s.compound_op,
        lower::astUnary(UnaryOp::Deref, lower::astName(name, s.target->span), s.target->span),
        lower::cloneAst(*s.expr),
        s.span);
    lowerAssign(*place, *bin, s.span, out);
}

void Lowerer::lowerJump(const Stmt& s, std::vector<hir::Stmt>& out) {
    const bool is_break = s.kind == Stmt::Kind::Break;
    const char* word = is_break ? "break" : "continue";
    if (loops_.empty()) {
        err("E0709",
            s.span,
            std::format("`{}` outside of a loop", word),
            "there is no enclosing loop");
        return;
    }
    std::size_t target = loops_.size() - 1;
    if (!s.name.empty()) {
        bool found = false;
        for (std::size_t i = loops_.size(); i-- > 0;) {
            if (loops_[i].label == s.name) {
                target = i;
                found = true;
                break;
            }
        }
        if (!found) {
            err("E0709",
                s.span,
                std::format("no enclosing loop is labeled `{}`", s.name),
                "write `label: while ...` on the loop first");
            return;
        }
    }
    emitDefers(loops_[target].defer_mark, out);
    hir::Stmt h;
    h.span = s.span;
    h.kind = is_break ? hir::StmtKind::Break : hir::StmtKind::Continue;
    h.depth = static_cast<std::uint32_t>(loops_.size() - 1 - target);
    out.push_back(std::move(h));
}

void Lowerer::lowerFor(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back(); // the header scope (no defers can land here, but keeps frames aligned)
    if (s.init) {
        lowerStmt(*s.init, block.body);
    }
    hir::Stmt loop;
    loop.kind = hir::StmtKind::While;
    loop.span = s.span;
    if (s.expr) {
        loop.value = coerce(expr(*s.expr, &boolean), boolean, s.expr->span);
    } else {
        loop.value = mk(hir::ExprKind::BoolConst, boolean, s.span);
        loop.value->bool_value = true;
    }
    loops_.push_back({s.label, defers_.size()});
    lowerBlock(*s.body, loop.body, true);
    loops_.pop_back();
    if (s.step) {
        lowerStmt(*s.step, loop.step);
    }
    defers_.pop_back();
    scopes_.pop();
    if (!loop.value) {
        return;
    }
    block.body.push_back(std::move(loop));
    out.push_back(std::move(block));
}

void Lowerer::lowerForRange(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    // An untyped integer literal on either side takes its type from the other bound, so `0..n`
    // works for any integer type of `n` without a written `as`. A typed bound wins; when both are
    // literals the lower one's default type (i32, or i64 when it does not fit) is used.
    const bool lo_literal = s.expr->kind == Expr::Kind::IntLiteral;
    const bool hi_literal = s.expr2->kind == Expr::Kind::IntLiteral;
    Type hint;
    bool have_hint = false;
    if (lo_literal && !hi_literal) {
        EP probe = expr(*s.expr2, nullptr);
        if (probe && !lower::isUnknown(probe->type) && probe->type.kind == TypeKind::Int) {
            hint = probe->type;
            have_hint = true;
        }
    }
    EP lo = expr(*s.expr, have_hint ? &hint : nullptr);
    if (!lo) {
        (void)expr(*s.expr2, nullptr);
        return;
    }
    const Type t = lo->type;
    EP hi = expr(*s.expr2, &t);
    if (!lower::isUnknown(t) && t.kind != TypeKind::Int) {
        err("E0708",
            s.expr->span,
            std::format("a range needs integer bounds, found `{}`", typeToString(t)),
            "write `for i in 0..n`");
        return;
    }
    hi = hi ? coerce(std::move(hi), t, s.expr2->span) : nullptr;
    if (!hi || lower::isUnknown(t)) {
        return;
    }
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();

    const hir::LocalId var = addLocal(s.name, t, false, s.span);
    const hir::LocalId end = hiddenLocal("end", t, s.span);
    {
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = var;
        d.value = std::move(lo);
        block.body.push_back(std::move(d));
        hir::Stmt e;
        e.kind = hir::StmtKind::Local;
        e.span = s.span;
        e.local = end;
        e.value = std::move(hi);
        block.body.push_back(std::move(e));
    }
    bindLocal(s.name, var, t, s.span);

    const auto ref = [&](hir::LocalId id) {
        EP r = mk(hir::ExprKind::LocalRef, t, s.span);
        r->local = id;
        r->lvalue = true;
        return r;
    };
    hir::Stmt loop;
    loop.kind = hir::StmtKind::While;
    loop.span = s.span;
    EP cond = mk(hir::ExprKind::Binary, boolean, s.span);
    // `in a..b` runs while `i < b`; `in a..=b` runs while `i <= b`.
    cond->binary_op = s.range_inclusive ? BinaryOp::Le : BinaryOp::Lt;
    cond->lhs = ref(var);
    cond->rhs = ref(end);
    loop.value = std::move(cond);
    loops_.push_back({s.label, defers_.size()});
    lowerBlock(*s.body, loop.body, true);
    loops_.pop_back();
    {
        hir::Stmt step;
        step.kind = hir::StmtKind::Assign;
        step.span = s.span;
        step.target = ref(var);
        EP add = mk(hir::ExprKind::Binary, t, s.span);
        add->binary_op = BinaryOp::Add;
        add->lhs = ref(var);
        add->rhs = intConst(1, t, s.span);
        step.value = std::move(add);
        loop.step.push_back(std::move(step));
    }
    defers_.pop_back();
    scopes_.pop();
    block.body.push_back(std::move(loop));
    out.push_back(std::move(block));
}

/// `for x in s { }` / `for x, i in s { }`: a compile-time lowering to the index loop a caller
/// writes by hand, with no vtable and no `Iter` protocol. The iterable is an array, a slice or a
/// `str`; the element is a pointer into the storage (`*T`, so nothing is copied and a write reaches
/// the owner) and the index is a `usize`. The bound is read once. `Vec<T>` stays stdlib: it is
/// iterated once it can hand out a slice, so the compiler never names a runtime type (CLAUDE.md,
/// compiler/runtime).
void Lowerer::lowerForEach(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    const Type usize = Type::makeInt(0, false);
    // A `for x in <name>` whose iterable is a bare name that names nothing is the leftover of a
    // pack loop that was not unrolled (the pack rewrite happens before lowering); keep the old
    // `E0735`.
    if (s.expr->kind == Expr::Kind::Name && lookupName(s.expr->text) == nullptr &&
        !s_.config_.lookup(s.expr->text)) {
        err("E0735",
            s.expr->span,
            "`for ... in` needs a parameter pack",
            "only the parameter pack of a function can be iterated");
        return;
    }
    EP it = expr(*s.expr, nullptr);
    if (!it) {
        return;
    }
    if (lower::isUnknown(it->type)) {
        return;
    }
    const Type iterable = it->type;
    if (lower::isAggregate(iterable) ||
        (iterable.kind == TypeKind::Pointer && iterable.elem && lower::isAggregate(*iterable.elem))) {
        lowerIterate(s, std::move(it), out); // `next` / `iter`: the iterator protocol
        return;
    }

    const bool is_array = iterable.kind == TypeKind::Array;
    const bool is_view = iterable.kind == TypeKind::Str || iterable.kind == TypeKind::Slice;
    if (!is_array && !is_view) {
        err("E0709",
            s.expr->span,
            std::format("`{}` cannot be iterated with `for ... in`", typeToString(iterable)),
            "iterate an array, a slice or `str`; a range is `for i in a..b`");
        return;
    }
    Type elem_type = Type::makeInt(8, false); // a `str` walks bytes
    if (iterable.elem) {
        elem_type = *iterable.elem;
    }
    const Type elem_ptr = Type::makePointer(elem_type);

    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();

    const hir::LocalId idx = hiddenLocal("i", usize, s.span);
    const hir::LocalId end = hiddenLocal("n", usize, s.span);
    // An array is *not* copied: it is indexed through its address, so a write reaches the owner.
    // A slice or `str` is a two-word view, so copying it is correct and keeps the code simple.
    hir::LocalId src{};
    if (is_array) {
        src = hiddenLocal("arrp", Type::makePointer(iterable), s.span);
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(iterable), s.span);
        addr->lhs = std::move(it);
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = src;
        d.value = std::move(addr);
        block.body.push_back(std::move(d));
    } else {
        src = hiddenLocal("seq", iterable, s.span);
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = src;
        d.value = std::move(it);
        block.body.push_back(std::move(d));
    }
    const auto ref = [&](hir::LocalId id, const Type& t) {
        EP r = mk(hir::ExprKind::LocalRef, t, s.span);
        r->local = id;
        r->lvalue = true;
        return r;
    };
    // The base the index reads through: for an array it is `(*arrp)`, a place of the array type so
    // that `[i]` yields one element (not a copy of the whole array); otherwise the copied view.
    const auto baseRef = [&]() -> EP {
        if (!is_array) {
            return ref(src, iterable);
        }
        EP deref = mk(hir::ExprKind::Deref, iterable, s.span);
        deref->lhs = ref(src, Type::makePointer(iterable));
        deref->lvalue = true;
        return deref;
    };
    // `end`: a constant for an array, `len` for a view.
    {
        EP len;
        if (is_array) {
            const auto n = arrayLength(s_, iterable);
            len = intConst(n.value_or(0), usize, s.span);
        } else {
            EP field = mk(hir::ExprKind::Field, usize, s.span);
            field->text = "len";
            field->index = 1;
            field->lhs = baseRef();
            len = std::move(field);
        }
        hir::Stmt e;
        e.kind = hir::StmtKind::Local;
        e.span = s.span;
        e.local = end;
        e.value = std::move(len);
        block.body.push_back(std::move(e));
    }
    {
        hir::Stmt z;
        z.kind = hir::StmtKind::Local;
        z.span = s.span;
        z.local = idx;
        z.value = intConst(0, usize, s.span);
        block.body.push_back(std::move(z));
    }

    hir::Stmt loop;
    loop.kind = hir::StmtKind::While;
    loop.span = s.span;
    EP cond = mk(hir::ExprKind::Binary, boolean, s.span);
    cond->binary_op = BinaryOp::Lt;
    cond->lhs = ref(idx, usize);
    cond->rhs = ref(end, usize);
    loop.value = std::move(cond);

    // The body is wrapped so the element and the index names live only inside the loop: their
    // declarations come first, then the user's block, then the index step.
    scopes_.push(ScopeKind::Block);
    defers_.emplace_back();
    const hir::LocalId elem = addLocal(s.name, elem_ptr, false, s.span);
    {
        EP index = mk(hir::ExprKind::Index, elem_type, s.span);
        index->checked = s_.config_.safety == "checked";
        index->lvalue = true;
        index->lhs = baseRef();
        index->rhs = ref(idx, usize);
        // The element is its address in the storage (`*T`), so a write through it reaches the
        // owner.
        EP addr = mk(hir::ExprKind::AddrOf, elem_ptr, s.span);
        addr->lhs = std::move(index);
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = elem;
        d.value = std::move(addr);
        loop.body.push_back(std::move(d));
    }
    bindLocal(s.name, elem, elem_ptr, s.span);
    if (!s.names.empty()) {
        const hir::LocalId ivid = addLocal(s.names[0], usize, false, s.span);
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = ivid;
        d.value = ref(idx, usize);
        loop.body.push_back(std::move(d));
        bindLocal(s.names[0], ivid, usize, s.span);
    }
    loops_.push_back({s.label, defers_.size()});
    lowerBlock(*s.body, loop.body, true);
    loops_.pop_back();
    defers_.pop_back();
    scopes_.pop();
    {
        hir::Stmt step;
        step.kind = hir::StmtKind::Assign;
        step.span = s.span;
        step.target = ref(idx, usize);
        EP add = mk(hir::ExprKind::Binary, usize, s.span);
        add->binary_op = BinaryOp::Add;
        add->lhs = ref(idx, usize);
        add->rhs = intConst(1, usize, s.span);
        step.value = std::move(add);
        loop.step.push_back(std::move(step));
    }
    defers_.pop_back();
    scopes_.pop();
    block.body.push_back(std::move(loop));
    out.push_back(std::move(block));
}

void Lowerer::lowerSwitch(const Stmt& s, std::vector<hir::Stmt>& out) {
    const Type boolean = Type::makeBool();
    EP scrutinee = expr(*s.expr, nullptr);
    const auto sum = scrutinee ? sumOfType(scrutinee->type) : std::nullopt;
    if (scrutinee && sum) {
        lowerSumSwitch(s, std::move(scrutinee), out);
        return;
    }
    const bool usable = scrutinee && (scrutinee->type.kind == TypeKind::Int ||
                                      scrutinee->type.kind == TypeKind::Bool ||
                                      scrutinee->type.kind == TypeKind::Enum);
    if (scrutinee && !usable && !lower::isUnknown(scrutinee->type)) {
        err("E0708",
            s.expr->span,
            std::format("cannot `switch` on `{}`", typeToString(scrutinee->type)),
            "integers, bools and enums can be switched on; use `if` for other types");
    }
    hir::Stmt block;
    block.kind = hir::StmtKind::Block;
    block.span = s.span;
    scopes_.push(ScopeKind::Block);
    const Type st = usable ? scrutinee->type : lower::unknownType();
    const hir::LocalId id = hiddenLocal("sw", st, s.span);
    const std::string name = fn_->locals[id].name;
    if (usable) {
        hir::Stmt d;
        d.kind = hir::StmtKind::Local;
        d.span = s.span;
        d.local = id;
        d.value = std::move(scrutinee);
        block.body.push_back(std::move(d));
    }
    bindLocal(name, id, st, s.span);

    struct Arm {
        EP cond;
        std::vector<hir::Stmt> body;
    };
    std::vector<Arm> arms;
    std::vector<hir::Stmt> default_body;
    bool has_default = false;
    std::set<std::uint64_t> seen;
    for (const SwitchCase& c : s.cases) {
        Arm arm;
        if (c.is_default) {
            if (has_default) {
                err("E0709",
                    c.span,
                    "a `switch` has more than one `default`",
                    "remove the duplicate");
            }
            has_default = true;
            lowerBlock(*c.body, default_body, true);
            continue;
        }
        if (c.values.empty()) {
            err("E0709", c.span, "`case` needs at least one value", "write `case 1, 2 { ... }`");
        }
        ExprPtr cond_ast;
        for (const ExprPtr& v : c.values) {
            ExprPtr eq = lower::astBinary(
                BinaryOp::Eq, lower::astName(name, v->span), lower::cloneAst(*v), v->span);
            cond_ast =
                cond_ast
                    ? lower::astBinary(BinaryOp::Or, std::move(cond_ast), std::move(eq), v->span)
                    : std::move(eq);
            const auto cv = s_.evalConst(*v);
            if (cv && cv->kind == ConstValue::Kind::Int &&
                !seen.insert(static_cast<std::uint64_t>(cv->int_value)).second) {
                err("E0709",
                    v->span,
                    "duplicate `case` value",
                    "this value is already handled by an earlier `case`");
            }
        }
        if (cond_ast && usable) {
            arm.cond = coerce(expr(*cond_ast, &boolean), boolean, c.span);
        }
        lowerBlock(*c.body, arm.body, true);
        if (arm.cond) {
            arms.push_back(std::move(arm));
        }
    }
    scopes_.pop();

    std::vector<hir::Stmt> chain = std::move(default_body);
    for (std::size_t i = arms.size(); i-- > 0;) {
        hir::Stmt ifs;
        ifs.kind = hir::StmtKind::If;
        ifs.span = s.span;
        ifs.value = std::move(arms[i].cond);
        ifs.body = std::move(arms[i].body);
        ifs.else_body = std::move(chain);
        chain.clear();
        chain.push_back(std::move(ifs));
    }
    if (!usable) {
        return;
    }
    for (hir::Stmt& c : chain) {
        block.body.push_back(std::move(c));
    }
    out.push_back(std::move(block));
}

Lowerer::EP Lowerer::ternaryExpr(const Expr& e, const Type* expected) {
    const Type boolean = Type::makeBool();
    EP cond = coerce(expr(*e.lhs, &boolean), boolean, e.lhs->span);
    const Expr& then_ast = *e.rhs;
    const Expr& else_ast = *e.args[0];
    const auto isLit = [](const Expr& x) {
        return x.kind == Expr::Kind::IntLiteral || x.kind == Expr::Kind::FloatLiteral;
    };
    EP a;
    EP b;
    ++lazy_; // only one branch runs: a `?` in either could not be hoisted
    if (isLit(then_ast) && !isLit(else_ast)) {
        b = expr(else_ast, expected);
        const Type want = b ? b->type : lower::unknownType();
        a = expr(then_ast, b ? &want : expected);
    } else {
        a = expr(then_ast, expected);
        const Type want = a ? a->type : lower::unknownType();
        b = expr(else_ast, a ? &want : expected);
    }
    --lazy_;
    if (!cond || !a || !b) {
        return nullptr;
    }
    if (a->type.isVoid() || lower::isUnknown(a->type)) {
        err("E0708",
            e.span,
            "the branches of `?:` need a value",
            "a `void` or unknown expression cannot be selected");
        return nullptr;
    }
    const Type t = a->type;
    b = coerce(std::move(b), t, else_ast.span);
    if (!b) {
        return nullptr;
    }
    EP sel = mk(hir::ExprKind::Select, t, e.span);
    sel->lhs = std::move(cond);
    sel->rhs = std::move(a);
    sel->args.push_back(std::move(b));
    return sel;
}

/// `local (a, b) = expr`: pure sugar over `local $t = expr` plus one `local a = $t.0` per name.
/// The hidden tuple lives in the same block, so the names are visible exactly as if each had been
/// declared by a plain `local`; the field reads are ordinary `Field` accesses on a `Tuple` type.
void Lowerer::lowerTupleLocal(const Stmt& s, std::vector<hir::Stmt>& out) {
    if (!s.expr) {
        err("E0708", s.span, "a tuple target needs an initializer", "write `local (a, b) = expr`");
        return;
    }
    EP init = expr(*s.expr, nullptr);
    if (!init) {
        return;
    }
    if (lower::isUnknown(init->type)) {
        return;
    }
    if (init->type.kind != TypeKind::Tuple) {
        err("E0708",
            s.expr->span,
            std::format("cannot take `{}` apart: it is not a tuple", typeToString(init->type)),
            "a tuple target needs a tuple value on the right");
        return;
    }
    const Type tuple_type = init->type;
    if (tuple_type.args.size() != s.names.size()) {
        err("E0708",
            s.span,
            std::format("the tuple target names {} variable(s), the value has {}",
                        s.names.size(),
                        tuple_type.args.size()),
            "write one name per tuple element");
        return;
    }
    // The whole tuple is held once, unnamed, so every element is read from one evaluation of the
    // right-hand side (a call returning the tuple runs exactly once).
    const hir::LocalId tmp = hiddenLocal("tuple", tuple_type, s.span);
    hir::Stmt hold;
    hold.kind = hir::StmtKind::Local;
    hold.span = s.span;
    hold.local = tmp;
    hold.value = std::move(init);
    out.push_back(std::move(hold));

    // Bind the names only after the value is computed: `local (a, b) = f(a)` reads the outer `a`.
    for (std::size_t i = 0; i < s.names.size(); ++i) {
        EP field = mk(hir::ExprKind::Field, tuple_type.args[i], s.span);
        field->index = static_cast<std::uint32_t>(i);
        field->lvalue = true;
        field->lhs = localRef(tmp, tuple_type, s.span);

        const Type t = tuple_type.args[i];
        hir::Stmt decl;
        decl.kind = hir::StmtKind::Local;
        decl.span = s.span;
        decl.local = addLocal(s.names[i], t, false, s.span);
        rejectDroppablePlace(t, s.span, "a local");
        if (needsDrop(t)) {
            registerDrop(decl.local, t, true, s.span, out);
        }
        decl.value = std::move(field);
        out.push_back(std::move(decl));
        bindLocal(s.names[i], out.back().local, t, s.span);
    }
}

void Lowerer::lowerStmt(const Stmt& s, std::vector<hir::Stmt>& out) {
    // The statements of the kinds below evaluate their expressions once, in order, before they
    // act: a `?` in them is hoisted in front. A loop condition runs every pass and a `defer`
    // body later, so there `?` is refused.
    using K = Stmt::Kind;
    const bool hoists = std::ranges::contains(
        std::array{K::Local, K::Assign, K::Return, K::Expr, K::If, K::Switch, K::Delete}, s.kind);
    std::vector<hir::Stmt> prelude;
    std::vector<hir::Stmt> body;
    const auto saved = std::exchange(prelude_, hoists ? &prelude : nullptr);
    lowerStmtBody(s, body);
    prelude_ = saved;
    std::ranges::move(prelude, std::back_inserter(out));
    std::ranges::move(body, std::back_inserter(out));
}

void Lowerer::lowerStmtBody(const Stmt& s, std::vector<hir::Stmt>& out) {
    hir::Stmt h;
    h.span = s.span;
    const Type boolean = Type::makeBool();
    switch (s.kind) {
    case Stmt::Kind::Local: {
        if (!s.names.empty()) {
            lowerTupleLocal(s, out);
            return;
        }
        Type t;
        EP init;
        if (s.has_type) {
            t = resolved(s.type);
            if (s.expr) {
                init = coerce(expr(*s.expr, &t), t, s.expr->span);
            }
        } else if (s.expr) {
            init = expr(*s.expr, nullptr);
            t = init ? init->type : lower::unknownType();
            if (init && t.isVoid()) {
                err("E0708",
                    s.expr->span,
                    std::format("`{}` cannot be bound to a `void` value", s.name),
                    "the expression has no value");
                init.reset();
                t = lower::unknownType();
            }
        } else {
            err("E0708",
                s.span,
                std::format("cannot infer the type of `{}`", s.name),
                "write a type (`local x: T`) or an initial value");
            t = lower::unknownType();
        }
        init = moveOut(std::move(init));
        h.kind = hir::StmtKind::Local;
        h.local = addLocal(s.name, t, false, s.span);
        rejectDroppablePlace(t, s.span, "a local");
        if (needsDrop(t)) {
            registerDrop(h.local, t, init != nullptr, s.span, out);
        }
        h.value = std::move(init);
        bindLocal(
            s.name, h.local, t, s.span); // after the initializer: `local x = x` sees the outer `x`
        break;
    }
    case Stmt::Kind::Const:
        lowerLocalConst(s);
        return;
    case Stmt::Kind::Delete: {
        // Drop the object (when its class needs drop), then free it through the allocator.
        EP p = expr(*s.expr, nullptr);
        if (!p) {
            return;
        }
        if (p->type.kind != TypeKind::Pointer || !p->type.elem ||
            !lower::isAggregate(*p->type.elem)) {
            err("E0709",
                s.expr->span,
                std::format("`delete` needs a pointer to a struct or class, found `{}`",
                            typeToString(p->type)),
                "only `new` objects can be deleted");
            return;
        }
        const Type t = *p->type.elem;
        const Type ptr = p->type;
        const hir::LocalId tmp = hiddenLocal("del", ptr, s.span);
        hir::Stmt store;
        store.kind = hir::StmtKind::Local;
        store.span = s.span;
        store.local = tmp;
        store.value = std::move(p);
        out.push_back(std::move(store));
        if (needsDrop(t)) {
            hir::Stmt drop;
            drop.kind = hir::StmtKind::Expr;
            drop.span = s.span;
            drop.value = dropCallOn(localRef(tmp, ptr, s.span), t, s.span);
            out.push_back(std::move(drop));
        }
        if (mod_.free_function.empty()) {
            err("E0501",
                s.span,
                "`delete` has no registered `free` function",
                "register one with `register(free) fn name(p: *void)`");
            return;
        }
        EP call = mk(hir::ExprKind::Call, Type::makeVoid(), s.span);
        call->text = mod_.free_function;
        EP raw = mk(hir::ExprKind::Cast, Type::makePointer(Type::makeVoid()), s.span);
        raw->lhs = localRef(tmp, ptr, s.span);
        call->args.push_back(std::move(raw));
        hir::Stmt free_stmt;
        free_stmt.kind = hir::StmtKind::Expr;
        free_stmt.span = s.span;
        free_stmt.value = std::move(call);
        out.push_back(std::move(free_stmt));
        return;
    }
    case Stmt::Kind::Assign:
        if (s.has_compound_op) {
            lowerCompound(s, out);
        } else {
            lowerAssign(*s.target, *s.expr, s.span, out);
        }
        return;
    case Stmt::Kind::Return: {
        h.kind = hir::StmtKind::Return;
        if (s.expr && inferring_ && lower::isUnknown(ret_)) {
            // A closure without `->` and without a `fn(...)` context: the first `return` gives
            // it its return type (a void value is only run).
            EP v = expr(*s.expr, nullptr);
            if (!v) {
                return;
            }
            ret_ = v->type;
            if (ret_.isVoid()) {
                hir::Stmt run;
                run.kind = hir::StmtKind::Expr;
                run.span = s.span;
                run.value = std::move(v);
                out.push_back(std::move(run));
            } else {
                h.value = moveOut(std::move(v));
            }
        } else if (s.expr) {
            if (ret_.isVoid()) {
                err("E0708",
                    s.expr->span,
                    "a `void` function cannot return a value",
                    "remove the value or add `-> T`");
                (void)expr(*s.expr, nullptr);
            } else {
                h.value = moveOut(coerce(expr(*s.expr, &ret_), ret_, s.expr->span));
                if (!h.value) {
                    return;
                }
                (void)rejectStackInterface(*h.value); // reported; lowering goes on (no W0701 noise)
            }
        } else if (!ret_.isVoid() && !lower::isUnknown(ret_)) {
            err("E0708",
                s.span,
                std::format("`return` needs a value of type `{}`", typeToString(ret_)),
                "missing value");
        }
        const bool pending = std::ranges::any_of(defers_, [](const auto& f) { return !f.empty(); });
        if (pending) {
            // The returned value is computed before the deferred statements run.
            if (h.value) {
                const Type vt = h.value->type;
                const hir::LocalId id = hiddenLocal("ret", vt, s.span);
                hir::Stmt keep;
                keep.kind = hir::StmtKind::Local;
                keep.span = s.span;
                keep.local = id;
                keep.value = std::move(h.value);
                out.push_back(std::move(keep));
                EP ref = mk(hir::ExprKind::LocalRef, vt, s.span);
                ref->local = id;
                ref->lvalue = true;
                h.value = std::move(ref);
            }
            emitDefers(0, out);
        }
        break;
    }
    case Stmt::Kind::DoWhile: {
        h.kind = hir::StmtKind::While;
        h.post_test = true;
        loops_.push_back({s.label, defers_.size()});
        lowerBlock(*s.body, h.body, true);
        loops_.pop_back();
        h.value = coerce(expr(*s.expr, &boolean), boolean, s.expr->span);
        if (!h.value) {
            return;
        }
        break;
    }
    case Stmt::Kind::For:
        lowerFor(s, out);
        return;
    case Stmt::Kind::ForRange:
        lowerForRange(s, out);
        return;
    case Stmt::Kind::ForPack:
        // A pack loop is unrolled when its pack function is instantiated; reaching it here means
        // it is outside such a function (the checker reports the same uses in pack functions).
        err("E0735",
            s.expr->span,
            "`for ... in` needs a parameter pack",
            "only the parameter pack of a function can be iterated");
        return;
    case Stmt::Kind::ForEach:
        lowerForEach(s, out);
        return;
    case Stmt::Kind::Switch:
        lowerSwitch(s, out);
        return;
    case Stmt::Kind::Defer: {
        const Stmt& inner = *s.step;
        switch (inner.kind) {
        case Stmt::Kind::Return:
        case Stmt::Kind::Break:
        case Stmt::Kind::Continue:
        case Stmt::Kind::Defer:
        case Stmt::Kind::Local:
        case Stmt::Kind::Const:
            err("E0709",
                inner.span,
                "this statement cannot be deferred",
                "a `defer` runs when the block is left; it cannot return, jump, declare a variable "
                "or defer again");
            return;
        default:
            break;
        }
        std::vector<hir::Stmt> lowered;
        ++lazy_; // a deferred body runs at block exit: a `?` in it has no place to hoist to
        lowerStmt(inner, lowered);
        --lazy_;
        // The deferred statement runs under a flag set here, so a panic unwinding through the
        // function runs exactly the defers that were registered and not yet run.
        const hir::LocalId flag = hiddenLocal("dfr", Type::makeBool(), s.span);
        entry_flags_.push_back(flag);
        hir::Stmt arm;
        arm.kind = hir::StmtKind::Assign;
        arm.span = s.span;
        arm.target = localRef(flag, Type::makeBool(), s.span);
        arm.value = boolConst(true, s.span);
        out.push_back(std::move(arm));
        hir::Stmt guard;
        guard.kind = hir::StmtKind::If;
        guard.span = s.span;
        guard.value = localRef(flag, Type::makeBool(), s.span);
        hir::Stmt clear;
        clear.kind = hir::StmtKind::Assign;
        clear.span = s.span;
        clear.target = localRef(flag, Type::makeBool(), s.span);
        clear.value = boolConst(false, s.span);
        guard.body.push_back(std::move(clear));
        for (hir::Stmt& d : lowered) {
            guard.body.push_back(std::move(d));
        }
        unwind_entries_.push_back(lower::cloneHirStmt(guard));
        defers_.back().push_back(std::move(guard));
        return;
    }
    case Stmt::Kind::Expr: {
        if (s.expr->kind == Expr::Kind::Call && s.expr->lhs->kind == Expr::Kind::Name &&
            s.expr->lhs->text == "staticAssert" && lookupName("staticAssert") == nullptr) {
            s_.checkStaticAssertCall(*s.expr);
            return;
        }
        if (s.expr->kind == Expr::Kind::Call && s.expr->lhs->kind == Expr::Kind::Name &&
            s.expr->lhs->text == "__builtin_write") {
            lowerRawWrite(*s.expr, s.span, out);
            return;
        }
        EP v = expr(*s.expr, nullptr);
        if (!v) {
            return;
        }
        if (needsDrop(v->type)) {
            // A discarded droppable value is dropped right after the statement.
            const Type t = v->type;
            const hir::LocalId tmp = hiddenLocal("disc", t, s.span);
            hir::Stmt store;
            store.kind = hir::StmtKind::Local;
            store.span = s.span;
            store.local = tmp;
            store.value = std::move(v);
            out.push_back(std::move(store));
            EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(t), s.span);
            addr->lhs = localRef(tmp, t, s.span);
            v = dropCallOn(std::move(addr), t, s.span);
        }
        h.kind = hir::StmtKind::Expr;
        h.value = std::move(v);
        break;
    }
    case Stmt::Kind::If: {
        h.kind = hir::StmtKind::If;
        h.value = coerce(expr(*s.expr, &boolean), boolean, s.expr->span);
        lowerBlock(*s.body, h.body, true);
        if (s.else_body) {
            lowerBlock(*s.else_body, h.else_body, true);
        }
        if (!h.value) {
            return;
        }
        break;
    }
    case Stmt::Kind::While: {
        h.kind = hir::StmtKind::While;
        h.value = coerce(expr(*s.expr, &boolean), boolean, s.expr->span);
        loops_.push_back({s.label, defers_.size()});
        lowerBlock(*s.body, h.body, true);
        loops_.pop_back();
        if (!h.value) {
            return;
        }
        break;
    }
    case Stmt::Kind::Continue:
    case Stmt::Kind::Break:
        lowerJump(s, out);
        return;
    case Stmt::Kind::Try:
        lowerTry(s, out);
        return;
    case Stmt::Kind::Unsafe:
        ++unsafe_;
        h.kind = hir::StmtKind::Block;
        lowerBlock(*s.body, h.body, true);
        --unsafe_;
        break;
    case Stmt::Kind::Block:
        h.kind = hir::StmtKind::Block;
        lowerBlock(*s.body, h.body, true);
        break;
    }
    out.push_back(std::move(h));
}

} // namespace xlang::lower
