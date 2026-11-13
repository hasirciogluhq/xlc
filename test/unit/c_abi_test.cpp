#include "check.h"
#include "ir.h"
#include "lowered.h"

#include "xlang/hir/prune.h"

// The C calling convention of `export(c)` and `declare` functions (src/codegen/llvm/c_abi/): the
// LLVM signature each target's classifier picks, the diagnostics around it and the pruning of an
// executable that links C objects.

namespace {

using xlang_test::contains;
using xlang_test::irFor;
using xlang_test::Lowered;

// One struct per rule; each function returns what it takes, so both directions show.
const std::string kShapes = "@repr(c)\nstruct Mixed {\n    n: i64\n    d: f64\n}\n"
                            "@repr(c)\nstruct Big {\n    a: i64\n    b: i64\n    c: i64\n    d: i64\n}\n"
                            "@repr(c)\nstruct Vec2 {\n    x: f32\n    y: f32\n}\n"
                            "@repr(c)\nstruct Tri {\n    x: f32\n    y: f32\n    z: f32\n}\n"
                            "@repr(c)\nstruct Quad {\n    a: f64\n    b: f64\n    c: f64\n    d: f64\n}\n"
                            "@repr(c)\nstruct Rgb {\n    r: u8\n    g: u8\n    b: u8\n}\n"
                            "@repr(c)\nstruct Two64 {\n    a: i64\n    b: i64\n}\n"
                            "export(c) fn mixed(m: Mixed) -> Mixed { return m }\n"
                            "export(c) fn big(b: Big) -> Big { return b }\n"
                            "export(c) fn vec2(v: Vec2) -> Vec2 { return v }\n"
                            "export(c) fn tri(t: Tri) -> Tri { return t }\n"
                            "export(c) fn quad(q: Quad) -> Quad { return q }\n"
                            "export(c) fn rgb(c: Rgb) -> Rgb { return c }\n"
                            "export(c) fn spill(a: i64, b: i64, c: i64, d: i64, e: i64, p: Two64, "
                            "f: i64) -> i64 { return f }\n"
                            "export fn call(m: Mixed) -> f64 { return mixed(m).d }\n";

// SysV x86-64: eightbytes in registers, MEMORY on the stack (`byval`) or through `sret`.
void testSysVClasses() {
    const std::string ir = irFor("x86_64-unknown-linux-gnu", kShapes);
    CHECK(contains(ir, "define { i64, double } @mixed({ i64, double } %m)"));
    CHECK(contains(ir, "define void @big(ptr noalias sret([32 x i8]) %0, ptr byval([32 x i8]) align 8 %b)"));
    CHECK(contains(ir, "define <2 x float> @vec2(<2 x float> %v)"));
    CHECK(contains(ir, "define { <2 x float>, float } @tri({ <2 x float>, float } %t)"));
    CHECK(contains(ir, "@quad(ptr noalias sret([32 x i8]) %0, ptr byval([32 x i8]) align 8 %q)"));
    CHECK(contains(ir, "define i24 @rgb(i24 %c)"));
    // Five integer registers are taken; the struct needs two, so it goes to the stack whole and
    // the last integer still gets r9.
    CHECK(contains(ir, "i64 %e, ptr byval([16 x i8]) align 8 %p, i64 %f)"));
    CHECK(contains(ir, "call { i64, double } @mixed({ i64, double }"));
}

// AAPCS64: HFAs in SIMD registers, other structs up to 16 bytes in general registers, larger ones
// through the address of a copy.
void testAapcs64Classes() {
    for (const char* triple : {"aarch64-apple-darwin", "aarch64-unknown-linux-gnu"}) {
        const std::string ir = irFor(triple, kShapes);
        CHECK(contains(ir, "define [2 x i64] @mixed([2 x i64] %m)"));
        CHECK(contains(ir, "define void @big(ptr noalias sret([32 x i8]) %0, ptr %b)"));
        CHECK(contains(ir, "define [2 x float] @vec2([2 x float] %v)"));
        CHECK(contains(ir, "define [3 x float] @tri([3 x float] %t)"));
        CHECK(contains(ir, "define [4 x double] @quad([4 x double] %q)"));
        CHECK(contains(ir, "define i24 @rgb(i64 %c)")); // a result fills only the low bytes of x0
        CHECK(contains(ir, "i64 %e, [2 x i64] %p, i64 %f)"));
    }
}

// Microsoft x64: 1, 2, 4 or 8 bytes as an integer (floats included), anything else by address.
void testWin64Classes() {
    const std::string ir = irFor("x86_64-pc-windows-msvc", kShapes);
    CHECK(contains(ir, "define void @mixed(ptr noalias sret([16 x i8]) %0, ptr %m)"));
    CHECK(contains(ir, "define i64 @vec2(i64 %v)"));
    CHECK(contains(ir, "define void @rgb(ptr noalias sret([3 x i8]) %0, ptr %c)"));
    CHECK(contains(ir, "i64 %e, ptr %p, i64 %f)"));
    CHECK(contains(ir, "call void @mixed(ptr noalias sret([16 x i8])"));
}

// Integers narrower than 32 bits are widened by the caller, under both conventions and through a
// `fn(...)` pointer, so a pointer to C and from C agrees.
void testNarrowIntegersAreWidened() {
    const std::string ir = irFor("aarch64-apple-darwin",
                                 "export(c) fn small(a: i8, b: bool) -> u16 { return 1 }\n"
                                 "export fn mine(a: u8) -> i16 { return 1 }\n"
                                 "export fn through(f: fn(u8) -> i16) -> i16 { return f(7) }\n");
    CHECK(contains(ir, "define zeroext i16 @small(i8 signext %a, i1 zeroext %b)"));
    CHECK(contains(ir, "signext i16 @\"_xl$main$mine$u8\"(i8 zeroext %a)"));
    CHECK(contains(ir, "= call signext i16 %0(i8 zeroext 7)"));
}

// A pointer call uses the xlang convention: a C function that passes a struct by value cannot be
// a `fn(...)` value (E0778).
void testCFunctionWithStructIsNotAValue() {
    const std::string pair = "@repr(c)\nstruct Pair {\n    a: i32\n    b: i32\n}\n";
    CHECK(Lowered(pair + "export(c) fn f(p: Pair) -> i32 { return p.a }\n"
                         "fn g() -> i32 {\n    local h = f\n    return 0\n}\n")
              .failedWith("E0778"));
    CHECK(Lowered(pair + "declare fn d() -> Pair\n"
                         "fn g() {\n    local h = d\n}\n")
              .failedWith("E0778"));
    const Lowered fine(pair + "export(c) fn f(p: *Pair) -> i32 { return p.a }\n"
                              "fn g() -> i32 {\n    local h = f\n    return 0\n}\n");
    CHECK(!fine.diag->hasErrors());
}

// `declare` follows the C convention, so its signature must be C as well (E0302).
void testDeclareNeedsCTypes() {
    CHECK(Lowered("declare fn d(s: str)\n").failedWith("E0302"));
    CHECK(Lowered("struct S {\n    a: i32\n}\ndeclare fn d(s: S)\n").failedWith("E0302"));
    const Lowered fine("declare fn d(p: *u8, n: usize) -> i32\n");
    CHECK(!fine.diag->hasErrors());
}

// An executable keeps its `export(c)` functions only when it links prebuilt objects.
void testExecutableKeepsCExportsForPrebuiltObjects() {
    const std::string text = "export(c) fn forC() { }\nexport fn forXl() { }\n"
                             "@entry fn main() -> i32 { return 0 }\n";
    const auto names = [&](xlang::hir::KeepExports keep) {
        Lowered r(text);
        xlang::hir::pruneUnreachable(r.module, keep);
        std::string out;
        for (const auto& f : r.module.functions) {
            out += f.name + " ";
        }
        return out;
    };
    const std::string none = names(xlang::hir::KeepExports::None);
    const std::string c = names(xlang::hir::KeepExports::C);
    const std::string all = names(xlang::hir::KeepExports::All);
    CHECK(none.find("forC") == std::string::npos && none.find("forXl") == std::string::npos);
    CHECK(c.find("forC") != std::string::npos && c.find("forXl") == std::string::npos);
    CHECK(all.find("forC") != std::string::npos && all.find("forXl") != std::string::npos);
}

} // namespace

void runCAbiTests() {
    RUN(testSysVClasses);
    RUN(testAapcs64Classes);
    RUN(testWin64Classes);
    RUN(testNarrowIntegersAreWidened);
    RUN(testCFunctionWithStructIsNotAValue);
    RUN(testDeclareNeedsCTypes);
    RUN(testExecutableKeepsCExportsForPrebuiltObjects);
}
