// E0780: use of a moved value.
//
// A forward dataflow over the MIR of one function. At every point each local is owned, moved, or
// moved on some paths only. A `move` operand of a whole local moves it; an assignment to the whole
// local owns it again; a `Drop` ends it without reading it (the drop flag guards it). Any other
// read of a moved or maybe-moved local (a copy, a move, `&x`, a field, a write into a part of it)
// is reported.
//
// Only the user's own locals and parameters are reported: a hidden local of lowering is moved
// exactly where lowering decided, so a finding on one would be a compiler bug, not a user error.
//
// Only a definite move (moved on every reaching path) is reported. A maybe-moved local is tracked
// but not reported: the released stdlib writes loops in the `done`-flag style, where the paths
// that move a local also set the flag that exits the loop, so the merge at the loop header sees a
// move that no continuing iteration can reach. Revisit maybe-moved reporting together with a
// stdlib release that exits such loops with `break`.

#include "analysis/mir/moves.hpp"

#include "mir/dataflow/forward.hpp"

#include <format>
#include <optional>
#include <utility>
#include <vector>

namespace xlang::analysis::mir {

namespace {

namespace m = xlang::mir;

enum class Status : std::uint8_t { Owned, Moved, MaybeMoved };

[[nodiscard]] bool sameSpan(const Span& a, const Span& b) {
    return a.file == b.file && a.line == b.line && a.column == b.column;
}

/// The status of every local at one point, and where each one was last seen moved.
struct State {
    bool reached{false};
    std::vector<Status> status;
    std::vector<std::optional<Span>> moved_at;

    /// Merges an incoming edge: a local moved on one edge and owned on the other is maybe moved.
    bool join(const State& other) {
        if (!other.reached) {
            return false;
        }
        if (!reached) {
            *this = other;
            return true;
        }
        bool changed = false;
        for (std::size_t i = 0; i < status.size(); ++i) {
            if (status[i] != other.status[i] && status[i] != Status::MaybeMoved) {
                status[i] = Status::MaybeMoved;
                changed = true;
            }
            if (!moved_at[i]) {
                moved_at[i] = other.moved_at[i];
            }
        }
        return changed;
    }
};

/// Applies the effect of statements and terminators to a `State`; with `findings` set it also
/// reports every read of a moved local.
class Walker {
public:
    Walker(const m::Body& body, State& state, Findings* findings)
        : body_(body), state_(state), findings_(findings) {}

    void block(const m::BasicBlock& b) {
        for (const m::Statement& s : b.statements) {
            statement(s);
        }
        terminator(b.terminator);
    }

private:
    void statement(const m::Statement& s) {
        rvalue(s.value, s.span);
        write(s.place, s.span);
    }

    void terminator(const m::Terminator& t) {
        switch (t.kind) {
        case m::Terminator::Kind::Branch:
        case m::Terminator::Kind::Return: operand(t.operand, t.span); break;
        case m::Terminator::Kind::Call:
            if (t.callee.kind == m::Callee::Kind::Indirect || t.callee.kind == m::Callee::Kind::Interface) {
                operand(t.callee.value, t.span);
            }
            for (const m::Operand& a : t.args) {
                operand(a, t.span);
            }
            if (t.destination) {
                write(*t.destination, t.span);
            }
            break;
        case m::Terminator::Kind::Drop:
            if (t.place.isLocal()) {
                moved(t.place.local, t.span);
            }
            break;
        case m::Terminator::Kind::Goto:
        case m::Terminator::Kind::Try:
        case m::Terminator::Kind::Unreachable: break;
        }
    }

    void rvalue(const m::Rvalue& r, const Span& at) {
        for (const m::Operand& o : r.operands) {
            operand(o, at);
        }
        if (r.kind == m::Rvalue::Kind::AddressOf || r.kind == m::Rvalue::Kind::Slice) {
            read(r.place, at);
        }
    }

    void operand(const m::Operand& o, const Span& at) {
        if (o.kind == m::Operand::Kind::Constant) {
            return;
        }
        read(o.place, at);
        if (o.kind == m::Operand::Kind::Move && o.place.isLocal()) {
            moved(o.place.local, at);
        }
    }

    /// A store owns a whole local again; a store into a part of one reads the rest of it.
    void write(const m::Place& p, const Span& at) {
        if (p.isLocal()) {
            state_.status[p.local] = Status::Owned;
            return;
        }
        read(p, at);
    }

    void read(const m::Place& p, const Span& at) {
        check(p.local, at);
        for (const m::Projection& pr : p.projections) {
            if (pr.kind == m::Projection::Kind::Index) {
                check(pr.index, at);
            }
        }
    }

    void moved(m::LocalId local, const Span& at) {
        state_.status[local] = Status::Moved;
        state_.moved_at[local] = at;
    }

    void check(m::LocalId local, const Span& at) {
        const Status s = state_.status[local];
        const m::Local& l = body_.locals[local];
        if (findings_ == nullptr || s != Status::Moved ||
            (l.kind != m::Local::Kind::User && l.kind != m::Local::Kind::Param)) {
            return;
        }
        Diagnostic d;
        d.code = "E0780";
        d.message = std::format("use of a moved value `{}`", l.name);
        const std::optional<Span>& from = state_.moved_at[local];
        if (from && sameSpan(*from, at)) {
            d.labels.push_back({at, "moved here in an earlier iteration of the loop"});
        } else {
            d.labels.push_back({at, "used here after the move"});
            if (from) {
                d.labels.push_back({*from, s == Status::Moved ? "moved here" : "moved here on some paths"});
            }
        }
        d.detail = std::format("`{}` was moved and not assigned again", l.name);
        d.reason = "a moved value belongs to its new owner, which may already have dropped it";
        d.fix = std::format("pass `&{0}` to borrow it, move a copy (`{0}.clone()`), or assign `{0}` "
                            "a new value before this use",
                            l.name);
        findings_->add(std::move(d));
    }

    const m::Body& body_;
    State& state_;
    Findings* findings_;
};

/// The analysis `solveForward` runs: every local starts owned.
struct MoveAnalysis {
    using State = mir::State;
    const m::Body& body;

    [[nodiscard]] State entry() const {
        return {true, std::vector(body.locals.size(), Status::Owned),
                std::vector<std::optional<Span>>(body.locals.size())};
    }
    [[nodiscard]] State bottom() const {
        return {false, std::vector(body.locals.size(), Status::Owned),
                std::vector<std::optional<Span>>(body.locals.size())};
    }
    void transfer(const m::BasicBlock& b, State& s) const { Walker(body, s, nullptr).block(b); }
};

} // namespace

/// Solves the move states, then walks each reached block once more from its entry state to report.
void checkMoves(const xlang::mir::Body& body, Findings& findings) {
    const MoveAnalysis analysis{body};
    std::vector<State> in = m::dataflow::solveForward(body, analysis);
    for (std::size_t b = 0; b < body.blocks.size(); ++b) {
        if (in[b].reached) {
            Walker(body, in[b], &findings).block(body.blocks[b]);
        }
    }
}

} // namespace xlang::analysis::mir
