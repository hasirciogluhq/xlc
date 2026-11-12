#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

// Closures: `|a: i32, b| -> i32 { return a + b }`, `|x| x * 2`, `move |x| x + n`.
//
// A closure becomes a function `__closureN` taking the closure's parameters:
//
//   - A closure that captures nothing is that function: its value is a `fn(A, B) -> R` pointer,
//     so it goes wherever a function pointer goes.
//   - A closure that uses variables of the enclosing function also gets a struct `__closureN`
//     with one field per captured variable, and the function takes a pointer to it first
//     (`__closureN$call(__env: *__closureN, ...)`). The value is the struct, built where the
//     closure is written. A call `f(x)` of a struct is `operator()(&f, x)`, so a closure is called
//     like a function, and a generic `fn apply<F>(f: F, x: i32)` takes any closure.
//     A variable is borrowed (the field is a pointer to it, so the closure sees and makes
//     changes), or copied with `move`. A borrowing closure must not outlive the function that
//     made it; the compiler does not check that.
//
// Parameter and return types are written, or come from the `fn(...)` type the closure is used
// for, or (return type only) from the first `return` of the body.
//
// The closure's function is lowered at once, in the middle of its parent's body, so its return
// type is known to the next expression. The parent's function-level state is set aside for that
// (`FnFrame`). The body is a copy of the written one in which every captured name is rewritten to
// `__env.name` (`*__env.name` when borrowed).

struct Lowerer::FnFrame {
    Lowerer& l;
    hir::Function* fn;
    bool in_ctor;
    Type ret;
    int unsafe;
    std::vector<LoopCtx> loops;
    std::vector<std::vector<hir::Stmt>> defers;
    std::vector<std::pair<hir::LocalId, hir::LocalId>> entry_temps;
    std::vector<hir::LocalId> entry_flags;
    std::vector<hir::Stmt> unwind_entries;
    bool has_try;
    std::vector<hir::Stmt>* prelude;
    int in_defer;
    std::map<hir::LocalId, hir::LocalId> drop_flags;
    std::string base_init_call;
    bool inferring;
    ScopeStack scopes;

    explicit FnFrame(Lowerer& self)
        : l(self),
          fn(std::exchange(self.fn_, nullptr)),
          in_ctor(std::exchange(self.in_ctor_, false)),
          ret(std::move(self.ret_)),
          unsafe(std::exchange(self.unsafe_, 0)),
          loops(std::exchange(self.loops_, {})),
          defers(std::exchange(self.defers_, {})),
          entry_temps(std::exchange(self.entry_temps_, {})),
          entry_flags(std::exchange(self.entry_flags_, {})),
          unwind_entries(std::exchange(self.unwind_entries_, {})),
          has_try(std::exchange(self.has_try_, false)),
          prelude(std::exchange(self.prelude_, nullptr)),
          in_defer(std::exchange(self.in_defer_, 0)),
          drop_flags(std::exchange(self.drop_flags_, {})),
          base_init_call(std::exchange(self.base_init_call_, {})),
          inferring(std::exchange(self.inferring_, false)),
          scopes(std::move(self.scopes_)) {
        self.scopes_ = scopes.moduleOnly(); // the closure sees module names, not the parent's locals
    }
    FnFrame(const FnFrame&) = delete;
    FnFrame& operator=(const FnFrame&) = delete;

    ~FnFrame() {
        l.fn_ = fn;
        l.in_ctor_ = in_ctor;
        l.ret_ = std::move(ret);
        l.unsafe_ = unsafe;
        l.loops_ = std::move(loops);
        l.defers_ = std::move(defers);
        l.entry_temps_ = std::move(entry_temps);
        l.entry_flags_ = std::move(entry_flags);
        l.unwind_entries_ = std::move(unwind_entries);
        l.has_try_ = has_try;
        l.prelude_ = prelude;
        l.in_defer_ = in_defer;
        l.drop_flags_ = std::move(drop_flags);
        l.base_init_call_ = std::move(base_init_call);
        l.inferring_ = inferring;
        l.scopes_ = std::move(scopes);
    }
};

