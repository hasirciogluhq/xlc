#include "check.h"
#include "lowered.h"

// User-defined operators (`impl T { fn +(self: T, o: T) -> T }`): the parser turns each into a
// function named `operator<symbol>`, the lowering turns an operator on a struct or class into its
// call. See xlc/README.md, "Operator overloading".

using xlang_test::Lowered;

namespace {

constexpr const char* kVec2 =
    "struct Vec2 { x: f32, y: f32 }\n"
    "impl Vec2 {\n"
    "    fn +(self: Vec2, other: Vec2) -> Vec2 {\n"
    "        return Vec2 { x: self.x + other.x, y: self.y + other.y }\n"
    "    }\n"
    "    fn ==(self: Vec2, other: Vec2) -> bool {\n"
    "        return self.x == other.x && self.y == other.y\n"
    "    }\n"
    "    fn +=(self: *Vec2, other: Vec2) {\n"
    "        self.x += other.x\n"
    "        self.y += other.y\n"
    "    }\n"
    "}\n";

void testInherentImplMakesOperatorFunctions() {
    Lowered r(std::string(kVec2) +
              "fn main() {\n"
              "    local a = Vec2 { x: 1.0, y: 2.0 }\n"
              "    local b = Vec2 { x: 3.0, y: 4.0 }\n"
              "    local c = a + b\n"
              "    local same = a == b\n"
              "    a += b\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn operator+(self: Vec2, other: Vec2) -> Vec2"));
    CHECK(r.has("fn operator==(self: Vec2, other: Vec2) -> bool"));
    CHECK(r.has("fn operator+=(self: *Vec2, other: Vec2)"));
    CHECK(r.has("(call operator+ a#"));
    CHECK(r.has("(call operator== a#"));
    CHECK(r.has("(call operator+= (addr a#"));
}

void testDerivedComparisons() {
    // `!=` follows from `==`.
    Lowered ne(std::string(kVec2) +
               "fn main() -> bool {\n"
               "    local a = Vec2 { x: 1.0, y: 2.0 }\n"
               "    return a != a\n"
               "}\n");
    CHECK(!ne.diag->hasErrors());
    CHECK(ne.has("(call operator=="));

    // `>`, `<=` and `>=` follow from `<`.
    Lowered ord("struct N { v: i32 }\n"
                "impl N { fn <(self: N, o: N) -> bool { return self.v < o.v } }\n"
                "fn main() {\n"
                "    local a = N { v: 1 }\n"
                "    local b = N { v: 2 }\n"
                "    local x = a > b\n"
                "    local y = a <= b\n"
                "    local z = a >= b\n"
                "}\n");
    CHECK(!ord.diag->hasErrors());
    CHECK(ord.has("(call operator<"));
}

void testUnaryOperators() {
    Lowered r("struct V { x: i32 }\n"
              "impl V {\n"
              "    fn -(self: V) -> V { return V { x: -self.x } }\n"
              "    fn !(self: V) -> bool { return self.x == 0 }\n"
              "    fn ~(self: V) -> V { return V { x: ~self.x } }\n"
              "}\n"
              "fn main() {\n"
              "    local v = V { x: 3 }\n"
              "    local n = -v\n"
              "    local z = !v\n"
              "    local b = ~v\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(call operator- v#"));
    CHECK(r.has("(call operator! v#"));
    CHECK(r.has("(call operator~ v#"));
}

