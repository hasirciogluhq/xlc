#include "xlang/mir/mir.h"

#include <format>
#include <ostream>
#include <string>

namespace xlang::mir {

// The text form `--dump-mir` prints and the unit tests match:
//
//   fn name -> R
//     let _0: T  (param a)
//     bb0:
//       _2 = move _1
//       call f(move _2) -> _3, bb1 unwind bb4
//
// Locals are `_n` (their HIR id), blocks `bbN`.

namespace {

[[nodiscard]] const char* kindName(Local::Kind k) {
    switch (k) {
    case Local::Kind::Param: return "param";
    case Local::Kind::User: return "user";
    case Local::Kind::DropFlag: return "flag";
    case Local::Kind::Temp: return "temp";
    }
    return "?";
}

[[nodiscard]] std::string place(const Place& p) {
    std::string out = std::format("_{}", p.local);
    for (const Projection& pr : p.projections) {
        switch (pr.kind) {
        case Projection::Kind::Field: out += std::format(".{}", pr.field); break;
        case Projection::Kind::Deref: out = std::format("(*{})", out); break;
        case Projection::Kind::Index: out += std::format("[_{}{}]", pr.index, pr.checked ? " checked" : ""); break;
        }
    }
    return out;
}

[[nodiscard]] std::string constant(const Constant& c) {
    switch (c.kind) {
    case Constant::Kind::None: return "()";
    case Constant::Kind::Int: return std::format("{}", std::get<std::uint64_t>(c.value));
    case Constant::Kind::Float: return std::format("{}", std::get<double>(c.value));
    case Constant::Kind::Bool: return std::get<bool>(c.value) ? "true" : "false";
    case Constant::Kind::Str: return std::format("\"{}\"", std::get<std::string>(c.value));
    case Constant::Kind::Nil: return "nil";
    case Constant::Kind::Function: return "fn " + std::get<std::string>(c.value);
    case Constant::Kind::Global: return "&" + std::get<std::string>(c.value);
    }
    return "?";
}

[[nodiscard]] std::string operand(const Operand& o) {
    switch (o.kind) {
    case Operand::Kind::Copy: return "copy " + place(o.place);
    case Operand::Kind::Move: return "move " + place(o.place);
    case Operand::Kind::Constant: return "const " + constant(o.constant);
    }
    return "?";
}

[[nodiscard]] std::string operands(const std::vector<Operand>& list) {
    std::string out;
    for (const Operand& o : list) {
        out += (out.empty() ? "" : ", ") + operand(o);
    }
    return out;
}

[[nodiscard]] std::string aggregate(const Rvalue& r) {
    std::string out;
    for (std::size_t i = 0; i < r.operands.size(); ++i) {
        out += out.empty() ? "" : ", ";
        out += r.fields.empty() ? operand(r.operands[i]) : std::format(".{} = {}", r.fields[i], operand(r.operands[i]));
    }
    return std::format("{} {{ {} }}", typeToString(r.type), out);
}

[[nodiscard]] std::string rvalue(const Rvalue& r) {
    const char* checked = r.checked ? " checked" : "";
    switch (r.kind) {
    case Rvalue::Kind::Use: return operands(r.operands);
    case Rvalue::Kind::AddressOf: return "&" + place(r.place);
    case Rvalue::Kind::Unary: return std::format("{}{}", unaryOpName(r.unary_op), operands(r.operands));
    case Rvalue::Kind::Binary:
        return std::format("{} {} {}{}", operand(r.operands.at(0)), binaryOpName(r.binary_op), operand(r.operands.at(1)), checked);
    case Rvalue::Kind::Cast: return std::format("{} as {}", operands(r.operands), typeToString(r.type));
    case Rvalue::Kind::Aggregate: return aggregate(r);
    case Rvalue::Kind::Repeat: return std::format("[{}; {}]", operands(r.operands), r.count);
    case Rvalue::Kind::Zero: return std::format("zeroed {}", typeToString(r.type));
    case Rvalue::Kind::Slice:
        return std::format("{}[{}..{}]{}", place(r.place), operand(r.operands.at(0)), operand(r.operands.at(1)), checked);
    case Rvalue::Kind::ToInterface:
        return std::format("{} as {} by {}", operands(r.operands), typeToString(r.type), r.name);
    }
    return "?";
}

[[nodiscard]] const char* builtinName(Builtin b) {
    switch (b) {
    case Builtin::Alloc: return "alloc";
    case Builtin::AtomicLoad: return "atomic.load";
    case Builtin::AtomicStore: return "atomic.store";
    case Builtin::AtomicXchg: return "atomic.xchg";
    case Builtin::AtomicCas: return "atomic.cas";
    case Builtin::AtomicAdd: return "atomic.add";
    case Builtin::Fence: return "fence";
    case Builtin::Asm: return "asm";
    case Builtin::UnwindPads: return "unwindPads";
    case Builtin::UnwindPadCount: return "unwindPadCount";
    }
    return "?";
}

[[nodiscard]] std::string callee(const Callee& c) {
    switch (c.kind) {
    case Callee::Kind::Direct: return c.name;
    case Callee::Kind::Indirect: return std::format("({})", operand(c.value));
    case Callee::Kind::Virtual: return std::format("virtual[{}] {}", c.index, c.name);
    case Callee::Kind::Interface: return std::format("({}).{}[{}]", operand(c.value), c.name, c.index);
    case Callee::Kind::Syscall: return "syscall";
    case Callee::Kind::Builtin:
        return c.builtin == Builtin::Alloc ? std::format("alloc<{}>", typeToString(c.type)) : builtinName(c.builtin);
    }
    return "?";
}

[[nodiscard]] std::string targets(const Terminator& t) {
    std::string out;
    for (const BlockId b : t.targets) {
        out += std::format("{}bb{}", out.empty() ? "" : ", ", b);
    }
    if (t.unwind) {
        out += std::format("{}unwind bb{}", out.empty() ? "" : " ", *t.unwind);
    }
    return out;
}

[[nodiscard]] std::string terminator(const Terminator& t) {
    switch (t.kind) {
    case Terminator::Kind::Goto: return "goto " + targets(t);
    case Terminator::Kind::Branch: return std::format("branch {} -> {}", operand(t.operand), targets(t));
    case Terminator::Kind::Call: {
        const std::string result = t.destination ? place(*t.destination) + " = " : "";
        const std::string next = t.targets.empty() && !t.unwind ? "never" : targets(t);
        return std::format("{}call {}({}) -> {}", result, callee(t.callee), operands(t.args), next);
    }
    case Terminator::Kind::Drop: return std::format("drop {} by {} -> {}", place(t.place), t.glue, targets(t));
    case Terminator::Kind::Try:
        return std::format("try {}, _{} -> {}", place(t.place), t.message, targets(t));
    case Terminator::Kind::Return: return "return " + operand(t.operand);
    case Terminator::Kind::Unreachable: return "unreachable";
    }
    return "?";
}

void dumpBody(std::ostream& out, const Body& b) {
    out << std::format("fn {} -> {}\n", b.name, typeToString(b.ret));
    // `std::views::enumerate` is C++23 but missing from the libc++ of the oldest supported LLVM.
    for (std::size_t id = 0; id < b.locals.size(); ++id) {
        const Local& l = b.locals[id];
        out << std::format("  let _{}: {}  ({}{}{})\n", id, typeToString(l.type), kindName(l.kind),
                           l.name.empty() ? "" : " ", l.name);
    }
    for (std::size_t id = 0; id < b.blocks.size(); ++id) {
        const BasicBlock& block = b.blocks[id];
        out << std::format("  bb{}:\n", id);
        for (const Statement& s : block.statements) {
            out << std::format("    {} = {}\n", place(s.place), rvalue(s.value));
        }
        out << "    " << terminator(block.terminator) << '\n';
    }
}

} // namespace

void dumpModule(std::ostream& out, const Module& module) {
    for (const Body& b : module.bodies) {
        dumpBody(out, b);
    }
}

} // namespace xlang::mir
