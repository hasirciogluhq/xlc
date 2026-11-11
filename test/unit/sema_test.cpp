#include "check.h"

#include "xlang/lang/ast.h"
#include "xlang/lang/frontend.h"
#include "xlang/sema/conversion.h"
#include "xlang/sema/layout.h"
#include "xlang/sema/sema.h"

#include <sstream>
#include <variant>

using namespace xlang;

namespace {

struct Run {
    SourceManager sources;
    std::ostringstream out;
    BuildConfig config;
    FileId file{0};
    std::unique_ptr<DiagnosticEngine> diag;
    Program program;
    MacroTable macros;

    explicit Run(std::string text, BuildConfig cfg = BuildConfig{}) : config(std::move(cfg)) {
        file = sources.addFile("t.xl", std::move(text));
        diag = std::make_unique<DiagnosticEngine>(sources, out);
        program = parseSource(sources, file, config, *diag, macros);
    }

    [[nodiscard]] std::size_t fnCount() const {
        std::size_t n = 0;
        for (const Item& i : program.items) {
            n += std::holds_alternative<FnDecl>(i.decl) ? 1 : 0;
        }
        return n;
    }

    [[nodiscard]] bool hasFn(const std::string& name) const {
        for (const Item& i : program.items) {
            if (const auto* f = std::get_if<FnDecl>(&i.decl); f != nullptr && f->name == name) {
                return true;
            }
        }
        return false;
    }
};

BuildConfig target(const char* triple) {
    BuildConfig c;
    c.target = *parseTargetTriple(triple);
    return c;
}

void testSingleLineAndBlockConditionals() {
    const std::string src =
        "#if os == \"linux\" fn onlyLinux() { }\n"
        "#if os == \"macos\" && arch == \"aarch64\"\n"
        "    fn mac() { }\n"
        "#elif os == \"windows\"\n"
        "    fn win() { }\n"
        "#else\n"
        "    fn other() { }\n"
        "#endif\n"
        "fn always() { }\n";
    {
        Run r(src, target("x86_64-unknown-linux-gnu"));
        CHECK(!r.diag->hasErrors());
        CHECK(r.hasFn("onlyLinux") && r.hasFn("other") && r.hasFn("always"));
        CHECK(!r.hasFn("mac") && !r.hasFn("win"));
    }
    {
        Run r(src, target("aarch64-apple-darwin"));
        CHECK(r.hasFn("mac") && !r.hasFn("onlyLinux") && !r.hasFn("other"));
    }
    {
        Run r(src, target("x86_64-pc-windows-msvc"));
        CHECK(r.hasFn("win") && !r.hasFn("other"));
    }
}

void testDefinesNestingAndDefined() {
    const std::string src =
        "define LEVEL = 3\n"
        "#ifdef FEAT\n"
        "fn feat() { }\n"
        "#endif\n"
        "#ifndef FEAT\n"
        "fn nofeat() { }\n"
        "#endif\n"
        "#if LEVEL >= 3 && !defined(NOPE)\n"
        "    #if debug\n"
        "    fn dbg() { }\n"
        "    #else\n"
        "    fn rel() { }\n"
        "    #endif\n"
        "#endif\n";
    {
        Run r(src);
        CHECK(!r.diag->hasErrors());
        CHECK(r.hasFn("nofeat") && !r.hasFn("feat") && r.hasFn("rel") && !r.hasFn("dbg"));
    }
    {
        BuildConfig c;
        c.defines["FEAT"] = ConfigValue::parse("");
        c.defines["LEVEL"] = ConfigValue::parse("1");  // CLI beats the file `define`
        c.debug = true;
        Run r(src, c);
        CHECK(r.hasFn("feat") && !r.hasFn("nofeat"));
        CHECK(!r.hasFn("dbg") && !r.hasFn("rel"));
    }
}

void testPreprocessorErrors() {
    {
        Run r("#error \"nope\"\n");
        CHECK(r.diag->errorCount() == 1);
    }
    {
        Run r("#if os == \"linux\"\nfn a() { }\n");  // never closed
        CHECK(r.diag->hasErrors());
    }
    {
        Run r("#endif\nfn a() { }\n");
        CHECK(r.diag->hasErrors());
    }
    {
        Run r("#if MISSING == 1\nfn a() { }\n#endif\n");
        CHECK(r.diag->hasErrors());
        CHECK(!r.hasFn("a"));
    }
    {
        Run r("#if false\n#error \"inactive\"\n#endif\nfn a() { }\n");  // inactive #error is silent
        CHECK(!r.diag->hasErrors());
    }
}

void testConditionalInsideStructAndFunction() {
    Run r("struct S {\n"
          "    a: i32\n"
          "#if defined(X)\n"
          "    b: i32\n"
          "#endif\n"
          "}\n"
          "fn f() {\n"
          "#if debug log(1)\n"
          "    local x: i32 = 0\n"
          "}\n");
    CHECK(!r.diag->hasErrors());
    const auto& s = std::get<StructDecl>(r.program.items[0].decl);
    CHECK(s.fields.size() == 1);
    const auto& f = std::get<FnDecl>(r.program.items[1].decl);
    CHECK(f.body.statements.size() == 1);
}

void testMacros() {
    Run r("define square(x: expr) { x * x }\n"
          "define swap(a: expr, b: expr, T: type) {\n"
          "    local tmp: T = a\n"
          "    a = b\n"
          "    b = tmp\n"
          "}\n"
          "fn main() -> i32 {\n"
          "    local tmp: i32 = 1\n"
          "    local q: i32 = 2\n"
          "    swap!(tmp, q, i32)\n"
          "    return square!(tmp + 1)\n"
          "}\n");
    CHECK(!r.diag->hasErrors());
    const auto& f = std::get<FnDecl>(r.program.items[2].decl);
    // tmp, q, (expanded: local tmp__m1, a = b, b = tmp__m1), return
    CHECK(f.body.statements.size() == 6);
    CHECK(f.body.statements[2].name != "tmp");  // hygienic rename
    const Expr& ret = *f.body.statements[5].expr;
    CHECK(ret.kind == Expr::Kind::Binary && ret.binary_op == BinaryOp::Mul);  // parenthesized product
    CHECK(ret.lhs->binary_op == BinaryOp::Add);                               // (tmp + 1) * (tmp + 1)
}

void testMacroErrors() {
    {
        Run r("define one(x: expr) { x }\nfn f() { one!(1, 2) }\n");
        CHECK(r.diag->errorCount() == 1);
    }
    {
        Run r("define named(n: ident) { local n: i32 = 0 }\nfn f() { named!(1 + 2) }\n");
        CHECK(r.diag->hasErrors());
    }
    {
        Run r("define loop(x: expr) { loop!(x) }\nfn f() { loop!(1) }\n");
        CHECK(r.diag->hasErrors());  // depth limit
    }
}

void testMacroGeneratesItemsAndNested() {
    Run r("define twice(x: expr) { x + x }\n"
          "define quad(x: expr) { twice!(x) + twice!(x) }\n"
          "define makeFn(name: ident, body: block) {\n"
          "    fn name() body\n"
          "}\n"
          "makeFn!(go, { return })\n"
          "fn f() -> i32 { return quad!(1) }\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.hasFn("go"));
    CHECK(r.hasFn("f"));
}

// --- sema ---------------------------------------------------------------------------------

struct Analyzed {
    Run run;
    std::unique_ptr<Sema> sema;
    explicit Analyzed(std::string text, BuildConfig cfg = BuildConfig{}, const LayoutHistory* h = nullptr)
        : run(std::move(text), std::move(cfg)) {
        sema = std::make_unique<Sema>(run.program, run.config, *run.diag, h);
        sema->analyze();
    }
};

void testLayoutsMatchClang() {
    Analyzed a("@repr(c) struct Header { tag: ui8\n length: ui32\n id: ui16 }\n"
               "@repr(c) struct Mixed { a: i8\n b: f64\n c: i16\n d: *i32\n e: [ui8; 5]\n f: bool }\n"
               "@repr(c) @packed struct Packed { a: ui8\n b: ui32\n c: ui16 }\n"
               "@repr(c) @align(32) struct Aligned { a: ui8\n b: ui16 }\n"
               "@repr(c) class Shape { id: i32\n virtual fn area(self) -> f64 { return 0.0 } }\n"
               "@repr(c) class Circle : Shape { radius: f64\n flag: ui8 }\n"
               "@repr(c) class Plain { a: i32\n b: ui8 }\n"
               "@repr(c) class Derived : Plain { c: ui8 }\n");
    CHECK(!a.run.diag->hasErrors());
    const auto header = a.sema->layoutOfName("Header", {});
    CHECK(header && header->size == 12 && header->align == 4 && header->fields[1].offset == 4);
    const auto mixed = a.sema->layoutOfName("Mixed", {});
    CHECK(mixed && mixed->size == 40 && mixed->fields[5].offset == 37);
    const auto packed = a.sema->layoutOfName("Packed", {});
    CHECK(packed && packed->size == 7 && packed->align == 1 && packed->fields[1].offset == 1);
    const auto aligned = a.sema->layoutOfName("Aligned", {});
    CHECK(aligned && aligned->size == 32 && aligned->align == 32);
    const auto shape = a.sema->layoutOfName("Shape", {});
    CHECK(shape && shape->size == 16 && shape->fields[0].offset == 8 && shape->has_vptr);
    const auto circle = a.sema->layoutOfName("Circle", {});
    CHECK(circle && circle->size == 32 && circle->fields[1].offset == 16 && circle->fields[2].offset == 24);
    const auto derived = a.sema->layoutOfName("Derived", {});
    CHECK(derived && derived->size == 12 && derived->fields[2].offset == 8);  // POD base: no tail reuse
}

void testDefaultSchemeReordersDeterministically() {
    Analyzed a("struct R { a: ui8\n b: ui64\n c: ui16\n d: ui32 }\n");
    const auto r = a.sema->layoutOfName("R", {});
    CHECK(r && r->size == 16 && r->fields[1].offset == 0 && r->fields[3].offset == 8 &&
          r->fields[2].offset == 12 && r->fields[0].offset == 14);
}

void testConstantsAndEnums() {
    Analyzed a("define N = 4\n"
               "define M = N * 2 + 1\n"
               "enum E: i32 { A, B = 10, C }\n"
               "enum S: *ui8 { X = \"x\" }\n"
               "staticAssert(M == 9, \"m\")\n"
               "staticAssert(E.C == 11, \"c\")\n"
               "staticAssert(MemoryOrder.seqCst == 4, \"order\")\n"
               "staticAssert(sizeOf(i64) == 8 && sizeOf(usize) == 8 && sizeOf(*ui8) == 8, \"sizes\")\n"
               "staticAssert((300 as ui8) == 44, \"wrap\")\n"
               "staticAssert(pointerWidth == 64, \"pw\")\n"
               "global t: [ui8; M]\n");
    CHECK(!a.run.diag->hasErrors());
    const auto v = a.sema->evalConst(*std::get<DefineConstDecl>(a.run.program.items[1].decl).value);
    CHECK(v && v->int_value == 9);
}

void testSemaErrors() {
    {
        Analyzed a("staticAssert(1 == 2, \"boom\")\n");
        CHECK(a.run.diag->errorCount() == 1);
    }
    {
        Analyzed a("type A = B\ntype B = A\n");
        CHECK(a.run.diag->hasErrors());
    }
    {
        Analyzed a("enum S: *ui8 { X }\n");  // string enum needs values
        CHECK(a.run.diag->hasErrors());
    }
    {
        Analyzed a("struct A { b: B }\nstruct B { a: A }\n");  // by-value recursion
        CHECK(a.run.diag->hasErrors());
    }
    {
        Analyzed a("global g: [ui8; unknownSize]\n");
        CHECK(a.run.diag->hasErrors());
    }
    {
        Analyzed a("declare struct Opaque\nstruct U { o: Opaque }\n");
        CHECK(a.run.diag->hasErrors());
    }
    {
        Analyzed a("struct P { n: *Node }\nstruct Node { p: P }\n");  // pointer breaks the cycle
        CHECK(!a.run.diag->hasErrors());
    }
}

void testAliasesResolve() {
    Analyzed a("type Byte = ui8\ntype Bytes = [Byte; 4]\nstruct S { b: Bytes\n c: Byte }\n"
               "staticAssert(sizeOf(S) == 5, \"s\")\n");
    CHECK(!a.run.diag->hasErrors());
}

void testDeclareLayoutWarningsAndHistory() {
    {
        Analyzed a("declare struct Point { x: f64\n y: f64 }\n");
        CHECK(a.run.diag->warningCount() == 1);  // W0201
    }
    {
        Analyzed a("@repr(c) declare struct Point { x: f64\n y: f64 }\n");
        CHECK(a.run.diag->warningCount() == 0);
    }
    {
        LayoutHistory h;
        LayoutRecord rec;
        rec.name = "Point";
        rec.layout.size = 24;
        rec.layout.align = 8;
        rec.layout.scheme = "v1";
        rec.layout.fields = {{"x", {}, 0, 8, 8, false}, {"y", {}, 16, 8, 8, false}};
        h.put(rec);
        Analyzed a("declare struct Point { x: f64\n y: f64 }\n", BuildConfig{}, &h);
        CHECK(a.run.diag->warningCount() == 0);
        const auto l = a.sema->layoutOfName("Point", {});
        CHECK(l && l->size == 24 && l->fields[1].offset == 16);
    }
    {
        LayoutHistory h;
        LayoutRecord rec;
        rec.name = "Point";
        rec.layout.size = 8;
        rec.layout.align = 4;
        rec.layout.scheme = "v0";
        rec.layout.fields = {{"z", {}, 0, 4, 4, false}};
        h.put(rec);
        Analyzed a("declare struct Point { x: f64 }\n", BuildConfig{}, &h);
        CHECK(a.run.diag->warningCount() == 2);  // W0202 + W0203
    }
}

void testLayoutHistoryRoundTrip() {
    LayoutHistory h;
    LayoutRecord rec;
    rec.name = "Vec3";
    rec.layout.size = 12;
    rec.layout.align = 4;
    rec.layout.fields = {{"x", {}, 0, 4, 4, false}, {"y", {}, 4, 4, 4, false}};
    h.put(rec);
    const std::string path = "/tmp/xlang_layout_roundtrip.layout";
    h.save(path);
    const LayoutHistory loaded = LayoutHistory::load(path);
    const LayoutRecord* r = loaded.find("Vec3");
    CHECK(r != nullptr && r->layout.size == 12 && r->layout.fields.size() == 2 &&
          r->layout.fields[1].offset == 4);
}

void testConversionRules() {
    const Type i8 = Type::makeInt(8, true);
    const Type i32 = Type::makeInt(32, true);
    const Type i64 = Type::makeInt(64, true);
    const Type u8 = Type::makeInt(8, false);
    const Type u32 = Type::makeInt(32, false);
    const Type f32 = Type::makeFloat(32);
    const Type f64 = Type::makeFloat(64);
    const Type p = Type::makePointer(i32);
    const Type q = Type::makePointer(u8);

    CHECK(implicitConversion(i8, i64, false).status == ConversionStatus::Ok);
    CHECK(implicitConversion(u8, u32, false).status == ConversionStatus::Ok);
    CHECK(implicitConversion(i32, i8, false).status == ConversionStatus::NeedsCast);
    CHECK(implicitConversion(i32, u32, false).status == ConversionStatus::Signedness);
    CHECK(implicitConversion(u8, i32, false).status == ConversionStatus::Signedness);
    CHECK(implicitConversion(u8, i32, true).status == ConversionStatus::Ok);
    CHECK(implicitConversion(i32, u8, true).status == ConversionStatus::NeedsCast);  // narrowing
    CHECK(implicitConversion(i32, f64, false).status == ConversionStatus::Warn);
    CHECK(implicitConversion(u32, f64, false).status == ConversionStatus::Signedness);
    CHECK(implicitConversion(u32, f64, true).status == ConversionStatus::Warn);
    CHECK(implicitConversion(f32, f64, false).status == ConversionStatus::Ok);
    CHECK(implicitConversion(f64, f32, false).status == ConversionStatus::NeedsCast);
    CHECK(implicitConversion(f64, i32, false).status == ConversionStatus::NeedsCast);
    CHECK(implicitConversion(p, q, false).status == ConversionStatus::NeedsCast);
    CHECK(implicitConversion(p, p, false).status == ConversionStatus::Ok);
    CHECK(implicitConversion(i32, Type::makeBool(), false).status == ConversionStatus::NeedsCast);

    CHECK(commonBinaryType(i32, i64).status == ConversionStatus::Ok);
    CHECK(commonBinaryType(i32, i64).type.bits == 64);
    CHECK(commonBinaryType(i32, u32).status == ConversionStatus::Signedness);
    CHECK(commonBinaryType(i32, f64).warn);
    CHECK(commonBinaryType(u32, f64).status == ConversionStatus::Signedness);
}

void testConfigParsing() {
    CHECK(ConfigValue::parse("").bool_value);
    CHECK(ConfigValue::parse("3").int_value == 3);
    CHECK(ConfigValue::parse("0x10").int_value == 16);
    CHECK(ConfigValue::parse("\"hi\"").string_value == "hi");
    CHECK(ConfigValue::parse("false").kind == ConfigValue::Kind::Bool);
    CHECK(parseTargetTriple("arm64-apple-macos")->arch == "aarch64");
    CHECK(!parseTargetTriple("riscv64-unknown-linux").has_value());
    BuildConfig c;
    CHECK(c.lookup("pointerWidth")->int_value == 64);
    c.defines["os"] = ConfigValue::makeString("custom");
    CHECK(c.lookup("os")->string_value == "custom");
}

}  // namespace

void runSemaTests() {
    testSingleLineAndBlockConditionals();
    testDefinesNestingAndDefined();
    testPreprocessorErrors();
    testConditionalInsideStructAndFunction();
    testMacros();
    testMacroErrors();
    testMacroGeneratesItemsAndNested();
    testLayoutsMatchClang();
    testDefaultSchemeReordersDeterministically();
    testConstantsAndEnums();
    testSemaErrors();
    testAliasesResolve();
    testDeclareLayoutWarningsAndHistory();
    testLayoutHistoryRoundTrip();
    testConversionRules();
    testConfigParsing();
}
