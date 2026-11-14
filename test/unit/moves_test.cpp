#include "check.h"
#include "lowered.h"

// The move check (src/analysis/mir/moves.cpp): E0780 for a use of a value moved on every reaching
// path, and no finding where the value was assigned again, borrowed, dropped, or moved on some
// paths only.

using xlang_test::Lowered;

namespace {

const std::string kRes = "class Res {\n    id: i32\n    fn drop(self) { }\n}\n"
                         "fn take(r: Res) { }\n"
                         "fn peek(r: *Res) -> i32 { return r.id }\n";

[[nodiscard]] Lowered body(const std::string& text) {
    return Lowered(kRes + "fn f(c: bool) {\n" + text + "}\n");
}

void testUseAfterMove() {
    CHECK(body("    local r = Res { id: 1 }\n    take(r)\n    take(r)\n").failedWith("E0780"));
    CHECK(body("    local r = Res { id: 1 }\n    take(r)\n    peek(&r)\n").failedWith("E0780"));
    CHECK(body("    local r = Res { id: 1 }\n    take(r)\n    local n = r.id\n").failedWith("E0780"));
    CHECK(body("    local r = Res { id: 1 }\n    take(r)\n    r.id = 2\n").failedWith("E0780"));
    CHECK(Lowered(kRes + "fn f(r: Res) {\n    take(r)\n    take(r)\n}\n").failedWith("E0780"));
}

// A move on one branch makes the value maybe-moved after the join. A maybe-moved local is tracked
// but not reported (see the note in src/analysis/mir/moves.cpp): only a definite move is E0780,
// so both shapes here compile until the stdlib exits such loops with `break`.
void testMoveOnSomePaths() {
    CHECK(!body("    local r = Res { id: 1 }\n    if c { take(r) }\n    take(r)\n").diag->hasErrors());
    CHECK(!body("    local r = Res { id: 1 }\n    for i in 0..3 {\n        take(r)\n    }\n")
               .diag->hasErrors());
}

void testNoFindingWhenOwnedAgain() {
    // Assigned again before the use.
    CHECK(!body("    local r = Res { id: 1 }\n    take(r)\n    r = Res { id: 2 }\n    take(r)\n")
               .diag->hasErrors());
    CHECK(!body("    local r = Res { id: 1 }\n    for i in 0..3 {\n        take(r)\n"
                "        r = Res { id: 2 }\n    }\n")
               .diag->hasErrors());
    // Moved on a path that leaves: the use after it is reached only on the other path.
    CHECK(!body("    local r = Res { id: 1 }\n    if c {\n        take(r)\n        return\n    }\n"
                "    take(r)\n")
               .diag->hasErrors());
    CHECK(!body("    local r = Res { id: 1 }\n    for i in 0..3 {\n        if c {\n"
                "            take(r)\n            break\n        }\n    }\n")
               .diag->hasErrors());
    // Borrowed, never moved; the drop at the end of the scope is not a use.
    CHECK(!body("    local r = Res { id: 1 }\n    peek(&r)\n    peek(&r)\n").diag->hasErrors());
    CHECK(!body("    local r = Res { id: 1 }\n    if c { take(r) }\n").diag->hasErrors());
}

// Plain data is copied, never moved.
void testCopiesAreNotMoves() {
    CHECK(!Lowered("fn g(x: i32) { }\nfn f() {\n    local x = 1\n    g(x)\n    g(x)\n}\n")
               .diag->hasErrors());
}

} // namespace

void runMoveTests() {
    RUN(testUseAfterMove);
    RUN(testMoveOnSomePaths);
    RUN(testNoFindingWhenOwnedAgain);
    RUN(testCopiesAreNotMoves);
}
