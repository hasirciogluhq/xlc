#include "check.h"
#include "lowered.h"

// Return rules: a function with `->` returns on every path (E0764), a function without `->` is
// `void` and may leave early, and `-> never` marks a function that does not come back (E0765).

namespace {

using xlang_test::Lowered;

const std::string kDie = "fn die() -> never { while true { } }\n";

void testValueFunctionMustReturnOnEveryPath() {
    CHECK(Lowered("fn f(c: bool) -> i32 {\n    if c { return 1 }\n}\n").failedWith("E0764"));
    CHECK(Lowered("fn f() -> i32 {\n    while true { break }\n}\n").failedWith("E0764"));
    const Lowered both("fn f(c: bool) -> i32 {\n    if c { return 1 } else { return 2 }\n}\n");
    CHECK(!both.diag->hasErrors());
    const Lowered loop("fn f() -> i32 {\n    while true { }\n}\n");
    CHECK(!loop.diag->hasErrors());
}

void testVoidFunctionMayReturnEarly() {
    const Lowered r("fn f(c: bool, p: *i32) {\n    if c { return }\n    *p = 1\n}\n"
                    "fn g() { }\n");
    CHECK(!r.diag->hasErrors());
    CHECK(Lowered("fn f() { return 1 }\n").failedWith("E0708"));
}

void testNeverCallEndsThePath() {
    const Lowered r(kDie + "fn f(c: bool) -> i32 {\n    if c { return 1 }\n    die()\n}\n"
                           "fn g(stop: fn() -> never) -> i32 { stop() }\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("never"));
}

void testNeverFunctionMustNotReachItsEnd() {
    CHECK(Lowered("fn die() -> never { }\n").failedWith("E0764"));
    CHECK(Lowered("fn die(c: bool) -> never {\n    if c { while true { } }\n}\n").failedWith("E0764"));
    CHECK(Lowered("fn die() -> never { return }\n").failedWith("E0765"));
}

void testNeverIsOnlyAReturnType() {
    CHECK(Lowered("fn f() { local x: never }\n").failedWith("E0765"));
    CHECK(Lowered("fn f(x: never) { }\n").failedWith("E0765"));
    CHECK(Lowered("fn f() -> *never { while true { } }\n").failedWith("E0765"));
    CHECK(Lowered(kDie + "fn f() { local x = die() }\n").failedWith("E0708"));
    CHECK(Lowered(kDie + "fn f(c: bool) -> i32 { return c ? 1 : die() }\n").failedWith("E0708"));
}

void testExhaustiveSwitchEndsThePath() {
    const std::string e = "enum Shape {\n Dot\n Circle(i32)\n}\n";
    const Lowered all(e + "fn area(s: Shape) -> i32 {\n"
                          "    switch s {\n"
                          "        case Shape.Dot { return 0 }\n"
                          "        case Shape.Circle(r) { return r * r }\n"
                          "    }\n"
                          "}\n");
    CHECK(!all.diag->hasErrors());
    CHECK(Lowered(e + "fn area(s: Shape) -> i32 {\n"
                      "    switch s {\n"
                      "        case Shape.Dot { return 0 }\n"
                      "        case Shape.Circle(r) { }\n"
                      "    }\n"
                      "}\n")
              .failedWith("E0764"));
}

void testPanicRoleIsNever() {
    CHECK(Lowered("register(panic) fn panic(msg: *u8) { }\n").failedWith("E0503"));
    const Lowered r("register(panic) fn panic(msg: *u8) -> never { while true { } }\n"
                    "fn f(c: bool) -> i32 {\n    if c { return 1 }\n    panic(\"no\")\n}\n");
    CHECK(!r.diag->hasErrors());
}

} // namespace

void runReturnTests() {
    RUN(testValueFunctionMustReturnOnEveryPath);
    RUN(testVoidFunctionMayReturnEarly);
    RUN(testNeverCallEndsThePath);
    RUN(testNeverFunctionMustNotReachItsEnd);
    RUN(testNeverIsOnlyAReturnType);
    RUN(testExhaustiveSwitchEndsThePath);
    RUN(testPanicRoleIsNever);
}
