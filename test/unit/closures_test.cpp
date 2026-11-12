#include "check.h"
#include "lowered.h"

// Closures: `|a: i32, b: i32| -> i32 { ... }`, `|x| x * 2`, `move |x| ...`. A closure that
// captures nothing is a function pointer; one that captures is a struct called through
// `operator()`. See xlc/README.md, "Closures".

using xlang_test::Lowered;

namespace {

void testTypedClosureIsAFunctionPointer() {
    Lowered r("fn main() -> i32 {\n"
              "    local add = |a: i32, b: i32| -> i32 { return a + b }\n"
              "    return add(1, 2)\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn __closure0(a: i32, b: i32) -> i32"));
    CHECK(r.has("local add#0: fn(i32, i32) -> i32 = &fn __closure0"));
    CHECK(r.has("(call.indirect add#0 1:i32 2:i32)"));
}

void testExpressionBodyTakesItsReturnTypeFromTheValue() {
    Lowered r("fn main() -> i64 {\n"
              "    local widen = |a: i32| a as i64\n"
              "    local nothing = || { local z = 1 }\n"
              "    nothing()\n"
              "    return widen(3)\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn __closure0(a: i32) -> i64"));
    CHECK(r.has("fn __closure1() -> void"));
}

void testParametersComeFromTheContext() {
    Lowered r("fn apply(f: fn(i32) -> i32, x: i32) -> i32 { return f(x) }\n"
              "fn make() -> fn(i32) -> i32 { return |x| x + 1 }\n"
              "fn main() -> i32 {\n"
              "    local f: fn(i32) -> i32 = |x| x * 2\n"
              "    return apply(|x| x - 1, 4) + f(1) + make()(2)\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn __closure0(x: i32) -> i32"));
    CHECK(r.has("(call apply &fn __closure"));
}

void testCapturesAreBorrowedThroughAStruct() {
    Lowered r("fn main() -> i32 {\n"
              "    local base = 10\n"
              "    local count = 0\n"
              "    local bump = |n: i32| { count += n }\n"
              "    local addBase = |x: i32| x + base\n"
              "    bump(5)\n"
              "    return addBase(1) + count\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("struct __closure0\n  count: *i32"));
    CHECK(r.has("struct __closure1\n  base: *i32"));
    CHECK(r.has("fn __closure0$call(__env0: *__closure0, n: i32) -> void"));
    CHECK(r.has("(deref (field.ptr count __env0#0):*i32)"));
    CHECK(r.has("(struct __closure1 base=(addr base#"));
    CHECK(r.has("(call __closure0$call (addr bump#"));
}

void testMoveCopiesTheValue() {
    Lowered r("fn main() -> i32 {\n"
              "    local base = 10\n"
              "    local f = move |x: i32| x + base\n"
              "    base = 99\n"
              "    return f(1)\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("struct __closure0\n  base: i32"));
    CHECK(r.has("(struct __closure0 base=base#"));
}

void testGenericFunctionTakesAnyClosure() {
    Lowered r("fn apply<F>(f: F, x: i32) -> i32 { return f(x) }\n"
              "fn main() -> i32 {\n"
              "    local k = 3\n"
              "    local inc = |x: i32| x + k\n"
              "    local twice = |x: i32| x * 2\n"
              "    return apply(inc, 1) + apply(twice, 2)\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn apply<__closure0>(f: __closure0, x: i32) -> i32"));
    CHECK(r.has("(call __closure0$call (addr f#0)"));
    CHECK(r.has("fn apply<fn(i32) -> i32>"));
}

void testGenericCallAlsoConvertsToAnInterface() {
    // `F` is inferred while another argument (`&q`) converts to an interface parameter.
    Lowered r("interface Shape { fn area(self) -> i32 }\n"
              "struct Sq { s: i32 }\n"
              "impl Shape for Sq { fn area(self) -> i32 { return self.s } }\n"
              "class Calc {\n fn apply<F>(self, sh: Shape, f: F) -> i32 { return f(sh.area()) }\n}\n"
              "fn apply2<F>(sh: Shape, f: F) -> i32 { return f(sh.area()) }\n"
              "fn main() -> i32 {\n"
              "    local q = Sq { s: 3 }\n"
              "    local c = Calc { }\n"
              "    local m = 2\n"
              "    return c.apply(&q, |x: i32| x * m) + apply2(&q, |x: i32| x * m)\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
}

void testKeywordAsMemberName() {
    // After `fn` in a type body and after `.` a keyword is a name: `fn delete`, `c.delete(1)`.
    Lowered r("class Client {\n fn delete(self, x: i32) -> i32 { return x }\n}\n"
              "fn main() -> i32 {\n local c = Client { }\n return c.delete(1)\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("Client.delete"));
}

void testNestedClosuresCaptureThroughEachOther() {
    Lowered r("fn main() -> i32 {\n"
              "    local k = 3\n"
              "    local outer = |a: i32| {\n"
              "        local inner = |b: i32| a + b + k\n"
              "        return inner(1)\n"
              "    }\n"
              "    return outer(10)\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn __closure0$call(__env0: *__closure0, a: i32) -> i32"));
}

void testLocalsOfTheClosureShadowTheOuterOnes() {
    Lowered r("fn main() -> i32 {\n"
              "    local n = 1\n"
              "    local f = |x: i32| { local n = x\n return n }\n"
              "    return f(2) + n\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn __closure0(x: i32) -> i32")); // `n` is its own: nothing is captured
}

void testClosureInsideAGenericFunction() {
    Lowered r("fn id<T>(x: T) -> T {\n"
              "    local f = |y: T| y\n"
              "    return f(x)\n"
              "}\n"
              "fn main() -> i32 { return id<i32>(4) }\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(y: i32) -> i32"));
}

