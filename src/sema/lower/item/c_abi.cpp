// Lowering: which types C can express (`export(c)`, `declare`).

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

// ---------------------------------------------------------------------------------------------
// Items

bool Lowerer::cCompatible(Sema& sema, const Type& t) {
    switch (t.kind) {
    case TypeKind::Void:
    case TypeKind::Never: // C11 `_Noreturn void`
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::FnPtr:
    case TypeKind::Pointer:
        return true;
    case TypeKind::Enum:
        return t.elem && t.elem->kind == TypeKind::Int;
    case TypeKind::Struct: {
        const auto ref = sema.entityByQName(t.path.front());
        if (!ref) {
            return false;
        }
        const Attribute* repr = lower::findAttribute(declBase(*ref->sym->item).attributes, "repr");
        return repr != nullptr &&
               std::ranges::any_of(repr->args, [](const AttributeArg& a) { return a.text == "c"; });
    }
    default:
        return false;
    }
}

void Lowerer::checkCFunctionValues() {
    // A call through a `fn(...)` pointer uses the xlang convention, where a struct travels as the
    // address of a copy. An `export(c)` or `declare` function that takes or returns a struct by
    // value follows the C convention instead (codegen/llvm/c_abi/), so it can only be called by
    // name; its address would be called the wrong way.
    std::map<std::string_view, const hir::Function*> by_name;
    for (const hir::Function& f : mod_.functions) {
        by_name.emplace(f.name, &f);
    }
    const auto passesStruct = [](const hir::Function& f) {
        return lower::isAggregate(f.ret) ||
               std::ranges::any_of(f.locals | std::views::take(f.param_count),
                                   [](const hir::Local& l) { return lower::isAggregate(l.type); });
    };
    std::function<void(const hir::Expr&)> expr;
    std::function<void(const hir::Stmt&)> stmt;
    expr = [&](const hir::Expr& e) {
        if (const auto it = by_name.find(e.text);
            e.kind == hir::ExprKind::FuncRef && it != by_name.end() &&
            (it->second->export_c || it->second->declared) && passesStruct(*it->second)) {
            err("E0778",
                e.span,
                std::format("`{}` passes a struct by value in the C convention and cannot be a "
                            "`fn(...)` value",
                            e.text),
                "call it by name, or take and return the struct through a pointer");
        }
        for (const hir::Expr* c : {e.lhs.get(), e.rhs.get()}) {
            if (c != nullptr) {
                expr(*c);
            }
        }
        for (const hir::ExprPtr& a : e.args) {
            expr(*a);
        }
        for (const hir::FieldInit& f : e.fields) {
            if (f.value) {
                expr(*f.value);
            }
        }
    };
    stmt = [&](const hir::Stmt& s) {
        for (const hir::Expr* e : {s.target.get(), s.value.get()}) {
            if (e != nullptr) {
                expr(*e);
            }
        }
        for (const auto* list : {&s.body, &s.else_body, &s.step}) {
            std::ranges::for_each(*list, stmt);
        }
    };
    for (const hir::Function& f : mod_.functions) {
        std::ranges::for_each(f.body, stmt);
    }
    for (const hir::Global& g : mod_.globals) {
        if (g.init) {
            expr(*g.init);
        }
    }
}

void Lowerer::assignSymbol(const std::vector<Attribute>& attrs,
                           bool export_c,
                           bool declared,
                           const std::string& local_name,
                           const std::vector<Type>& params,
                           bool overloaded,
                           std::string& symbol,
                           const Span& at) {
    if (!overloaded && !export_c) {
        if (const Attribute* m = lower::findAttribute(attrs, "mangle");
            m != nullptr && !m->args.empty() && m->args[0].kind == AttributeArg::Kind::Ident &&
            m->args[0].text != "none") {
            err("E0705", m->args[0].span, "`@mangle` takes `none` or a string", "invalid argument");
        }
    }
    symbol = s_.linkName(attrs, export_c, declared, local_name, params, overloaded);
    if (symbol == "_start" || symbol == "mainCRTStartup" || symbol == "_xl_initAll") {
        err("E0726",
            at,
            std::format("symbol `{}` is reserved for the compiler's entry point", symbol),
            "rename it: the compiler generates this symbol to start the program");
    }
    const auto [it, inserted] = sh_.link_names.emplace(symbol, at);
    // Plain names must be unique; mangled names are unique by their parameter types.
    if (!inserted && !declared && symbol.find("_xl$") != 0) {
        Diagnostic d;
        d.code = "E0401";
        d.severity = Severity::Error;
        d.message = std::format("symbol `{}` is defined twice", symbol);
        d.labels.push_back({it->second, "first definition"});
        d.labels.push_back({at, "second definition"});
        d.fix = "enable name mangling or rename one with `@mangle(\"...\")`";
        s_.diag_.report(std::move(d));
    }
}

} // namespace xlang::lower
