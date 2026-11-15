#include "analysis/xl_bridge.hpp"

#include <cstdint>
#include <type_traits>
#include <vector>

// The xlang side: src/analysis/abi.xl (the records) and src/analysis/analysis.xl (the entry points).
extern "C" {
struct XamStr {
    const std::uint8_t* bytes;
    std::uint32_t len;
};
struct XamLocal {
    std::uint32_t kind;
    XamStr name;
};
struct XamPlace {
    std::uint32_t local, projFirst, projCount;
};
struct XamProj {
    std::uint32_t kind, index;
};
struct XamOperand {
    std::uint32_t kind;
    XamPlace place;
};
struct XamSpan {
    std::uint32_t file, line, column, endLine, endColumn;
};
struct XamStmt {
    XamPlace place;
    std::uint32_t rvKind;
    XamPlace rvPlace;
    std::uint32_t opFirst, opCount;
    XamSpan span;
};
struct XamTerm {
    std::uint32_t kind;
    XamOperand operand;
    std::uint32_t calleeKind;
    XamOperand calleeValue;
    std::uint32_t argFirst, argCount, hasDest;
    XamPlace dest, place;
    std::uint32_t targetFirst, targetCount, hasUnwind, unwind;
    XamSpan span;
};
struct XamBlock {
    std::uint32_t stmtFirst, stmtCount;
    XamTerm term;
};
struct XamBody {
    const XamLocal* locals;
    std::uint32_t localCount;
    const XamBlock* blocks;
    std::uint32_t blockCount;
    const XamStmt* stmts;
    const XamProj* projs;
    const XamOperand* operands;
    const std::uint32_t* targets;
};

void* xlcAnalysisNew();
void xlcAnalysisBody(void* findings, const XamBody* body);
void xlcAnalysisFinish(void* findings, void* engine);
}

namespace xlang::analysis::xl {

namespace {

using namespace xlang::mir;

// The numbers in abi.xl are the declaration order of the C++ enums.
constexpr auto n = [](auto e) { return static_cast<std::uint32_t>(e); };
static_assert(n(Local::Kind::Param) == 0 && n(Local::Kind::User) == 1 &&
              n(Local::Kind::DropFlag) == 2 && n(Local::Kind::Temp) == 3);
static_assert(n(Projection::Kind::Field) == 0 && n(Projection::Kind::Deref) == 1 &&
              n(Projection::Kind::Index) == 2);
static_assert(n(Operand::Kind::Copy) == 0 && n(Operand::Kind::Move) == 1 &&
              n(Operand::Kind::Constant) == 2);
static_assert(n(Rvalue::Kind::Use) == 0 && n(Rvalue::Kind::AddressOf) == 1 &&
              n(Rvalue::Kind::Slice) == 8 && n(Rvalue::Kind::ToInterface) == 9);
static_assert(n(Terminator::Kind::Goto) == 0 && n(Terminator::Kind::Branch) == 1 &&
              n(Terminator::Kind::Call) == 2 && n(Terminator::Kind::Drop) == 3 &&
              n(Terminator::Kind::Try) == 4 && n(Terminator::Kind::Return) == 5 &&
              n(Terminator::Kind::Unreachable) == 6);
static_assert(n(Callee::Kind::Direct) == 0 && n(Callee::Kind::Indirect) == 1 &&
              n(Callee::Kind::Interface) == 3 && n(Callee::Kind::Builtin) == 5);
static_assert(sizeof(XamSpan) == sizeof(Span) && std::is_standard_layout_v<XamSpan>);

XamSpan flat(const Span& s) {
    return {s.file, s.line, s.column, s.end_line, s.end_column};
}

class Flattener {
public:
    XamPlace place(const Place& p) {
        XamPlace out{p.local, static_cast<std::uint32_t>(projs_.size()),
                     static_cast<std::uint32_t>(p.projections.size())};
        for (const Projection& pr : p.projections) {
            projs_.push_back({n(pr.kind), pr.kind == Projection::Kind::Index ? pr.index : 0});
        }
        return out;
    }

    XamOperand operand(const Operand& o) {
        return {n(o.kind), o.kind == Operand::Kind::Constant ? XamPlace{} : place(o.place)};
    }

    XamBody body(const Body& b) {
        for (const Local& l : b.locals) {
            locals_.push_back({n(l.kind),
                               {reinterpret_cast<const std::uint8_t*>(l.name.data()),
                                static_cast<std::uint32_t>(l.name.size())}});
        }
        // Records hold indices into the arenas, so the vectors may grow freely; the pointers in
        // the returned XamBody are taken once, after the last append.
        for (const BasicBlock& bb : b.blocks) {
            XamBlock out{};
            out.stmtFirst = static_cast<std::uint32_t>(stmts_.size());
            out.stmtCount = static_cast<std::uint32_t>(bb.statements.size());
            for (const Statement& s : bb.statements) {
                XamStmt st{};
                st.place = place(s.place);
                st.rvKind = n(s.value.kind);
                st.rvPlace = place(s.value.place);
                st.opFirst = static_cast<std::uint32_t>(operands_.size());
                st.opCount = static_cast<std::uint32_t>(s.value.operands.size());
                for (const Operand& o : s.value.operands) {
                    operands_.push_back(operand(o));
                }
                st.span = flat(s.span);
                stmts_.push_back(st);
            }
            out.term = terminator(bb.terminator);
            blocks_.push_back(out);
        }
        return {locals_.data(), static_cast<std::uint32_t>(locals_.size()), blocks_.data(),
                static_cast<std::uint32_t>(blocks_.size()), stmts_.data(), projs_.data(),
                operands_.data(), targets_.data()};
    }

private:
    XamTerm terminator(const Terminator& t) {
        XamTerm out{};
        out.kind = n(t.kind);
        out.operand = operand(t.operand);
        out.calleeKind = n(t.callee.kind);
        out.calleeValue = operand(t.callee.value);
        out.argFirst = static_cast<std::uint32_t>(operands_.size());
        out.argCount = static_cast<std::uint32_t>(t.args.size());
        for (const Operand& a : t.args) {
            operands_.push_back(operand(a));
        }
        out.hasDest = t.destination ? 1 : 0;
        if (t.destination) {
            out.dest = place(*t.destination);
        }
        out.place = place(t.place);
        out.targetFirst = static_cast<std::uint32_t>(targets_.size());
        out.targetCount = static_cast<std::uint32_t>(t.targets.size());
        targets_.insert(targets_.end(), t.targets.begin(), t.targets.end());
        out.hasUnwind = t.unwind ? 1 : 0;
        out.unwind = t.unwind.value_or(0);
        out.span = flat(t.span);
        return out;
    }

    std::vector<XamLocal> locals_;
    std::vector<XamBlock> blocks_;
    std::vector<XamStmt> stmts_;
    std::vector<XamProj> projs_;
    std::vector<XamOperand> operands_;
    std::vector<std::uint32_t> targets_;
};

} // namespace

void run(const Module& mir, DiagnosticEngine& diagnostics) {
    void* session = xlcAnalysisNew();
    for (const Body& b : mir.bodies) {
        Flattener flattener;
        const XamBody flat = flattener.body(b);
        xlcAnalysisBody(session, &flat);
    }
    xlcAnalysisFinish(session, diagnostics.handle());
}

} // namespace xlang::analysis::xl
