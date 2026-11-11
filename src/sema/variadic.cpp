// Parameter packs: `fn f(a: T, ..args)` takes any number of trailing arguments of any types.
//
// A pack function has no code of its own. Each call with `n` pack arguments instantiates a copy
// of the function (a template per `n`) whose pack is `n` ordinary parameters of type parameters
// `pack#type#i`. In that copy `args.len` is the constant `n`, `..args` in a call is the `n`
// element parameters, and `for a in args { }` is unrolled into one block per element. The copy
// then goes through the normal generic instantiation.
#include "xlang/sema/sema.h"

#include <format>
#include <functional>

namespace xlang {

namespace {

/// The name of element `i` of the pack named `pack` inside a template copy.
std::string elementName(const std::string& pack, std::size_t i) {
    return pack + "#" + std::to_string(i);
}

/// The type parameter that stands for element `i`; it cannot be written in source (`#`).
std::string elementTypeParam(std::size_t i) {
    return "pack#type#" + std::to_string(i);
}

ExprPtr makeExpr(Expr::Kind kind, const Span& span) {
    auto e = std::make_unique<Expr>();
    e->kind = kind;
    e->span = span;
    return e;
}

/// Rewrites one template copy: `args.len`, `..args` and `for a in args` for pack `pack` with `n`
/// elements. Loops are unrolled into `do { } while false` blocks, so that `break` and `continue`
/// can be retargeted to the unrolled loop: `break` leaves all elements, `continue` the element.
class PackRewriter {
public:
    PackRewriter(std::string pack, std::size_t count) : pack_(std::move(pack)), count_(count) {}

    void block(Block& b) { statements(b.statements); }

private:
    struct Labels {
        std::string user;   ///< the label written on the `for` (empty = none)
        std::string whole;  ///< breaks out of every element
        std::string item;   ///< leaves the current element
    };

    std::string fresh() { return "pack#" + std::to_string(next_label_++); }

    void statements(std::vector<Stmt>& list) {
        for (Stmt& s : list) {
            if (s.kind == Stmt::Kind::ForPack || isPackLoop(s)) {
                s = unroll(s);
            } else {
                stmt(s);
            }
        }
    }

    /// A `for x in <pack> { }` written with the collection form but naming the pack is a pack loop:
    /// there is no index form and the iterable is exactly the pack's name.
    [[nodiscard]] bool isPackLoop(const Stmt& s) const {
        return s.kind == Stmt::Kind::ForEach && s.names.empty() && s.expr &&
               s.expr->kind == Expr::Kind::Name && s.expr->text == pack_;
    }

    void stmt(Stmt& s) {
        if (s.target) {
            expr(s.target);
        }
        if (s.expr) {
            expr(s.expr);
        }
        if (s.body) {
            block(*s.body);
        }
        if (s.else_body) {
            block(*s.else_body);
        }
        if (s.init) {
            stmt(*s.init);
        }
        if (s.step) {
            stmt(*s.step);
        }
        for (SwitchCase& c : s.cases) {
            for (ExprPtr& v : c.values) {
                expr(v);
            }
            if (c.body) {
                block(*c.body);
            }
        }
    }

    void expr(ExprPtr& e) {
        if (e->kind == Expr::Kind::Field && e->text == "len" && e->lhs &&
            e->lhs->kind == Expr::Kind::Name && e->lhs->text == pack_) {
            ExprPtr literal = makeExpr(Expr::Kind::IntLiteral, e->span);
            literal->int_value = count_;
            e = std::move(literal);
            return;
        }
        if (e->lhs) {
            expr(e->lhs);
        }
        if (e->rhs) {
            expr(e->rhs);
        }
        std::vector<ExprPtr> args;
        for (ExprPtr& a : e->args) {
            if (e->kind == Expr::Kind::Call && a->kind == Expr::Kind::Spread && a->text == pack_) {
                for (std::size_t i = 0; i < count_; ++i) {
                    ExprPtr name = makeExpr(Expr::Kind::Name, a->span);
                    name->text = elementName(pack_, i);
                    args.push_back(std::move(name));
                }
                continue;
            }
            expr(a);
            args.push_back(std::move(a));
        }
        e->args = std::move(args);
        for (FieldInit& f : e->fields) {
            if (f.value) {
                expr(f.value);
            }
        }
    }

