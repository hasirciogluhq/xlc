// The util bridge in both directions: the xlang source manager and diagnostic engine
// (src/util/source.xl, src/util/diagnostics.xl) must produce byte-identical output to the C++
// DiagnosticEngine for the same inputs, so every rendering rule of src/util/diagnostics.cpp is
// checked by comparison, not by re-stating it.
#include "check.h"

#include <functional>
#include <sstream>

#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"
#include "util/xl_bridge.hpp"

using namespace xlang;
using xlang::xl::UtilBridge;

namespace {

/// One case: the same files and diagnostics go into a C++ engine and the bridge; both render and
/// the outputs must match byte for byte.
struct Case {
    DiagFormat format{DiagFormat::Text};
    bool color{false};
    std::vector<std::string> allow;
    std::vector<std::string> deny;
    bool warn_all{false};
    bool allow_all{false};
    std::size_t context_errors{10};
    std::size_t context_warnings{2};
    std::function<void(DiagnosticEngine&)> cpp;
    std::function<void(UtilBridge&)> xl;
};

const char* kFileA = "app/main.xl";
const char* kFileB = "lib/util.xl";

const char* kTextA =
    "fn main() -> i32 {\n"
    "\tlocal x = 1\n"
    "    local long = \"aaaa bbbb cccc dddd eeee ffff gggg hhhh iiii jjjj kkkk llll mmmm nnnn oooo pppp qqqq\"\n"
    "    return x\n"
    "}\n";

const char* kTextB =
    "export fn f() {\n"
    "    // héllo — ünïcode\n"
    "    return 1\n"
    "}\n";

std::string renderCpp(const Case& c) {
    SourceManager sources;
    sources.addFile(kFileA, kTextA);
    sources.addFile(kFileB, kTextB);
    std::ostringstream out;
    DiagnosticEngine engine(sources, out);
    DiagnosticOptions options;
    options.allow = c.allow;
    options.deny = c.deny;
    options.warn_all = c.warn_all;
    options.allow_all = c.allow_all;
    options.context_errors = c.context_errors;
    options.context_warnings = c.context_warnings;
    options.format = c.format;
    options.color = c.color;
    engine.configure(options);
    c.cpp(engine);
    engine.flush();
    return out.str();
}

std::string renderXl(const Case& c) {
    xl::UtilBridge bridge;
    bridge.addFile(kFileA, kTextA);
    bridge.addFile(kFileB, kTextB);
    DiagnosticOptions options;
    options.allow = c.allow;
    options.deny = c.deny;
    options.warn_all = c.warn_all;
    options.allow_all = c.allow_all;
    options.context_errors = c.context_errors;
    options.context_warnings = c.context_warnings;
    options.format = c.format;
    options.color = c.color;
    bridge.configure(options);
    c.xl(bridge);
    const xl::XbFlush& flush = bridge.flush();
    return std::string(static_cast<const char*>(flush.text), flush.text_len);
}

void expectSame(const char* name, const Case& c) {
    const std::string cpp = renderCpp(c);
    const std::string xl = renderXl(c);
    if (cpp != xl) {
        ++xlang_test::failures();
        std::cerr << "util_bridge_test: case " << name << " differs\n--- cpp ---\n" << cpp
                  << "--- xl ---\n" << xl << "\n";
    }
}

Diagnostic simpleError(FileId file) {
    Diagnostic d;
    d.code = "E0708";
    d.message = "type mismatch in the initializer";
    d.labels.push_back({Span{file, 2, 17, 2, 20}, "i32 here"});
    d.detail = "the initializer has type f64";
    d.reason = "the local is declared i32";
    d.fix = "write `local x: i32 = 1`";
    return d;
}

void testSimpleError() {
    Case c;
    c.cpp = [](DiagnosticEngine& e) { e.report(simpleError(0)); };
    c.xl = [](UtilBridge& b) { b.report(simpleError(0)); };
    expectSame("simple", c);
}

void testMultiLineSpanAndTwoFiles() {
    Diagnostic d;
    d.code = "E0713";
    d.message = "`impl` does not match its interface";
    d.labels.push_back({Span{0, 1, 1, 4, 2}, "the impl"});
    d.labels.push_back({Span{1, 2, 5, 2, 12}, "the interface method"});
    d.notes.push_back({Span{1, 3, 5, 3, 5}, "requested here"});
    d.notes.push_back({Span{}, "from the driver"});
    Case c;
    c.cpp = [d](DiagnosticEngine& e) { e.report(d); };
    c.xl = [d](UtilBridge& b) { b.report(d); };
    expectSame("multiline", c);
}

void testTwoMarksOnOneLine() {
    Diagnostic d;
    d.code = "E0601";
    d.message = "binary operation mixes signedness";
    d.labels.push_back({Span{0, 2, 13, 2, 14}, "i32"});
    d.labels.push_back({Span{0, 2, 18, 2, 19}, "u32"});
    Case c;
    c.cpp = [d](DiagnosticEngine& e) { e.report(d); };
    c.xl = [d](UtilBridge& b) { b.report(d); };
    expectSame("two-marks", c);
}

void testLongLineWindowAndTabs() {
    Diagnostic d;
    d.code = "E0709";
    d.message = "invalid operation";
    d.labels.push_back({Span{0, 3, 100, 3, 104}, "past the window"});
    d.labels.push_back({Span{0, 2, 3, 2, 5}, "a tab"});
    Case c;
    c.cpp = [d](DiagnosticEngine& e) { e.report(d); };
    c.xl = [d](UtilBridge& b) { b.report(d); };
    expectSame("window", c);
}

void testMergedContextRanges() {
    Diagnostic d;
    d.code = "E0711";
    d.message = "module not found";
    d.labels.push_back({Span{0, 2, 1, 2, 4}, "first"});
    d.labels.push_back({Span{0, 5, 1, 5, 4}, "second"});
    Case c;
    c.context_errors = 1;
    c.cpp = [d](DiagnosticEngine& e) { e.report(d); };
    c.xl = [d](UtilBridge& b) { b.report(d); };
    expectSame("ranges", c);
}

void testWarningSuppressedAndDenied() {
    Diagnostic w;
    w.code = "W0601";
    w.severity = Severity::Warning;
    w.message = "implicit integer to float conversion";
    w.labels.push_back({Span{0, 2, 14, 2, 15}, "i32 to f64"});

    Case allowed;
    allowed.allow = {"W0601"};
    allowed.cpp = [w](DiagnosticEngine& e) { e.report(w); };
    allowed.xl = [w](UtilBridge& b) { b.report(w); };
    expectSame("allowed", allowed);

    Case denied;
    denied.deny = {"W0601"};
    denied.cpp = [w](DiagnosticEngine& e) { e.report(w); };
    denied.xl = [w](UtilBridge& b) { b.report(w); };
    expectSame("denied", denied);

    Case warnAll;
    warnAll.warn_all = true;
    warnAll.cpp = [w](DiagnosticEngine& e) { e.report(w); };
    warnAll.xl = [w](UtilBridge& b) { b.report(w); };
    expectSame("warn-all", warnAll);
}

void testAllowScope() {
    Diagnostic w;
    w.code = "W0401";
    w.severity = Severity::Warning;
    w.message = "same entity through two paths";
    w.labels.push_back({Span{0, 2, 5, 2, 9}, "here"});
    Case inside;
    inside.cpp = [w](DiagnosticEngine& e) {
        e.addAllowScope(Span{0, 1, 1, 3, 1}, {"W0401"});
        e.report(w);
    };
    inside.xl = [w](UtilBridge& b) {
        b.addAllowScope(Span{0, 1, 1, 3, 1}, {"W0401"});
        b.report(w);
    };
    expectSame("scope-inside", inside);

    Case outside;
    outside.cpp = [w](DiagnosticEngine& e) {
        e.addAllowScope(Span{0, 4, 1, 5, 1}, {"W0401"});
        e.report(w);
    };
    outside.xl = [w](UtilBridge& b) {
        b.addAllowScope(Span{0, 4, 1, 5, 1}, {"W0401"});
        b.report(w);
    };
    expectSame("scope-outside", outside);
}

void testJson() {
    Case c;
    c.format = DiagFormat::Json;
    c.cpp = [](DiagnosticEngine& e) {
        e.report(simpleError(0));
        Diagnostic w;
        w.code = "W0601";
        w.severity = Severity::Warning;
        w.message = "implicit conversion \"quoted\"";
        w.labels.push_back({Span{1, 2, 5, 2, 9}, "f64"});
        e.report(w);
    };
    c.xl = [](UtilBridge& b) {
        b.report(simpleError(0));
        Diagnostic w;
        w.code = "W0601";
        w.severity = Severity::Warning;
        w.message = "implicit conversion \"quoted\"";
        w.labels.push_back({Span{1, 2, 5, 2, 9}, "f64"});
        b.report(w);
    };
    expectSame("json", c);
}

void testSortedByPositionAndCounts() {
    Case c;
    c.cpp = [](DiagnosticEngine& e) {
        e.report(simpleError(0));
        Diagnostic early;
        early.code = "E0002";
        early.message = "syntax error";
        early.labels.push_back({Span{1, 3, 1, 3, 5}, "there"});
        e.report(early);
        e.beginQuiet();
        e.error("E0706", Span{0, 1, 1, 1, 2}, "quiet one", "");
        e.endQuiet();
    };
    c.xl = [](UtilBridge& b) {
        b.report(simpleError(0));
        Diagnostic early;
        early.code = "E0002";
        early.message = "syntax error";
        early.labels.push_back({Span{1, 3, 1, 3, 5}, "there"});
        b.report(early);
        b.beginQuiet();
        Diagnostic quiet;
        quiet.code = "E0706";
        quiet.message = "quiet one";
        quiet.labels.push_back({Span{0, 1, 1, 1, 2}, ""});
        b.report(quiet);
        b.endQuiet();
    };
    expectSame("sorted", c);
    // The counts come from the bridge flush: one error, the quiet report dropped.
    xl::UtilBridge bridge;
    bridge.addFile(kFileA, kTextA);
    bridge.addFile(kFileB, kTextB);
    DiagnosticOptions options;
    bridge.configure(options);
    bridge.report(simpleError(0));
    const xl::XbFlush& flush = bridge.flush();
    CHECK(flush.errors == 1);
    CHECK(flush.warnings == 0);
}

void testColorized() {
    Case c;
    c.color = true;
    c.cpp = [](DiagnosticEngine& e) { e.report(simpleError(0)); };
    c.xl = [](UtilBridge& b) { b.report(simpleError(0)); };
    expectSame("color", c);
}

void testExplain() {
    xl::UtilBridge bridge;
    CHECK_EQ(bridge.explain("E0708"), DiagnosticEngine::explain("E0708"));
    CHECK(bridge.explain("NOPE").empty());
    CHECK(DiagnosticEngine::explain("NOPE").empty());
}

}  // namespace

void runUtilBridgeTests() {
    RUN(testSimpleError);
    RUN(testMultiLineSpanAndTwoFiles);
    RUN(testTwoMarksOnOneLine);
    RUN(testLongLineWindowAndTabs);
    RUN(testMergedContextRanges);
    RUN(testWarningSuppressedAndDenied);
    RUN(testAllowScope);
    RUN(testJson);
    RUN(testSortedByPositionAndCounts);
    RUN(testColorized);
    RUN(testExplain);
}
