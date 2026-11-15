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
    {"cast","fn narrow(a: i32) -> u8 {\n    return a as u8\n}\n"},
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
    // Conversions, casts, slices, struct literals and fields, the clean cases.
    {"widen-same-sign", "fn wide(a: u8) -> u32 {\n    return a\n}\n"},
    {"explicit-cast-widen", "fn wide(a: i32) -> i64 {\n    return a as i64\n}\n"},
    {"struct-literal-coerce",
     "struct Wide { x: i64 }\nfn make() -> Wide {\n    return Wide { x: 7 }\n}\n"},
    {"struct-field-of-literal",
     "struct P { x: i32, y: i32 }\nfn sumOf() -> i32 {\n    local p = P { x: 1, y: 2 }\n"
     "    return p.x\n}\n"},
    {"str-slice", "fn part(s: str) -> str {\n    return s[1..2]\n}\n"},
    {"str-len-field", "fn size(s: str) -> usize {\n    return s.len\n}\n"},
    // Lane B: calls (overloads, class methods).
    {"overload-by-float",
     "fn pick(a: i32) -> i32 {\n    return a\n}\n"
     "fn pick(a: f64) -> f64 {\n    return a\n}\n"
     "fn main() -> f64 {\n    return pick(2.5)\n}\n"},
    {"overload-by-int-width",
     "fn pick(a: i32) -> i32 {\n    return a\n}\n"
     "fn pick(a: i64) -> i64 {\n    return a\n}\n"
     "fn main() -> i64 {\n    local x: i32 = 1\n    return pick(x) + pick(7)\n}\n"},
    {"method-call",
     "class Counter {\n    n: i32\n    fn get(self) -> i32 {\n        return self.n\n    }\n}\n"
     "fn read(c: Counter) -> i32 {\n    return c.get()\n}\n"},
    {"method-call-through-pointer",
     "class Counter {\n    n: i32\n    fn get(self) -> i32 {\n        return self.n\n    }\n}\n"
     "fn read(c: *Counter) -> i32 {\n    return c.get() + 1\n}\n"},
};

/// Samples the C++ pass rejects: the xlang pass must report the same diagnostics (codes, spans and
/// text) and produce no dump. Each rejected statement is a `local`, not a `return`, so that the
/// function still returns on every path (the C++ flow check adds nothing). A warning sample has a
/// dump too.
const Sample kDiagnosticSamples[] = {
    {"err-signedness", "fn f(a: u32) -> i32 {\n    local r: i32 = a\n    return 0\n}\n"},
    {"err-narrowing", "fn f(a: i64) -> i32 {\n    local r: i32 = a\n    return 0\n}\n"},
    {"err-float-to-int", "fn f() -> i32 {\n    local r: i32 = 1.5\n    return 0\n}\n"},
    {"err-no-conversion",
     "struct A { x: i32 }\nstruct B { x: i32 }\n"
     "fn f(a: A) -> i32 {\n    local r: B = a\n    return 0\n}\n"},
    {"warn-int-to-float", "fn f(a: i32) -> f64 {\n    return a\n}\n"},
    {"err-cast-pointer-float",
     "fn f(p: *i32) -> i32 {\n    local r = p as f64\n    return 0\n}\n"},
    {"err-deref-scalar", "fn f(a: i32) -> i32 {\n    local r = *a\n    return 0\n}\n"},
    {"err-deref-void", "fn f(p: *void) -> i32 {\n    local r = *p\n    return 0\n}\n"},
    {"err-index-float",
     "fn f(a: [i32; 3]) -> i32 {\n    local r = a[1.5]\n    return 0\n}\n"},
    {"err-index-scalar", "fn f(a: i32) -> i32 {\n    local r = a[0]\n    return 0\n}\n"},
    {"err-index-void", "fn f(p: *void) -> i32 {\n    local r = p[0]\n    return 0\n}\n"},
    {"err-slice-scalar", "fn f(a: i32) -> i32 {\n    local s = a[0..1]\n    return 0\n}\n"},
    {"err-field-unknown",
     "struct P { x: i32 }\nfn f(p: P) -> i32 {\n    local r = p.z\n    return 0\n}\n"},
    {"err-field-scalar", "fn f(a: i32) -> i32 {\n    local r = a.x\n    return 0\n}\n"},
    {"err-field-str", "fn f(s: str) -> i32 {\n    local r = s.size\n    return 0\n}\n"},
    {"err-tuple-range", "fn f(t: (i32, i32)) -> i32 {\n    local r = t.5\n    return 0\n}\n"},
    {"err-literal-unknown-field",
     "struct P { x: i32 }\nfn f() -> i32 {\n    local r = P { y: 1 }\n    return 0\n}\n"},
    {"err-literal-duplicate",
     "struct P { x: i32 }\nfn f() -> i32 {\n    local r = P { x: 1, x: 2 }\n    return 0\n}\n"},
    {"err-literal-field-type",
     "struct P { x: i32 }\nfn f() -> i32 {\n    local r = P { x: 1.5 }\n    return 0\n}\n"},
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

// A sample is equal when the diagnostics of both passes are the same text, and, when the C++ pass
// reports none, the HIR dumps are the same text too.
bool sampleEqual(const Sample& s) {
    const xlang_test::Lowered cpp(s.text);
    std::string diagnostics;
    const std::string xl = xlangDump(s.text, diagnostics);
    const bool dumps = cpp.diag->hasErrors() || (!cpp.hir_text.empty() && xl == cpp.hir_text);
    const bool diags = diagnostics == cpp.out.str();
    if (dumps && diags) {
        return true;
    }
    std::cerr << "sample `" << s.name << "` differs\n--- C++ ---\n" << cpp.hir_text
              << "--- C++ diagnostics ---\n" << cpp.out.str() << "--- xlang ---\n" << xl
              << "--- xlang diagnostics ---\n" << diagnostics;
    return false;
}

void testSamplesMatchTheCppLowering() {
    int equal = 0;
    for (const Sample& s : kSamples) {
        equal += sampleEqual(s) ? 1 : 0;
    }
    for (const Sample& s : kDiagnosticSamples) {
        equal += sampleEqual(s) ? 1 : 0;
    }
    const int total = static_cast<int>(std::size(kSamples) + std::size(kDiagnosticSamples));
    std::cout << "xlang lowering: " << equal << " of " << total
              << " samples equal the C++ pass (HIR and diagnostics)\n";
    CHECK_EQ(equal, total);
}

} // namespace

void runXlLowerTests() {
    RUN(testSamplesMatchTheCppLowering);
}
