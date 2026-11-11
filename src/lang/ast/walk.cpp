#include "xlang/lang/ast.h"

namespace xlang {

namespace {

struct Walker {
    const std::function<void(Stmt&)>& on_stmt;
    const std::function<void(Expr&)>& on_expr;

    void block(Block& b) const {
        for (Stmt& s : b.statements) {
            stmt(s);
        }
    }

    void stmt(Stmt& s) const {
        if (on_stmt) {
            on_stmt(s);
        }
        for (Expr* e : {s.target.get(), s.expr.get(), s.expr2.get()}) {
            if (e != nullptr) {
                expr(*e);
            }
        }
        if (s.init) {
            stmt(*s.init);
        }
        if (s.step) {
            stmt(*s.step);
        }
        if (s.body) {
            block(*s.body);
        }
        if (s.else_body) {
            block(*s.else_body);
        }
        for (SwitchCase& c : s.cases) {
            for (ExprPtr& v : c.values) {
                expr(*v);
            }
            if (c.body) {
                block(*c.body);
            }
        }
    }

    void expr(Expr& e) const {
        if (on_expr) {
            on_expr(e);
        }
        for (Expr* child : {e.lhs.get(), e.rhs.get()}) {
            if (child != nullptr) {
                expr(*child);
            }
        }
        for (ExprPtr& a : e.args) {
            expr(*a);
        }
        for (FieldInit& f : e.fields) {
            if (f.value) {
                expr(*f.value);
            }
        }
        if (e.closure) {
            block(e.closure->body);
        }
    }
};

} // namespace

void walkAst(Block& block,
             const std::function<void(Stmt&)>& on_stmt,
             const std::function<void(Expr&)>& on_expr) {
    Walker{on_stmt, on_expr}.block(block);
}

} // namespace xlang
