// Lowering: which types need drop, and the drop glue of classes and payload enums.

#include "sema/lower/lower.hpp"

#include <array>
#include <utility>

namespace xlang::lower {

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

/// @invariant `needsDrop` is memoized with a `false` written before the recursion, so a type that owns itself through a pointer terminates instead of recursing forever.
bool Lowerer::needsDrop(const Type& t) {
    if (t.kind == TypeKind::Tuple) {
        // A tuple owns its elements, like a struct owns its fields.
        return std::ranges::any_of(t.args, [&](const Type& e) { return needsDrop(e); });
    }
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

bool Lowerer::dropsVirtually(const Type& t) {
    if (t.kind != TypeKind::Class || t.path.empty()) {
        return false;
    }
    const auto& slots = vtableOf(t.path.front());
    return !slots.empty() && slots.front().slot.method == kDropSlot;
}

bool Lowerer::dropsThroughPointer(const Type& t) {
    return needsDrop(t) || dropsVirtually(t) || !derivedDropLostThrough(t).empty();
}

/// @invariant a class dropped through a pointer to its base loses the derived destructor unless the base has a virtual drop slot; this returns the name of the lost one and is empty when nothing is lost.
std::string Lowerer::derivedDropLostThrough(const Type& t) {
    if (t.kind != TypeKind::Class || t.path.empty() || dropsVirtually(t)) {
        return {};
    }
    const std::string& target = t.path.front();
    for (Sema* m : sh_.modules) {
        for (const Item& item : m->program().items) {
            const auto* cl = std::get_if<ClassDecl>(&item.decl);
            if (cl == nullptr || !cl->has_base || !cl->generic_params.empty()) {
                continue;
            }
            // Its own drop (a `drop` method or a field that needs drop) is what a static drop
            // through `*t` would skip.
            const bool own = std::ranges::any_of(cl->methods,
                                                 [](const FnDecl& f) { return f.name == "drop"; }) ||
                             std::ranges::any_of(cl->fields, [&](const FieldDecl& f) {
                                 return needsDrop(resolvedIn(*m, f.type));
                             });
            if (!own) {
                continue;
            }
            // Walk the base chain (a cycle is reported elsewhere; the depth bound ends it here).
            std::optional<Type> base = m->resolve(Type::makeNamed(cl->base));
            for (int depth = 0; base && base->kind == TypeKind::Class && depth < 64; ++depth) {
                if (base->path.front() == target) {
                    return m->qualify(cl->name);
                }
                const auto ref = s_.entityByQName(base->path.front());
                const auto* bd = ref ? std::get_if<ClassDecl>(&ref->sym->item->decl) : nullptr;
                if (bd == nullptr || !bd->has_base) {
                    break;
                }
                base = ref->owner->resolve(Type::makeNamed(bd->base));
            }
        }
    }
    return {};
}

Lowerer::EP Lowerer::dropThrough(EP ptr, const Type& t, const Span& span) {
    if (const std::string lost = derivedDropLostThrough(t); !lost.empty()) {
        err("E0766",
            span,
            std::format("dropping through `*{}` cannot run the drop of `{}`", typeToString(t), lost),
            std::format("make a method of `{}` `virtual` (it then drops through its vtable), or "
                        "drop through `*{}`",
                        typeToString(t),
                        lost));
    }
    if (!dropsVirtually(t)) {
        return dropCallOn(std::move(ptr), t, span);
    }
    // `vtable[0](p)`: the glue named here only gives the call its type.
    EP call = mk(hir::ExprKind::CallVirtual, Type::makeVoid(), span);
    call->text = dropGlueName(t.path.front());
    call->index = 0;
    call->args.push_back(std::move(ptr));
    return call;
}

void Lowerer::emitDropGlue(const std::string& qname, const ClassDecl& c, const Span& span) {
    Type cls = Type::makeNamed({qname});
    cls.kind = TypeKind::Class;
    // A polymorphic class always has glue (maybe empty): its vtable slot must name a function.
    if (!needsDrop(cls) && !dropsVirtually(cls)) {
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

/// `$tuple<mangled>$dropGlue(p: *(A, B, C)) { drop(&p.2) drop(&p.1) drop(&p.0) }`, skipping the
/// elements that need no drop. Every module may use the same tuple type, so the glue is internal
/// and built once (a static library and the program each keep their own copy).
std::string Lowerer::tupleDropGlue(const Type& tuple, const Span& span) {
    std::string name = "$tuple" + mangleType(tuple) + "$dropGlue";
    if (!sh_.tuple_glues.insert(name).second) {
        return name;
    }
    const Type ptr = Type::makePointer(tuple);
    hir::Function g;
    g.name = name;
    g.symbol = name;
    g.module = s_.key_;
    g.has_body = true;
    g.span = span;
    g.ret = Type::makeVoid();
    g.locals.push_back(hir::Local{"p", ptr, true, span});
    g.param_count = 1;
    for (std::size_t i = tuple.args.size(); i-- > 0;) {
        const Type& et = tuple.args[i];
        if (!needsDrop(et)) {
            continue;
        }
        EP field = mk(hir::ExprKind::Field, et, span);
        field->text = std::to_string(i);
        field->index = static_cast<std::uint32_t>(i);
        field->through_pointer = true;
        field->lvalue = true;
        field->lhs = localRef(0, ptr, span);
        EP addr = mk(hir::ExprKind::AddrOf, Type::makePointer(et), span);
        addr->lhs = std::move(field);
        hir::Stmt s;
        s.kind = hir::StmtKind::Expr;
        s.span = span;
        s.value = dropCallOn(std::move(addr), et, span);
        g.body.push_back(std::move(s));
    }
    mod_.functions.push_back(std::move(g));
    return name;
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

} // namespace xlang::lower
