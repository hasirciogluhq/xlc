#include "xlang/hir/hir.h"

#include <format>
#include <ostream>

namespace xlang::hir {

namespace {

[[nodiscard]] bool isUnknown(const Type& t) {
    return t.kind == TypeKind::Named;
}

[[nodiscard]] std::string typeName(const Type& t) {
    return isUnknown(t) ? "?" : typeToString(t);
}

void dumpExpr(std::ostream& out, const Function* fn, const Expr& e);

void dumpArgs(std::ostream& out, const Function* fn, const std::vector<ExprPtr>& args) {
    for (const ExprPtr& a : args) {
        out << ' ';
        dumpExpr(out, fn, *a);
    }
}

void dumpExpr(std::ostream& out, const Function* fn, const Expr& e) {
    const auto sub = [&](const char* head, bool with_text = false) {
        out << '(' << head;
        if (with_text) {
            out << ' ' << e.text;
        }
    };
    switch (e.kind) {
    case ExprKind::IntConst:
        out << std::format("{}:{}", static_cast<std::int64_t>(e.int_value), typeName(e.type));
        return;
    case ExprKind::FloatConst:
        out << std::format("{}:{}", e.float_value, typeName(e.type));
        return;
    case ExprKind::BoolConst:
        out << (e.bool_value ? "true" : "false");
        return;
    case ExprKind::StrConst:
        out << '"' << e.text << '"';
        return;
    case ExprKind::Nil:
        out << "nil:" << typeName(e.type);
        return;
    case ExprKind::LocalRef:
        out << (fn != nullptr && e.local < fn->locals.size() ? fn->locals[e.local].name : "?")
            << '#' << e.local;
        return;
    case ExprKind::GlobalRef:
        out << '@' << e.text;
        return;
    case ExprKind::FuncRef:
        out << "&fn " << e.text;
        return;
    case ExprKind::External:
        out << "external(" << e.text << '.' << e.member << ')';
        return;
    case ExprKind::Construct:
        out << "construct " << typeName(e.target) << ' ' << e.text;
        break;
    case ExprKind::Move:
        out << "move#" << e.local;
        break;
    case ExprKind::Materialize:
        out << "materialize#" << e.local << " live#" << e.index;
        break;
    case ExprKind::Unary:
        sub(unaryOpName(e.unary_op));
        break;
    case ExprKind::Binary:
        sub(binaryOpName(e.binary_op));
        break;
    case ExprKind::Cast:
        sub(e.implicit ? "cast.implicit" : "cast");
        break;
    case ExprKind::Call:
        sub("call", true);
        break;
    case ExprKind::CallIndirect:
        sub("call.indirect");
        break;
    case ExprKind::CallVirtual:
        out << std::format("(call.virtual {}[{}]", e.text, e.index);
        break;
    case ExprKind::CallItable:
        out << std::format("(call.itable {}[{}]", e.text, e.index);
        break;
    case ExprKind::ExternalCall:
        out << "(call.external " << e.text << '.' << e.member;
        break;
    case ExprKind::Syscall:
        sub("syscall");
        break;
    case ExprKind::Field:
        out << std::format("(field{} {}", e.through_pointer ? ".ptr" : "", e.text);
        break;
    case ExprKind::Select:
        sub("select");
        break;
    case ExprKind::Index:
        sub(e.checked ? "index.checked" : "index");
        break;
    case ExprKind::Slice:
        sub(e.checked ? "slice.checked" : "slice");
        break;
    case ExprKind::Deref:
        sub("deref");
        break;
    case ExprKind::RawRead:
        sub("rawRead");
        break;
    case ExprKind::AddrOf:
        sub("addr");
        break;
    case ExprKind::ArrayList:
        sub("array");
        break;
    case ExprKind::TupleLit:
        sub("tuple");
        break;
    case ExprKind::ArrayRepeat:
        out << std::format("(array.repeat {}", e.int_value);
        break;
    case ExprKind::StructLit:
        out << "(struct " << typeName(e.type);
        break;
    case ExprKind::New:
        out << "(new " << typeName(e.target) << (e.registered ? " registered" : "");
        break;
    case ExprKind::ToInterface:
        out << "(to.interface " << e.text;
        break;
    case ExprKind::AtomicLoad:
        out << "(atomic.load order=" << e.index;
        break;
    case ExprKind::AtomicStore:
        out << "(atomic.store order=" << e.index;
        break;
    case ExprKind::AtomicXchg:
        out << "(atomic.xchg order=" << e.index;
        break;
    case ExprKind::AtomicCas:
        out << "(atomic.cas order=" << e.index;
        break;
    case ExprKind::AtomicAdd:
        out << "(atomic.add order=" << e.index;
        break;
    case ExprKind::Fence:
        out << "(fence order=" << e.index;
        break;
    case ExprKind::UnwindPads:
        out << "(unwind.pads";
        break;
    case ExprKind::UnwindPadCount:
        out << "(unwind.pad.count";
        break;
    case ExprKind::Asm:
        out << "(asm \"" << e.text << "\"" << (e.nomem ? " nomem" : "")
            << (e.noreturn ? " noreturn" : "");
        for (const AsmOperand& op : e.asm_operands) {
            out << (op.kind == AsmOperand::Kind::In    ? " in("
                    : op.kind == AsmOperand::Kind::Out ? " out("
                                                       : " inout(")
                << op.reg << ") ";
            dumpExpr(out, fn, *op.value);
        }
        for (const std::string& c : e.clobbers) {
            out << " clobber(" << c << ")";
        }
        break;
    }
    if (e.lhs) {
        out << ' ';
        dumpExpr(out, fn, *e.lhs);
    }
    if (e.rhs) {
        out << ' ';
        dumpExpr(out, fn, *e.rhs);
    }
    dumpArgs(out, fn, e.args);
    for (const FieldInit& f : e.fields) {
        out << ' ' << f.name << '=';
        dumpExpr(out, fn, *f.value);
    }
    out << "):" << typeName(e.type);
}

void dumpStmts(std::ostream& out, const Function& fn, const std::vector<Stmt>& body, int indent);

void dumpStmt(std::ostream& out, const Function& fn, const Stmt& s, int indent) {
    const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
    out << pad;
    switch (s.kind) {
    case StmtKind::Local:
        out << "local " << fn.locals[s.local].name << '#' << s.local << ": "
            << typeName(fn.locals[s.local].type);
        if (s.value) {
            out << " = ";
            dumpExpr(out, &fn, *s.value);
        }
        out << '\n';
        break;
    case StmtKind::Assign:
        out << "assign ";
        dumpExpr(out, &fn, *s.target);
        out << " = ";
        dumpExpr(out, &fn, *s.value);
        out << '\n';
        break;
    case StmtKind::Return:
        out << "return";
        if (s.value) {
            out << ' ';
            dumpExpr(out, &fn, *s.value);
        }
        out << '\n';
        break;
    case StmtKind::Expr:
        dumpExpr(out, &fn, *s.value);
        out << '\n';
        break;
    case StmtKind::If:
        out << "if ";
        dumpExpr(out, &fn, *s.value);
        out << '\n';
        dumpStmts(out, fn, s.body, indent + 1);
        if (!s.else_body.empty()) {
            out << pad << "else\n";
            dumpStmts(out, fn, s.else_body, indent + 1);
        }
        break;
    case StmtKind::While:
        out << (s.post_test ? "do-while " : "while ");
        dumpExpr(out, &fn, *s.value);
        out << '\n';
        dumpStmts(out, fn, s.body, indent + 1);
        if (!s.step.empty()) {
            out << pad << "step\n";
            dumpStmts(out, fn, s.step, indent + 1);
        }
        break;
    case StmtKind::Continue:
        out << "continue" << (s.depth != 0 ? std::format(" depth={}", s.depth) : std::string())
            << '\n';
        break;
    case StmtKind::Break:
        out << "break" << (s.depth != 0 ? std::format(" depth={}", s.depth) : std::string())
            << '\n';
        break;
    case StmtKind::Block:
        out << "block\n";
        dumpStmts(out, fn, s.body, indent + 1);
        break;
    case StmtKind::Try:
        out << "try snapshot=" << fn.locals[s.local].name << '#' << s.local
            << " message=" << s.msg_local << '\n';
        dumpStmts(out, fn, s.body, indent + 1);
        out << pad << "catch\n";
        dumpStmts(out, fn, s.else_body, indent + 1);
        break;
    }
}

void dumpStmts(std::ostream& out, const Function& fn, const std::vector<Stmt>& body, int indent) {
    for (const Stmt& s : body) {
        dumpStmt(out, fn, s, indent);
    }
}

} // namespace

void dumpModule(std::ostream& out, const Module& m) {
    if (!m.panic_function.empty()) {
        out << "register(panic) " << m.panic_function << '\n';
    }
    if (!m.alloc_function.empty()) {
        out << "register(alloc) " << m.alloc_function << '\n';
    }
    if (!m.free_function.empty()) {
        out << "register(free) " << m.free_function << '\n';
    }
    if (!m.unwind_function.empty()) {
        out << "register(unwind) " << m.unwind_function << '\n';
    }
    if (!m.args_function.empty()) {
        out << "register(args) " << m.args_function << '\n';
    }
    for (const std::string& init : m.init_functions) {
        out << "init " << init << '\n';
    }
    for (const std::string& fini : m.fini_functions) {
        out << "fini " << fini << '\n';
    }
    for (const Enum& e : m.enums) {
        out << "enum " << e.name << ": " << typeName(e.underlying) << '\n';
        for (const EnumMemberValue& v : e.members) {
            out << "  " << v.name << " = "
                << (v.text.empty() ? std::to_string(v.int_value) : v.text) << '\n';
        }
    }
    for (const Aggregate& a : m.aggregates) {
        out << (a.is_class ? "class " : (a.is_union ? "union " : "struct ")) << a.name;
        if (!a.base.empty()) {
            out << " : " << a.base;
        }
        out << (a.opaque ? " (opaque)\n" : "\n");
        for (const Field& f : a.fields) {
            out << "  " << f.name << ": " << typeName(f.type) << '\n';
        }
    }
    for (const Interface& i : m.interfaces) {
        out << "interface " << i.name << '\n';
        for (const InterfaceMethod& im : i.methods) {
            out << "  fn " << im.name << '(';
            for (std::size_t k = 0; k < im.params.size(); ++k) {
                out << (k != 0 ? ", " : "") << typeName(im.params[k]);
            }
            out << ") -> " << typeName(im.ret) << '\n';
        }
    }
    for (const Impl& i : m.impls) {
        out << "impl " << i.interface << " for " << i.type << '\n';
    }
    for (const Global& g : m.globals) {
        out << (g.constant ? "const " : "global ") << g.name << ": " << typeName(g.type);
        if (g.init) {
            out << " = ";
            dumpExpr(out, nullptr, *g.init);
        }
        out << '\n';
    }
    for (const Function& f : m.functions) {
        out << "fn " << f.name << '(';
        for (std::uint32_t i = 0; i < f.param_count; ++i) {
            out << (i != 0 ? ", " : "") << f.locals[i].name << ": " << typeName(f.locals[i].type);
        }
        out << ") -> " << typeName(f.ret) << (f.has_body ? "\n" : " (no body)\n");
        dumpStmts(out, f, f.body, 1);
        if (!f.unwind_body.empty()) {
            out << "  unwind\n";
            dumpStmts(out, f, f.unwind_body, 2);
        }
    }
}

} // namespace xlang::hir
