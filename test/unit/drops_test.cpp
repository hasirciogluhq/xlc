#include "check.h"
#include "lowered.h"

// Ownership of compound values: what drops, in which order, and which moves are allowed.

namespace {

using xlang_test::Lowered;

const std::string kRes = "class Res {\n    id: i32\n    fn drop(self) { }\n}\n";

// A tuple owns its elements: one glue per tuple type drops them in reverse order.
void testTupleDropsItsElements() {
    const Lowered r(kRes + "fn f() {\n    local t = (Res { id: 1 }, 2, Res { id: 3 })\n}\n");
    CHECK(!r.diag->hasErrors());
    const std::string glue = "$tupleT";
    CHECK(r.has(glue));
    const std::size_t two = r.hir_text.find("(field.ptr 2 p#0)");
    const std::size_t zero = r.hir_text.find("(field.ptr 0 p#0)");
    CHECK(two != std::string::npos && zero != std::string::npos && two < zero);
    CHECK(!r.has("(field.ptr 1 p#0)")); // the `i32` element needs no drop
    const Lowered plain("fn f() {\n    local t = (1, 2)\n}\n");
    CHECK(!plain.has("$tuple"));
}

// Building a tuple from a droppable local and taking one apart both move.
void testTupleMovesInAndOut() {
    const Lowered in(kRes + "fn f() {\n    local r = Res { id: 1 }\n    local t = (r, 2)\n}\n");
    CHECK(!in.diag->hasErrors());
    CHECK(in.has("move#"));
    const Lowered out(kRes + "fn make() -> (Res, i32) { return (Res { id: 1 }, 2) }\n"
                             "fn f() {\n    local t = make()\n    local (a, n) = t\n}\n");
    CHECK(!out.diag->hasErrors());
    CHECK(out.has("move#"));
    CHECK(Lowered(kRes + "fn f(t: (Res, i32)) -> Res { return t.0 }\n").failedWith("E0720"));
}

// A tuple that needs drop cannot sit where nothing drops it.
void testDroppableTupleInPlainPlaces() {
    CHECK(Lowered(kRes + "struct S { t: (Res, i32) }\n").failedWith("E0721"));
    CHECK(Lowered(kRes + "fn f() {\n    local a: [(Res, i32); 2]\n}\n").failedWith("E0721"));
}

const std::string kHeap = "register(alloc) fn alloc(size: usize, align: usize) -> *void { return nil }\n"
                          "register(free) fn release(p: *void) { }\n";

// A polymorphic class drops through vtable slot 0, which every class of the hierarchy fills with
// its own glue (a C++ virtual destructor); the virtual methods follow it.
void testPolymorphicClassDropsThroughItsVtable() {
    const Lowered r(kHeap + "class Base {\n    virtual fn name(self) -> i32 { return 1 }\n}\n"
                            "class Child : Base {\n    fn drop(self) { }\n"
                            "    override fn name(self) -> i32 { return 2 }\n}\n"
                            "fn f(p: *Base) { delete p }\n"
                            "fn g(p: *Base) { __builtin_drop(p) }\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("fn Base$dropGlue(p: *Base)")); // exists although `Base` itself needs no drop
    CHECK(r.has("(call.virtual Base$dropGlue[0] "));
    CHECK(r.module.aggregates.size() >= 2);
    for (const auto& a : r.module.aggregates) {
        if (a.name == "Child") {
            CHECK(a.vtable.size() == 2 && a.vtable[0].function == "Child$dropGlue" &&
                  a.vtable[1].function == "Child.name");
        }
    }
}

// A `@repr(c)` root mirrors a C++ vtable: it gets no drop slot and drops statically.
void testReprCVtableHasNoDropSlot() {
    const Lowered r(kHeap + "@repr(c)\nclass Base {\n    virtual fn name(self) -> i32 { return 1 }\n"
                            "    fn drop(self) { }\n}\n"
                            "fn f(p: *Base) { delete p }\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(call Base$dropGlue "));
    for (const auto& a : r.module.aggregates) {
        if (a.name == "Base") {
            CHECK(a.vtable.size() == 1 && a.vtable[0].function == "Base.name");
        }
    }
}

// Dropping through a base without a vtable would skip the derived drop: E0766.
void testStaticDropThatSkipsADerivedDropIsAnError() {
    const std::string classes = "class Base {\n    id: i32\n}\n"
                                "class Child : Base {\n    fn drop(self) { }\n}\n";
    CHECK(Lowered(kHeap + classes + "fn f(p: *Base) { delete p }\n").failedWith("E0766"));
    CHECK(Lowered(kHeap + classes + "fn f(p: *Base) { __builtin_drop(p) }\n").failedWith("E0766"));
    const Lowered exact(kHeap + classes + "fn f(p: *Child) { delete p }\n");
    CHECK(!exact.diag->hasErrors());
}

const std::string kTwo = kRes + "enum Two {\n    Empty\n    Pair(Res, Res)\n}\n"
                                 "fn keep(r: Res) { }\n";

// A `switch` on an owner takes the payload: the owner's flag is cleared and every payload value,
// `_` included, lands in a local that drops on its own unless it is moved.
void testPayloadSwitchOnAnOwnerTakesThePayload() {
    const Lowered r(kTwo + "fn f(t: Two) {\n    switch t {\n"
                           "        case Two.Pair(a, _) { keep(a) }\n"
                           "        case Two.Empty { }\n    }\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("local $payload"));                       // `_` still holds its droppable value
    CHECK(r.has("(call Res$dropGlue (addr $payload"));    // and drops it at the arm's end
    CHECK(r.has("(call keep move#"));                     // `a` is moved out
    CHECK(r.has("(call Res$dropGlue (addr a#"));          // guarded by its own flag
    const std::size_t entry = r.hir_text.find("    if (== (field tag $sw");
    const std::size_t clear = r.hir_text.find("assign $live1#1 = false", entry);
    CHECK(entry != std::string::npos && clear != std::string::npos);
    // An arm that binds nothing droppable leaves the payload to its owner.
    const Lowered view(kTwo + "fn f(t: Two) {\n    switch t {\n"
                              "        case Two.Pair(_, _) { }\n        case Two.Empty { }\n    }\n}\n");
    CHECK(!view.diag->hasErrors());
    CHECK(!view.has("$payload"));
}

// A temporary scrutinee is held by the switch and dropped when it ends.
void testPayloadSwitchOnATemporaryDropsIt() {
    const Lowered r(kTwo + "fn make() -> Two { return Two.Empty }\n"
                           "fn f() {\n    switch make() {\n"
                           "        case Two.Pair(a, b) { keep(a) }\n"
                           "        case Two.Empty { }\n    }\n}\n");
    CHECK(!r.diag->hasErrors());
    CHECK(r.has("(call Two$dropGlue (addr $sw"));
    CHECK(r.has("(call Res$dropGlue (addr b#"));
}

// A `switch` on a place only lends the payload: reading is fine, moving it out is E0720.
void testPayloadSwitchOnAPlaceLends() {
    CHECK(Lowered(kTwo + "fn f(t: *Two) {\n    switch *t {\n"
                         "        case Two.Pair(a, _) { keep(a) }\n"
                         "        case Two.Empty { }\n    }\n}\n")
              .failedWith("E0720"));
    const Lowered read(kTwo + "fn f(t: *Two) -> i32 {\n    switch *t {\n"
                              "        case Two.Pair(a, _) { return a.id }\n"
                              "        case Two.Empty { return 0 }\n    }\n}\n");
    CHECK(!read.diag->hasErrors());
    CHECK(!read.has("(addr a#")); // a view: never dropped
}

} // namespace

void runDropTests() {
    RUN(testTupleDropsItsElements);
    RUN(testTupleMovesInAndOut);
    RUN(testDroppableTupleInPlainPlaces);
    RUN(testPolymorphicClassDropsThroughItsVtable);
    RUN(testReprCVtableHasNoDropSlot);
    RUN(testStaticDropThatSkipsADerivedDropIsAnError);
    RUN(testPayloadSwitchOnAnOwnerTakesThePayload);
    RUN(testPayloadSwitchOnATemporaryDropsIt);
    RUN(testPayloadSwitchOnAPlaceLends);
}