    /// `for a in args { body }` becomes `whole: do { item_i: do { local a = args#i; body } while
    /// false ... } while false`, one `item_i` block per element. The body is copied per element,
    /// so that nested pack loops and the retargeted labels stay distinct.
    Stmt unroll(const Stmt& loop) {
        Stmt whole;
        whole.kind = Stmt::Kind::DoWhile;
        whole.span = loop.span;
        whole.label = fresh();
        whole.expr = makeExpr(Expr::Kind::BoolLiteral, loop.span);
        whole.expr->bool_value = false;
        whole.body = std::make_unique<Block>();
        whole.body->span = loop.body->span;
        for (std::size_t i = 0; i < count_; ++i) {
            Labels labels{loop.label, whole.label, fresh()};
            Block body = copyBlock(*loop.body);
            block(body);
            Block element;
            element.span = body.span;
            Stmt bind;
            bind.kind = Stmt::Kind::Local;
            bind.span = loop.span;
            bind.name = loop.name;
            bind.expr = makeExpr(Expr::Kind::Name, loop.span);
            bind.expr->text = elementName(pack_, i);
            element.statements.push_back(std::move(bind));
            for (Stmt& s : body.statements) {
                element.statements.push_back(std::move(s));
            }
            retarget(element, labels, 0, 0);
            Stmt item;
            item.kind = Stmt::Kind::DoWhile;
            item.span = loop.span;
            item.label = labels.item;
            item.expr = makeExpr(Expr::Kind::BoolLiteral, loop.span);
            item.expr->bool_value = false;
            item.body = std::make_unique<Block>(std::move(element));
            whole.body->statements.push_back(std::move(item));
        }
        return whole;
    }

    /// Points the `break` and `continue` that leave the unrolled loop at its labels. `loops` and
    /// `switches` count the enclosing statements that take an unlabeled `break` or `continue`.
    void retarget(Block& b, const Labels& labels, int loops, int switches) {
        for (Stmt& s : b.statements) {
            retargetStmt(s, labels, loops, switches);
        }
    }

    void retargetStmt(Stmt& s, const Labels& labels, int loops, int switches) {
        switch (s.kind) {
        case Stmt::Kind::Break:
        case Stmt::Kind::Continue: {
            const bool is_break = s.kind == Stmt::Kind::Break;
            if (s.name.empty()) {
                if (is_break && loops == 0 && switches == 0) {
                    s.name = labels.whole;
                } else if (!is_break && loops == 0) {
                    s.name = labels.item;
                }
            } else if (!labels.user.empty() && s.name == labels.user) {
                s.name = is_break ? labels.whole : labels.item;
            }
            return;
        }
        case Stmt::Kind::While:
        case Stmt::Kind::DoWhile:
        case Stmt::Kind::For:
        case Stmt::Kind::ForRange:
        case Stmt::Kind::ForPack:
        case Stmt::Kind::ForEach:
            if (s.body) {
                retarget(*s.body, labels, loops + 1, switches);
            }
            return;
        case Stmt::Kind::Switch:
            for (SwitchCase& c : s.cases) {
                if (c.body) {
                    retarget(*c.body, labels, loops, switches + 1);
                }
            }
            return;
        default:
            break;
        }
        if (s.body) {
            retarget(*s.body, labels, loops, switches);
        }
        if (s.else_body) {
            retarget(*s.else_body, labels, loops, switches);
        }
        if (s.init) {
            retargetStmt(*s.init, labels, loops, switches);
        }
        if (s.step) {
            retargetStmt(*s.step, labels, loops, switches);
        }
    }

