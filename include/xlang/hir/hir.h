#pragma once

#include "xlang/lang/ast.h"
#include "xlang/lang/types.h"

#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

namespace xlang::hir {

// HIR (plan 4.3): types resolved, implicit conversions explicit (`Cast`), sugar lowered, class
// methods are plain functions `fn(self: *T, ...)`, interface calls are explicit itable calls.
// The tree is typed: every expression carries its `Type`. A type of kind `Named` marks an unknown
// type (a name from a module that is not loaded); codegen never sees it.

using LocalId = std::uint32_t;

struct Local {
    std::string name;
    Type type;
    bool is_param{false};
    Span span;
};

enum class ExprKind {
    IntConst,     ///< int_value (two's complement bits), type Int/Enum
    FloatConst,   ///< float_value
    BoolConst,    ///< bool_value
    StrConst,     ///< text; type `*u8` (or an enum over `*u8`), or `str` = { ptr, len }
    Nil,          ///< `nil`, typed as the pointer/function pointer it was converted to
    LocalRef,     ///< local
    GlobalRef,    ///< text = global name
    FuncRef,      ///< text = function name, type FnPtr
    External,     ///< text = module, member = name; unresolved until module loading (4.6)
    Unary,        ///< unary_op (Neg, Not); lhs
    Binary,       ///< binary_op; lhs, rhs already converted to a common type; `checked` for int / %
    Select,       ///< lhs ? rhs : args[0]; only the chosen branch is evaluated
    Cast,         ///< lhs -> type; `implicit` when the compiler inserted it
    Call,         ///< direct call: text = function name, args (methods: args[0] is `self`)
    CallIndirect, ///< lhs = function pointer value, args
    CallVirtual,  ///< vtable call: index = slot, text = statically resolved function; args[0] is
                  ///< `self`
    CallItable,   ///< lhs = interface value, text = method, index = method index in the interface,
                  ///< args
    ExternalCall, ///< text = module, member = name, args (unresolved until module loading)
    Syscall,      ///< args[0] = number, rest = arguments; type i64
    Field,  ///< lhs.text; `through_pointer` when lhs is a pointer to the aggregate; index = field
            ///< index
    Index,  ///< lhs[rhs]; `checked` for arrays under --safety=checked
    Slice,  ///< `lhs[lo..hi]`: a `str` view of the bytes from `lo` up to `hi`; `rhs` = `hi` (null when
            ///< the slice runs to the end), `args[0]` = `lo` (null for `[..hi]`). `checked`
            ///< under --safety=checked: both bounds are compared against the base length.
    Deref,  ///< *lhs
    AddrOf, ///< &lhs
    ArrayList,   ///< [args...]
    ArrayRepeat, ///< [lhs; int_value]
    TupleLit,    ///< (args...): a tuple value; the `type` is the Tuple type with `args.size()`
                 ///< elements
    StructLit,   ///< type { fields }
    New, ///< init `target` in the memory `lhs` points at (placement), then fields; type `*target`;
         ///< `registered` (no lhs): the memory comes from the `register(alloc)` function
    ToInterface, ///< lhs (a pointer to the object) -> interface `type`; text = impl type name
    AtomicLoad,  ///< lhs = pointer, index = MemoryOrder
    AtomicStore, ///< lhs = pointer, rhs = value, index = MemoryOrder; void
    AtomicXchg,  ///< lhs = pointer, rhs = value; returns the old value
    AtomicCas,   ///< lhs = pointer, rhs = expected, args[0] = desired; returns the old value
    AtomicAdd,   ///< lhs = pointer, rhs = value; returns the old value
    Fence,       ///< index = MemoryOrder; void
    Asm,         ///< text = template, asm_operands, clobbers, nomem/noreturn; void
    Construct,   ///< `T(args)` of a class: a new object of type `target` (zeroed, vptr set); text =
                 ///< its `init` function (empty = none), args = the init arguments (no `self`)
    Move,        ///< lhs (a droppable local) read by value; clears the drop flag `local`
    Materialize, ///< lhs (a droppable temporary) stored in local `local`, drop flag `index` set; an
                 ///< lvalue: the stored object
    RawRead, ///< `__builtin_read`: lhs = pointer; the pointee read by value. Neither the drop flag
             ///< nor the place changes; the caller owns the copy. Codegen treats it as `Deref`
    UnwindPads,     ///< `__builtin_unwindPads()`: `*usize` to the table of (function, landing pad)
                    ///< address pairs
    UnwindPadCount, ///< `__builtin_unwindPadCount()`: `usize`, the number of pairs in that table
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct FieldInit {
    std::string name;
    std::uint32_t index{0};
    ExprPtr value;
};

/// `in(reg) x` / `out("rax") y` / `inout(reg) z` of `__builtin_asm`; `value` is the input value
/// (In) or the place that receives the output (Out, InOut: also read first).
struct AsmOperand {
    enum class Kind { In, Out, InOut } kind{Kind::In};
    std::string reg; ///< `reg` (any register) or a register name
    ExprPtr value;
};

struct Expr {
    ExprKind kind{ExprKind::IntConst};
    Type type;
    Span span;
    bool lvalue{false};
    /// A place that must not be written: a `const`, or an element/field of one (`readonly` is
    /// not set through a pointer, so `*p` of a `const` pointer stays writable).
    bool readonly{false};

