#include "check.h"
#include "lowered.h"

// The MIR builder (src/mir/builder): the blocks, statements and terminators each HIR form becomes,
// checked on the `--dump-mir` text.

using xlang_test::Lowered;

namespace {

const std::string kRes = "class Res {\n    id: i32\n    fn drop(self) { }\n}\n"
                         "fn take(r: Res) { }\n";

// Every operand that is not a constant is read into its own temporary first.
void testOperandsAreTemporaries() {
    const Lowered r("fn f(a: i32, b: i32) -> i32 {\n    return a + b * 2\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.hasMir("_3 = copy _0"));
    CHECK(r.hasMir("_5 = copy _1"));
    CHECK(r.hasMir("_4 = move _5 * const 2"));
    CHECK(r.hasMir("_2 = move _3 + move _4"));
    CHECK(r.hasMir("return move _2"));
}

// `if` branches to two blocks that join; a `void` function returns `()` at its end.
void testIfJoins() {
    const Lowered r("fn f(c: bool) {\n    local x = 0\n    if c { x = 1 } else { x = 2 }\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.hasMir("branch move _2 -> bb1, bb2"));
    CHECK(r.hasMir("  bb1:\n    _1 = const 1\n    goto bb3"));
    CHECK(r.hasMir("  bb2:\n    _1 = const 2\n    goto bb3"));
    CHECK(r.hasMir("  bb3:\n    return const ()"));
}

// A loop has a test block, a body and an exit; `break` jumps to the exit, `continue` to the test
// (or to the step of a `for`).
void testLoopsAndJumps() {
    const Lowered w("fn f(n: i32) {\n    local i = 0\n    while i < n {\n        i++\n"
                    "        if i == 3 { break }\n        continue\n    }\n}\n");
    CHECK(!w.diag->hasErrors());
    CHECK(w.hasMir("    goto bb1\n  bb1:"));
    CHECK(w.hasMir("-> bb2, bb3"));
    const Lowered s("fn f(n: i32) {\n    for i in 0..n {\n        continue\n    }\n}\n");
    CHECK(!s.diag->hasErrors());
    // The body (bb2) continues at the step (bb3), which goes back to the test (bb1).
    CHECK(s.hasMir("  bb2:\n    goto bb3"));
    CHECK(s.hasMir("  bb3:\n"));
}

// `&&` evaluates its right side only when the left one is true.
void testShortCircuit() {
    const Lowered r("fn f(a: bool, b: bool) -> bool {\n    return a && b\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.hasMir("branch move _3 -> bb1, bb2"));
    CHECK(r.hasMir("  bb2:\n    _2 = const false"));
    CHECK(r.hasMir("  bb1:\n    _2 = copy _1\n    goto bb3"));
}

// A call ends its block; a call that never returns has no successor.
void testCallsEndBlocks() {
    const Lowered r("fn g(x: i32) -> i32 { return x }\n"
                    "fn stop() -> never { while true { } }\n"
                    "fn f() -> i32 {\n    local y = g(1)\n    stop()\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.hasMir("_0 = call g(const 1) -> bb1"));
    CHECK(r.hasMir("call stop() -> never"));
}

// A move of a droppable local clears its flag; the drop at the end of the scope is a `Drop`
// terminator behind the flag test, not a call that borrows the local.
void testMovesAndDrops() {
    const Lowered r(kRes + "fn f() {\n    local r = Res { id: 1 }\n    take(r)\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.hasMir("_1 = const false\n    _2 = move _0\n    call take(move _2)"));
    CHECK(r.hasMir("branch copy _1 -> bb2, bb4"));
    CHECK(r.hasMir("drop _0 by Res$dropGlue -> bb3"));
}

// Every call in a `try` body unwinds to the `catch` block.
void testTryUnwindsToCatch() {
    const Lowered r("register(unwind) fn unwind(op: i32, snap: *usize, msg: *u8) { }\n"
                    "fn g() { }\n"
                    "fn f() {\n    try {\n        g()\n    } catch e {\n        g()\n    }\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.hasMir("try _0, _1 -> bb1, bb2"));
    CHECK(r.hasMir("  bb1:\n    call g() -> bb4 unwind bb2"));
    // The handler itself is outside the `try`: its calls have no unwind edge.
    CHECK(r.hasMir("  bb2:\n    _5 = &_1\n    _2 = move _5 as *u8\n    call g() -> bb6\n"));
}

} // namespace

void runMirTests() {
    RUN(testOperandsAreTemporaries);
    RUN(testIfJoins);
    RUN(testLoopsAndJumps);
    RUN(testShortCircuit);
    RUN(testCallsEndBlocks);
    RUN(testMovesAndDrops);
    RUN(testTryUnwindsToCatch);
}
