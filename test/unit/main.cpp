#include "check.h"

#include "xlang/lang/ast.h"
#include "xlang/lang/lexer.h"
#include "xlang/lang/parser.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/error.h"
#include "xlang/util/source.h"

#include <sstream>
#include <variant>

using namespace xlang;

namespace {

struct Fixture {
    SourceManager sources;
    std::ostringstream out;
    FileId file{0};

    explicit Fixture(std::string text, std::string name = "app.xl") {
        file = sources.addFile(name, std::move(text));
    }

    [[nodiscard]] Span span(std::uint32_t line, std::uint32_t col, std::uint32_t end_col) const {
        Span s;
        s.file = file;
        s.line = line;
        s.column = col;
        s.end_line = line;
        s.end_column = end_col;
        return s;
    }
};

std::string numbered(int count) {
    std::string text;
    for (int i = 1; i <= count; ++i) {
        text += "line" + std::to_string(i) + "\n";
    }
    return text;
}

void testWarningContextAndTwoSidedLabels() {
    Fixture f(numbered(12));
    DiagnosticEngine engine(f.sources, f.out);

    Diagnostic d;
    d.code = "W0401";
    d.severity = Severity::Warning;
    d.message = "`lorem` imported twice";
    d.labels.push_back({f.span(5, 1, 6), "comes from a"});
    d.labels.push_back({f.span(6, 1, 6), "and from b"});
    d.detail = "same entity";
    d.fix = "import once";
    const std::string text = engine.renderText(d);

    const std::string expected =
        "warning[W0401]: `lorem` imported twice\n"
        " --> app.xl:5:1\n"
        "  |\n"
        "3 | line3\n"
        "4 | line4\n"
        "5 | line5    <======= comes from a\n"
        "6 | line6    <======= and from b\n"
        "7 | line7\n"
        "8 | line8\n"
        "  |\n"
        "detail: same entity\n"
        "fix:    import once";
    CHECK_EQ(text, expected);
}

void testErrorContextIsWiderAndGutterIsAligned() {
    Fixture f(numbered(30));
    DiagnosticEngine engine(f.sources, f.out);
    Diagnostic d;
    d.code = "E0401";
    d.severity = Severity::Error;
    d.message = "conflict";
    d.labels.push_back({f.span(15, 1, 3), "here"});
    const std::string text = engine.renderText(d);
    CHECK(text.find("\n 5 | line5\n") != std::string::npos);   // -10
    CHECK(text.find("\n25 | line25\n") != std::string::npos);   // +10
    CHECK(text.find("line4\n") == std::string::npos);
    CHECK(text.find("line26") == std::string::npos);
    CHECK(text.find("15 | line15    <======= here") != std::string::npos);
}

void testDistantMarksAreSeparatedAndNearOnesMerged() {
    Fixture f(numbered(40));
    DiagnosticEngine engine(f.sources, f.out);
    DiagnosticOptions options;
    options.context_warnings = 1;
    engine.configure(options);
    Diagnostic d;
    d.code = "W0401";
    d.severity = Severity::Warning;
    d.message = "far apart";
    d.labels.push_back({f.span(3, 1, 2), "a"});
    d.labels.push_back({f.span(30, 1, 2), "b"});
    const std::string text = engine.renderText(d);
    CHECK(text.find("\n  ...\n") != std::string::npos);
    CHECK(text.find(" 2 | line2\n") != std::string::npos);
    CHECK(text.find("29 | line29\n") != std::string::npos);
    CHECK(text.find("line10\n") == std::string::npos);
}

void testSameLineGetsCaretLines() {
    Fixture f("declare struct Point { x: f64 }\n");
    DiagnosticEngine engine(f.sources, f.out);
    Diagnostic d;
    d.code = "W0201";
    d.severity = Severity::Warning;
    d.message = "layout unknown";
    d.labels.push_back({f.span(1, 16, 21), "this type"});
    d.labels.push_back({f.span(1, 24, 25), "this field"});
    const std::string text = engine.renderText(d);
    CHECK(text.find("<======= this type") != std::string::npos);
    CHECK(text.find("\n  |                ^^^^^ this type\n") != std::string::npos);
    CHECK(text.find("\n  |                        ^ this field\n") != std::string::npos);
}

void testTabsAndLongLines() {
    std::string line = "\tx = " + std::string(300, 'a') + " BAD\n";
    Fixture f(line);
    DiagnosticEngine engine(f.sources, f.out);
    Diagnostic d;
    d.code = "W0401";
    d.severity = Severity::Warning;
    d.message = "long";
    d.labels.push_back({f.span(1, 306, 309), "here"});
    const std::string text = engine.renderText(d);
    CHECK(text.find('\t') == std::string::npos);                    // tabs expanded
    CHECK(text.find("\xE2\x80\xA6") != std::string::npos);          // truncated with an ellipsis
    CHECK(text.find("BAD") != std::string::npos);                   // marked column stays visible
}

void testSuppressionAndDeny() {
    {
        Fixture f(numbered(5));
        DiagnosticEngine engine(f.sources, f.out);
        DiagnosticOptions options;
        options.allow = {"W0601"};
        engine.configure(options);
        engine.warning("W0601", f.span(2, 1, 3), "x");
        engine.flush();
        CHECK(engine.warningCount() == 0);
        CHECK(f.out.str().empty());
    }
    {
        Fixture f(numbered(5));
        DiagnosticEngine engine(f.sources, f.out);
        engine.addAllowScope(f.span(2, 1, 6), {"W0601"});
        engine.warning("W0601", f.span(2, 2, 4), "inside");
        engine.warning("W0601", f.span(4, 1, 3), "outside");
        engine.flush();
        CHECK(engine.warningCount() == 1);
        CHECK(f.out.str().find("inside") == std::string::npos);   // silenced by the scope
        CHECK(f.out.str().find("outside") != std::string::npos);
        CHECK(f.out.str().find("warning[W0601]") != std::string::npos);
    }
    {
        Fixture f(numbered(5));
        DiagnosticEngine engine(f.sources, f.out);
        DiagnosticOptions options;
        options.deny = {"W0601"};
        engine.configure(options);
        engine.warning("W0601", f.span(2, 1, 3), "x");
        CHECK(engine.errorCount() == 1);
        CHECK(engine.warningCount() == 0);
    }
    {
        Fixture f(numbered(5));
        DiagnosticEngine engine(f.sources, f.out);
        DiagnosticOptions options;
        options.allow = {"E0601"};
        bool threw = false;
        try {
            engine.configure(options);
        } catch (const XlangError&) {
            threw = true;
        }
        CHECK(threw);  // errors cannot be silenced
    }
    {
        Fixture f(numbered(5));
        DiagnosticEngine engine(f.sources, f.out);
        DiagnosticOptions options;
        options.allow_all = true;
        options.warn_all = true;  // --warn-all wins over --allow-all
        engine.configure(options);
        engine.warning("W0601", f.span(2, 1, 3), "x");
        CHECK(engine.warningCount() == 1);
    }
}

void testJsonFormat() {
    Fixture f("a \"quoted\" line\n");
    DiagnosticEngine engine(f.sources, f.out);
    Diagnostic d;
    d.code = "E0002";
    d.message = "bad \"thing\"";
    d.labels.push_back({f.span(1, 1, 2), "tab\there"});
    const std::string json = engine.renderJson(d);
    CHECK(json.find("\"code\":\"E0002\"") != std::string::npos);
    CHECK(json.find("\"message\":\"bad \\\"thing\\\"\"") != std::string::npos);
    CHECK(json.find("\"text\":\"tab\\there\"") != std::string::npos);
    CHECK(json.find('\n') == std::string::npos);
}

void testRegistry() {
    for (const char* code : {"W0201", "W0202", "W0203", "W0301", "E0302", "W0401", "E0401", "E0501",
                             "E0502", "E0503", "E0504", "W0402", "E0601", "E0701", "W0601",
                             "E0726", "E0727", "E0729"}) {
        CHECK(findDiagnosticInfo(code) != nullptr);
    }
    CHECK(findDiagnosticInfo("E0601")->severity == Severity::Error);
    CHECK(findDiagnosticInfo("W0601")->severity == Severity::Warning);
    CHECK(!DiagnosticEngine::explain("W0601").empty());
    CHECK(DiagnosticEngine::explain("Z9999").empty());
}

// --- lexer / parser ------------------------------------------------------------------------

struct Parsed {
    Fixture fixture;
    DiagnosticEngine engine;
    Program program;