    std::uint64_t int_value{0};
    double float_value{0.0};
    bool bool_value{false};
    std::string text;
    std::string member;
    LocalId local{0};
    UnaryOp unary_op{UnaryOp::Neg};
    BinaryOp binary_op{BinaryOp::Add};
    ExprPtr lhs;
    ExprPtr rhs;
    std::vector<ExprPtr> args;
    std::vector<FieldInit> fields;
    std::vector<AsmOperand> asm_operands;
    std::vector<std::string> clobbers;
    Type target; ///< New: the allocated type
    std::uint32_t index{0};
    bool implicit{false};
    bool checked{false};
    bool through_pointer{false};
    bool registered{false};
    bool nomem{false};    ///< Asm: `options("nomem")`
    bool noreturn{false}; ///< Asm: `options("noreturn")`
};

enum class StmtKind { Local, Assign, Return, Expr, If, While, Continue, Break, Block, Try };

struct Stmt {
    StmtKind kind{StmtKind::Expr};
    Span span;
    LocalId local{0};     ///< Local; Try: the snapshot buffer (`[usize; 32]`)
    LocalId msg_local{0}; ///< Try: the message buffer (`[u8; 256]`) the panic text is written to
    ExprPtr target;       ///< Assign
    ExprPtr value;        ///< Local init / Assign value / Return value / Expr / If,While condition
    std::vector<Stmt> body;
    std::vector<Stmt> else_body;
    /// While: statements run after the body on every iteration, including after `continue`
    /// (the step of a `for`). Empty for a plain `while`.
    std::vector<Stmt> step;
    /// While: the condition is tested after the body (`do { } while c`).
    bool post_test{false};
    /// Break/Continue: how many enclosing loops to leave (0 = the innermost one).
    std::uint32_t depth{0};
};

struct Function {
    std::string name; ///< qualified name; methods: `Type.method`, impl methods: `Type.Iface.method`
    std::string module;        ///< defining module (mangling name)
    std::string symbol;        ///< final link symbol (mangling applied, plan 4.9)
    std::string owner;         ///< class/struct name for methods, empty otherwise
    std::vector<Local> locals; ///< parameters first
    std::uint32_t param_count{0};
    Type ret;
    std::vector<Stmt> body;
    /// The cleanups (flag-guarded drops and `defer`s) of the whole function in reverse registration
    /// order: the landing pad a panic unwinding through this frame runs. Empty = no landing pad.
    std::vector<Stmt> unwind_body;
    bool has_try{false}; ///< the body contains a `try`: all locals stay in memory
    bool has_body{false};
    bool exported{false};
    bool export_c{false};
    bool declared{false};
    std::string import_library; ///< `@library("name")` on a `declare fn`: the OS library it is
                                ///< imported from
    bool registered{false};
    std::string register_role;
    bool is_init{false};  ///< `@init`: called by `_xl_initAll` before the entry
    bool is_fini{false};  ///< `@fini`: called by `_xl_finiAll` after the entry, in reverse order
    bool is_entry{false}; ///< `@entry`: the program's entry, called by the generated start code
    bool is_virtual{false};
    bool is_override{false};
    Span span;
};

struct Global {
    std::string name;
    std::string module;
    std::string symbol;
    Type type;
    ExprPtr init;         ///< compile-time constant; null = zero-initialized / external
    bool constant{false}; ///< `const`: emitted read-only (.rodata); written only by its initializer
    bool exported{false};
    bool export_c{false};
    bool declared{false};
    bool registered{false};
    std::string register_role;
    Span span;
};

struct Field {
    std::string name;
    Type type;
};

struct VtableSlot {
    std::string method;   ///< method name
    std::string function; ///< implementing function (qualified HIR name)
};

struct Aggregate {
    std::string name;
    std::vector<VtableSlot>
        vtable;         ///< class: complete vtable (base slots first); empty = no virtuals
    bool repr_c{false}; ///< `@repr(c)`: Itanium vtable (offset-to-top, typeinfo, functions)
    bool is_class{false};
    std::string base;                 ///< class: base class name, empty = none
    std::vector<Field> fields;        ///< own fields (base fields come first in layouts)
    std::vector<std::string> methods; ///< function names of the methods
    bool opaque{false};
    /// A union: every field sits at offset 0 (the `E$Data` of a payload enum).
    bool is_union{false};
    Span span;
};

struct InterfaceMethod {
    std::string name;
    std::vector<Type> params; ///< without `self`
    Type ret;
};

struct Interface {
    std::string name;
    std::vector<InterfaceMethod> methods;
    Span span;
};

struct Impl {
    std::string interface; ///< qualified interface name
    std::string type;      ///< implementing struct/class (qualified)
    std::vector<std::string>
        methods; ///< function names, in the interface's method order (the itable)
    Span span;
};

struct EnumMemberValue {
    std::string name;
    std::int64_t int_value{0};
    std::string text; ///< string/float/bool enums: the constant spelled out
};

struct Enum {
    std::string name;
    Type underlying;
    std::vector<EnumMemberValue> members;
};

struct Module {
    std::vector<Function> functions;
    std::vector<Global> globals;
    std::vector<Aggregate> aggregates;
    std::vector<Interface> interfaces;
    std::vector<Impl> impls;
    std::vector<Enum> enums;
    std::string panic_function;  ///< `register(panic)` function, empty = plain trap
    std::string alloc_function;  ///< `register(alloc)` function, empty = none (`new T` is E0501)
    std::string free_function;   ///< `register(free)` function, empty = none (`delete p` is E0501)
    std::string unwind_function; ///< `register(unwind)` function, empty = none (no landing pads,
                                 ///< `try` is E0501)
    std::vector<std::string>
        init_functions; ///< `@init` functions in run order (library group first)
    std::vector<std::string> fini_functions; ///< `@fini` functions in the order they were declared;
                                             ///< teardown runs them reversed
};

void dumpModule(std::ostream& out, const Module& module);

} // namespace xlang::hir
