#include "check.h"
#include "lowered.h"

// The iterator protocol of `for x in value` (`next`, or `iter` + `next`) and the methods of
// structs (`impl T { fn name(self: *T) }`). See xlc/README.md, "Iterators".

using xlang_test::Lowered;

namespace {

constexpr const char* kOption = "enum Option<T> {\n Some(T)\n None\n}\n";

std::size_t countOfIter(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

std::string withOption(const std::string& body) {
    return std::string(kOption) + body;
}

constexpr const char* kCounter =
    "struct Counter { n: i32, end: i32 }\n"
    "impl Counter {\n"
    "    fn next(self: *Counter) -> Option<i32> {\n"
    "        if self.n >= self.end { return Option.None }\n"
    "        local v = self.n\n"
    "        self.n += 1\n"
    "        return Option.Some(v)\n"
    "    }\n"
    "}\n";

void testStructMethodsAreFunctions() {
    Lowered r("struct V { x: i32 }\n"
              "impl V {\n"
              "    fn get(self: *V) -> i32 { return self.x }\n"
              "    fn plus(self: V, k: i32) -> i32 { return self.x + k }\n"
              "}\n"
              "fn main() -> i32 {\n"
              "    local v = V { x: 1 }\n"
              "    local p = &v\n"
              "    return v.get() + v.plus(2) + p.get()\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn V.get(self: *V) -> i32"));
    CHECK(r.has("(call V.get (addr v#"));
    CHECK(r.has("(call V.plus v#"));
}

void testGenericStructMethods() {
    Lowered r("struct Box<T> { v: T }\n"
              "impl<T> Box<T> { fn get(self: *Box<T>) -> T { return self.v } }\n"
              "fn main() -> i32 {\n"
              "    local b = Box<i32> { v: 4 }\n"
              "    return b.get()\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
}

void testUnknownMethodStillFails() {
    Lowered r("struct V { x: i32 }\n"
              "fn main() { local v = V { x: 1 }\n v.nope() }\n");
    CHECK(r.failedWith("E0709"));
}

void testNextAloneIsItsOwnIterator() {
    Lowered r(withOption(std::string(kCounter) +
                         "fn main() -> i32 {\n"
                         "    local c = Counter { n: 0, end: 3 }\n"
                         "    local sum = 0\n"
                         "    for x in c { sum += x }\n"
                         "    for y in c { sum += y }\n"
                         "    return sum\n"
                         "}\n"));
    CHECK(!r.diag->hasErrors());
    // The loop calls `next` on the variable itself, so a second loop goes on.
    CHECK(r.has("(call Counter.next"));
    CHECK(r.has("$src"));
}

void testIterAndNextRestartEveryLoop() {
    Lowered r(withOption(
        "struct Range { end: i32 }\n"
        "struct RangeIter { n: i32, end: i32 }\n"
        "impl Range { fn iter(self: *Range) -> RangeIter { return RangeIter { n: 0, end: self.end } } }\n"
        "impl RangeIter {\n"
        "    fn next(self: *RangeIter) -> Option<i32> {\n"
        "        if self.n >= self.end { return Option.None }\n"
        "        self.n += 1\n"
        "        return Option.Some(self.n)\n"
        "    }\n"
        "}\n"
        "fn main() -> i32 {\n"
        "    local r = Range { end: 3 }\n"
        "    local sum = 0\n"
        "    for x in r { sum += x }\n"
        "    for y in r { sum += y }\n"
        "    return sum\n"
        "}\n"));
    CHECK(!r.diag->hasErrors());
    CHECK(countOfIter(r.hir_text, "(call Range.iter") == 2); // a new iterator for each loop
}

void testGenericIterableLikeAVec() {
    Lowered r(withOption(
        "struct Vec<T> { ptr: *T, len: usize }\n"
        "struct VecIter<T> { ptr: *T, end: *T }\n"
        "impl<T> Vec<T> { fn iter(self: *Vec<T>) -> VecIter<T> {\n"
        "    return VecIter<T> { ptr: self.ptr, end: self.ptr }\n"
        "} }\n"
        "impl<T> VecIter<T> { fn next(self: *VecIter<T>) -> Option<T> { return Option.None } }\n"
        "fn main() {\n"
        "    local a: i32 = 1\n"
        "    local v = Vec<i32> { ptr: &a, len: 1 }\n"
        "    for x in v { local y: i32 = x }\n"
        "}\n"));
    CHECK(!r.diag->hasErrors());
}

void testIndexAndBreakAndContinue() {
    Lowered r(withOption(std::string(kCounter) +
                         "fn main() {\n"
                         "    local c = Counter { n: 0, end: 5 }\n"
                         "    outer: for x, i in c {\n"
                         "        if x == 1 { continue }\n"
                         "        if i == 3 { break }\n"
                         "    }\n"
                         "}\n"));
    CHECK(!r.diag->hasErrors());
}

void testIteratingATemporaryAndAPointer() {
    Lowered r(withOption(std::string(kCounter) +
                         "fn make() -> Counter { return Counter { n: 0, end: 2 } }\n"
                         "fn main() {\n"
                         "    for a in make() { }\n"
                         "    local c = Counter { n: 0, end: 2 }\n"
                         "    local p = &c\n"
                         "    for b in p { }\n"
                         "}\n"));
    CHECK(!r.diag->hasErrors());
}

void testClassIterators() {
    Lowered r(withOption("class Walker {\n"
                         "    n: i32\n"
                         "    fn next(self) -> Option<i32> {\n"
                         "        if self.n >= 2 { return Option.None }\n"
                         "        self.n += 1\n"
                         "        return Option.Some(self.n)\n"
                         "    }\n"
                         "}\n"
                         "fn main() {\n"
                         "    local w = Walker()\n"
                         "    for x in w { }\n"
                         "}\n"));
    CHECK(!r.diag->hasErrors());
}

void testOtherTypesKeepTheirOwnLoops() {
    Lowered r("fn main() {\n"
              "    local a = [1, 2, 3]\n"
              "    for x in a { }\n"
              "    for i in 0..3 { }\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
}

void testIterationErrors() {
    // Neither `next` nor `iter`.
    Lowered none("struct V { x: i32 }\n"
                 "fn main() { local v = V { x: 1 }\n for x in v { } }\n");
    CHECK(none.failedWith("E0775"));
    // `iter` alone: the iterator it returns has no `next`.
    Lowered onlyIter(withOption(
        "struct C { x: i32 }\n"
        "struct I { x: i32 }\n"
        "impl C { fn iter(self: *C) -> I { return I { x: 1 } } }\n"
        "fn main() { local c = C { x: 1 }\n for x in c { } }\n"));
    CHECK(onlyIter.failedWith("E0776"));
    // `iter` returns something that is not an iterator.
    Lowered notIter("struct C { x: i32 }\n"
                    "impl C { fn iter(self: *C) -> i32 { return 1 } }\n"
                    "fn main() { local c = C { x: 1 }\n for x in c { } }\n");
    CHECK(notIter.failedWith("E0776"));
    // `next` returns something that cannot end the loop.
    Lowered badNext("enum Result<T, E> {\n Ok(T)\n Err(E)\n}\n"
                    "struct C { x: i32 }\n"
                    "impl C { fn next(self: *C) -> Result<i32, i32> { return Result.Ok(1) } }\n"
                    "fn main() { local c = C { x: 1 }\n for x in c { } }\n");
    CHECK(badNext.failedWith("E0777"));
    Lowered plainNext("struct C { x: i32 }\n"
                      "impl C { fn next(self: *C) -> i32 { return 1 } }\n"
                      "fn main() { local c = C { x: 1 }\n for x in c { } }\n");
    CHECK(plainNext.failedWith("E0777"));
    // A type that is not iterable at all keeps its message.
    Lowered number("fn main() { local n = 1\n for x in n { } }\n");
    CHECK(number.failedWith("E0709"));
}

void testIterationWarnings() {
    // `next` by value works on a copy.
    Lowered byValue(withOption("struct C { x: i32 }\n"
                               "impl C { fn next(self: C) -> Option<i32> { return Option.None } }\n"
                               "fn main() { local c = C { x: 1 }\n for x in c { } }\n"));
    CHECK(!byValue.diag->hasErrors());
    CHECK(byValue.out.str().find("W0770") != std::string::npos);
    // Both `iter` and `next`, on different types.
    Lowered both(withOption(
        "struct R { end: i32 }\n"
        "struct RI { n: i32 }\n"
        "impl R {\n"
        "    fn iter(self: *R) -> RI { return RI { n: 0 } }\n"
        "    fn next(self: *R) -> Option<i32> { return Option.None }\n"
        "}\n"
        "impl RI { fn next(self: *RI) -> Option<i32> { return Option.None } }\n"
        "fn main() { local r = R { end: 1 }\n for x in r { } }\n"));
    CHECK(!both.diag->hasErrors());
    CHECK(both.out.str().find("W0771") != std::string::npos);
    // A well-formed loop is quiet.
    Lowered quiet(withOption(std::string(kCounter) +
                             "fn main() { local c = Counter { n: 0, end: 1 }\n for x in c { } }\n"));
    CHECK(quiet.out.str().find("warning") == std::string::npos);
}

} // namespace

void runIteratorTests() {
    RUN(testStructMethodsAreFunctions);
    RUN(testGenericStructMethods);
    RUN(testUnknownMethodStillFails);
    RUN(testNextAloneIsItsOwnIterator);
    RUN(testIterAndNextRestartEveryLoop);
    RUN(testGenericIterableLikeAVec);
    RUN(testIndexAndBreakAndContinue);
    RUN(testIteratingATemporaryAndAPointer);
    RUN(testClassIterators);
    RUN(testOtherTypesKeepTheirOwnLoops);
    RUN(testIterationErrors);
    RUN(testIterationWarnings);
}