    explicit Parsed(std::string text) : fixture(std::move(text)), engine(fixture.sources, fixture.out) {
        program = parseFile(fixture.sources, fixture.file, engine);
    }
};

void testLexer() {
    Fixture f("fn f(a: u8) -> *i32 { return a != 0x1F && b <= 1_000 && c == 0o755 && d == 0644 }\n"
              "#if debug\nlocal s = \"a\\n\\x41\" // comment\n");
    DiagnosticEngine engine(f.sources, f.out);
    const std::vector<Token> tokens = lex(f.sources, f.file, engine);
    CHECK(!engine.hasErrors());
    CHECK(tokens.front().kind == TokenKind::Fn);
    CHECK(tokens.back().kind == TokenKind::End);
    bool saw_arrow = false;
    bool saw_hex = false;
    bool saw_directive = false;
    bool saw_string = false;
    bool saw_octal = false;
    bool saw_decimal_zero = false;
    for (const Token& t : tokens) {
        saw_arrow |= t.kind == TokenKind::Arrow;
        saw_hex |= t.kind == TokenKind::Int && t.int_value == 0x1F;
        saw_octal |= t.kind == TokenKind::Int && t.int_value == 0755;
        saw_decimal_zero |= t.kind == TokenKind::Int && t.int_value == 644;
        saw_directive |= t.kind == TokenKind::DirIf && t.newline_before;
        saw_string |= t.kind == TokenKind::String && t.text == "a\nA";
    }
    CHECK(saw_arrow);
    CHECK(saw_hex);
    CHECK(saw_octal);
    CHECK(saw_decimal_zero);
    CHECK(saw_directive);
    CHECK(saw_string);
}

void testLexerErrors() {
    Fixture f("local a = 1 $\nlocal s = \"open\nlocal n = 99999999999999999999\nx # if\nlocal o = 0o78\n");
    DiagnosticEngine engine(f.sources, f.out);
    (void)lex(f.sources, f.file, engine);
    CHECK(engine.errorCount() == 5);
}

// `b'x'`: one byte per literal, every escape form, and `b'` never read as the identifier `b`.
void testByteLiteralLexing() {
    Fixture f("local a = b')'\nlocal b = b'\\n'\nlocal c = b'\\x7f'\nlocal d = b'\\0'\n"
              "local e = b'\\\\'\nlocal g = b'\\''\nlocal h = b'a'\nlocal b1 = 1\n");
    DiagnosticEngine engine(f.sources, f.out);
    const std::vector<Token> tokens = lex(f.sources, f.file, engine);
    CHECK(!engine.hasErrors());
    std::vector<std::uint64_t> bytes;
    int idents = 0;
    for (const Token& t : tokens) {
        if (t.kind == TokenKind::ByteLit) {
            bytes.push_back(t.int_value);
        }
        idents += t.kind == TokenKind::Ident && t.text.rfind("b", 0) == 0 ? 1 : 0;
    }
    CHECK(bytes.size() == 7);
    CHECK(bytes == (std::vector<std::uint64_t>{')', '\n', 0x7f, 0, '\\', '\'', 'a'}));
    CHECK(idents == 2);  // `b` and `b1` stay ordinary identifiers
}

// A malformed byte literal is one diagnostic per literal, and lexing continues after it.
void testByteLiteralErrors() {
    Fixture f("local a = b'ab'\nlocal b = b'\nlocal c = b'\\q'\nlocal d = 1\n");
    DiagnosticEngine engine(f.sources, f.out);
    (void)lex(f.sources, f.file, engine);
    CHECK(engine.errorCount() >= 3);
}

void testParsesFunctionsAndTypes() {
    Parsed p("fn apply(f: fn(i32) -> i32, v: i32) -> i32 {\n"
             "    local a: [i32; 4] = [1, 2, 3, 4]\n"
             "    local r: *u8 = &a[0] as *u8\n"
             "    return f(v) + a[1] * 2\n"
             "}\n"
             "fn logValue(v: i32) { }\n");
    CHECK(!p.engine.hasErrors());
    CHECK(p.program.items.size() == 2);
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    CHECK(fn.name == "apply");
    CHECK(fn.params.size() == 2);
    CHECK(typeToString(fn.params[0].type) == "fn(i32) -> i32");
    CHECK(typeToString(fn.return_type) == "i32");
    CHECK(fn.body.statements.size() == 3);
    const auto& log = std::get<FnDecl>(p.program.items[1].decl);
    CHECK(!log.has_return_type);
    CHECK(log.return_type.isVoid());
}

void testNewlinesEndStatements() {
    // `*a = *b` on its own line must not continue `local t: i32 = *a`.
    Parsed p("fn swap(a: *i32, b: *i32) {\n"
             "    local t: i32 = *a\n"
             "    *a = *b\n"
             "    *b = t\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    CHECK(fn.body.statements.size() == 3);
    CHECK(fn.body.statements[1].kind == Stmt::Kind::Assign);
}

void testIfConditionDoesNotTakeStructLiteral() {
    Parsed p("fn f(x: i32, y: i32) -> i32 {\n"
             "    if x == y { return 1 }\n"
             "    local p: Point = Point { x: 1, y: 2 }\n"
             "    return 0\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    CHECK(fn.body.statements.size() == 3);
    CHECK(fn.body.statements[2].kind == Stmt::Kind::Return);
}

void testImportForms() {
    Parsed p("import \"m\" as a\n"
             "import * from \"m\"\n"
             "import * as a from \"m\"\n"
             "from \"m\" import x as y, z\n"
             "import x as y, z from \"m\"\n");
    CHECK(!p.engine.hasErrors());
    CHECK(p.program.items.size() == 5);
    const auto& from_first = std::get<ImportDecl>(p.program.items[3].decl);
    const auto& import_first = std::get<ImportDecl>(p.program.items[4].decl);
    CHECK(from_first.form == ImportDecl::Form::Names);
    CHECK(import_first.form == ImportDecl::Form::Names);
    CHECK(from_first.module == import_first.module);
    CHECK(from_first.names.size() == 2 && import_first.names.size() == 2);
    CHECK(from_first.names[0].alias == "y" && import_first.names[0].alias == "y");
}

void testReExports() {
    Parsed p("export * from \"m\"\n"
             "export * as ns from \"m\"\n"
             "export { a, b as c } from \"m\"\n"
             "export * from \"m\" except a, b\n");
    CHECK(!p.engine.hasErrors());
    CHECK(p.program.items.size() == 4);
    const auto& except = std::get<ExportFromDecl>(p.program.items[3].decl);
    CHECK(except.except.size() == 2);
    CHECK(std::get<ExportFromDecl>(p.program.items[2].decl).names[1].alias == "c");
}

void testDeclareRegisterAndModifiers() {
    Parsed p("declare fn api(a: i32) -> i32\n"
             "register(panic) fn panic(msg: *u8) { }\n"
             "export(c) fn memcpy(d: *void) { }\n"
             "@repr(c)\n"
             "declare struct P { x: f64 }\n"
             "declare struct Opaque\n");
    CHECK(!p.engine.hasErrors());
    CHECK(p.program.items[0].modifiers.declared);
    CHECK(p.program.items[1].modifiers.registered);
    CHECK(p.program.items[1].modifiers.register_role == "panic");
    CHECK(p.program.items[2].modifiers.export_c && p.program.items[2].modifiers.exported);
    CHECK(declBase(p.program.items[3]).attributes.size() == 1);
    CHECK(std::get<StructDecl>(p.program.items[4].decl).opaque);
}

void testClassInterfaceImplEnum() {
    Parsed p("class Shape { id: i32\n virtual fn area(self) -> f64 { return 0.0 } }\n"
             "class Circle : Shape { r: f64\n override fn area(self) -> f64 { return 1.0 } }\n"
             "interface Writer { fn write(self, buf: *u8, len: usize) -> isize }\n"
             "impl Writer for Circle { fn write(self, buf: *u8, len: usize) -> isize { return 0 } }\n"
             "enum E: i32 { A, B = 5, C }\n");
    CHECK(!p.engine.hasErrors());
    CHECK(p.program.items.size() == 5);
    const auto& circle = std::get<ClassDecl>(p.program.items[1].decl);
    CHECK(circle.has_base && circle.base[0] == "Shape");
    CHECK(circle.methods[0].is_override);
    CHECK(std::get<ClassDecl>(p.program.items[0].decl).methods[0].is_virtual);
    CHECK(std::get<InterfaceDecl>(p.program.items[2].decl).methods.size() == 1);
    CHECK(std::get<ImplDecl>(p.program.items[3].decl).methods.size() == 1);
    CHECK(std::get<EnumDecl>(p.program.items[4].decl).members.size() == 3);
}

void testDefineAndMacros() {
    Parsed p("define PAGE = 4096\n"
             "define square(x: expr) { x * x }\n"
             "type Byte = u8\n"
             "fn f() -> i32 { return square!(1 + 2) }\n");
    CHECK(!p.engine.hasErrors());
    CHECK(std::holds_alternative<DefineConstDecl>(p.program.items[0].decl));
    const auto& macro = std::get<DefineMacroDecl>(p.program.items[1].decl);
    CHECK(macro.params.size() == 1 && macro.body.size() == 3);
    const auto& fn = std::get<FnDecl>(p.program.items[3].decl);
    const Expr& call = *fn.body.statements[0].expr;
    CHECK(call.kind == Expr::Kind::MacroCall);
    CHECK(call.macro_args.size() == 1 && call.macro_args[0].size() == 3);
}

void testNewForms() {
    Parsed p("fn f() {\n"
             "    local a: *Point = new Point { x: 1, y: 2 }\n"
             "    local b: *Point = new(&arena) Point { x: 1, y: 2 }\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    const Expr& plain = *fn.body.statements[0].expr;
    const Expr& explicit_alloc = *fn.body.statements[1].expr;
    CHECK(plain.kind == Expr::Kind::New && plain.lhs == nullptr);
    CHECK(explicit_alloc.kind == Expr::Kind::New && explicit_alloc.lhs != nullptr);
    CHECK(explicit_alloc.fields.size() == 2);
}

void testAsmOperands() {
    Parsed p("fn f() {\n"
             "    __builtin_asm(\"xchg [{0}], {1}\", in(reg) a, inout(reg) x, clobber(\"cc\"))\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    const Expr& call = *fn.body.statements[0].expr;
    CHECK(call.args.size() == 4);
    CHECK(call.args[1]->kind == Expr::Kind::AsmOperand && call.args[1]->text == "in");
    CHECK(call.args[2]->kind == Expr::Kind::AsmOperand && call.args[2]->text == "inout");
    CHECK(call.args[3]->kind == Expr::Kind::Call);
}

void testAttributesOnStatementsRegisterAllowScope() {
    Parsed p("fn f(total: i64) {\n"
             "    @allow(W0601)\n"
             "    local x: f64 = total\n"
             "    local y: f64 = total\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    CHECK(fn.body.statements[0].attributes.size() == 1);
    CHECK(fn.body.statements[0].attributes[0].name == "allow");

    // The scope covers the attributed statement only.
    Span inside = fn.body.statements[0].span;
    Span outside = fn.body.statements[1].span;
    p.engine.warning("W0601", inside, "in scope");
    p.engine.warning("W0601", outside, "out of scope");
    CHECK(p.engine.warningCount() == 1);
}

void testRecoveryReportsSeveralErrors() {
    Parsed p("fn a( { }\n"
             "fn ok() -> i32 { return 1 }\n"
             "struct S { fn m() }\n"
             "fn b() {\n"
             "    local = 3\n"
             "    return 1 2\n"
             "}\n"
             "fn c() -> i32 { return 2 }\n");
    CHECK(p.engine.errorCount() >= 4);
    // Items that parse cleanly survive the errors around them.
    int fns = 0;
    for (const Item& item : p.program.items) {
        fns += std::holds_alternative<FnDecl>(item.decl) ? 1 : 0;
    }
    CHECK(fns >= 3);  // ok, b (with recovered body), c
}

void testDirectivesAreRejectedByTheParser() {
    Parsed p("#if os == \"linux\" fn pageSize() -> usize { return 4096 }\n");
    CHECK(p.engine.hasErrors());
}

void testOperatorPrecedenceAndCasts() {
    Parsed p("fn f(a: i32, b: i32) -> i64 {\n"
             "    return a + b * 2 as i64 == 3 && !x || y\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    const Expr& e = *fn.body.statements[0].expr;
    CHECK(e.kind == Expr::Kind::Binary && e.binary_op == BinaryOp::Or);
    CHECK(e.lhs->binary_op == BinaryOp::And);
    CHECK(e.lhs->lhs->binary_op == BinaryOp::Eq);
    CHECK(e.lhs->lhs->lhs->binary_op == BinaryOp::Add);
    CHECK(e.lhs->lhs->lhs->rhs->binary_op == BinaryOp::Mul);
    CHECK(e.lhs->lhs->lhs->rhs->rhs->kind == Expr::Kind::Cast);  // `as` binds tighter than `*`
}

// A byte literal reaches the AST as a `ByteLiteral` holding the byte, not as an integer or a name.
void testByteLiteralParsesAsExpr() {
    Parsed p("fn f() -> u8 { return b')' }\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    const Expr& e = *fn.body.statements[0].expr;
    CHECK(e.kind == Expr::Kind::ByteLiteral);
    CHECK(e.int_value == ')');
}

// `a[lo..hi]`, `a[lo..]` and `a[..hi]` parse to a `Slice`; `a[i]` stays an `Index`.
void testSliceParsing() {
    Parsed p("fn f(s: str) -> str {\n"
             "    local a = s[1..3]\n"
             "    local b = s[2..]\n"
             "    local c = s[..4]\n"
             "    local d = s[0]\n"
             "    return a\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    const Expr& a = *fn.body.statements[0].expr;
    const Expr& b = *fn.body.statements[1].expr;
    const Expr& c = *fn.body.statements[2].expr;
    const Expr& d = *fn.body.statements[3].expr;
    CHECK(a.kind == Expr::Kind::Slice && a.args.size() == 1 && a.rhs != nullptr);
    CHECK(b.kind == Expr::Kind::Slice && b.args.size() == 1 && b.rhs == nullptr);
    CHECK(c.kind == Expr::Kind::Slice && c.args.empty() && c.rhs != nullptr);
    CHECK(d.kind == Expr::Kind::Index);
}

// A slice type is a call argument (`sizeOf([]i32)`, `sizeOf([]mut i32)`, `sizeOf([][]u8)`); `[]` that
// no type follows stays the empty array literal, also with a newline in between.
void testSliceTypeAsArgument() {
    Parsed p("fn f() {\n"
             "    local a = sizeOf([]i32)\n"
             "    local b = sizeOf([]mut i32)\n"
             "    local c = sizeOf([][]u8)\n"
             "    local d = sizeOf(*[]i32)\n"
             "    local e = []\n"
             "    local g = [\n"
             "    ]\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    const auto& fn = std::get<FnDecl>(p.program.items[0].decl);
    const auto argument = [&](std::size_t statement) -> const Expr& {
        return *fn.body.statements[statement].expr->args.at(0);
    };
    CHECK(argument(0).kind == Expr::Kind::TypeOperand);
    CHECK(argument(0).type.kind == TypeKind::Slice && !argument(0).type.is_mutable);
    CHECK(argument(1).kind == Expr::Kind::TypeOperand && argument(1).type.is_mutable);
    CHECK(argument(2).kind == Expr::Kind::TypeOperand);
    CHECK(argument(2).type.elem->kind == TypeKind::Slice);
    CHECK(argument(3).kind == Expr::Kind::Unary);  // `*T`: a pointer type, as before
    CHECK(argument(3).lhs->kind == Expr::Kind::TypeOperand);
    CHECK(fn.body.statements[4].expr->kind == Expr::Kind::ArrayList);
    CHECK(fn.body.statements[5].expr->kind == Expr::Kind::ArrayList);
}

// The statements of the first function of `p`.
const std::vector<Stmt>& bodyOf(const Parsed& p) {
    return std::get<FnDecl>(p.program.items.at(0).decl).body.statements;
}

// `t.0.1` is two tuple accesses: a number right after `.` takes no fraction (`0.1` is not a float
// there). Elsewhere `0.1` stays a float, and floats are read with `.` as the decimal point.
void testTupleAccessChainAndFloats() {
    Parsed p("fn f() -> i32 {\n"
             "    local a = t.0.1\n"
             "    local b = 0.1\n"
             "    local c = 1_000.5e-3\n"
             "    return a\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    if (p.engine.hasErrors()) {
        return; // the shape checks below would read a tree that is not there
    }
    const auto& body = bodyOf(p);
    const Expr& a = *body.at(0).expr;
    CHECK(a.kind == Expr::Kind::Field && a.text.empty() && a.int_value == 1);
    CHECK(a.lhs->kind == Expr::Kind::Field && a.lhs->int_value == 0);
    CHECK(a.lhs->lhs->kind == Expr::Kind::Name && a.lhs->lhs->text == "t");
    CHECK(body.at(1).expr->kind == Expr::Kind::FloatLiteral && body.at(1).expr->float_value == 0.1);
    CHECK(body.at(2).expr->float_value == 1.0005);
    // `t.1e3` is no tuple position, and a float beyond `f64` is reported, not read as infinity.
    Parsed bad("fn f() {\n    local a = t.1e3\n    local b = 1e999\n}\n");
    CHECK(bad.engine.errorCount() == 2);
}

// Type arguments in an expression may hold tuple and function types: `id<(i32, i32)>(x)`,
// `mk<fn(i32) -> i32>()`, `id<(Vec<i32>, i32)>(x)`. `a < (b + c)` stays a comparison.
void testGenericArgsWithTupleAndFnTypes() {
    Parsed p("fn f() {\n"
             "    local a = id<(i32, i32)>((1, 2))\n"
             "    local b = mk<fn(i32) -> i32>()\n"
             "    local c = id<(Vec<i32>, [u8; 4])>(x)\n"
             "    local d = a < (b + c)\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    if (p.engine.hasErrors()) {
        return; // the shape checks below would read a tree that is not there
    }
    const auto& body = bodyOf(p);
    const Expr& a = *body.at(0).expr;
    CHECK(a.kind == Expr::Kind::Call && a.lhs->generic_args.size() == 1);
    CHECK(a.lhs->generic_args.at(0).kind == TypeKind::Tuple);
    const Expr& b = *body.at(1).expr;
    CHECK(b.kind == Expr::Kind::Call && b.lhs->generic_args.at(0).kind == TypeKind::FnPtr);
    const Expr& c = *body.at(2).expr;
    CHECK(c.kind == Expr::Kind::Call && c.lhs->generic_args.at(0).args.size() == 2);
    const Expr& d = *body.at(3).expr;
    CHECK(d.kind == Expr::Kind::Binary && d.binary_op == BinaryOp::Lt);
}

// After `as`, `<` opens type arguments only when they close with type tokens and no operand
// follows the `>`: `x as Idx < 3` compares, `x as Box<i32>` and `p as *Node<T> == nil` cast.
void testCastFollowedByComparison() {
    Parsed p("fn f() {\n"
             "    local a = x as Idx < 3\n"
             "    local b = x as Idx < n\n"
             "    local c = x as Box<i32>\n"
             "    local d = p as *Node<T> == nil\n"
             "    local e = x as Idx < n && y > m\n"
             "    local g = f(x as Idx < n, m > k)\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    if (p.engine.hasErrors()) {
        return; // the shape checks below would read a tree that is not there
    }
    const auto& body = bodyOf(p);
    for (const std::size_t i : {0, 1}) {
        const Expr& e = *body.at(i).expr;
        CHECK(e.kind == Expr::Kind::Binary && e.binary_op == BinaryOp::Lt);
        CHECK(e.lhs->kind == Expr::Kind::Cast && e.lhs->type.args.empty());
    }
    const Expr& c = *body.at(2).expr;
    CHECK(c.kind == Expr::Kind::Cast && c.type.args.size() == 1);
    const Expr& d = *body.at(3).expr;
    CHECK(d.kind == Expr::Kind::Binary && d.binary_op == BinaryOp::Eq);
    CHECK(d.lhs->kind == Expr::Kind::Cast && d.lhs->type.elem->args.size() == 1);
    CHECK(body.at(4).expr->binary_op == BinaryOp::And);
    const Expr& g = *body.at(5).expr;
    CHECK(g.kind == Expr::Kind::Call && g.args.size() == 2);
    CHECK(g.args.at(0)->binary_op == BinaryOp::Lt && g.args.at(1)->binary_op == BinaryOp::Gt);
}

// A `?` against its operand followed by `-`, `*`, `&`, a name or a literal is a ternary only when
// its own `:` follows: `f()? - 1` propagates, `c? -1 : 1` is a ternary, and inside a ternary's
// then-branch the enclosing `:` is not taken (`c ? f()? - 1 : 0`).
void testPropagationBeforeOperandTokens() {
    Parsed p("fn f() {\n"
             "    local a = g()? - 1\n"
             "    local b = g()? * 2 & m\n"
             "    local c = k? -1 : 1\n"
             "    local d = k ? g()? - 1 : 0\n"
             "    local e = g()? - 1 > 0 ? 10 : 20\n"
             "    local h = q(g()? - 1, k? 1 : 2)\n"
             "}\n");
    CHECK(!p.engine.hasErrors());
    if (p.engine.hasErrors()) {
        return; // the shape checks below would read a tree that is not there
    }
    const auto& body = bodyOf(p);
    const Expr& a = *body.at(0).expr;
    CHECK(a.kind == Expr::Kind::Binary && a.binary_op == BinaryOp::Sub);
    CHECK(a.lhs->kind == Expr::Kind::Try);
    const Expr& b = *body.at(1).expr;
    CHECK(b.binary_op == BinaryOp::BitAnd && b.lhs->lhs->kind == Expr::Kind::Try);
    const Expr& c = *body.at(2).expr;
    CHECK(c.kind == Expr::Kind::Ternary && c.lhs->kind == Expr::Kind::Name);
    const Expr& d = *body.at(3).expr;
    CHECK(d.kind == Expr::Kind::Ternary && d.rhs->kind == Expr::Kind::Binary);
    CHECK(d.rhs->lhs->kind == Expr::Kind::Try);
    const Expr& e = *body.at(4).expr;
    CHECK(e.kind == Expr::Kind::Ternary && e.lhs->binary_op == BinaryOp::Gt);
    CHECK(e.lhs->lhs->lhs->kind == Expr::Kind::Try);
    const Expr& h = *body.at(5).expr;
    CHECK(h.args.at(0)->lhs->kind == Expr::Kind::Try);
    CHECK(h.args.at(1)->kind == Expr::Kind::Ternary);
}

// An error inside a `switch` arm skips the rest of the `switch` body: one error, and the
// statements after the `switch` and the next function still parse.
void testRecoverySkipsTheBracesTheStatementOpened() {
    Parsed p("fn main() -> i32 {\n"
             "    switch x {\n"
             "        case 1 + { return 1 }\n"
             "        case 2 { return 2 }\n"
             "        default { return 0 }\n"
             "    }\n"
             "    local after = (1 +\n"
             "    return 0\n"
             "}\n"
             "fn other() -> i32 { return 0 }\n");
    CHECK_EQ(p.engine.errorCount(), std::size_t{2});
    CHECK(p.program.items.size() == 2);
    // A failure inside `( ... )` does not leave the parser in its "line breaks continue" mode:
    // `*p = 2` stays its own statement instead of becoming `x * p = 2`.
    Parsed q("fn f() {\n    local a = g(1,\n    local b = x\n    *p = 2\n}\n");
    CHECK_EQ(q.engine.errorCount(), std::size_t{1});
    CHECK(bodyOf(q).size() == 2);
}

// A block inside a `for` header (a closure body) ends its statements at line breaks again: `;`
// and `{` end a statement only in the header itself.
void testBlockInsideForHeaderIsNotTheHeader() {
    Parsed p("fn f() {\n"
             "    for local i = 0; run(|| { a() { } }); i++ { }\n"
             "}\n");
    CHECK(p.engine.hasErrors());
}

}  // namespace

void runSemaTests();
void runLowerTests();
void runOperatorTests();
void runClosureTests();
void runIteratorTests();
void runCodegenTests();

int main() {
    RUN(runSemaTests);
    RUN(runCodegenTests);
    RUN(runLowerTests);
    RUN(runOperatorTests);
    RUN(runClosureTests);
    RUN(runIteratorTests);
    RUN(testWarningContextAndTwoSidedLabels);
    RUN(testErrorContextIsWiderAndGutterIsAligned);
    RUN(testDistantMarksAreSeparatedAndNearOnesMerged);
    RUN(testSameLineGetsCaretLines);
    RUN(testTabsAndLongLines);
    RUN(testSuppressionAndDeny);
    RUN(testJsonFormat);
    RUN(testRegistry);
    RUN(testLexer);
    RUN(testLexerErrors);
    RUN(testByteLiteralLexing);
    RUN(testByteLiteralErrors);
    RUN(testParsesFunctionsAndTypes);
    RUN(testNewlinesEndStatements);
    RUN(testIfConditionDoesNotTakeStructLiteral);
    RUN(testImportForms);
    RUN(testReExports);
    RUN(testDeclareRegisterAndModifiers);
    RUN(testClassInterfaceImplEnum);
    RUN(testDefineAndMacros);
    RUN(testNewForms);
    RUN(testAsmOperands);
    RUN(testAttributesOnStatementsRegisterAllowScope);
    RUN(testRecoveryReportsSeveralErrors);
    RUN(testDirectivesAreRejectedByTheParser);
    RUN(testOperatorPrecedenceAndCasts);
    RUN(testByteLiteralParsesAsExpr);
    RUN(testSliceParsing);
    RUN(testSliceTypeAsArgument);
    RUN(testTupleAccessChainAndFloats);
    RUN(testGenericArgsWithTupleAndFnTypes);
    RUN(testCastFollowedByComparison);
    RUN(testPropagationBeforeOperandTokens);
    RUN(testRecoverySkipsTheBracesTheStatementOpened);
    RUN(testBlockInsideForHeaderIsNotTheHeader);

    if (xlang_test::failures() != 0) {
        std::cerr << xlang_test::failures() << " check(s) failed\n";
        return 1;
    }
    std::cout << "all checks passed\n";
    return 0;
}