void testUnaryAndBinaryMinusAreOverloads() {
    Lowered r("struct V { x: i32 }\n"
              "impl V {\n"
              "    fn -(self: V) -> V { return V { x: -self.x } }\n"
              "    fn -(self: V, o: V) -> V { return V { x: self.x - o.x } }\n"
              "}\n"
              "fn main() {\n"
              "    local v = V { x: 3 }\n"
              "    local a = -v\n"
              "    local b = v - v\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
}

void testMixedOperandTypes() {
    Lowered r("struct V { x: f32 }\n"
              "impl V {\n"
              "    fn *(self: V, k: f32) -> V { return V { x: self.x * k } }\n"
              "    fn *(k: f32, self: V) -> V { return V { x: self.x * k } }\n"
              "}\n"
              "fn main() {\n"
              "    local v = V { x: 3.0 }\n"
              "    local a = v * 2.0\n"
              "    local b = 2.0 * v\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
}

void testIndexAndIndexAssign() {
    Lowered r("struct Grid { cells: [f32; 4] }\n"
              "impl Grid {\n"
              "    fn [](self: *Grid, i: usize) -> f32 { return self.cells[i] }\n"
              "    fn []=(self: *Grid, i: usize, v: f32) { self.cells[i] = v }\n"
              "}\n"
              "fn main() {\n"
              "    local g = Grid { cells: [0.0, 0.0, 0.0, 0.0] }\n"
              "    local x = g[1]\n"
              "    g[2] = 5.0\n"
              "    g[3] += 1.0\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(call operator[]"));
    CHECK(r.has("(call operator[]= (addr g#"));
}

void testCallOperator() {
    Lowered r("struct Adder { by: i32 }\n"
              "impl Adder { fn ()(self: *Adder, x: i32) -> i32 { return x + self.by } }\n"
              "fn main() -> i32 {\n"
              "    local add = Adder { by: 2 }\n"
              "    return add(40)\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(call operator() (addr add#"));
}

void testIncrementOperators() {
    // `operator++` when written ...
    Lowered own("struct C { n: i32 }\n"
                "impl C {\n"
                "    fn ++(self: *C) { self.n += 1 }\n"
                "    fn --(self: *C) { self.n -= 1 }\n"
                "}\n"
                "fn main() {\n"
                "    local c = C { n: 0 }\n"
                "    c++\n"
                "    c--\n"
                "}\n");
    CHECK(!own.diag->hasErrors());
    CHECK(own.has("(call operator++ (addr c#"));
    CHECK(own.has("(call operator-- (addr c#"));

    // ... otherwise `+= 1`, otherwise `x = x + 1`.
    Lowered viaAssign("struct C { n: i32 }\n"
                      "impl C { fn +=(self: *C, k: i32) { self.n += k } }\n"
                      "fn main() { local c = C { n: 0 }\n c++ }\n");
    CHECK(!viaAssign.diag->hasErrors());
    CHECK(viaAssign.has("(call operator+= (addr c#"));

    Lowered viaPlus("struct C { n: i32 }\n"
                    "impl C { fn +(self: C, k: i32) -> C { return C { n: self.n + k } } }\n"
                    "fn main() { local c = C { n: 0 }\n c++\n c += 2 }\n");
    CHECK(!viaPlus.diag->hasErrors());
}

