// The acceptance gate of the xlang lowering pass: for every sample, the text of the HIR the xlang
// sema and lowering build (src/lower/program.xl, through src/sema/xl_bridge.cpp) must equal the
// text the C++ pass dumps for the same source. The C++ pass is deleted only after its part of
// the samples compares equal. Samples cover the ported expression forms; the statement forms
// (while, if, for, switch, ...) and method calls are ported by later lanes and join this table
// then.

#include "check.h"
#include "lowered.h"

#include "sema/xl_bridge.hpp"
#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

#include <iostream>
#include <iterator>
#include <sstream>
#include <string>

using namespace xlang;

namespace {

struct Sample {
    const char* name;
    const char* text;
};

const Sample kSamples[] = {
    // Literals and names.
    {"return-literal", "fn main() -> i32 {\n    return 1\n}\n"},
    {"add-params", "fn add(a: i32, b: i32) -> i32 {\n    return a + b\n}\n"},
    {"float-scale", "fn scale(x: f64) -> f64 {\n    return x * 2.5\n}\n"},
    {"float-literal-declared", "fn half() -> f64 {\n    local z: f64 = 3\n    return z / 2.0\n}\n"},
    {"f32-literal", "fn tiny(x: f32) -> f32 {\n    return x * 0.5\n}\n"},
    {"literal-takes-u8", "fn bump(b: u8) -> u8 {\n    return b + 1\n}\n"},
    {"byte-literal", "fn ch() -> u8 {\n    return b'a'\n}\n"},
    {"string-literal", "fn name() -> str {\n    return \"hi\"\n}\n"},
    {"local-string", "fn len2() -> usize {\n    local s = \"abc\"\n    return 3\n}\n"},
    {"global-constant", "const LIMIT: i32 = 10\nfn lim() -> i32 {\n    return LIMIT\n}\n"},
    {"define-constant", "define SIZE = 16\nfn size() -> i32 {\n    return SIZE\n}\n"},
    // Locals, calls and operators.
    {"locals-and-calls",
     "fn add(a: i32, b: i32) -> i32 {\n    return a + b\n}\n"
     "fn main() -> i32 {\n    local x: i32 = 1\n    local y = add(x, 2)\n    return y * 3\n}\n"},
    {"nested-calls",
     "fn add(a: i32, b: i32) -> i32 {\n    return a + b\n}\n"
     "fn twice(a: i32) -> i32 {\n    return add(add(a, 1), a)\n}\n"},
    {"compare", "fn lt(a: i32, b: i32) -> bool {\n    return a < b\n}\n"},
    {"compare-u8-literal", "fn lt8(a: u8) -> bool {\n    return a < 10\n}\n"},
    {"logical-and", "fn both(a: i32, b: i32) -> bool {\n    return a == 0 && b > 1\n}\n"},
    {"logical-not", "fn flip(f: bool) -> bool {\n    return !f\n}\n"},
    {"shift", "fn shl(x: u32) -> u32 {\n    return x << 2\n}\n"},
    {"divide", "fn quo(a: i32, b: i32) -> i32 {\n    return a / b\n}\n"},
    {"modulo", "fn rem(a: i32) -> i32 {\n    return a % 3\n}\n"},
    {"negate", "fn neg(a: i32) -> i32 {\n    return -a\n}\n"},
    {"ternary", "fn pick(a: i32) -> i32 {\n    return a > 0 ? a : 0\n}\n"},
    // Casts, pointers and aggregates.
    {"cast", "fn narrow(a: i32) -> u8 {\n    return a as u8\n}\n"},
    {"cast-float", "fn toi(f: f64) -> i32 {\n    return f as i32\n}\n"},
    {"address-and-deref",
     "fn addr(a: i32) -> *i32 {\n    return &a\n}\n"
     "fn first(p: *i32) -> i32 {\n    return *p\n}\n"},
    {"struct-field",
     "struct Point { x: i32, y: i32 }\n"
     "fn sum(p: Point) -> i32 {\n    return p.x + p.y\n}\n"},
    {"struct-field-through-pointer",
     "struct Point { x: i32, y: i32 }\n"
     "fn getx(p: *Point) -> i32 {\n    return p.x\n}\n"},
    {"struct-literal",
     "struct Point { x: i32, y: i32 }\n"
     "fn origin() -> Point {\n    return Point { x: 0, y: 0 }\n}\n"},
    {"array-parameter-index", "fn third(a: [i32; 3]) -> i32 {\n    return a[2]\n}\n"},
    {"array-literal-index",
     "fn make() -> i32 {\n    local a = [1, 2, 3]\n    return a[0]\n}\n"},
    {"slice-index", "fn at(a: []i32, i: usize) -> i32 {\n    return a[i]\n}\n"},
    // Builtins.
    {"syscall", "fn pid() -> i64 {\n    return syscall(39)\n}\n"},
    {"size-of", "fn sz() -> u64 {\n    return sizeOf(i64)\n}\n"},
};

/// The text of the HIR the xlang pass builds for `text`; the diagnostics go to `diagnostics`.
std::string xlangDump(const std::string& text, std::string& diagnostics) {
    SourceManager sources;
    const FileId file = sources.addFile("t.xl", text);
    std::ostringstream out;
    DiagnosticEngine engine(sources, out);
    XlLowered lowered = XlLowered::lower(sources, file, BuildConfig{}, engine);
    engine.flush();
    diagnostics = out.str();
    return lowered.dump();
}

void testSamplesMatchTheCppLowering() {
    int equal = 0;
    for (const Sample& s : kSamples) {
        const xlang_test::Lowered cpp(s.text);
        std::string diagnostics;
        const std::string xl = xlangDump(s.text, diagnostics);
        if (!cpp.hir_text.empty() && xl == cpp.hir_text) {
            ++equal;
            continue;
        }
        std::cerr << "sample `" << s.name << "` differs\n--- C++ ---\n" << cpp.hir_text
                  << "--- xlang ---\n" << xl << "--- xlang diagnostics ---\n" << diagnostics;
    }
    std::cout << "xlang lowering: " << equal << " of " << std::size(kSamples)
              << " samples equal the C++ HIR\n";
    CHECK_EQ(equal, static_cast<int>(std::size(kSamples)));
}

} // namespace

void runXlLowerTests() {
    RUN(testSamplesMatchTheCppLowering);
}