void testUntypedClosureInAnOverloadedCall() {
    Lowered r("fn run(a: i32, f: fn(i32) -> i32) -> i32 { return f(a) }\n"
              "fn run(a: i32, b: i32, f: fn(i32) -> i32) -> i32 { return f(a + b) }\n"
              "fn main() -> i32 { return run(1, |x| x + 1) + run(1, 2, |x| x * 2) }\n");
    CHECK(!r.diag->hasErrors());
}

void testClosureOperatorsAndPlainBars() {
    // `||` and `|` between values stay the operators; `move` is a name unless a bar follows.
    Lowered r("fn main() -> bool {\n"
              "    local move = 1\n"
              "    local a = true\n"
              "    local b = false\n"
              "    local c = move + 1\n"
              "    local bits = 4 | 2\n"
              "    return a || b\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
}

void testClosureErrors() {
    // No context for the parameter type.
    Lowered untyped("fn main() { local d = |x| x * 2 }\n");
    CHECK(untyped.failedWith("E0770"));
    // The function type has a different number of parameters.
    Lowered arity("fn f(g: fn(i32) -> i32) {}\n"
                  "fn main() { f(|a, b| a) }\n");
    CHECK(arity.failedWith("E0771"));
    // A `fn(...)` pointer has no environment.
    Lowered pointer("fn f(g: fn(i32) -> i32) {}\n"
                    "fn main() { local n = 1\n f(|x| x + n) }\n");
    CHECK(pointer.failedWith("E0773"));
    // `->` needs a block body.
    Lowered arrow("fn main() { local f = |x: i32| -> i32 x }\n");
    CHECK(arrow.failedWith("E0002"));
    // A closure with captures cannot be called with the wrong arguments.
    Lowered args("fn main() -> i32 {\n"
                 "    local n = 1\n"
                 "    local f = |x: i32| x + n\n"
                 "    return f(1, 2)\n"
                 "}\n");
    CHECK(args.failedWith("E0710"));
}

} // namespace

void runClosureTests() {
    RUN(testTypedClosureIsAFunctionPointer);
    RUN(testExpressionBodyTakesItsReturnTypeFromTheValue);
    RUN(testParametersComeFromTheContext);
    RUN(testCapturesAreBorrowedThroughAStruct);
    RUN(testMoveCopiesTheValue);
    RUN(testGenericFunctionTakesAnyClosure);
    RUN(testGenericCallAlsoConvertsToAnInterface);
    RUN(testKeywordAsMemberName);
    RUN(testNestedClosuresCaptureThroughEachOther);
    RUN(testLocalsOfTheClosureShadowTheOuterOnes);
    RUN(testClosureInsideAGenericFunction);
    RUN(testUntypedClosureInAnOverloadedCall);
    RUN(testClosureOperatorsAndPlainBars);
    RUN(testClosureErrors);
}
