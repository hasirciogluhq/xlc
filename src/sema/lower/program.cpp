// Lowering: the whole-program entry point. Lowers every module, then the methods only reached
// through calls, and returns one HIR module.

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang {

namespace {

/// The functions an expression or statement names (calls, function values, constructions).
void namedFunctions(const hir::Expr& e, std::set<std::string>& out) {
    if (e.kind == hir::ExprKind::Call || e.kind == hir::ExprKind::CallVirtual ||
        e.kind == hir::ExprKind::FuncRef || e.kind == hir::ExprKind::Construct) {
        out.insert(e.text);
    }
    for (const hir::Expr* c : {e.lhs.get(), e.rhs.get()}) {
        if (c != nullptr) {
            namedFunctions(*c, out);
        }
    }
    for (const hir::ExprPtr& a : e.args) {
        namedFunctions(*a, out);
    }
    for (const hir::FieldInit& f : e.fields) {
        if (f.value) {
            namedFunctions(*f.value, out);
        }
    }
    for (const hir::AsmOperand& op : e.asm_operands) {
        if (op.value) {
            namedFunctions(*op.value, out);
        }
    }
}

void namedFunctions(const hir::Stmt& s, std::set<std::string>& out) {
    for (const hir::Expr* e : {s.target.get(), s.value.get()}) {
        if (e != nullptr) {
            namedFunctions(*e, out);
        }
    }
    for (const auto* list : {&s.body, &s.else_body, &s.step}) {
        for (const hir::Stmt& c : *list) {
            namedFunctions(c, out);
        }
    }
}

/// Lowers the lazy methods (`LowerShared::lazy_methods`) that the functions lowered since the last
/// call refer to; true when it lowered any.
bool lowerReferencedMethods(lower::LowerShared& shared,
                            const std::map<const Sema*, lower::Lowerer*>& by_sema) {
    if (shared.lazy_methods.empty()) {
        shared.lazy_scanned = shared.module.functions.size();
        return false;
    }
    std::set<std::string> named;
    for (; shared.lazy_scanned < shared.module.functions.size(); ++shared.lazy_scanned) {
        const hir::Function& f = shared.module.functions[shared.lazy_scanned];
        for (const auto* list : {&f.body, &f.unwind_body}) {
            for (const hir::Stmt& s : *list) {
                namedFunctions(s, named);
            }
        }
    }
    for (const hir::Global& g : shared.module.globals) {
        if (g.init) {
            namedFunctions(*g.init, named);
        }
    }
    bool any = false;
    for (const std::string& name : named) {
        const auto it = shared.lazy_methods.find(name);
        if (it == shared.lazy_methods.end()) {
            continue;
        }
        const lower::LowerShared::LazyMethod m = it->second;
        shared.lazy_methods.erase(it);
        by_sema.at(m.owner)->lowerLazyMethod(m, name);
        any = true;
    }
    return any;
}

} // namespace

hir::Module lowerProgram(const std::vector<Sema*>& modules,
                         const std::vector<Sema*>& init_order,
                         const std::set<const Sema*>& library_modules) {
    using lower::Lowerer;
    using lower::LowerShared;
    LowerShared shared;
    shared.modules = modules;
    std::vector<std::unique_ptr<Lowerer>> lowerers;
    std::map<const Sema*, Lowerer*> by_sema;
    for (Sema* s : modules) {
        lowerers.push_back(std::make_unique<Lowerer>(*s, shared, library_modules.contains(s)));
        by_sema[s] = lowerers.back().get();
    }
    for (auto& l : lowerers) {
        l->collectImpls();
    }
    for (auto& l : lowerers) {
        l->checkRegistrations();
    }
    for (auto& l : lowerers) {
        l->checkHookFunctions();
    }
    for (auto& l : lowerers) {
        l->run();
    }
    // Generic instances requested by one module can belong to another: lower until none is
    // left.
    // A lazy method is lowered once a lowered function names it; its body can make new instances.
    bool more = true;
    while (more) {
        more = false;
        for (auto& l : lowerers) {
            more = l->lowerPendingInstances() || more;
        }
        more = lowerReferencedMethods(shared, by_sema) || more;
    }
    for (auto& l : lowerers) {
        l->finish();
    }
    if (!lowerers.empty()) {
        lowerers.front()->checkCFunctionValues();
    }
    for (Sema* s : init_order) {
        for (const std::string& name : by_sema.at(s)->initFunctions()) {
            shared.module.init_functions.push_back(name);
        }
        for (const std::string& name : by_sema.at(s)->finiFunctions()) {
            shared.module.fini_functions.push_back(name);
        }
    }
    return std::move(shared.module);
}

} // namespace xlang
