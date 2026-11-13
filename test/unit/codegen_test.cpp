#include "check.h"

#include "xlang/codegen/codegen.h"
#include "xlang/lang/frontend.h"
#include "xlang/sema/sema.h"

#include <sstream>
#include <string>

// LLVM IR for a given target: what the generator writes depends only on the source and the triple, not on the host.

namespace {

/// The unoptimized IR of `text` built for `triple` as an object file; empty after an error.
std::string irFor(const std::string& triple, std::string text) {
    xlang::SourceManager sources;
    std::ostringstream out;
    xlang::BuildConfig config;
    config.target = *xlang::parseTargetTriple(triple);
    xlang::DiagnosticEngine diag(sources, out);
    const xlang::FileId file = sources.addFile("t.xl", std::move(text));
    xlang::Program program = xlang::parseSource(sources, file, config, diag);
    xlang::Sema sema(program, config, diag);
    if (!diag.hasErrors()) {
        sema.analyze();
    }
    if (diag.hasErrors()) {
        diag.flush();
        std::cerr << out.str();
        return {};
    }
    const xlang::hir::Module module = sema.lower();
    const xlang::codegen::CodegenInput input{module, config, sources, diag,
                                             [&sema](const xlang::Type& t) { return sema.layoutOf(t, xlang::Span{}); },
                                             "t", xlang::codegen::Output::Object};
    const auto artifact = xlang::codegen::generate(input);
    return artifact ? artifact->ir() : std::string{};
}

bool contains(const std::string& text, const std::string& needle) {
    const bool found = text.find(needle) != std::string::npos;
    if (!found) {
        std::cerr << "--- missing `" << needle << "` in ---\n" << text << "\n";
    }
    return found;
}

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

} // namespace

void runCodegenTests() {
    RUN(testDarwinSyscallDeclaresTheSecondResult);
    RUN(testOtherSyscallRegisters);
    RUN(testNeverIsNoreturn);
}