    std::string pack_;
    std::size_t count_;
    std::size_t next_label_{0};
};

/// The body and signature of pack function `f` for `count` elements, as a copy with the pack
/// replaced by ordinary parameters of type parameters.
FnDecl packTemplateDecl(const FnDecl& f, std::size_t count) {
    FnDecl c = instantiateFn(f, TypeEnv{});
    c.generic_params.clear();
    c.params.clear();
    std::string pack;
    for (const Param& p : f.params) {
        if (p.is_pack) {
            if (!pack.empty()) {
                continue;  // E0734 reports a second pack; the first one is expanded
            }
            pack = p.name;
            for (std::size_t i = 0; i < count; ++i) {
                Param e;
                e.name = elementName(pack, i);
                e.type = Type::makeNamed({elementTypeParam(i)});
                e.span = p.span;
                c.params.push_back(std::move(e));
                c.generic_params.push_back(elementTypeParam(i));
            }
            continue;
        }
        c.params.push_back(p);
    }
    if (!pack.empty()) {
        PackRewriter rewriter(pack, count);
        rewriter.block(c.body);
    }
    return c;
}

/// Checks the uses of pack `pack` in a function body.
class PackUseChecker {
public:
    using Report = std::function<void(const char*, const Span&, std::string, std::string)>;

    PackUseChecker(std::string pack, Report report) : pack_(std::move(pack)), report_(std::move(report)) {}

    void block(const Block& b) {
        for (const Stmt& s : b.statements) {
            stmt(s);
        }
    }

private:
    [[nodiscard]] bool isPack(const Expr& e) const {
        return e.kind == Expr::Kind::Name && e.text == pack_;
    }

    /// `for x in <pack> { }` and `for x in <something else> { }` share one statement kind (the parser
    /// cannot know that a name is a pack), so a pack loop is recognised here by its iterable.
    [[nodiscard]] bool isPackLoop(const Stmt& s) const {
        return (s.kind == Stmt::Kind::ForPack || s.kind == Stmt::Kind::ForEach) && s.names.empty() &&
               s.expr && isPack(*s.expr);
    }

    void stmt(const Stmt& s) {
        if (isPackLoop(s)) {
            block(*s.body);
            return;
        }
        if (s.kind == Stmt::Kind::ForPack) {
            if (!isPack(*s.expr)) {
                report_("E0735", s.expr->span,
                        "`for ... in` needs a parameter pack",
                        "only the pack of this function can be iterated");
            }
            block(*s.body);
            return;
        }
        if (s.target) {
            expr(*s.target, false);
        }
        if (s.expr) {
            expr(*s.expr, false);
        }
        if (s.body) {
            block(*s.body);
        }
        if (s.else_body) {
            block(*s.else_body);
        }
        if (s.init) {
            stmt(*s.init);
        }
        if (s.step) {
            stmt(*s.step);
        }
        for (const SwitchCase& c : s.cases) {
            for (const ExprPtr& v : c.values) {
                expr(*v, false);
            }
            if (c.body) {
                block(*c.body);
            }
        }
    }

    void expr(const Expr& e, bool call_arg) {
        switch (e.kind) {
        case Expr::Kind::Name:
            if (e.text == pack_) {
                report_("E0735", e.span,
                        std::format("parameter pack `{}` used as a value", pack_),
                        "read `" + pack_ + ".len`, iterate it with `for x in " + pack_ + "`, or spread it as `.." + pack_ + "` in a call");
            }
            return;
        case Expr::Kind::Field:
            if (e.lhs && isPack(*e.lhs)) {
                if (e.text != "len") {
                    report_("E0735", e.span,
                            std::format("parameter pack `{}` has no member `{}`", pack_, e.text),
                            "the only member of a pack is `len`");
                }
                return;
            }
            break;
        case Expr::Kind::Index:
            if (e.lhs && isPack(*e.lhs)) {
                report_("E0738", e.span,
                        std::format("parameter pack `{}` cannot be indexed", pack_),
                        "iterate it with `for x in " + pack_ + "` instead");
                if (e.rhs) {
                    expr(*e.rhs, false);
                }
                return;
            }
            break;
        case Expr::Kind::Spread:
            if (!call_arg || e.text != pack_) {
                report_("E0737", e.span,
                        std::format("`..{}` is not a spread of this function's parameter pack", e.text),
                        "`..name` is valid only as a call argument, where `name` is the pack");
            }
            return;
        case Expr::Kind::Call:
            if (e.lhs) {
                expr(*e.lhs, false);
            }
            for (const ExprPtr& a : e.args) {
                expr(*a, true);
            }
            return;
        default:
            break;
        }
        if (e.lhs) {
            expr(*e.lhs, false);
        }
        if (e.rhs) {
            expr(*e.rhs, false);
        }
        for (const ExprPtr& a : e.args) {
            expr(*a, false);
        }
        for (const FieldInit& f : e.fields) {
            if (f.value) {
                expr(*f.value, false);
            }
        }
    }

