#include "check.h"
#include "ir.h"

// The IR the generator writes for one target (see ir.h).

namespace {

using xlang_test::contains;
using xlang_test::irFor;

constexpr const char* kWrite = "export fn f(p: *u8, n: usize) -> i64 { return syscall(4, 1, p, n) }\n";

// XNU writes x1 (arm64) / rdx (x86-64) on every return, also when it carried an argument: the register is a second output
// with that argument tied to it. Without it the optimizer kept `mmap`'s length in x1 across the trap and the heap of every
// program built with -O1 or more broke on macOS (xlang-docs std/syscalls/macos-xnu.md).
void testDarwinSyscallDeclaresTheSecondResult() {
    const std::string arm = irFor("aarch64-apple-darwin", kWrite);
    CHECK(contains(arm, "\"={x0},={x1},{x16},0,1,{x2},~{cc},~{memory}\""));
    CHECK(contains(arm, "extractvalue { i64, i64 }"));
    const std::string x86 = irFor("x86_64-apple-darwin", kWrite);
    CHECK(contains(x86, "\"={rax},={rdx},0,{rdi},{rsi},1,~{rcx},~{r11},~{cc},~{memory}\""));
}

// Linux writes only the result register; Windows NT may write every volatile argument register.
void testOtherSyscallRegisters() {
    CHECK(contains(irFor("aarch64-unknown-linux-gnu", kWrite), "\"={x0},{x8},0,{x1},{x2},~{memory}\""));
    CHECK(contains(irFor("x86_64-unknown-linux-gnu", kWrite), "\"={rax},0,{rdi},{rsi},{rdx},~{rcx},~{r11},~{memory}\""));
    CHECK(contains(irFor("x86_64-pc-windows-msvc", kWrite),
                   "\"={rax},={r10},={rdx},={r8},={r9},0,1,2,3,~{rcx},~{r11},~{cc},~{memory}\""));
}

// A `-> never` function is `noreturn`, and the code after every call of it is `unreachable`.
void testNeverIsNoreturn() {
    const std::string ir = irFor("x86_64-unknown-linux-gnu",
                                 "export fn die() -> never { while true { } }\n"
                                 "export fn f(c: bool) -> i32 {\n    if c { return 1 }\n    die()\n}\n");
    CHECK(contains(ir, "; Function Attrs: noreturn nounwind\ndefine void @\"_xl$main$die\"()"));
    CHECK(contains(ir, "call void @\"_xl$main$die\"() #"));
    CHECK(contains(ir, "unreachable"));
}

// `h.p = V(h.p.x + dx, h.p.y)` and `p = P { x: p.y, y: p.x }` read the target they overwrite: the value
// is built in its own slot and copied, because a constructor or a literal zeroes its destination
// before it reads the operands (the target was built in place once and read back as zeros).
void testAssignBuildsTheValueBeforeTheTarget() {
    const std::string ir = irFor("x86_64-unknown-linux-gnu",
                                 "class V {\n    x: i32\n    y: i32\n"
                                 "    fn init(self, x: i32, y: i32) {\n        self.x = x\n        self.y = y\n    }\n}\n"
                                 "class H {\n    p: V\n"
                                 "    fn move(self, dx: i32) { self.p = V(self.p.x + dx, self.p.y) }\n}\n"
                                 "export fn go(h: *H) { h.move(1) }\n"
                                 "struct P {\n    x: i32\n    y: i32\n}\n"
                                 "export fn swap(p: *P) { *p = P { x: p.y, y: p.x } }\n");
    CHECK(contains(ir, "V_init$PN1V$i32$i32\"(ptr %tmp,"));
    CHECK(contains(ir, "call void @llvm.memset.p0.i64(ptr align 4 %tmp, i8 0, i64 8"));
    CHECK(contains(ir, ", ptr align 4 %tmp, i64 8, i1 false)"));
    CHECK(ir.find("call void @llvm.memset.p0.i64(ptr align 4 %0, i8 0") == std::string::npos);
}

// A negative literal of a type narrower than 64 bits keeps only its low bits. It was stored with the
// high bits set, so IPSCCP merged `pick`'s returns into a range without 0 and folded the compare to
// `false` (at -O2 `xl lc install` could not finish a TLS handshake).
void testNegativeLiteralFitsItsType() {
    const std::string ir = irFor("aarch64-apple-darwin",
                                 "define OK = 0\ndefine BAD = -2\n"
                                 "fn pick(a: i32) -> i32 {\n    if a == 1 {\n        return OK\n    }\n"
                                 "    if a == 2 {\n        return BAD\n    }\n    return 5\n}\n"
                                 "export fn isOk(a: i32) -> bool {\n    return pick(a) == 0\n}\n",
                                 1);
    CHECK(contains(ir, "@\"_xl$main$isOk$i32\"(i32 %a)"));
    CHECK(contains(ir, "icmp eq i32 %a, 1"));
    CHECK(ir.find("ret i1 false") == std::string::npos);
}

} // namespace

void runCodegenTests() {
    RUN(testDarwinSyscallDeclaresTheSecondResult);
    RUN(testOtherSyscallRegisters);
    RUN(testNeverIsNoreturn);
    RUN(testAssignBuildsTheValueBeforeTheTarget);
    RUN(testNegativeLiteralFitsItsType);
}
