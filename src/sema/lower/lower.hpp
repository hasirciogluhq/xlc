// Internal header of the lowering pass (plan 4.1-4.12): the `Lowerer` class and the helpers the
// parts in `lower_items.cpp`, `lower_stmt.cpp` and `lower_expr.cpp` share. Not installed.
#pragma once

#include "xlang/hir/hir.h"
#include "xlang/sema/compilation.h"
#include "xlang/sema/conversion.h"
#include "xlang/sema/scope.h"
#include "xlang/sema/sema.h"

#include <algorithm>
#include <deque>
#include <format>
#include <iterator>
#include <ranges>
#include <set>

namespace xlang {

/// Lowers every module of one compilation: module scopes first, then the items of each, then the
/// generic instances they requested (which can belong to another module), then the init/fini order.
/// `library_modules` marks the modules of the library group (found through a module root), which the language treats
/// specially. The definition is in `src/sema/lower/items.cpp` (`sema/lower`), and it is qualified
/// there as `xlang::lowerProgram`: it sits in the outer namespace, because that is the name the
/// compilation driver calls and the one the pass has always had.
hir::Module lowerProgram(const std::vector<Sema*>& modules,
                         const std::vector<Sema*>& init_order,
                         const std::set<const Sema*>& library_modules);

namespace lower {

[[nodiscard]] inline Type unknownType() {
    return Type::makeNamed({});
}

[[nodiscard]] inline bool isUnknown(const Type& t) {
    return t.kind == TypeKind::Named;
}

[[nodiscard]] inline bool isNumeric(const Type& t) {
    return t.kind == TypeKind::Int || t.kind == TypeKind::Float;
}

[[nodiscard]] inline bool sameType(const Type& a, const Type& b) {
    return typeToString(a) == typeToString(b);
}

[[nodiscard]] inline std::string joinPath(const std::vector<std::string>& path) {
    std::string out;
    for (const std::string& p : path) {
        out += out.empty() ? p : "." + p;
    }
    return out;
}

[[nodiscard]] inline bool isAggregate(const Type& t) {
    return t.kind == TypeKind::Struct || t.kind == TypeKind::Class;
}

/// Default type of an untyped `const` from its value: integers i32 (i64 when they do not fit),
/// floats f64, bools, strings str. nullopt when the value is not a literal constant.
[[nodiscard]] inline std::optional<Type> literalConstType(Sema& sema, const Expr& init) {
    const auto v = sema.evalConst(init);
    if (!v) {
        return std::nullopt;
    }
    switch (v->kind) {
    case ConstValue::Kind::Int:
        return (v->int_value > 2147483647 || v->int_value < -2147483648LL)
                   ? Type::makeInt(64, true)
                   : Type::makeInt(32, true);
    case ConstValue::Kind::Float:
        return Type::makeFloat(64);
    case ConstValue::Kind::Bool:
        return Type::makeBool();
    case ConstValue::Kind::String:
        return Type::makeStr();
    }
    return std::nullopt;
}

/// Name of the variable at the root of a place (`c` for `c.x[1]`), without its module prefix.
[[nodiscard]] inline std::string placeRootName(const hir::Expr& e) {
    const hir::Expr* cur = &e;
    while ((cur->kind == hir::ExprKind::Field || cur->kind == hir::ExprKind::Index) && cur->lhs) {
        cur = cur->lhs.get();
    }
    const auto dot = cur->text.rfind('.');
    return dot == std::string::npos ? cur->text : cur->text.substr(dot + 1);
}

/// `s[i]`, `s.ptr` or `s.len` where `s` is a `str` (or a pointer to one): never assignable.
/// A `[]T` slice is the same fat pointer: `s.len` is read-only, and `s.ptr` reads the data pointer.
/// `s[i]` on a `[]mut T` *is* a place (see `indexExpr`), so it is not covered here.
[[nodiscard]] inline bool isStrPlace(const hir::Expr& e) {
    const auto isView = [](const Type& t) {
        return t.kind == TypeKind::Str || t.kind == TypeKind::Slice;
    };
    if (e.kind == hir::ExprKind::Index) {
        return e.lhs && (e.lhs->type.kind == TypeKind::Str ||
                         (e.lhs->type.kind == TypeKind::Slice && !e.lhs->type.is_mutable));
    }
    if (e.kind == hir::ExprKind::Slice) {
        return true; // a slice yields a `str` value, not a place
    }
    if (e.kind == hir::ExprKind::Field && e.lhs) {
        const Type* owner = &e.lhs->type;
        if (e.through_pointer) {
            if (!e.lhs->type.elem) {
                return false;
            }
            owner = e.lhs->type.elem.get();
        }
        return isView(*owner);
    }
    return false;
}

/// Maximum number of syscall arguments (after the number) for the target; nullopt = no fixed
/// limit in the target table (Windows NT: the compiler spills to the stack, plan 5.3).
[[nodiscard]] inline std::optional<unsigned> syscallMaxArgs(const TargetInfo& t) {
    if (t.os == "windows") {
        return std::nullopt;
    }
    return 6;
}

[[nodiscard]] inline const Attribute* findAttribute(const std::vector<Attribute>& attrs,
                                                    std::string_view name) {
    for (const Attribute& a : attrs) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
}

/// Deep copy of an AST expression (used to desugar `a op= b` and `switch`).
inline ExprPtr cloneAst(const Expr& e) {
    auto c = std::make_unique<Expr>();
    c->kind = e.kind;
    c->span = e.span;
    c->int_value = e.int_value;
    c->float_value = e.float_value;
    c->bool_value = e.bool_value;
    c->text = e.text;
    c->unary_op = e.unary_op;
    c->binary_op = e.binary_op;
    c->type = e.type;
    c->macro_args = e.macro_args;
    if (e.lhs) {
        c->lhs = cloneAst(*e.lhs);
    }
    if (e.rhs) {
        c->rhs = cloneAst(*e.rhs);
    }
    for (const ExprPtr& a : e.args) {
        c->args.push_back(cloneAst(*a));
    }
    for (const FieldInit& f : e.fields) {
        c->fields.push_back({f.name, cloneAst(*f.value), f.span});
    }
    return c;
}

/// An expression whose evaluation may have side effects (so it must not be evaluated twice).
inline bool astHasSideEffects(const Expr& e) {
    switch (e.kind) {
    case Expr::Kind::Call:
    case Expr::Kind::MacroCall:
    case Expr::Kind::New:
        return true;
    default:
        break;
    }
    if (e.lhs && astHasSideEffects(*e.lhs)) {
        return true;
    }
    if (e.rhs && astHasSideEffects(*e.rhs)) {
        return true;
    }
    for (const ExprPtr& a : e.args) {
        if (astHasSideEffects(*a)) {
            return true;
        }
    }
    for (const FieldInit& f : e.fields) {
        if (astHasSideEffects(*f.value)) {
            return true;
        }
    }
    return false;
}

inline ExprPtr astName(const std::string& name, const Span& span) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Name;
    e->text = name;
    e->span = span;
    return e;
}

inline ExprPtr astUnary(UnaryOp op, ExprPtr operand, const Span& span) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Unary;
    e->unary_op = op;
    e->lhs = std::move(operand);
    e->span = span;
    return e;
}