namespace {

/// `env.name`; `env` is the name of the closure's own environment parameter. It is different for
/// every closure, so a closure inside a closure can capture the outer one's `__env` as a variable.
[[nodiscard]] ExprPtr envField(const std::string& env, const std::string& name, const Span& span) {
    auto f = std::make_unique<Expr>();
    f->kind = Expr::Kind::Field;
    f->span = span;
    f->text = name;
    f->lhs = astName(env, span);
    return f;
}

} // namespace

std::vector<Lowerer::Capture>
Lowerer::takeCaptures(ClosureData& closure, bool by_value, const std::string& env) {
    // Names the closure declares itself shadow the enclosing function's: parameters, locals, loop
    // variables, catch bindings and the parameters of closures inside it.
    std::set<std::string> declared;
    for (const Param& p : closure.params) {
        declared.insert(p.name);
    }
    walkAst(
        closure.body,
        [&declared](Stmt& s) {
            switch (s.kind) {
            case Stmt::Kind::Local:
            case Stmt::Kind::Const:
            case Stmt::Kind::ForRange:
            case Stmt::Kind::ForEach:
            case Stmt::Kind::ForPack:
            case Stmt::Kind::Try:
                if (!s.name.empty()) {
                    declared.insert(s.name);
                }
                declared.insert(s.names.begin(), s.names.end());
                break;
            default:
                break;
            }
        },
        [&declared](Expr& e) {
            if (e.kind == Expr::Kind::Closure) {
                for (const Param& p : e.closure->params) {
                    declared.insert(p.name);
                }
            }
        });

    std::vector<Capture> captures;
    std::set<std::string> seen;
    walkAst(closure.body, {}, [&](Expr& e) {
        if (e.kind != Expr::Kind::Name || declared.contains(e.text) || seen.contains(e.text)) {
            return;
        }
        const ScopeEntry* entry = scopes_.lookup(e.text);
        if (entry != nullptr && entry->kind == ScopeEntry::Kind::Local) {
            seen.insert(e.text);
            captures.push_back({e.text, entry->type});
        }
    });
    if (captures.empty()) {
        return captures;
    }
    walkAst(closure.body, {}, [&](Expr& e) {
        if (e.kind != Expr::Kind::Name || !seen.contains(e.text)) {
            return;
        }
        const Span span = e.span;
        ExprPtr value = envField(env, e.text, span);
        if (!by_value) {
            value = astUnary(UnaryOp::Deref, std::move(value), span); // `*__env.name`
        }
        e = std::move(*value);
    });
    return captures;
}