void testAllCompoundOperatorsFallBackToTheBinaryOne() {
    Lowered r("struct B { v: u32 }\n"
              "impl B {\n"
              "    fn |(self: B, o: B) -> B { return B { v: self.v | o.v } }\n"
              "    fn &(self: B, o: B) -> B { return B { v: self.v & o.v } }\n"
              "    fn ^(self: B, o: B) -> B { return B { v: self.v ^ o.v } }\n"
              "    fn <<(self: B, n: u32) -> B { return B { v: self.v << n } }\n"
              "    fn >>(self: B, n: u32) -> B { return B { v: self.v >> n } }\n"
              "    fn %(self: B, o: B) -> B { return B { v: self.v % o.v } }\n"
              "    fn /(self: B, o: B) -> B { return B { v: self.v / o.v } }\n"
              "}\n"
              "fn main() {\n"
              "    local a = B { v: 1 }\n"
              "    a |= a\n"
              "    a &= a\n"
              "    a ^= a\n"
              "    a <<= 1\n"
              "    a >>= 1\n"
              "    a %= a\n"
              "    a /= a\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
}

void testClassOperatorsAreMethods() {
    Lowered r("class Counter {\n"
              "    n: i32\n"
              "    fn +(self, k: i32) -> i32 { return self.n + k }\n"
              "    fn +=(self, k: i32) { self.n += k }\n"
              "}\n"
              "fn main() {\n"
              "    local c = Counter()\n"
              "    local x = c + 1\n"
              "    c += 2\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("Counter.operator+"));
}

void testGenericOperators() {
    Lowered r("struct Pair<T> { a: T, b: T }\n"
              "impl<T> Pair<T> {\n"
              "    fn +(self: Pair<T>, o: Pair<T>) -> Pair<T> {\n"
              "        return Pair<T> { a: self.a + o.a, b: self.b + o.b }\n"
              "    }\n"
              "}\n"
              "fn main() {\n"
              "    local p = Pair<i32> { a: 1, b: 2 }\n"
              "    local q = p + p\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
}

void testPlainNumbersAreUntouched() {
    Lowered r(std::string(kVec2) +
              "fn main() -> i32 {\n"
              "    local n = 1\n"
              "    n += 2\n"
              "    n++\n"
              "    return n + 3\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(!r.has("(call operator"));
}

void testOperatorErrors() {
    // No operator for the operands.
    Lowered missing("struct V { x: i32 }\n"
                    "fn main() { local v = V { x: 1 }\n local w = v + v }\n");
    CHECK(missing.failedWith("E0709"));
    Lowered noIndex("struct V { x: i32 }\n"
                    "fn main() { local v = V { x: 1 }\n local w = v[0] }\n");
    CHECK(noIndex.failedWith("E0708"));
    Lowered noCall("struct V { x: i32 }\n"
                   "fn main() { local v = V { x: 1 }\n v(1) }\n");
    CHECK(noCall.failedWith("E0709"));
    // Wrong operand type.
    Lowered wrong(std::string(kVec2) +
                  "fn main() { local a = Vec2 { x: 1.0, y: 1.0 }\n local b = a + true }\n");
    CHECK(wrong.failedWith("E0708"));
    // An inherent impl also holds the methods of the struct (`V.len`).
    Lowered method("struct V { x: i32 }\n"
                   "impl V { fn len(self: V) -> i32 { return self.x } }\n");
    CHECK(!method.diag->hasErrors());
    // `self: T` belongs to the inherent impl.
    Lowered typedSelf("class C { fn f(self: C) { } }\n");
    CHECK(typedSelf.failedWith("E0002"));
    // `*a` needs a pointer from `operator*`.
    Lowered deref("struct V { x: i32 }\n"
                  "impl V { fn *(self: V) -> i32 { return self.x } }\n"
                  "fn main() { local v = V { x: 1 }\n local w = *v }\n");
    CHECK(deref.failedWith("E0709"));
}

void testDerefOperatorReturnsAPlace() {
    Lowered r("struct Ref { p: *i32 }\n"
              "impl Ref { fn *(self: Ref) -> *i32 { return self.p } }\n"
              "fn main() {\n"
              "    local n: i32 = 5\n"
              "    local r = Ref { p: &n }\n"
              "    local x = *r\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(call operator*"));
}

} // namespace

void runOperatorTests() {
    RUN(testInherentImplMakesOperatorFunctions);
    RUN(testDerivedComparisons);
    RUN(testUnaryOperators);
    RUN(testUnaryAndBinaryMinusAreOverloads);
    RUN(testMixedOperandTypes);
    RUN(testIndexAndIndexAssign);
    RUN(testCallOperator);
    RUN(testIncrementOperators);
    RUN(testAllCompoundOperatorsFallBackToTheBinaryOne);
    RUN(testClassOperatorsAreMethods);
    RUN(testGenericOperators);
    RUN(testPlainNumbersAreUntouched);
    RUN(testOperatorErrors);
    RUN(testDerefOperatorReturnsAPlace);
}