inline ExprPtr astBinary(BinaryOp op, ExprPtr lhs, ExprPtr rhs, const Span& span) {
    auto e = std::make_unique<Expr>();
    e->kind = Expr::Kind::Binary;
    e->binary_op = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    e->span = span;
    return e;
}

inline hir::ExprPtr cloneHirExpr(const hir::Expr& e) {
    auto c = std::make_unique<hir::Expr>();
    c->kind = e.kind;
    c->type = e.type;
    c->span = e.span;
    c->lvalue = e.lvalue;
    c->int_value = e.int_value;
    c->float_value = e.float_value;
    c->bool_value = e.bool_value;
    c->text = e.text;
    c->member = e.member;
    c->local = e.local;
    c->unary_op = e.unary_op;
    c->binary_op = e.binary_op;
    c->clobbers = e.clobbers;
    c->target = e.target;
    c->index = e.index;
    c->implicit = e.implicit;
    c->checked = e.checked;
    c->through_pointer = e.through_pointer;
    c->registered = e.registered;
    c->nomem = e.nomem;
    c->noreturn = e.noreturn;
    if (e.lhs) {
        c->lhs = cloneHirExpr(*e.lhs);
    }
    if (e.rhs) {
        c->rhs = cloneHirExpr(*e.rhs);
    }
    for (const hir::ExprPtr& a : e.args) {
        c->args.push_back(cloneHirExpr(*a));
    }
    for (const hir::FieldInit& f : e.fields) {
        c->fields.push_back({f.name, f.index, cloneHirExpr(*f.value)});
    }
    for (const hir::AsmOperand& op : e.asm_operands) {
        c->asm_operands.push_back({op.kind, op.reg, cloneHirExpr(*op.value)});
    }
    return c;
}

