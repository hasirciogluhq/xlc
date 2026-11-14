#pragma once

#include "xlang/mir/mir.h"

#include <deque>
#include <vector>

namespace xlang::mir::dataflow {

/// What a forward analysis provides to `solveForward`.
///
/// `State` is the fact at one program point: copyable, with `==`, and `join(other)` that merges
/// the facts of two incoming edges and returns whether `*this` changed. Joins must only move up a
/// lattice of finite height, so the solver ends.
template <typename A>
concept ForwardAnalysis = requires(const A& a, typename A::State& s, const BasicBlock& b) {
    { a.entry() } -> std::same_as<typename A::State>;   // the fact on entry to block 0
    { a.bottom() } -> std::same_as<typename A::State>;  // the fact of a block no edge reached yet
    { a.transfer(b, s) };                               // applies the block's effect to `s`
    { s.join(s) } -> std::same_as<bool>;
};

/// The fact on entry to every block of `body`, at the fixed point of `analysis`.
/// @invariant the worklist is FIFO from block 0, so the result and the step count depend only on the body, never on hashing or addresses.
/// @cost each block is processed once per change of its entry fact: at most (lattice height) times.
template <ForwardAnalysis A>
[[nodiscard]] std::vector<typename A::State> solveForward(const Body& body, const A& analysis) {
    std::vector<typename A::State> in(body.blocks.size(), analysis.bottom());
    if (body.blocks.empty()) {
        return in;
    }
    in[0] = analysis.entry();
    std::deque<BlockId> work{0};
    std::vector<bool> queued(body.blocks.size(), false);
    queued[0] = true;
    while (!work.empty()) {
        const BlockId b = work.front();
        work.pop_front();
        queued[b] = false;
        typename A::State out = in[b];
        analysis.transfer(body.blocks[b], out);
        for (const BlockId next : body.blocks[b].terminator.successors()) {
            if (in[next].join(out) && !queued[next]) {
                queued[next] = true;
                work.push_back(next);
            }
        }
    }
    return in;
}

} // namespace xlang::mir::dataflow