Lowerer::EP Lowerer::closureExpr(const Expr& e, const Type* expected) {
    const ClosureData& c = *e.closure;
    const Type want = expected != nullptr ? resolved(*expected) : unknownType();
    const bool fn_context = want.kind == TypeKind::FnPtr && want.ret != nullptr;
    if (fn_context && want.params.size() != c.params.size()) {
        err("E0771",
            e.span,
            std::format("the closure takes {} parameter(s), but `{}` takes {}",
                        c.params.size(),
                        typeToString(want),
                        want.params.size()),
            "write as many parameters as the function type has");
        return nullptr;
    }

    // Parameter and return types.
    std::vector<Type> params;
    for (std::size_t i = 0; i < c.params.size(); ++i) {
        const Param& p = c.params[i];
        if (p.has_type) {
            params.push_back(resolved(p.type));
        } else if (fn_context) {
            params.push_back(want.params[i]);
        } else {
            err("E0770",
                p.span,
                std::format("the type of closure parameter `{}` is not known", p.name),
                std::format("write `|{}: T|`, or use the closure where a `fn(...)` type is expected",
                            p.name));
            return nullptr;
        }
    }
    const bool infer = !c.has_return_type && !fn_context;
    Type ret = c.has_return_type ? resolved(c.return_type)
               : fn_context      ? *want.ret
                                 : Type::makeVoid();

    // The variables of the enclosing function the body uses.
    ExprPtr own = copyExpr(e);
    ClosureData& body = *own->closure;
    const unsigned id = sh_.closure_count++;
    const std::string base = std::format("__closure{}", id);
    const std::string env_name = std::format("__env{}", id);
    const std::vector<Capture> captures = takeCaptures(body, c.is_move, env_name);
    if (!captures.empty() && fn_context) {
        err("E0773",
            e.span,
            std::format("the closure uses `{}`, but a `fn(...)` pointer has no place to keep it",
                        captures.front().name),
            "take the closure as a generic parameter (`fn apply<F>(f: F)`) or pass the variable "
            "as an argument");
        return nullptr;
    }
    if (c.is_move) {
        for (const Capture& cap : captures) {
            if (needsDrop(cap.type)) {
                err("E0774",
                    e.span,
                    std::format("`move` cannot copy `{}`: `{}` needs drop",
                                cap.name,
                                typeToString(cap.type)),
                    "a struct of captures is plain data; borrow it (drop `move`) instead");
                return nullptr;
            }
        }
    }

    Type env_type;
    if (!captures.empty()) {
        StructDecl st;
        st.name = base;
        st.span = e.span;
        st.name_span = e.span;
        for (const Capture& cap : captures) {
            st.fields.push_back(
                {cap.name, c.is_move ? cap.type : Type::makePointer(cap.type), {}, e.span});
        }
        Item item;
        item.decl = std::move(st);
        s_.instances_.push_back(std::move(item));
        (void)s_.internInstance(s_.instances_.back(), base, "closure", {}, e.span);
        env_type = resolved(Type::makeNamed({base}));
    }

    // The function.
    FnDecl fn;
    fn.name = captures.empty() ? base : base + "$call";
    fn.span = e.span;
    fn.name_span = e.span;
    if (!captures.empty()) {
        Param env;
        env.name = env_name;
        env.type = Type::makePointer(env_type);
        env.span = e.span;
        fn.params.push_back(std::move(env));
    }
    for (std::size_t i = 0; i < c.params.size(); ++i) {
        Param p;
        p.name = c.params[i].name;
        p.type = params[i];
        p.span = c.params[i].span;
        fn.params.push_back(std::move(p));
    }
    fn.has_return_type = !infer;
    fn.return_type = ret;
    fn.has_body = true;
    fn.body = std::move(body.body);
    if (body.expr_body && (infer || !ret.isVoid())) {
        fn.body.statements.front().kind = Stmt::Kind::Return; // `|x| x * 2` returns its value
    }

    const std::string qname = s_.qualify(fn.name);
    infer_request_ = infer;
    {
        const FnFrame frame(*this);
        lowerFunction(fn, Modifiers{}, qname, fn.name, {}, "", nullptr, false);
    }
    if (infer) {
        ret = inferred_ret_;
    }

    // Keep the declaration (with its final return type) for the calls that resolve against it.
    fn.has_return_type = true;
    fn.return_type = ret;
    s_.instances_.push_back(Item{Modifiers{}, std::move(fn)});
    const Item* item = &s_.instances_.back();

    if (captures.empty()) {
        EP f = mk(hir::ExprKind::FuncRef, Type::makeFnPtr(params, ret), e.span);
        f->text = qname;
        return f;
    }
    sh_.closures[env_type.path.front()] = {&s_, item, qname};

    // The value: the struct, holding the captured variables (their addresses unless `move`).
    auto literal = std::make_unique<Expr>();
    literal->kind = Expr::Kind::StructLiteral;
    literal->span = e.span;
    literal->type = Type::makeNamed({base});
    for (const Capture& cap : captures) {
        ExprPtr value = astName(cap.name, e.span);
        if (!c.is_move) {
            value = astUnary(UnaryOp::AddrOf, std::move(value), e.span);
        }
        literal->fields.push_back({cap.name, std::move(value), e.span});
    }
    return structLiteral(*literal);
}

} // namespace xlang::lower