inline hir::Stmt cloneHirStmt(const hir::Stmt& s) {
    hir::Stmt c;
    c.kind = s.kind;
    c.span = s.span;
    c.local = s.local;
    c.msg_local = s.msg_local;
    c.post_test = s.post_test;
    c.depth = s.depth;
    if (s.target) {
        c.target = cloneHirExpr(*s.target);
    }
    if (s.value) {
        c.value = cloneHirExpr(*s.value);
    }
    for (const hir::Stmt& b : s.body) {
        c.body.push_back(cloneHirStmt(b));
    }
    for (const hir::Stmt& b : s.else_body) {
        c.else_body.push_back(cloneHirStmt(b));
    }
    for (const hir::Stmt& b : s.step) {
        c.step.push_back(cloneHirStmt(b));
    }
    return c;
}

/// Does `body` contain a `break` that leaves the loop whose body it is (nesting level `level`)?
inline bool breaksOut(const std::vector<hir::Stmt>& body, std::uint32_t level) {
    for (const hir::Stmt& s : body) {
        switch (s.kind) {
        case hir::StmtKind::Break:
            if (s.depth == level) {
                return true;
            }
            break;
        case hir::StmtKind::If:
            if (breaksOut(s.body, level) || breaksOut(s.else_body, level)) {
                return true;
            }
            break;
        case hir::StmtKind::Block:
            if (breaksOut(s.body, level)) {
                return true;
            }
            break;
        case hir::StmtKind::Try:
            if (breaksOut(s.body, level) || breaksOut(s.else_body, level)) {
                return true;
            }
            break;
        case hir::StmtKind::While:
            if (breaksOut(s.body, level + 1)) {
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

/// True when control cannot fall off the end of `body` (every path returns, loops forever or
/// calls a `noreturn` function).
inline bool terminates(const std::vector<hir::Stmt>& body, const std::string& panic_fn) {
    for (const hir::Stmt& s : body) {
        switch (s.kind) {
        case hir::StmtKind::Return:
            return true;
        case hir::StmtKind::Block:
            if (terminates(s.body, panic_fn)) {
                return true;
            }
            break;
        case hir::StmtKind::If:
        case hir::StmtKind::Try:
            if (!s.else_body.empty() && terminates(s.body, panic_fn) &&
                terminates(s.else_body, panic_fn)) {
                return true;
            }
            break;
        case hir::StmtKind::While:
            if (s.value && s.value->kind == hir::ExprKind::BoolConst && s.value->bool_value &&
                !breaksOut(s.body, 0)) {
                return true;
            }
            break;
        case hir::StmtKind::Expr:
            if (s.value->kind == hir::ExprKind::Asm && s.value->noreturn) {
                return true;
            }
            if (s.value->kind == hir::ExprKind::Call && !panic_fn.empty() &&
                s.value->text == panic_fn) {
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

/// State shared by the per-module lowerers of one program.
struct LowerShared {
    struct ImplInfo {
        const ImplDecl* decl{nullptr};
        Sema* owner{nullptr};
        const Item* item{nullptr};
        std::string type_q;
        std::string iface_q;
        std::string prefix; ///< `Type.Iface.`
        Span span;
        bool instance{false}; ///< made from a generic `impl` for concrete types
    };
    /// A generic `impl<T> Iface<T> for Target<T>`: instantiated for each instance of its target.
    struct ImplTemplate {
        const Item* item{nullptr};
        Sema* owner{nullptr};
    };
    struct SlotInfo {
        hir::VtableSlot slot;
        const FnDecl* decl{nullptr};
        Sema* owner{nullptr};
    };

    hir::Module module;
    /// A deque: lowering can add impls while a caller holds a reference to one.
    std::deque<ImplInfo> impls;
    std::vector<ImplTemplate> impl_templates;
    std::set<std::pair<const Item*, std::string>> impl_done; ///< template x instance already tried
    std::vector<Sema*> modules;
    std::map<std::string, std::vector<SlotInfo>> vtables;
    std::set<std::string> vtables_in_progress;
    /// The registration of a role: the global or function that holds it, and the module it is in.
    struct Role {
        Span span;
        std::string name;
        std::string qname;
        bool from_library{false};
        Sema* owner{nullptr};
        const Sema::Symbol* sym{nullptr};
    };
    std::map<std::string, Role> roles;
    std::map<std::string, Span> link_names;
    std::set<std::string> init_fns; ///< qualified names of the `@init` functions
    std::set<std::string> fini_fns; ///< qualified names of the `@fini` functions
    std::string entry_fn;           ///< qualified name of the `@entry` function (empty = none yet)

    /// `@init`, `@fini` and `@entry` functions are started by the generated code, never called by
    /// user code.
    [[nodiscard]] bool isHook(const std::string& qname) const {
        return init_fns.contains(qname) || fini_fns.contains(qname) || entry_fn == qname;
    }
};

class Lowerer {
  public:
    Lowerer(Sema& sema, LowerShared& shared, bool is_library)
        : s_(sema), sh_(shared), mod_(shared.module), is_library_(is_library) {}

    void collectImpls();
    /// Lowers the generic instances created since the last call; true when there were any.
    bool lowerPendingInstances();
    /// Ends the module scope (after every instance is lowered).
    void finish();
    void checkRegistrations();
    /// Finds the `@init`, `@fini` and `@entry` functions of this module (E0729 / E0752 / E0753 when
    /// one does not fit).
    void checkHookFunctions();
    /// Validates the signature of an `@entry` function: none, or `(argc: i32, argv: **u8)`,
    /// returning void or i32.
    void checkEntrySignature(const FnDecl& f, const std::vector<Type>& params, const Type& ret);
    void run();
    /// The `@init` functions of this module, in source order.
    [[nodiscard]] const std::vector<std::string>& initFunctions() const { return init_order_; }
    /// The `@fini` functions of this module, in source order.
    [[nodiscard]] const std::vector<std::string>& finiFunctions() const { return fini_order_; }

  private:
    using EP = hir::ExprPtr;
    using ImplInfo = LowerShared::ImplInfo;
    using SlotInfo = LowerShared::SlotInfo;

    struct MethodRef {
        enum class Kind { Direct, Virtual, Itable } kind{Kind::Direct};
        std::string fn_name;
        const FnDecl* decl{nullptr};
        Sema* owner{nullptr};   ///< module that resolves the declaration's types
        std::uint32_t index{0}; ///< Itable: method index, Virtual: vtable slot
        bool from_impl{false};
        const ClassDecl* cls{nullptr}; ///< Direct method of a class: its declaration (overloads)
        std::string cls_q;             ///< qualified name of that class
    };

    struct FieldInfo {
        const FieldDecl* decl;
        Sema* owner;
    };

    // --- setup
    void buildModuleScope();
    [[nodiscard]] ScopeEntry
    entryOf(Sema::EntityRef ref, const std::string& local, const Span& span);

    // --- module items
    void lowerItems();
    /// Lowers the items of `items` (declarations of this module, or generic instances).
    void lowerBatch(const std::vector<const Item*>& items);
    void lowerFunction(const FnDecl& f,
                       const Modifiers& mods,
                       const std::string& qname,
                       const std::string& local_name,
                       const std::vector<Attribute>& attrs,
                       const std::string& owner,
                       const Type* self_type,
                       bool overloaded = false);
    void lowerGlobal(const GlobalDecl& g, const Modifiers& mods);
    void lowerLocalConst(const Stmt& s);
    /// `local (a, b) = expr`: the tuple is held once and each name reads one element.
    void lowerTupleLocal(const Stmt& s, std::vector<hir::Stmt>& out);

    // --- ownership: drop glue, drop flags, moves (core-language phase 5)
    /// A class needs drop when it has a `drop` method or a field or base class that needs drop.
    /// A payload enum needs drop when any variant payload type does (only the live variant is
    /// dropped, see `emitSumDropGlue`).
    [[nodiscard]] bool needsDrop(const Type& t);
    /// An array whose element type needs drop, at any depth of nested arrays.
    [[nodiscard]] bool droppableArray(const Type& t);
    /// Whether `t`, or an array element below it, is a payload enum whose highlighted payload needs
    /// drop (`E0721` covers plain classes).
    [[nodiscard]] bool droppableSum(const Type& t);
    /// E0721 when `t` is a droppable array; `where` completes "cannot be ...".
    void rejectDroppableArray(const Type& t, const Span& at, const char* where);
    /// E0759 when `t` is an array of a payload enum that needs drop, else `rejectDroppableArray`.
    void rejectDroppablePlace(const Type& t, const Span& at, const char* where);
    [[nodiscard]] static std::string dropGlueName(const std::string& qname);
    void emitDropGlue(const std::string& qname, const ClassDecl& c, const Span& span);
    /// The drop glue of a payload enum: `switch` on the tag, then the payload of the live variant
    /// only. A variant whose payload needs no drop is not entered.
    void emitSumDropGlue(const std::string& qname, const StructDecl& sum, const Span& span);
    [[nodiscard]] EP localRef(hir::LocalId id, const Type& type, const Span& span);
    [[nodiscard]] EP boolConst(bool value, const Span& span);
    /// `glue(addr)`: runs the drop glue of `t` on the object at `addr`.
    [[nodiscard]] EP dropCallOn(EP addr, const Type& t, const Span& span);
    [[nodiscard]] hir::Stmt
    dropStmt(hir::LocalId var, hir::LocalId flag, const Type& t, const Span& span);
    /// Gives `var` a drop flag (true when initialized) and a deferred drop; the flag is declared
    /// through `decls`, which must be emitted before the variable.
    void registerDrop(hir::LocalId var,
                      const Type& t,
                      bool initialized,
                      const Span& span,
                      std::vector<hir::Stmt>& decls);
    /// A droppable value used by value: a local is moved (its flag cleared); a field, element or
    /// pointer target is E0720; a temporary passes its ownership on. `fromSum` marks the payload
    /// of an enum read by a `case` pattern: that value is the live payload of a scrutinee the
    /// switch owns, so it moves as a whole.
    [[nodiscard]] EP moveOut(EP value, bool fromSum = false);
    /// A droppable temporary used as a receiver: stored in a hidden local dropped at block end.
    [[nodiscard]] EP materializeTemp(EP value, const Span& span);
    void lowerDropAssign(EP target, EP value, const Span& span, std::vector<hir::Stmt>& out);
    [[nodiscard]] EP constructCall(const Expr& e, const ScopeEntry& entry);
    /// `Vec<i32>(args)`: the constructor of the instance of a generic class.
    [[nodiscard]] EP genericConstruct(const Expr& e, const Expr& callee, const ScopeEntry& entry);
    /// Resolves and lowers the `init` a `T(args)` / `new T(args)` of class `t` calls (key empty
    /// when the class has none). False when diagnosed.
    [[nodiscard]] bool
    lowerInitCall(const Expr& e, const Type& t, std::string& key, std::vector<EP>& args);
    /// Whether the class declares an `init` method.
    [[nodiscard]] bool hasInit(const std::string& qname);
    /// `qname.init` when the class has a constructor taking no arguments, else empty.
    [[nodiscard]] std::string zeroArgInitOf(const std::string& qname);
    /// The base class constructor a derived `init` runs first (empty = none).
    [[nodiscard]] std::string baseInitKey(const ClassDecl& cl, Sema& owner);
    std::map<hir::LocalId, hir::LocalId> drop_flags_; ///< droppable local -> its drop flag
    /// Any type -> needsDrop. Structs are cached en route to their fields, so a payload enum that
    /// mentions another payload enum by value terminates instead of recursing.
    std::map<std::string, bool> needs_drop_;
    std::vector<std::pair<hir::LocalId, hir::LocalId>> entry_temps_; ///< (temp, flag) per function
    /// Cleanup flags (drop flags of locals, `defer` flags) declared `false` at the function head so
    /// a landing pad can read them before the statement that sets them has run.
    std::vector<hir::LocalId> entry_flags_;
    /// Every flag-guarded cleanup of the function in registration order; the landing pad runs them
    /// in reverse. A `try` handler runs the ones registered inside its body.
    std::vector<hir::Stmt> unwind_entries_;
    bool has_try_{false}; ///< the function being lowered contains a `try`
    void lowerTry(const Stmt& s, std::vector<hir::Stmt>& out);
    /// `unwind(0, snapshot, nil)`: the `try` body is left normally; the record is removed.
    [[nodiscard]] hir::Stmt leaveCall(hir::LocalId snap, const Span& span);
    std::string base_init_call_; ///< set for the next `init` lowered: the base constructor it runs
    void lowerEnum(const EnumDecl& e);

    // --- payload enums (sum types): `{ tag, data }` over a union of per-variant structs
    struct Sum {
        Sema* owner;
        const StructDecl* decl;
    };
    [[nodiscard]] static const StructDecl* sumDecl(const Item* item);
    /// True when `named` (what a `case` names) and `scrutinee` describe the same enum: the same
    /// declaration, or the declaration and one of its instances.
    [[nodiscard]] static bool sameVariants(const StructDecl& named, const StructDecl& scrutinee);
    [[nodiscard]] std::optional<Sum> sumOfType(const Type& t);
    [[nodiscard]] std::optional<Sum> sumOfEntry(const ScopeEntry& head);
    /// The resolved type of `Name` + `suffix` (`$Tag`, `$Data`, `$Variant`; empty = the enum
    /// itself).
    [[nodiscard]] Type sumPart(const Sum& sum, std::string_view suffix, const Span& span);
    [[nodiscard]] static std::size_t sumDataIndex(const Sum& sum, std::size_t variant);
    [[nodiscard]] static std::optional<std::size_t> sumVariantIndex(const Sum& sum,
                                                                    std::string_view name);
    EP sumConstruct(const Sum& sum, std::size_t variant, std::vector<EP> values, const Span& span);
    EP sumVariantValue(const Sum& sum, const Expr& e);
    EP sumVariantCall(const Sum& sum, const Expr& call);
    EP sumPayloadRead(const Sum& sum,
                      hir::LocalId scrutinee,
                      const Type& self,
                      std::size_t variant,
                      std::size_t index,
                      const Span& span);
    void lowerSumSwitch(const Stmt& s, EP scrutinee, std::vector<hir::Stmt>& out);
    void lowerAggregate(const Item& item);
    void checkImpl(const ImplInfo& info);
    [[nodiscard]] bool cCompatible(Sema& sema, const Type& t);
    void assignSymbol(const std::vector<Attribute>& attrs,
                      bool export_c,
                      bool declared,
                      const std::string& local_name,
                      const std::vector<Type>& params,
                      bool overloaded,
                      std::string& symbol,
                      const Span& at);
    /// Unique name of an overloaded function: its qualified name plus its mangled parameter types.
    [[nodiscard]] static std::string overloadKey(const std::string& qname,
                                                 const std::vector<Type>& params);

    /// A declaration a call may bind to; `key` is the name of its HIR function.
    struct OverloadCandidate {
        const FnDecl* decl{nullptr};
        std::vector<Type> params;
        Type ret;
        std::string key;
    };
    [[nodiscard]] std::vector<OverloadCandidate> candidatesOf(const ScopeEntry& entry);
    /// An argument of an overloaded call, lowered once for overload resolution. A non-literal keeps
    /// its value; an untyped literal (int, float, string, `nil`) keeps only its AST and is costed
    /// and lowered with the parameter type of the chosen overload.
    struct PreArg {
        EP value;
        Type type; ///< the value's type, or the type a literal takes without a context
        bool literal{false};
    };
    [[nodiscard]] std::vector<PreArg> preLowerArgs(const Expr& call);
    /// Instances of the generic overloads of `entry` that fit the call: type arguments come from
    /// `f<...>(...)`, or are inferred from the pre-lowered argument types (E0724 when one stays
    /// unknown).
    [[nodiscard]] std::vector<OverloadCandidate>
    genericCandidates(const ScopeEntry& entry, const Expr& call, const std::vector<PreArg>& pre);
    /// Instances of the pack overloads of `entry` for the call: the pack takes the types of the
    /// arguments after the fixed parameters, one instance per element count (variadic.cpp).
    [[nodiscard]] std::vector<OverloadCandidate>
    packCandidates(const ScopeEntry& entry, const Expr& call, const std::vector<PreArg>& pre);
    /// Binds the type parameters `names` so that `pattern` matches `actual`.
    [[nodiscard]] bool unifyParam(const Type& pattern,
                                  const Type& actual,
                                  const std::vector<std::string>& names,
                                  std::map<std::string, Type>& bound,
                                  Sema& owner);
    [[nodiscard]] std::vector<OverloadCandidate> methodCandidates(const MethodRef& m,
                                                                  const std::string& name);
    /// Index of the overload a call binds to (E0403 ambiguous, E0404 none); nullopt when diagnosed.
    [[nodiscard]] std::optional<std::size_t>
    resolveOverload(const Expr& call,
                    const std::string& name,
                    const std::vector<OverloadCandidate>& cands,
                    const std::vector<PreArg>& pre);
    /// Lowers the arguments of the chosen overload from `pre` (E0710 on a wrong count).
    bool lowerArgsPre(const Expr& call,
                      const std::vector<Type>& params,
                      std::vector<PreArg>& pre,
                      std::vector<EP>& out,
                      const std::string& callee);
    /// Whether a lowered value converts implicitly to `to`: the rules of `coerce`, without
    /// lowering or diagnostics.
    [[nodiscard]] bool convertible(const hir::Expr& value, const Type& to);
    /// Whether the untyped literal `arg` can take the type `param` (the rules of `expr`).
    [[nodiscard]] bool literalFits(const Expr& arg, const Type& param);
    /// Signature text for diagnostics: `name(i32, str)`.
    [[nodiscard]] static std::string signatureOf(const std::string& name,
                                                 const std::vector<Type>& params);

    // --- statements
    void lowerBlock(const Block& block, std::vector<hir::Stmt>& out, bool new_scope);
    void lowerStmt(const Stmt& s, std::vector<hir::Stmt>& out);
    void lowerStmtBody(const Stmt& s, std::vector<hir::Stmt>& out);
    void lowerAssign(const Expr& target,
                     const Expr& value,
                     const Span& span,
                     std::vector<hir::Stmt>& out);
    /// Stores the value of `value_ast` into `target` (already checked as a place). `raw` stores
    /// without dropping the old value (a constructor's droppable field).
    void assignTo(EP target,
                  const Expr& value_ast,
                  const Span& span,
                  std::vector<hir::Stmt>& out,
                  bool raw = false);
    /// `__builtin_write(p, value)` as a statement: stores without dropping the old value.
    void lowerRawWrite(const Expr& call, const Span& span, std::vector<hir::Stmt>& out);
    void lowerCompound(const Stmt& s, std::vector<hir::Stmt>& out);
    void lowerFor(const Stmt& s, std::vector<hir::Stmt>& out);
    void lowerForRange(const Stmt& s, std::vector<hir::Stmt>& out);
    void lowerForEach(const Stmt& s, std::vector<hir::Stmt>& out);
    void lowerSwitch(const Stmt& s, std::vector<hir::Stmt>& out);
    void lowerJump(const Stmt& s, std::vector<hir::Stmt>& out);
    /// Appends clones of the deferred statements of the frames `[from, defers_.size())`, innermost
    /// first.
    void emitDefers(std::size_t from, std::vector<hir::Stmt>& out) const;
    EP ternaryExpr(const Expr& e, const Type* expected);
    /// `e?`: hoists the evaluation of `e` and the early `return` of its failure into `prelude_`
    /// (the statements run before the statement being lowered) and yields the success value.
    EP tryExpr(const Expr& e);
    /// Statements the statement being lowered runs first (`?` hoists there); null where nothing
    /// may be hoisted (loop conditions, `defer`).
    std::vector<hir::Stmt>* prelude_{nullptr};
    int lazy_{0}; ///< inside an operand that may not run (`&&`/`||` right side, a ternary branch)
    hir::LocalId hiddenLocal(const char* prefix, const Type& type, const Span& span);
    hir::LocalId
    addLocal(const std::string& name, const Type& type, bool is_param, const Span& span);
    void bindLocal(const std::string& name, hir::LocalId id, const Type& type, const Span& span);

    // --- expressions
    EP expr(const Expr& e, const Type* expected);
    EP nameExpr(const Expr& e, const Type* expected);
    EP fromEntry(const ScopeEntry& entry, const Expr& at, const Type* expected);
    /// A generic `fn` named as a value: its type arguments are written (`f<i32>`) or taken from
    /// the expected `fn(...)` type.
    EP genericFnValue(const ScopeEntry& entry, const Expr& at, const Type* expected);
    /// `a.b`: a namespace member, an enum member or a variant of a payload enum (both are
    /// resolved through the expected type when the enum is generic), or a field of a value.
    EP fieldExpr(const Expr& e, const Type* expected = nullptr);
    /// The instance of a generic enum that `expected` names (`Result<i32, E>` for `Result`):
    /// `E.Ok` and friends of a generic enum take their type from the surrounding expression.
    [[nodiscard]] std::optional<Type> instancesOf(const Type* expected, const std::string& qname);
    /// The sum behind `head`: its instance when the head names a generic payload enum (chosen by
    /// what `expected` names), the declaration's own variant list otherwise. `at` is the use, for
    /// the E0724 of a generic enum named without an expected type.
    [[nodiscard]] std::optional<Sum>
    sumFor(const ScopeEntry& head, const Type* expected, const Span& at);
    /// True when `head` names a payload enum that has type parameters of its own (whose instance
    /// `sumFor` needs an expected type for).
    [[nodiscard]] bool isGenericPayloadEnum(const ScopeEntry& head) const;
    EP unaryExpr(const Expr& e, const Type* expected);
    EP binaryExpr(const Expr& e, const Type* expected);
    EP castExpr(const Expr& e);
    EP callExpr(const Expr& e, const Type* expected);
    EP callEntry(const Expr& e, const ScopeEntry& entry, const std::string& name);
    EP methodCall(const Expr& e, const Expr& recv_ast);
    EP syscallExpr(const Expr& e);
    EP builtinCall(const Expr& e, const std::string& name);
    EP asmCall(const Expr& e);
    std::optional<std::uint32_t> memoryOrder(const Expr& arg);
    EP indexExpr(const Expr& e);
    EP sliceExpr(const Expr& e);
    EP arrayExpr(const Expr& e, const Type* expected);
    EP tupleExpr(const Expr& e, const Type* expected);
    EP structLiteral(const Expr& e);
    EP newExpr(const Expr& e);
    EP constExpr(const Expr& e, const Type* expected);
    EP fromConst(const ConstValue& v, const Type* expected, const Span& span);
    EP stringLiteral(const std::string& text, const Type* expected, const Span& span);

    bool lowerArgs(const Expr& call,
                   const std::vector<Type>& params,
                   std::vector<EP>& out,
                   const std::string& callee);
    void lowerFields(const Expr& e, const Type& agg, std::vector<hir::FieldInit>& out);

    EP coerce(EP value, const Type& to, const Span& at);
    EP castTo(EP value, const Type& to, bool implicit);
    EP toInterface(EP value, const Type& iface, const Span& at);
    /// E0760 when `value` is, or holds, an interface whose data pointer addresses a stack object of
    /// this function; such a fat pointer dangles once the frame is gone. True when reported.
    bool rejectStackInterface(const hir::Expr& value);
    [[nodiscard]] bool castable(const Type& from, const Type& to) const;

    // --- queries
    /// `t` with the envelope of a generic payload enum applied: `Result$Ok<A, B>` reads
    /// `Result<A, B>$Ok`, the payload struct of the instance. Other types are unchanged.
    [[nodiscard]] Type sumPartResolved(Sema& sema, const Type& t);
    [[nodiscard]] Type resolvedIn(Sema& sema, const Type& t);
    [[nodiscard]] Type resolved(const Type& t) { return resolvedIn(s_, t); }
    /// Declared type of a global; for an untyped `const`, the default type of its literal value.
    [[nodiscard]] Type globalType(Sema& sema, const GlobalDecl& g);
    [[nodiscard]] Type normalized(Sema& sema, const Type& t);
    [[nodiscard]] Type fnTypeOf(Sema& owner, const FnDecl& f);
    [[nodiscard]] std::optional<MethodRef> findMethod(const Type& agg, const std::string& name);
    [[nodiscard]] const ImplInfo* findImplExact(const std::string& type_q,
                                                const std::string& iface_q) const;
    /// The impl of `iface_q` for `type_q`; instantiates generic impls that apply first.
    [[nodiscard]] const ImplInfo* findImpl(const std::string& type_q, const std::string& iface_q);
    /// Instantiates every generic impl for every instance it applies to (E0731, E0732 reported).
    /// True when at least one impl was added.
    bool instantiateImpls();
    void instantiateImpl(const LowerShared::ImplTemplate& t,
                         Sema& m,
                         const std::string& local,
                         const Sema::InstanceInfo& info);
    /// Lowers the module's impls (concrete ones; generic instances are lowered as pending items).
    void lowerImpls();
    void lowerImplInfo(const ImplInfo& info);
    [[nodiscard]] std::optional<Sema::EntityRef> entityOfType(const Type& t);
    void collectFields(const std::string& qname, std::vector<FieldInfo>& out);
    [[nodiscard]] const std::vector<SlotInfo>& vtableOf(const std::string& qname);
    [[nodiscard]] bool isConstValue(const hir::Expr& e, const hir::Expr** offender) const;
    [[nodiscard]] std::optional<std::uint64_t> arrayLength(Sema& sema, const Type& t);
    [[nodiscard]] const ScopeEntry* lookupName(const std::string& name);
    [[nodiscard]] const ScopeEntry* chainEntry(const Expr& e);
    [[nodiscard]] const ScopeEntry*
    namespaceMember(const ScopeEntry& ns, const std::string& name, const Span& at, bool report);
    [[nodiscard]] bool isRegisteredName(const std::string& name) const;

    // --- helpers
    [[nodiscard]] EP mk(hir::ExprKind kind, Type type, const Span& span) const;
    [[nodiscard]] EP intConst(std::uint64_t v, Type type, const Span& span) const;
    void err(const char* code, const Span& at, std::string message, std::string label);
    [[nodiscard]] bool inUnsafe() const { return unsafe_ > 0; }
    [[nodiscard]] unsigned ptrBits() const { return s_.config_.target.pointer_width; }

    Sema& s_;
    LowerShared& sh_;
    hir::Module& mod_;
    bool is_library_;                         ///< module is part of the standard library
    std::vector<std::string> init_order_; ///< this module's `@init` functions (qualified names)
    std::vector<std::string> fini_order_; ///< this module's `@fini` functions (qualified names)
    ScopeStack scopes_;
    hir::Function* fn_{nullptr};
    bool in_ctor_{false}; ///< the function is a class `init`: `self.f = v` on a droppable field is
                          ///< a raw store
    Type ret_;
    int unsafe_{0};
    struct LoopCtx {
        std::string label;
        std::size_t defer_mark; ///< `defers_.size()` when the loop was entered
    };
    std::vector<LoopCtx> loops_; ///< enclosing loops of the statement being lowered, innermost last
    /// One frame per block being lowered: the lowered `defer` statements registered in it, in
    /// order.
    std::vector<std::vector<hir::Stmt>> defers_;
    unsigned hidden_counter_{0};    ///< unique suffix for compiler-made locals
    std::set<std::string> fn_keys_; ///< HIR names of the module's functions (overloads included)
    bool star_import_{false};
    std::set<std::string> registered_names_;
    std::map<std::string, ScopeEntry> extra_entries_; ///< registered globals / namespace members
};

} // namespace lower

} // namespace xlang
