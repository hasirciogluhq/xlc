// Lowering: `syscall`, the `__builtin_` functions and inline `asm`.

#include "sema/lower/lower.hpp"

namespace xlang::lower {

/// @grammar `syscall(nr, args...)`, returning `i64`: a negative result is `-errno`, so the caller checks the sign rather than a separate error out.
Lowerer::EP Lowerer::syscallExpr(const Expr& e) {
    if (e.args.empty()) {
        err("E0710", e.span, "`syscall` needs the syscall number", "wrong number of arguments");
        return nullptr;
    }
    const auto limit = lower::syscallMaxArgs(s_.config_.target);
    if (limit && e.args.size() - 1 > *limit) {
        err("E0710",
            e.span,
            std::format("`syscall` has {} argument(s) after the number, `{}` allows at most {}",
                        e.args.size() - 1,
                        s_.config_.target.triple.empty()
                            ? s_.config_.target.os + "/" + s_.config_.target.arch
                            : s_.config_.target.triple,
                        *limit),
            "too many arguments for this target");
        return nullptr;
    }
    EP out = mk(hir::ExprKind::Syscall, Type::makeInt(64, true), e.span);
    bool ok = true;
    for (const ExprPtr& a : e.args) {
        EP v = expr(*a, nullptr);
        if (!v) {
            ok = false;
            continue;
        }
        if (!lower::isUnknown(v->type) && v->type.kind != TypeKind::Int &&
            v->type.kind != TypeKind::Pointer) {
            err("E0708",
                a->span,
                std::format("`syscall` arguments are integers or pointers, found `{}`",
                            typeToString(v->type)),
                "integer or `*T` expected");
            ok = false;
        }
        out->args.push_back(std::move(v));
    }
    if (!ok) {
        return nullptr;
    }
    return out;
}

/// A compile-time `MemoryOrder` member (`MemoryOrder.seqCst`): 0 relaxed .. 4 seqCst.
std::optional<std::uint32_t> Lowerer::memoryOrder(const Expr& arg) {
    EP v = expr(arg, nullptr);
    if (!v) {
        return std::nullopt;
    }
    if (v->kind != hir::ExprKind::IntConst || v->type.kind != TypeKind::Enum ||
        v->type.path.empty() || v->type.path.front() != "MemoryOrder") {
        err("E0708",
            arg.span,
            "the memory order must be a `MemoryOrder` member",
            "write `MemoryOrder.seqCst` (a compile-time constant)");
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(v->int_value);
}

/// @grammar `__builtin_atomicLoad(p, order)`, `__builtin_atomicStore(p, v, order)`, `__builtin_atomicXchg(p, v, order)`, `__builtin_atomicCas(p, expected, new, order)`, `__builtin_atomicAdd(p, v, order)`.
/// @grammar `__builtin_read(p)`, `__builtin_write(p, v)`, `__builtin_drop(p)`, `__builtin_needsDrop<T>()`, `__builtin_fence(order)`, `__builtin_unwindPads()`, `__builtin_unwindPadCount()`, `__builtin_asm(...)`.
/// @invariant the `__builtin_` set is closed: a name not listed here is not a builtin, and the compiler never learns a new one from the standard library.
/// @invariant `__builtin_read` copies the value out of the pointer and drops nothing, so two reads of one pointer yield two owners of the same bytes and only the caller's discipline keeps that sound.
/// @invariant `__builtin_write` is a statement, never a value, and moves the value through the pointer without running a destructor on the old bytes.
Lowerer::EP Lowerer::builtinCall(const Expr& e, const std::string& name) {
    const bool is_load = name == "__builtin_atomicLoad";
    const bool is_store = name == "__builtin_atomicStore";
    const bool is_xchg = name == "__builtin_atomicXchg";
    const bool is_cas = name == "__builtin_atomicCas";
    const bool is_add = name == "__builtin_atomicAdd";
    if (name == "__builtin_asm") {
        return asmCall(e);
    }
    if (name == "__builtin_write") {
        err("E0709",
            e.span,
            "`__builtin_write` is a statement, not a value",
            "call it as a statement: `__builtin_write(p, v)`");
        return nullptr;
    }
    if (name == "__builtin_read") {
        // `__builtin_read(p)`: the value at `p`, copied out. Nothing is dropped or moved; the
        // caller owns the copy.
        if (e.args.size() != 1) {
            err("E0710", e.span, "`__builtin_read` takes one pointer", "wrong number of arguments");
            return nullptr;
        }
        EP p = expr(*e.args[0], nullptr);
        if (!p) {
            return nullptr;
        }
        if (p->type.kind != TypeKind::Pointer || !p->type.elem) {
            err("E0709",
                e.args[0]->span,
                std::format("`__builtin_read` needs a pointer, found `{}`", typeToString(p->type)),
                "pass `&x`");
            return nullptr;
        }
        if (p->type.elem->isVoid()) {
            err("E0709",
                e.args[0]->span,
                "`__builtin_read` cannot read a `void`",
                "pass a pointer to a typed value");
            return nullptr;
        }
        EP r = mk(hir::ExprKind::RawRead, *p->type.elem, e.span);
        r->lhs = std::move(p);
        return r;
    }
    if (name == "__builtin_needsDrop") {
        // `__builtin_needsDrop<T>()`: a compile-time bool.
        static const std::vector<Type> none;
        const auto& written = e.lhs ? e.lhs->generic_args : none;
        if (written.size() != 1 || !e.args.empty()) {
            err("E0710",
                e.span,
                "`__builtin_needsDrop` takes one type: `__builtin_needsDrop<T>()`",
                "wrong number of arguments");
            return nullptr;
        }
        return boolConst(needsDrop(resolved(written[0])), e.span);
    }
    if (name == "__builtin_drop") {
        // `__builtin_drop(p)`: runs the drop of the pointee of `p` (nothing when it needs none).
        if (e.args.size() != 1) {
            err("E0710", e.span, "`__builtin_drop` takes one pointer", "wrong number of arguments");
            return nullptr;
        }
        EP p = expr(*e.args[0], nullptr);
        if (!p) {
            return nullptr;
        }
        if (p->type.kind != TypeKind::Pointer || !p->type.elem) {
            err("E0709",
                e.args[0]->span,
                std::format("`__builtin_drop` needs a pointer, found `{}`", typeToString(p->type)),
                "pass `&x`");
            return nullptr;
        }
        const Type t = *p->type.elem;
        if (!dropsThroughPointer(t)) {
            EP nop = mk(hir::ExprKind::Asm, Type::makeVoid(), e.span);
            return nop;
        }
        return dropThrough(std::move(p), t, e.span);
    }
    if (name == "__builtin_unwindPads" || name == "__builtin_unwindPadCount") {
        if (!e.args.empty()) {
            err("E0710",
                e.span,
                std::format("`{}` takes no arguments", name),
                "wrong number of arguments");
            return nullptr;
        }
        const bool pads = name == "__builtin_unwindPads";
        return mk(pads ? hir::ExprKind::UnwindPads : hir::ExprKind::UnwindPadCount,
                  pads ? Type::makePointer(Type::makeInt(0, false)) : Type::makeInt(0, false),
                  e.span);
    }
    if (name == "__builtin_fence") {
        if (e.args.size() != 1) {
            err("E0710",
                e.span,
                "`__builtin_fence` takes the memory order",
                "wrong number of arguments");
            return nullptr;
        }
        const auto order = memoryOrder(*e.args[0]);
        if (!order) {
            return nullptr;
        }
        if (*order == 0) {
            err("E0708",
                e.args[0]->span,
                "a fence cannot be `relaxed`",
                "use acquire, release, acqRel or seqCst");
            return nullptr;
        }
        EP f = mk(hir::ExprKind::Fence, Type::makeVoid(), e.span);
        f->index = *order;
        return f;
    }
    if (!(is_load || is_store || is_xchg || is_cas || is_add)) {
        err("E0706",
            e.lhs->span,
            std::format("unknown builtin `{}`", name),
            "not in the closed builtin list");
        return nullptr;
    }
    const std::size_t want_args = is_load ? 2 : (is_cas ? 4 : 3);
    if (e.args.size() != want_args) {
        err("E0710",
            e.span,
            std::format("`{}` takes {} arguments, {} given", name, want_args, e.args.size()),
            "wrong number of arguments");
        for (const ExprPtr& a : e.args) {
            (void)expr(*a, nullptr);
        }
        return nullptr;
    }
    EP ptr = expr(*e.args[0], nullptr);
    if (!ptr) {
        return nullptr;
    }
    if (lower::isUnknown(ptr->type)) {
        return mk(hir::ExprKind::AtomicLoad, lower::unknownType(), e.span);
    }
    if (ptr->type.kind != TypeKind::Pointer || !ptr->type.elem) {
        err("E0708",
            e.args[0]->span,
            std::format("atomics take a pointer, found `{}`", typeToString(ptr->type)),
            "pointer expected");
        return nullptr;
    }
    const Type elem = *ptr->type.elem;
    const bool ok_type =
        elem.kind == TypeKind::Int || elem.kind == TypeKind::Bool || elem.kind == TypeKind::Pointer;
    if (!ok_type) {
        err("E0708",
            e.args[0]->span,
            std::format("atomics work on integers, `bool` and pointers, not `{}`",
                        typeToString(elem)),
            "unsupported atomic type");
        return nullptr;
    }
    if (is_add && elem.kind != TypeKind::Int) {
        err("E0708", e.args[0]->span, "`__builtin_atomicAdd` needs an integer", "not an integer");
        return nullptr;
    }
    const auto order = memoryOrder(*e.args[want_args - 1]);
    if (!order) {
        return nullptr;
    }
    if (is_load && (*order == 2 || *order == 3)) {
        err("E0708",
            e.args[want_args - 1]->span,
            "a load cannot be `release` or `acqRel`",
            "use relaxed, acquire or seqCst");
        return nullptr;
    }
    if (is_store && (*order == 1 || *order == 3)) {
        err("E0708",
            e.args[want_args - 1]->span,
            "a store cannot be `acquire` or `acqRel`",
            "use relaxed, release or seqCst");
        return nullptr;
    }
    const auto valueArg = [&](std::size_t i) {
        return coerce(expr(*e.args[i], &elem), elem, e.args[i]->span);
    };
    hir::ExprKind kind = hir::ExprKind::AtomicLoad;
    Type result = elem;
    if (is_store) {
        kind = hir::ExprKind::AtomicStore;
        result = Type::makeVoid();
    } else if (is_xchg) {
        kind = hir::ExprKind::AtomicXchg;
    } else if (is_cas) {
        kind = hir::ExprKind::AtomicCas;
    } else if (is_add) {
        kind = hir::ExprKind::AtomicAdd;
    }
    EP out = mk(kind, result, e.span);
    out->index = *order;
    out->lhs = std::move(ptr);
    if (is_store || is_xchg || is_add) {
        out->rhs = valueArg(1);
        if (!out->rhs) {
            return nullptr;
        }
    } else if (is_cas) {
        out->rhs = valueArg(1);
        EP desired = valueArg(2);
        if (!out->rhs || !desired) {
            return nullptr;
        }
        out->args.push_back(std::move(desired));
    }
    return out;
}

/// @grammar `__builtin_asm("template", in(reg) x, out("rax") y, inout(reg) z, clobber("memory"), options(nomem|noreturn))`, with `{k}` placeholders numbered by operand order.
Lowerer::EP Lowerer::asmCall(const Expr& e) {
    if (e.args.empty() || e.args[0]->kind != Expr::Kind::StringLiteral) {
        err("E0709",
            e.span,
            "`__builtin_asm` needs a template string first",
            "write `__builtin_asm(\"nop\")`");
        return nullptr;
    }
    EP out = mk(hir::ExprKind::Asm, Type::makeVoid(), e.span);
    out->text = e.args[0]->text;
    bool ok = true;
    for (std::size_t i = 1; i < e.args.size(); ++i) {
        const Expr& a = *e.args[i];
        if (a.kind == Expr::Kind::AsmOperand) {
            hir::AsmOperand op;
            op.kind = a.text == "in"    ? hir::AsmOperand::Kind::In
                      : a.text == "out" ? hir::AsmOperand::Kind::Out
                                        : hir::AsmOperand::Kind::InOut;
            if (a.lhs->kind == Expr::Kind::StringLiteral) {
                op.reg = a.lhs->text;
            } else if (a.lhs->kind == Expr::Kind::Name && a.lhs->text == "reg") {
                op.reg = "reg";
            } else {
                err("E0709",
                    a.lhs->span,
                    "register must be `reg` or a register name string",
                    "invalid register");
                ok = false;
                continue;
            }
            EP v = expr(*a.rhs, nullptr);
            if (!v) {
                ok = false;
                continue;
            }
            if (!lower::isUnknown(v->type) && v->type.kind != TypeKind::Int &&
                v->type.kind != TypeKind::Pointer && v->type.kind != TypeKind::Float &&
                v->type.kind != TypeKind::FnPtr) {
                err("E0708",
                    a.rhs->span,
                    std::format("asm operands are scalars, found `{}`", typeToString(v->type)),
                    "integer, float or pointer expected");
                ok = false;
                continue;
            }
            if (op.kind != hir::AsmOperand::Kind::In && !v->lvalue) {
                err("E0709",
                    a.rhs->span,
                    "an asm output needs a place",
                    "write a variable or `*p`");
                ok = false;
                continue;
            }
            op.value = std::move(v);
            out->asm_operands.push_back(std::move(op));
        } else if (a.kind == Expr::Kind::Call && a.lhs->kind == Expr::Kind::Name &&
                   (a.lhs->text == "clobber" || a.lhs->text == "options")) {
            for (const ExprPtr& arg : a.args) {
                if (arg->kind != Expr::Kind::StringLiteral) {
                    err("E0709",
                        arg->span,
                        std::format("`{}` takes strings", a.lhs->text),
                        "string expected");
                    ok = false;
                    continue;
                }
                if (a.lhs->text == "clobber") {
                    out->clobbers.push_back(arg->text);
                } else if (arg->text == "nomem") {
                    out->nomem = true;
                } else if (arg->text == "noreturn") {
                    out->noreturn = true;
                } else {
                    err("E0709",
                        arg->span,
                        std::format("unknown asm option `{}`", arg->text),
                        "known options: nomem, noreturn");
                    ok = false;
                }
            }
        } else {
            err("E0709",
                a.span,
                "unexpected `__builtin_asm` argument",
                "operands are `in(..)`, `out(..)`, `inout(..)`, `clobber(..)`, `options(..)`");
            ok = false;
        }
    }
    if (!ok) {
        return nullptr;
    }
    return out;
}

} // namespace xlang::lower
