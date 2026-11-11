#include "check.h"

#include "xlang/hir/hir.h"
#include "xlang/lang/frontend.h"
#include "xlang/sema/compilation.h"
#include "xlang/sema/scope.h"
#include "xlang/sema/sema.h"

#include <sstream>

using namespace xlang;

namespace {

struct Lowered {
    SourceManager sources;
    std::ostringstream out;
    BuildConfig config;
    std::unique_ptr<DiagnosticEngine> diag;
    Program program;
    hir::Module module;
    std::string hir_text;

    explicit Lowered(std::string text, BuildConfig cfg = BuildConfig{}) : config(std::move(cfg)) {
        const FileId file = sources.addFile("t.xl", std::move(text));
        diag = std::make_unique<DiagnosticEngine>(sources, out);
        program = parseSource(sources, file, config, *diag);
        Sema sema(program, config, *diag);
        if (!diag->hasErrors()) {
            sema.analyze();
        }
        if (!diag->hasErrors()) {
            module = sema.lower();
            std::ostringstream h;
            hir::dumpModule(h, module);
            hir_text = h.str();
        }
        diag->flush();
    }

    [[nodiscard]] bool has(const std::string& needle) const {
        return hir_text.find(needle) != std::string::npos;
    }
    [[nodiscard]] bool failedWith(const std::string& code) const {
        return diag->hasErrors() && out.str().find(code) != std::string::npos;
    }
};

std::size_t countOf(const std::string& text, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) {
        ++n;
    }
    return n;
}

void testScopeStack() {
    ScopeStack s;
    s.push(ScopeKind::Module);
    ScopeEntry g;
    g.name = "x";
    g.kind = ScopeEntry::Kind::Global;
    CHECK(s.declare(g) == nullptr);
    CHECK(s.declare(g) != nullptr);  // second binding in the same scope reports the first
    s.push(ScopeKind::Function);
    ScopeEntry l;
    l.name = "x";
    l.kind = ScopeEntry::Kind::Local;
    CHECK(s.declare(l) == nullptr);  // inner scope shadows
    CHECK(s.lookup("x")->kind == ScopeEntry::Kind::Local);
    CHECK(s.lookupModule("x")->kind == ScopeEntry::Kind::Global);
    s.pop();
    CHECK(s.lookup("x")->kind == ScopeEntry::Kind::Global);
    CHECK(s.lookup("missing") == nullptr);
}

void testLowersPlainFunctions() {
    Lowered r("fn add(a: i32, b: i32) -> i32 { return a + b }\n"
              "fn main() -> i32 {\n"
              "    local x: i64 = 1\n"
              "    local y: i32 = add(1, 2)\n"
              "    return y\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn add(a: i32, b: i32) -> i32"));
    CHECK(r.has("(call add 1:i32 2:i32):i32"));
    CHECK(r.has("local x#0: i64 = 1:i64"));  // literal takes the declared type
}

void testImplicitConversionsBecomeCasts() {
    Lowered r("fn main() -> i64 {\n"
              "    local a: i8 = 1\n"
              "    local b: i64 = a\n"
              "    return b + a\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(cast.implicit a#0):i64"));
}

void testConversionDiagnostics() {
    {
        Lowered r("fn f() { local n: u8 = 5\n local m: i32 = n }\n");
        CHECK(r.failedWith("E0601"));
    }
    {
        Lowered r("fn f() { local n: u8 = 5\n unsafe { local m: i32 = n } }\n");
        CHECK(!r.diag->hasErrors());
    }
    {
        Lowered r("fn f() { local n: i64 = 5\n local m: i8 = n }\n");
        CHECK(r.failedWith("E0708"));
    }
    {
        Lowered r("fn f() { local n: i32 = 5\n local m: f64 = n }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.diag->warningCount() == 1);
        CHECK(r.out.str().find("W0601") != std::string::npos);
    }
    {
        Lowered r("fn f() { local n: i32 = 5\n local m: f64 = n as f64 }\n");
        CHECK(r.diag->warningCount() == 0);
    }
    {
        Lowered r("fn f() { local a: i32 = 1\n local b: u32 = 2\n local c: i32 = a + b }\n");
        CHECK(r.failedWith("E0601"));
    }
}

void testNamesAndShadowing() {
    {
        Lowered r("fn f() { local x = nope }\n");
        CHECK(r.failedWith("E0706"));
    }
    {
        Lowered r("fn f(a: i32) { local a: i32 = 1 }\n");
        CHECK(r.failedWith("E0401"));
    }
    {
        Lowered r("fn f() -> i32 { local x: i32 = 1\n if true { local x: i32 = 2\n return x }\n return x }\n");
        CHECK(!r.diag->hasErrors());
    }
    {
        Lowered r("struct P { x: i32 }\nfn f() -> i32 { return P }\n");
        CHECK(r.failedWith("E0706"));
    }
}

void testCallsAndArity() {
    Lowered r("fn one(a: i32) -> i32 { return a }\nfn f() -> i32 { return one(1, 2) }\n");
    CHECK(r.failedWith("E0710"));
    Lowered p("fn apply(f: fn(i32) -> i32, v: i32) -> i32 { return f(v) }\n"
              "fn dbl(x: i32) -> i32 { return x * 2 }\n"
              "fn main() -> i32 { return apply(dbl, 3) }\n");
    CHECK(!p.diag->hasErrors());
    CHECK(p.has("call.indirect"));
    CHECK(p.has("&fn dbl"));
}

void testSyscallValidation() {
    {
        BuildConfig c;
        c.target = *parseTargetTriple("x86_64-unknown-linux-gnu");
        Lowered ok("fn f(p: *u8) -> i64 { return syscall(1, 1, p, 3) }\n", c);
        CHECK(!ok.diag->hasErrors());
        CHECK(ok.has("(syscall 1:i32 1:i32 p#0 3:i32):i64"));
        Lowered many("fn f() -> i64 { return syscall(1, 1, 2, 3, 4, 5, 6, 7) }\n", c);
        CHECK(many.failedWith("E0710"));
        Lowered bad("fn f() -> i64 { return syscall(1, 1.5) }\n", c);
        CHECK(bad.failedWith("E0708"));
    }
}

void testFieldsPointersAndIndexing() {
    Lowered r("struct Point { x: i32, y: i32 }\n"
              "fn f(p: *Point) -> i32 {\n"
              "    local a: [i32; 4]\n"
              "    a[1] = p.x\n"
              "    p.y = *(&a[1])\n"
              "    return a[0]\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(field.ptr x p#0)"));
    CHECK(r.has("index.checked"));
    {
        Lowered b("fn f() { local a: [i32; 4]\n a[4] = 1 }\n");
        CHECK(b.failedWith("E0701"));
    }
    {
        BuildConfig c;
        c.safety = "unchecked";
        Lowered u("fn f(i: usize) { local a: [i32; 4]\n a[i] = 1 }\n", c);
        CHECK(!u.diag->hasErrors());
        CHECK(!u.has("index.checked"));
    }
    {
        Lowered b("struct P { x: i32 }\nfn f(p: P) { p.z = 1 }\n");
        CHECK(b.failedWith("E0709"));
    }
    {
        Lowered b("fn f() { [1, 2][0] = 1 }\n");
        CHECK(b.failedWith("E0709"));
    }
}

