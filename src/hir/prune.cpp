#include "xlang/hir/prune.h"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace xlang::hir {
namespace {

/// Marks the functions and globals reachable from the roots, then erases the rest. The marking is a
/// worklist over function indices: a function is pushed once, when it is first marked, and its body
/// is walked when it is popped.
class Reachability {
public:
    explicit Reachability(Module& module)
        : module_(module),
          fnLive_(module.functions.size(), false),
          gLive_(module.globals.size(), false) {
        for (std::size_t i = 0; i < module.functions.size(); ++i) fnIndex_.emplace(module.functions[i].name, i);
        for (std::size_t i = 0; i < module.globals.size(); ++i) gIndex_.emplace(module.globals[i].name, i);
    }

    void markFunction(const std::string& name) {
        const auto it = fnIndex_.find(name);
        if (it == fnIndex_.end() || fnLive_[it->second]) return;
        fnLive_[it->second] = true;
        work_.push_back(it->second);
    }

    void markGlobal(const std::string& name) {
        const auto it = gIndex_.find(name);
        if (it == gIndex_.end() || gLive_[it->second]) return;
        gLive_[it->second] = true;
        // A global's initializer is a constant expression, but it may name a function (`FuncRef`).
        if (module_.globals[it->second].init) expr(*module_.globals[it->second].init);
    }

    /// Drains the worklist: every function marked so far has its body and cleanup body walked.
    void run() {
        while (!work_.empty()) {
            const std::size_t i = work_.back();
            work_.pop_back();
            const Function& f = module_.functions[i];
            for (const Stmt& s : f.body) stmt(s);
            for (const Stmt& s : f.unwind_body) stmt(s);
        }
    }

    /// Erases the unmarked functions and globals; keeps the order of the survivors.
    std::size_t sweep() {
        std::size_t removed = 0;
        std::vector<Function> functions;
        functions.reserve(module_.functions.size());
        for (std::size_t i = 0; i < module_.functions.size(); ++i) {
            if (fnLive_[i]) {
                functions.push_back(std::move(module_.functions[i]));
            } else {
                ++removed;
            }
        }
        module_.functions = std::move(functions);

        std::vector<Global> globals;
        globals.reserve(module_.globals.size());
        for (std::size_t i = 0; i < module_.globals.size(); ++i) {
            if (gLive_[i]) globals.push_back(std::move(module_.globals[i]));
        }
        module_.globals = std::move(globals);
        return removed;
    }

private:
    void expr(const Expr& e) {
        switch (e.kind) {
            case ExprKind::Call:
            case ExprKind::CallVirtual:
            case ExprKind::FuncRef:
            case ExprKind::Construct:
                markFunction(e.text);
                break;
            case ExprKind::GlobalRef:
                markGlobal(e.text);
                break;
            default:
                break;
        }
        if (e.lhs) expr(*e.lhs);
        if (e.rhs) expr(*e.rhs);
        for (const ExprPtr& a : e.args) expr(*a);
        for (const FieldInit& f : e.fields) {
            if (f.value) expr(*f.value);
        }
        for (const AsmOperand& op : e.asm_operands) {
            if (op.value) expr(*op.value);
        }
    }

    void stmt(const Stmt& s) {
        if (s.target) expr(*s.target);
        if (s.value) expr(*s.value);
        for (const Stmt& c : s.body) stmt(c);
        for (const Stmt& c : s.else_body) stmt(c);
        for (const Stmt& c : s.step) stmt(c);
    }

    Module& module_;
    std::vector<bool> fnLive_;
    std::vector<bool> gLive_;
    std::unordered_map<std::string, std::size_t> fnIndex_;
    std::unordered_map<std::string, std::size_t> gIndex_;
    std::vector<std::size_t> work_;
};

/// C-named functions LLVM may call without any call in the program: it lowers aggregate copies and zeroing
/// to `memcpy`/`memmove`/`memset`/`bzero` (Darwin), and comparisons of aggregates to `memcmp`/`bcmp`. Codegen
/// emits no such call itself, so these stay roots by symbol name (they are `export(c)` in std/mem).
[[nodiscard]] bool isImplicitLibcall(const std::string& symbol) {
    return symbol == "memcpy" || symbol == "memmove" || symbol == "memset" || symbol == "memcmp" ||
           symbol == "bcmp" || symbol == "bzero";
}

/// Marks the roots listed in the module header: hooks named by codegen, init and fini lists.
void markNamedHooks(Reachability& r, const Module& m) {
    r.markFunction(m.panic_function);
    r.markFunction(m.alloc_function);
    r.markFunction(m.free_function);
    r.markFunction(m.unwind_function);
    r.markFunction(m.args_function);
    for (const std::string& n : m.init_functions) r.markFunction(n);
    for (const std::string& n : m.fini_functions) r.markFunction(n);
}

}  // namespace

std::size_t pruneUnreachable(Module& module, bool keepExports) {
    Reachability r(module);

    // Functions the program owns at run time, whether or not code calls them.
    for (const Function& f : module.functions) {
        if (f.is_entry || f.is_init || f.is_fini || f.registered || isImplicitLibcall(f.symbol)) {
            r.markFunction(f.name);
        }
        if (keepExports && (f.exported || f.export_c)) r.markFunction(f.name);
    }
    for (const Global& g : module.globals) {
        if (g.registered || (keepExports && (g.exported || g.export_c))) r.markGlobal(g.name);
    }
    markNamedHooks(r, module);

    // Tables codegen writes for every class and impl name their functions, so those must exist.
    for (const Aggregate& a : module.aggregates) {
        for (const VtableSlot& slot : a.vtable) r.markFunction(slot.function);
    }
    for (const Impl& im : module.impls) {
        for (const std::string& name : im.methods) r.markFunction(name);
    }

    r.run();
    return r.sweep();
}

}  // namespace xlang::hir