    std::string pack_;
    Report report_;
};

}  // namespace

bool Sema::isPackFn(const Item& item) {
    const auto* f = std::get_if<FnDecl>(&item.decl);
    if (f == nullptr) {
        return false;
    }
    for (const Param& p : f->params) {
        if (p.is_pack) {
            return true;
        }
    }
    return false;
}

void Sema::checkPackFn(const Item& item) {
    const auto& f = std::get<FnDecl>(item.decl);
    std::string pack;
    for (std::size_t i = 0; i < f.params.size(); ++i) {
        const Param& p = f.params[i];
        if (!p.is_pack) {
            if (!p.is_self) {
                checkType(p.type, p.span);
            }
            continue;
        }
        if (!pack.empty()) {
            error("E0734", p.span, "a function can have only one parameter pack",
                  "remove the second `..` parameter");
        } else if (i + 1 != f.params.size()) {
            error("E0734", p.span, "a parameter pack must be the last parameter",
                  "move `..` to the end of the parameter list");
        }
        if (pack.empty()) {
            pack = p.name;
        }
    }
    checkType(f.return_type, declBase(item).span);
    if (item.modifiers.declared || item.modifiers.export_c || !f.has_body || f.is_virtual ||
        f.is_override || !f.generic_params.empty()) {
        error("E0736", declBase(item).name_span,
              std::format("`{}` cannot take a parameter pack here", f.name),
              "a pack function needs a body and no `declare`, `export(c)`, `virtual`, `override` or type parameters");
    }
    if (pack.empty()) {
        return;
    }
    PackUseChecker checker(pack, [this](const char* code, const Span& at, std::string message,
                                        std::string label) {
        error(code, at, std::move(message), std::move(label));
    });
    checker.block(f.body);
}

void Sema::checkNoPackMethod(const FnDecl& method) {
    for (const Param& p : method.params) {
        if (p.is_pack) {
            error("E0736", method.name_span,
                  std::format("method `{}` cannot take a parameter pack", method.name),
                  "parameter packs are supported on free functions only");
            return;
        }
    }
}

const Sema::PackTemplate& Sema::packTemplate(const Item& generic, std::size_t count) {
    const auto key = std::make_pair(&generic, count);
    if (const auto it = pack_templates_.find(key); it != pack_templates_.end()) {
        return *it->second;
    }
    auto t = std::make_unique<PackTemplate>();
    t->item.modifiers = generic.modifiers;
    t->item.decl = packTemplateDecl(std::get<FnDecl>(generic.decl), count);
    t->sym.kind = Symbol::Kind::Fn;
    t->sym.item = &t->item;
    return *pack_templates_.emplace(key, std::move(t)).first->second;
}

std::optional<Sema::EntityRef> Sema::instantiatePack(const Item& generic,
                                                     const std::string& local,
                                                     const std::vector<Type>& elements,
                                                     const Span& at) {
    const PackTemplate& t = packTemplate(generic, elements.size());
    return instantiate(EntityRef{this, &t.sym, local}, elements, at);
}

}  // namespace xlang