void testInterfacesAreNominal() {
    const std::string common =
        "interface Writer { fn write(self, n: i32) -> i32 }\n"
        "struct Out { fd: i32 }\n"
        "fn emit(w: Writer) -> i32 { return w.write(1) }\n";
    {
        Lowered r(common + "impl Writer for Out { fn write(self, n: i32) -> i32 { return self.fd + n } }\n"
                           "fn main() -> i32 { local o: Out = Out { fd: 1 }\n return emit(o) }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("(call.itable write[0] w#0 1:i32):i32"));
        CHECK(r.has("to.interface Out"));
    }
    {
        Lowered r(common + "fn main() -> i32 { local o: Out = Out { fd: 1 }\n return emit(o) }\n");
        CHECK(r.failedWith("E0504"));
    }
}

void testMethodsAreFunctionsWithSelf() {
    Lowered r("class Shape {\n"
              "    id: i32\n"
              "    fn get(self) -> i32 { return self.id }\n"
              "    virtual fn area(self) -> f64 { return 0.0 }\n"
              "}\n"
              "fn f(s: *Shape) -> i32 { local v: f64 = s.area()\n return s.get() }\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn Shape.get(self: *Shape) -> i32"));
    CHECK(r.has("(call Shape.get s#0):i32"));
    CHECK(r.has("(call.virtual Shape.area[0] s#0):f64"));
}

void testNewAndRegisteredAllocator() {
    const std::string types =
        "struct Point { x: i32, y: i32 }\n"
        "global slab: [u8; 64]\n";
    {
        Lowered r(types + "fn main() -> i32 { local p: *Point = new Point { x: 1, y: 2 }\n return p.x }\n");
        CHECK(r.failedWith("E0501"));
    }
    {
        Lowered r(types + "fn main() { local p: *Point = nil\n delete p }\n");
        CHECK(r.failedWith("E0709") || r.failedWith("E0501"));
    }
    {
        Lowered r(types + "register(alloc) fn grab(size: usize, align: usize) -> *void { return nil }\n"
                          "register(free) fn drop(p: *void) { }\n"
                          "fn main() -> i32 { local p: *Point = new Point { x: 1, y: 2 }\n"
                          "delete p\n return p.x }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("(new Point registered"));
        CHECK(r.module.alloc_function == "grab");
        CHECK(r.module.free_function == "drop");
    }
    {
        Lowered r(types + "register(alloc) fn grab(size: i32) -> *void { return nil }\n");
        CHECK(r.failedWith("E0503"));
    }
    {
        Lowered r(types + "register(free) fn drop(p: *u8) { }\n");
        CHECK(r.failedWith("E0503"));
    }
    {
        Lowered r(types + "fn main() -> i32 {\n"
                          "local p: *Point = new(&slab[0]) Point { x: 1, y: 2 }\n return p.x }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("(new Point"));
    }
    {
        Lowered r(types + "fn main() -> i32 { local pt: Point = Point { x: 0, y: 0 }\n"
                          "local p: *Point = new(pt) Point { x: 1, y: 2 }\n return p.x }\n");
        CHECK(r.failedWith("E0709"));
    }
}

void testRegisterRoles() {
    {
        Lowered r("register(panic) fn panic(msg: *u8) { }\nregister(panic) fn panic2(msg: *u8) { }\n");
        CHECK(r.failedWith("E0502"));
    }
    {
        Lowered r("register(panic) fn panic(code: i32) { }\n");
        CHECK(r.failedWith("E0503"));
    }
    {
        Lowered r("register(panic) fn panic(msg: *u8) { }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.module.panic_function == "panic");
    }
    {
        Lowered r("register(panic) fn panic(msg: *u8) { }\nfn f() { local panic: i32 = 1 }\n");
        CHECK(r.out.str().find("W0402") != std::string::npos);
    }
    {
        // `start` is no longer a role: `@init` replaces it.
        Lowered r("register(start) fn start() { }\n");
        CHECK(r.failedWith("E0503"));
    }
}

void testInitFunctions() {
    {
        Lowered r("@init fn boot() { }\nfn main() -> i32 { return 0 }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.module.init_functions == std::vector<std::string>{"boot"});
        CHECK(r.has("init boot"));
    }
    {
        Lowered r("@init fn first() { }\n@init fn second() { }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.module.init_functions == (std::vector<std::string>{"first", "second"}));
    }
    {
        Lowered r("@init fn boot(x: i32) { }\n");
        CHECK(r.failedWith("E0729"));
    }
    {
        Lowered r("@init fn boot() -> i32 { return 0 }\n");
        CHECK(r.failedWith("E0729"));
    }
    {
        Lowered r("@init fn boot<T>() { }\n");
        CHECK(r.failedWith("E0729"));
    }
    {
        Lowered r("@init fn boot();\n");
        CHECK(r.failedWith("E0002"));
    }
    {
        Lowered r("@init global x: i32 = 1\n");
        CHECK(r.failedWith("E0729"));
    }
    {
        Lowered r("@init fn boot() { }\nfn f() { boot() }\n");
        CHECK(r.failedWith("E0727"));
    }
    {
        Lowered r("export(c) fn _start() { }\n");
        CHECK(r.failedWith("E0726"));
    }
    {
        Lowered r("export(c) fn mainCRTStartup() { }\n");
        CHECK(r.failedWith("E0726"));
    }
}

void testGlobalInitializersAreConstant() {
    {
        Lowered r("define N = 4\n"
                  "global table: [u8; N]\n"
                  "global first: *u8 = &table[0]\n"
                  "global limit: usize = N * 2\n"
                  "global banner: *u8 = \"xlang\"\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("global table: [u8; 4]"));
        CHECK(r.has("global limit: usize = (* 4:usize 2:usize):usize"));
    }
    {
        Lowered r("fn f() -> i32 { return 1 }\nglobal g: i32 = f()\n");
        CHECK(r.failedWith("E0707"));
    }
    {
        Lowered r("global a: i32 = 1\nglobal b: i32 = a\n");
        CHECK(r.failedWith("E0707"));
    }
}

void testImportsAreExternal() {
    Lowered r("import \"math\" as m\n"
              "from \"math\" import cube as c\n"
              "fn f() -> i32 { local x = m.square(3)\n local y = c(2)\n return 0 }\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("call.external math.square"));
    CHECK(r.has("call.external math.cube"));
    Lowered dup("import \"a\" as x\nimport \"b\" as x\n");
    CHECK(dup.failedWith("E0401"));
}

void testEnumsAndStaticAssertInBodies() {
    Lowered r("enum Color: i32 { Red, Green = 5 }\n"
              "fn f() -> i32 {\n"
              "    staticAssert(sizeOf(i32) == 4)\n"
              "    local c: Color = Color.Green\n"
              "    if c == Color.Red { return 1 }\n"
              "    return c as i32\n"
              "}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("local c#0: Color = 5:Color"));
    Lowered bad("fn f() { staticAssert(sizeOf(i32) == 5) }\n");
    CHECK(bad.failedWith("E0702"));
}

// ---- payload enums: `Add(A, B)` variants are built like calls and matched by `switch`

void testPayloadEnums() {
    constexpr std::string_view shapes =
        "enum Shape {\n Circle(i64)\n Rect(i32, i32)\n Empty\n}\n";
    const auto program = [&](std::string_view body) { return Lowered(std::string(shapes) + std::string(body)); };
    {
        // Every variant shape builds, matches and binds; the union is as big as its largest payload.
        Lowered ok = program(
            "staticAssert(sizeOf(Shape) == 16)\n"
            "fn area(s: Shape) -> i64 {\n"
            "    switch s {\n"
            "        case Shape.Circle(r) { return r }\n"
            "        case Shape.Rect(w, _) { return w as i64 }\n"
            "        case Shape.Empty { return 0 }\n"
            "    }\n"
            "    return -1\n"
            "}\n"
            "fn f() -> i64 { return area(Shape.Rect(3, 4)) + area(Shape.Empty) }\n");
        CHECK(!ok.diag->hasErrors());
    }
    {
        // `default` replaces missing cases; several variants can share a case when nothing is bound.
        Lowered ok = program(
            "fn f(s: Shape) -> i32 {\n"
            "    switch s {\n"
            "        case Shape.Circle, Shape.Rect { return 1 }\n"
            "        default { return 0 }\n"
            "    }\n"
            "}\n");
        CHECK(!ok.diag->hasErrors());
    }
    CHECK(program("fn f(s: Shape) {\n switch s {\n case Shape.Empty { }\n }\n}\n").failedWith("E0750"));
    CHECK(program("fn f() { local s: Shape = Shape.Circle }\n").failedWith("E0751"));
    CHECK(program("fn f() { local s: Shape = Shape.Empty(1) }\n").failedWith("E0751"));
    CHECK(program("fn f() { local s: Shape = Shape.Rect(1) }\n").failedWith("E0751"));
    CHECK(program("fn f() { local s: Shape = Shape.Missing }\n").failedWith("E0706"));
    CHECK(program("fn f(s: Shape) {\n switch s {\n case Shape.Rect(w) { }\n default { }\n }\n}\n")
              .failedWith("E0749"));
    CHECK(program("fn f(s: Shape) {\n switch s {\n case 1 { }\n default { }\n }\n}\n").failedWith("E0749"));
    CHECK(program("fn f(s: Shape) {\n switch s {\n case Shape.Empty { }\n case Shape.Empty { }\n default { }\n }\n}\n")
              .failedWith("E0709"));
    CHECK(Lowered("enum E: i32 { A(i32) }\n").failedWith("E0747"));
    CHECK(Lowered("enum E { A(i32), B = 2 }\n").failedWith("E0747"));
    {
        // A payload type that needs drop cannot sit in a variant: it would sit in a struct.
        Lowered bad("class Res {\n fn drop(self) { }\n}\nenum E {\n A(Res)\n}\n");
        CHECK(bad.failedWith("E0721"));
    }
}

// ---- multi-module programs (plan 4.6, 4.8, 4.9, 4.12)

struct Multi {
    SourceManager sources;
    std::ostringstream out;
    BuildConfig config;
    std::unique_ptr<DiagnosticEngine> diag;
    std::unique_ptr<Compilation> comp;
    hir::Module module;
    std::string hir_text;

    Multi(std::initializer_list<std::pair<const char*, const char*>> files, BuildConfig cfg = BuildConfig{})
        : config(std::move(cfg)) {
        diag = std::make_unique<DiagnosticEngine>(sources, out);
        comp = std::make_unique<Compilation>(sources, config, *diag);
        for (const auto& [name, text] : files) {
            comp->addVirtualFile(name, text);
        }
        comp->loadRoot(files.begin()->first);
        comp->finish();
        if (!diag->hasErrors()) {
            comp->analyzeAll();
        }
        if (!diag->hasErrors()) {
            module = comp->lowerAll();
            std::ostringstream h;
            hir::dumpModule(h, module);
            hir_text = h.str();
        }
        diag->flush();
    }

    [[nodiscard]] bool has(const std::string& needle) const { return hir_text.find(needle) != std::string::npos; }
    [[nodiscard]] bool failedWith(const std::string& code) const {
        return diag->hasErrors() && out.str().find(code) != std::string::npos;
    }
};

void testCyclicModules() {
    {
        // A name that reaches the importer only through an `export *` loop is never exported.
        Multi r({{"app/a.xl", "export * from \"b\"\nfn main() -> i32 { return 0 }\n"},
                 {"app/b.xl", "export * from \"a\"\nfrom \"a\" import ghost\n"}});
        CHECK(r.failedWith("E0711"));
    }
    {
        // Mutual imports with calls in both directions are legal.
        Multi r({{"app/a.xl",
                  "from \"b\" import fb\nexport fn fa() -> i32 { return 1 }\n"
                  "fn main() -> i32 { return fb() }\n"},
                 {"app/b.xl", "from \"a\" import fa\nexport fn fb() -> i32 { return fa() + 1 }\n"}});
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("fn fa("));
        CHECK(r.has("fn b::fb("));
    }
}

void testModulesImportAndExport() {
    {
        Multi r({{"app/main.xl",
                  "from \"math\" import square as sq\nimport \"util\" as u\n"
                  "fn main() -> i32 { return sq(3) + u.twice(2) }\n"},
                 {"app/math.xl", "export fn square(x: i32) -> i32 { return x * x }\nfn hidden() { }\n"},
                 {"app/util.xl", "export fn twice(x: i32) -> i32 { return x * 2 }\n"}});
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("(call math::square 3:i32)"));
        CHECK(r.has("(call util::twice 2:i32)"));
        CHECK(r.has("fn math::square("));
    }
    {
        Multi r({{"app/main.xl", "from \"math\" import hidden\nfn main() { }\n"},
                 {"app/math.xl", "fn hidden() { }\n"}});
        CHECK(r.failedWith("E0711"));
    }
    {
        Multi r({{"app/main.xl", "import \"nope\" as n\nfn main() { }\n"}});
        CHECK(r.failedWith("E0711"));
    }
    {
        Multi r({{"app/main.xl", "import * from \"a\"\nimport * from \"b\"\nfn main() { }\n"},
                 {"app/a.xl", "export fn lorem() { }\n"},
                 {"app/b.xl", "export fn lorem() { }\n"}});
        CHECK(r.failedWith("E0401"));
    }
    {
        Multi r({{"app/main.xl", "import * as aa from \"a\"\nimport * as bb from \"b\"\nfn main() { aa.lorem()\n bb.lorem() }\n"},
                 {"app/a.xl", "export fn lorem() { }\n"},
                 {"app/b.xl", "export fn lorem() { }\n"}});
        CHECK(!r.diag->hasErrors());
    }
}

void testModulesShareTypes() {
    Multi r({{"app/main.xl",
              "from \"geo\" import Point, Shape\n"
              "fn main() -> i32 { local p: Point = Point { x: 1, y: 2 }\n"
              "local s: Shape = Shape { id: 7 }\n return p.x + s.id }\n"},
             {"app/geo.xl", "export struct Point { x: i32, y: i32 }\nexport class Shape { id: i32 }\n"}});
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("struct geo::Point"));
    CHECK(r.has("local p#0: geo::Point"));
    CHECK(r.has("(field x p#0)"));
}

void testModuleRegistrationsAndMangling() {
    {
        Multi r({{"app/main.xl", "from \"rt\" import ping\nfn main() { panic(\"x\") }\n"},
                 {"app/rt.xl", "export fn ping() { }\nregister(panic) fn panic(msg: *u8) { }\n"}});
        CHECK(!r.diag->hasErrors());
        CHECK(r.module.panic_function == "rt::panic");
        CHECK(r.has("(call rt::panic"));
    }
    {
        Multi r({{"app/main.xl", "fn square(x: i32) -> i32 { return x }\n@mangle(\"my_sq\")\nfn other() { }\n"
                                 "export(c) fn plain(x: i32) -> i32 { return x }\n"}});
        CHECK(!r.diag->hasErrors());
        bool seen_mangled = false;
        bool seen_renamed = false;
        bool seen_plain = false;
        for (const hir::Function& f : r.module.functions) {
            seen_mangled = seen_mangled || f.symbol == "_xl$main$square$i32";
            seen_renamed = seen_renamed || f.symbol == "my_sq";
            seen_plain = seen_plain || f.symbol == "plain";
        }
        CHECK(seen_mangled && seen_renamed && seen_plain);
    }
    {
        BuildConfig c;
        c.mangle = "none";
        Multi r({{"app/main.xl", "import \"b\" as b\nexport fn dup() { }\n"},
                 {"app/b.xl", "export fn dup() { }\n"}},
                c);
        CHECK(r.failedWith("E0401"));
    }
    {
        Multi r({{"app/main.xl", "export(c) fn bad(a: [i32; 2]) { }\n"}});
        CHECK(r.failedWith("E0302"));
    }
}

void testClassesAndInterfacesChecks() {
    {
        Multi r({{"app/main.xl",
                  "class A { virtual fn f(self) -> i32 { return 1 } }\n"
                  "class B : A { override fn f(self) -> i32 { return 2 } }\n"
                  "fn main() -> i32 { local b: B = B { }\n return b.f() }\n"}});
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("(call.virtual B.f[0] "));
    }
    {
        Multi r({{"app/main.xl", "class A { fn f(self) { } }\nclass B : A { override fn f(self) { } }\n"}});
        CHECK(r.failedWith("E0712"));
    }
    {
        Multi r({{"app/main.xl",
                  "class A { virtual fn f(self) -> i32 { return 1 } }\n"
                  "class B : A { override fn f(self) -> i64 { return 2 } }\n"}});
        CHECK(r.failedWith("E0712"));
    }
    {
        Multi r({{"app/main.xl",
                  "interface W { fn write(self, n: i32) -> i32 }\nstruct S { }\n"
                  "impl W for S { fn write(self, n: i64) -> i32 { return 0 } }\n"}});
        CHECK(r.failedWith("E0713"));
    }
}

void testStrings() {
    {
        Lowered b("fn f() {\n local s = \"abc\"\n s[0] = 1\n}\n");
        CHECK(b.failedWith("E0716"));
    }
    {
        Lowered b("fn f() {\n local s = \"abc\"\n s.len = 1\n}\n");
        CHECK(b.failedWith("E0716"));
    }
    {
        Lowered b("fn f() {\n local b: [u8; 2] = \"abc\"\n}\n");
        CHECK(b.failedWith("E0717"));
    }
    {
        Lowered b("fn f() {\n local b: [u8; 3] = \"abc\"\n local s = \"x\\0y\"\n"
                  " local n: usize = s.len\n local c: u8 = s[1]\n}\n");
        CHECK(!b.diag->hasErrors());
    }
    {
        Lowered b("fn f(x: i32) {\n switch x {\n case 1 { }\n case 1 { }\n }\n}\n");
        CHECK(b.failedWith("E0709"));
    }
    {
        Lowered b("enum C { A, B }\nfn f(c: C) {\n switch c {\n case C.A { }\n case C.A { }\n }\n}\n");
        CHECK(b.failedWith("E0709"));
    }
}

void testConst() {
    {
        Lowered b("fn f(x: i32) {\n const y = x\n}\n");
        CHECK(b.failedWith("E0715"));
    }
    {
        Lowered b("global y: i32 = 1\nconst X = y\n");
        CHECK(b.failedWith("E0715"));
    }
    {
        Lowered b("const N = 1\nfn f() {\n N = 2\n}\n");
        CHECK(b.failedWith("E0716"));
    }
    {
        Lowered b("fn f() {\n const M = 2\n M = 3\n}\n");
        CHECK(b.failedWith("E0716"));
    }
    {
        Lowered b("const T: [i32; 2] = [1, 2]\nfn f() {\n T[0] = 3\n}\n");
        CHECK(b.failedWith("E0716"));
    }
    {
        Lowered b("const N = 1\nfn f() -> i32 {\n local p: *i32 = &N\n return N\n}\n");
        CHECK(!b.diag->hasErrors());
        CHECK(b.has("const "));
    }
}

void testOverloads() {
    {
        Lowered b("fn h(a: i64, b: i32) { }\nfn h(a: i32, b: i64) { }\n"
                  "fn main() -> i32 {\n h(1, 2)\n return 0 }\n");
        CHECK(b.failedWith("E0403"));
    }
    {
        Lowered b("fn k(x: str) { }\nfn k(x: bool) { }\nfn main() -> i32 {\n k(1.5)\n return 0 }\n");
        CHECK(b.failedWith("E0404"));
    }
    {
        Lowered b("export(c) fn e(x: i32) { }\nfn e(x: i64) { }\n");
        CHECK(b.failedWith("E0405"));
    }
    {
        Lowered b("fn d(x: i32) { }\nfn d(x: i32) { }\n");
        CHECK(b.failedWith("E0401"));
    }
    {
        Lowered b("fn q(x: i32) { }\nfn q(x: str) { }\nfn main() -> i32 {\n local g = q\n return 0 }\n");
        CHECK(b.failedWith("E0403"));
    }
    {
        Lowered b("fn q(x: i32) { }\nfn q(x: str) { }\nfn main() -> i32 {\n q(\"a\")\n return 0 }\n");
        CHECK(!b.diag->hasErrors());
    }
    {
        Lowered b("fn a = b\nfn b = a\nfn main() -> i32 {\n a(1)\n return 0 }\n");
        CHECK(b.failedWith("E0704"));
    }
}

void testGenerics() {
    {
        Lowered b("fn f<T>() -> i32 { return 0 }\nfn main() -> i32 {\n f()\n return 0 }\n");
        CHECK(b.failedWith("E0724"));
    }
    {
        Lowered b("interface I<T> { fn m(self) }\nfn f(x: I) { }\nfn main() -> i32 { return 0 }\n");
        CHECK(b.failedWith("E0724"));
    }
    {
        Lowered b("struct Box<T> { v: T }\nfn main() -> i32 {\n local b: Box<i32, i32>\n return 0 }\n");
        CHECK(b.failedWith("E0724"));
    }
    {
        Lowered b("struct Chain<T> { next: *Chain<*T> }\nfn main() -> i32 {\n local c: Chain<i32>\n"
                  " return 0 }\n");
        CHECK(b.failedWith("E0725"));
    }
}

// ---- generic payload enums: `enum Boxed<T> { Empty Full(T) }` is one type per argument list

void testGenericPayloadEnums() {
    constexpr std::string_view boxed =
        "enum Boxed<T> {\n Empty\n Full(T)\n}\n"
        "fn f(b: Boxed<i32>) -> i32 {\n"
        "    switch b {\n"
        "        case Boxed.Empty { return 0 }\n"
        "        case Boxed.Full(v) { return v }\n"
        "    }\n"
        "    return -1\n"
        "}\n";
    {
        // Constructing, matching and binding a single-parameter instance.
        Lowered ok(std::string(boxed) +
                   "fn main() -> i32 { return f(Boxed.Full(7)) }\n");
        CHECK(!ok.diag->hasErrors());
    }
    {
        // Two parameters, and a variant without data used as a plain value.
        Lowered ok("enum Result<T, E> {\n Ok(T)\n Err(E)\n}\n"
                   "fn g(r: Result<i32, i32>) -> i32 {\n"
                   "    switch r {\n"
                   "        case Result.Ok(v) { return v }\n"
                   "        case Result.Err(e) { return e }\n"
                   "    }\n"
                   "    return 0\n"
                   "}\n"
                   "fn main() -> i32 { return g(Result.Ok(1)) }\n");
        CHECK(!ok.diag->hasErrors());
    }
    {
        // A droppable payload: the enum's glue drops only the live variant's value.
        Lowered ok("class Res {\n fn drop(self) { }\n}\n"
                   "enum Boxed<T> {\n Empty\n Full(T)\n}\n"
                   "fn f(b: Boxed<Res>) -> i32 {\n"
                   "    switch b {\n"
                   "        case Boxed.Empty { return 0 }\n"
                   "        case Boxed.Full(v) { return 1 }\n"
                   "    }\n"
                   "    return -1\n"
                   "}\n"
                   "fn main() -> i32 { return 0 }\n");
        CHECK(!ok.diag->hasErrors());
    }
    CHECK(Lowered(std::string(boxed) + "fn main() -> i32 { return f(Boxed.Full) }\n")
              .failedWith("E0751"));
    CHECK(Lowered(std::string(boxed) + "fn main() -> i32 { local b: Boxed<i32> = Boxed.Full }\n"
              "fn g() -> i32 { return 0 }\n")
              .failedWith("E0751"));
    {
        // A droppable payload of a generic instance: the payload enum itself needs drop, so a
        // global or struct-field position is rejected (E0758), and its array (E0759).
        Lowered ok("class Res {\n fn drop(self) { }\n}\n"
                   "enum Boxed<T> {\n Empty\n Full(T)\n}\n"
                   "fn f(b: Boxed<Res>) -> i32 {\n"
                   "    switch b {\n"
                   "        case Boxed.Empty { return 0 }\n"
                   "        case Boxed.Full(v) { return 1 }\n"
                   "    }\n"
                   "    return -1\n"
                   "}\n"
                   "fn main() -> i32 {\n local b: Boxed<Res> = Boxed.Full(Res())\n return f(b)\n }\n");
        CHECK(!ok.diag->hasErrors());
    }
    {
        // A droppable payload type in a generic instance, used and dropped in a local.
        Lowered ok("class Res {\n fn drop(self) { }\n}\n"
                   "enum Result<T, E> {\n Ok(T)\n Err(E)\n}\n"
                   "fn f(r: Result<Res, i32>) -> i32 {\n"
                   "    switch r {\n"
                   "        case Result.Ok(v) { return 1 }\n"
                   "        case Result.Err(c) { return c }\n"
                   "    }\n"
                   "    return 0\n"
                   "}\n"
                   "fn main() -> i32 {\n local r: Result<Res, i32> = Result.Ok(Res())\n"
                   " return f(r)\n }\n");
        CHECK(!ok.diag->hasErrors());
    }
    {
        // A nested generic argument: `Result<Boxed<i32>, i32>` is one instance of `Result`.
        Lowered ok("enum Boxed<T> {\n Empty\n Full(T)\n}\n"
                   "enum Result<T, E> {\n Ok(T)\n Err(E)\n}\n"
                   "fn f(r: Result<Boxed<i32>, i32>) -> i32 {\n"
                   "    switch r {\n"
                   "        case Result.Err(c) { return c }\n"
                   "        case Result.Ok(b) {\n"
                   "            switch b {\n"
                   "                case Boxed.Empty { return 0 }\n"
                   "                case Boxed.Full(v) { return v }\n"
                   "            }\n"
                   "            return 1\n"
                   "        }\n"
                   "    }\n"
                   "    return -1\n"
                   "}\n"
                   "fn main() -> i32 {\n"
                   " local r: Result<Boxed<i32>, i32> = Result.Ok(Boxed.Full(5))\n"
                   " return f(r)\n }\n");
        CHECK(!ok.diag->hasErrors());
    }
    // E0756: a variant of a generic enum built where no target type is known.
    CHECK(Lowered("enum Result<T, E> {\n Ok(T)\n Err(E)\n}\n"
                  "fn main() -> i32 {\n local r = Result.Ok(1)\n return 0\n }\n")
              .failedWith("E0756"));
    {
        // E0757: a `case` that names a variant of a different enum.
        Lowered wrong("enum A {\n X\n Y(i32)\n}\n"
                      "enum B {\n X\n Y(i32)\n}\n"
                      "fn f(a: A) -> i32 {\n"
                      "    switch a {\n"
                      "        case B.X { return 0 }\n"
                      "        default { return 1 }\n"
                      "    }\n"
                      "}\n"
                      "fn main() -> i32 { return 0 }\n");
        CHECK(wrong.failedWith("E0757"));
    }
    {
        // E0758: a droppable payload enum in a struct field.
        Lowered bad("class Res {\n fn drop(self) { }\n}\n"
                    "enum Boxed<T> {\n Empty\n Full(T)\n}\n"
                    "struct Holder {\n b: Boxed<Res>\n}\n"
                    "fn main() -> i32 { return 0 }\n");
        CHECK(bad.failedWith("E0758"));
    }
    {
        // E0759: an array of a droppable payload enum.
        Lowered bad("class Res {\n fn drop(self) { }\n}\n"
                    "enum Boxed<T> {\n Empty\n Full(T)\n}\n"
                    "fn main() -> i32 {\n local a: [Boxed<Res>; 2]\n return 0\n }\n");
        CHECK(bad.failedWith("E0759"));
    }
    CHECK(Lowered("enum Boxed<T> {\n Empty\n Full(T)\n}\n"
                  "fn main() -> i32 { local b: Boxed<i32, i32> = Boxed.Empty\n return 0 }\n")
              .failedWith("E0724"));
}

void testRawMoves() {
    {
        Lowered r("fn main() -> i32 {\n local x: i32 = 0\n __builtin_write(&x, 1)\n"
                  " return __builtin_read(&x) }\n");
        CHECK(!r.diag->hasErrors());
    }
    {
        Lowered r("fn main() -> i32 {\n local x: i32 = 0\n local y = __builtin_write(&x, 1)\n return 0 }\n");
        CHECK(r.failedWith("E0709"));
    }
    {
        Lowered r("fn main() -> i32 {\n local x: i32 = 0\n return __builtin_read(x) }\n");
        CHECK(r.failedWith("E0709"));
    }
    {
        Lowered r("fn main() -> i32 {\n __builtin_write(1, 2)\n return 0 }\n");
        CHECK(r.failedWith("E0709"));
    }
    {
        Lowered r("fn main() -> i32 {\n return __builtin_read() }\n");
        CHECK(r.failedWith("E0710"));
    }
}

void testDroppableArraysAndNotes() {
    const std::string cls = "class C { fn drop(self) { } }\n";
    {
        Lowered r(cls + "fn main() -> i32 {\n local a: [C; 2]\n return 0 }\n");
        CHECK(r.failedWith("E0721"));
    }
    {
        Lowered r(cls + "fn f(a: [C; 2]) { }\nfn main() -> i32 { return 0 }\n");
        CHECK(r.failedWith("E0721"));
    }
    {
        Lowered r(cls + "global g: [C; 2]\n");
        CHECK(r.failedWith("E0721"));
    }
    {
        Lowered r(cls + "struct S { a: [C; 2] }\n");
        CHECK(r.failedWith("E0721"));
    }
    {
        Lowered r(cls + "class D { a: [C; 2] }\n");
        CHECK(r.failedWith("E0721"));
    }
    {
        Lowered r(cls + "fn main() -> i32 {\n local a: [[C; 2]; 2]\n return 0 }\n");
        CHECK(r.failedWith("E0721"));
    }
    {
        Lowered r("fn main() -> i32 {\n local a: [i32; 2]\n return 0 }\n");
        CHECK(!r.diag->hasErrors());
    }
    {
        Lowered r("@init fn boot() { }\nfn main() -> i32 {\n local f = boot\n return 0 }\n");
        CHECK(r.failedWith("E0727"));
    }
    {
        Lowered r("fn bad<T>(x: T) -> i32 {\n return missingName\n}\n"
                  "fn main() -> i32 {\n return bad<i32>(1)\n}\n");
        CHECK(r.failedWith("E0706"));
        CHECK(r.out.str().find("in instantiation of `bad<i32>` requested here") !=
              std::string::npos);
    }
    {
        Lowered r("fn inner<T>(x: T) -> i32 {\n return missingName\n}\n"
                  "fn outer<T>(x: T) -> i32 {\n return inner<T>(x)\n}\n"
                  "fn main() -> i32 {\n return outer<i32>(1)\n}\n");
        CHECK(r.diag->hasErrors());
        CHECK(r.out.str().find("in instantiation of `inner<i32>` requested here") !=
              std::string::npos);
        CHECK(r.out.str().find("in instantiation of `outer<i32>` requested here") !=
              std::string::npos);
    }
    {
        // Each argument is lowered once, so six nested calls with four overloads stay linear.
        Lowered r("fn f(x: i64) -> i64 { return x }\nfn f(x: i32) -> i64 { return x as i64 }\n"
                  "fn f(x: bool) -> i64 { return 1 }\nfn f(x: f64) -> i64 { return 2 }\n"
                  "fn main() -> i32 {\n return f(f(f(f(f(f(1)))))) as i32\n}\n");
        CHECK(!r.diag->hasErrors());
    }
}

void testOwnership() {
    {
        Lowered b("class C { fn drop(self) { } }\nclass H { c: C }\n"
                  "fn f(h: H) {\n local x = h.c\n}\n");
        CHECK(b.failedWith("E0720"));
    }
    {
        Lowered b("class C { fn drop(self) { } }\nstruct S { c: C }\n");
        CHECK(b.failedWith("E0721"));
    }
    {
        Lowered b("class C { fn init(self) -> i32 { return 1 } }\n");
        CHECK(b.failedWith("E0722"));
    }
    {
        Lowered b("class C { fn drop(self, x: i32) { } }\n");
        CHECK(b.failedWith("E0722"));
    }
    {
        Lowered b("class C { fn init(self, x: i32) { } }\nfn f() {\n local c = C { }\n}\n");
        CHECK(b.failedWith("E0723"));
    }
    {
        Lowered b("class C { fn init(self, x: i32) { } fn drop(self) { } }\n"
                  "fn f() {\n local c = C(1)\n}\n");
        CHECK(!b.diag->hasErrors());
    }
}

void testVariadicPacks() {
    {
        // Each argument count gets its own instance, and `args.len` folds to a constant.
        Lowered r("fn count(..args) -> usize { return args.len as usize }\n"
                  "fn main() -> i32 {\n local a: usize = count()\n local b: usize = count(1)\n"
                  " local c: usize = count(1, 2, 3)\n return (a + b + c) as i32 }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("count<>"));
        CHECK(r.has("count<i32>"));
        CHECK(r.has("count<i32, i32, i32>"));
        CHECK(r.has("(cast 3:i32):usize"));  // args.len folded to the element count
        CHECK(!r.has("args.len"));
    }
    {
        // A `for` over the pack is unrolled: one call per argument, each with its own type.
        Lowered r("fn tap(x: i32) -> i32 { return x }\nfn tap(x: str) -> i32 { return 0 }\n"
                  "fn unroll(..args) -> i32 {\n local s: i32 = 0\n for a in args {\n  s = s + tap(a)\n }\n"
                  " return s }\n"
                  "fn main() -> i32 {\n return unroll(1, 2, 3) }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(countOf(r.hir_text, "(call tap$i32 ") == 3);
        CHECK(r.has("local a#5: i32 = args#1#1"));
    }
    {
        // A pack forwarded through another pack: the inner call receives the same elements.
        Lowered r("fn inner(..args) -> i32 { return args.len as i32 }\n"
                  "fn outer(..args) -> i32 { return inner(..args) }\n"
                  "fn main() -> i32 { return outer(1, 2) }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("(call inner<i32, i32> args#0#0 args#1#1):i32"));
    }
    {
        // A fixed-arity overload with the same call shape wins over a pack instance.
        Lowered r("fn pick(a: i32) -> i32 { return a }\nfn pick(..args) -> i32 { return 0 }\n"
                  "fn main() -> i32 { return pick(1) }\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("(call pick$i32 1:i32):i32"));
        CHECK(!r.has("pick<"));
    }
    {
        Lowered r("fn bad(..a, x: i32) { }\n");
        CHECK(r.failedWith("E0734"));
    }
    {
        Lowered r("fn bad(..a, ..b) { }\n");
        CHECK(r.failedWith("E0734"));
    }
    {
        Lowered r("fn bad(..a) -> i32 {\n return a\n}\n");
        CHECK(r.failedWith("E0735"));
    }
    {
        Lowered r("export(c) fn bad(..a) { }\n");
        CHECK(r.failedWith("E0736"));
    }
    {
        Lowered r("class C { fn m(self, ..a) { } }\n");
        CHECK(r.failedWith("E0736"));
    }
    {
        Lowered r("fn bad(v: i32) { g(..v) }\nfn g(x: i32) { }\n");
        CHECK(r.failedWith("E0737"));
    }
    {
        Lowered r("fn bad(..a) -> i32 {\n return a[0]\n}\n");
        CHECK(r.failedWith("E0738"));
    }
}

// A byte literal lowers to a `u8` constant, and it takes any integer context through the normal
// widening: `0x1f`-style masks and `eq`-style comparisons need no `as`.
void testByteLiterals() {
    Lowered r("fn f() -> u8 { return b')' }\n"
              "fn g() -> i32 {\n local c = b'\\n'\n local n: i64 = b'x'\n"
              " local m: u8 = b')'\n return n as i32\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("41:u8"));            // `b')'` is 0x29 = 41
    CHECK(r.has("local c#0: u8 = 10")); // `b'\\n'`
    CHECK(r.has("120:u8"));            // `b'x'`
    {
        Lowered bad("fn f() { local n: u8 = 300 }\n");
        CHECK(bad.failedWith("E0708"));  // narrowing still needs `as`, byte literals are not special
    }
}

// `s[lo..hi]` lowers to a `Slice` of type `str`: a view, never a place. Bounds stay checked under
// --safety=checked, and a base that is not `str`/`[u8; N]` is rejected.
void testSlices() {
    {
        Lowered r("fn f(s: str) -> str {\n return s[1..3]\n}\n");
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("slice.checked"));
        CHECK(r.has("):str"));
    }
    {
        BuildConfig unchecked;
        unchecked.safety = "none";
        Lowered r("fn f(s: str) -> str {\n return s[1..3]\n}\n", unchecked);
        CHECK(!r.diag->hasErrors());
        CHECK(r.has("(slice "));
        CHECK(!r.has("slice.checked"));
    }
    {
        // Open-ended and array bases take the same node; a constant end past the array is E0701.
        Lowered r("fn f(s: str, b: [u8; 4]) {\n local x = s[2..]\n local y = b[..2]\n local z = b[0..]\n}\n");
        CHECK(!r.diag->hasErrors());
    }
    {
        Lowered bad("fn f(b: [u8; 4]) {\n local x = b[0..5]\n}\n");
        CHECK(bad.failedWith("E0701"));
    }
    {
        Lowered bad("fn f(p: *u8) {\n local x = p[0..2]\n}\n");
        CHECK(bad.failedWith("E0709"));
    }
    {
        // A slice is a `str` value, so writing through it is the same error as writing through a `str`.
        Lowered bad("fn f(s: str) {\n s[0..1] = \"x\"\n}\n");
        CHECK(bad.failedWith("E0716"));
    }
}

}  // namespace

void runLowerTests() {
    testScopeStack();
    testLowersPlainFunctions();
    testImplicitConversionsBecomeCasts();
    testConversionDiagnostics();
    testNamesAndShadowing();
    testCallsAndArity();
    testSyscallValidation();
    testFieldsPointersAndIndexing();
    testInterfacesAreNominal();
    testMethodsAreFunctionsWithSelf();
    testNewAndRegisteredAllocator();
    testRegisterRoles();
    testInitFunctions();
    testGlobalInitializersAreConstant();
    testImportsAreExternal();
    testEnumsAndStaticAssertInBodies();
    testPayloadEnums();
    testGenericPayloadEnums();
    testModulesImportAndExport();
    testCyclicModules();
    testModulesShareTypes();
    testModuleRegistrationsAndMangling();
    testClassesAndInterfacesChecks();
    testStrings();
    testConst();
    testOverloads();
    testOwnership();
    testDroppableArraysAndNotes();
    testRawMoves();
    testGenerics();
    testVariadicPacks();
    testByteLiterals();
    testSlices();
}
