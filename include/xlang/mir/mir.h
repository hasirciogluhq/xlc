#pragma once

#include "xlang/lang/ast.h"
#include "xlang/lang/types.h"
#include "xlang/util/source.h"

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace xlang::mir {

// MIR: the control-flow graph of each function, built from the HIR (`mir/build.h`).
//
// HIR is a typed tree: good for checks about types and names. MIR is what the flow analyses
// (moves, borrows, initialization) and the optimizations work on:
//   - a function is a list of basic blocks; each block is straight-line statements ending in one
//     terminator, and every edge between blocks is explicit;
//   - every intermediate value is a local, so an expression never nests: an operand is a place
//     read by copy or by move, or a constant;
//   - a place is a local with a path of projections (`a.f`, `*p`, `a[i]`), the unit a move, a
//     borrow or a drop acts on.
//
// @invariant MIR is built only from HIR and never refers back to it; a span on every statement and
// terminator points at the source so a finding is reported at the code the user wrote.
// @todo MIR does not yet carry what only code generation needs (the `asm` template and clobbers,
// the landing pad of `unwind_body`); codegen reads the HIR until it does.

using LocalId = std::uint32_t;
using BlockId = std::uint32_t;

/// A slot of the function frame: a parameter, a user variable, a drop flag or a temporary.
struct Local {
    enum class Kind : std::uint8_t { Param, User, DropFlag, Temp };
    Kind kind{Kind::Temp};
    std::string name; ///< the source name; empty for a temporary the builder made
    Type type;
    Span span;
};

/// One step of a place path.
struct Projection {
    enum class Kind : std::uint8_t { Field, Deref, Index };
    Kind kind{Kind::Field};
    std::uint32_t field{0}; ///< Field: the field index
    LocalId index{0};       ///< Index: the local that holds the element index
    bool checked{false};    ///< Index: bounds-checked (`--safety=checked`)
};

/// A memory location: `local` followed by `projections` (`a.f`, `*p`, `(*p).f`, `a[i]`).
struct Place {
    LocalId local{0};
    std::vector<Projection> projections;

    [[nodiscard]] bool isLocal() const { return projections.empty(); }
};

/// A compile-time value.
struct Constant {
    enum class Kind : std::uint8_t {
        None,     ///< no value: the operand of a `void` return, a missing slice bound
        Int,      ///< value: the two's complement bits
        Float,    ///< value: the double
        Bool,     ///< value: the bool
        Str,      ///< value: the bytes
        Nil,      ///< the null pointer of `type`
        Function, ///< value: the HIR name; the address of the function
        Global,   ///< value: the HIR name; the address of the global (its place is `*` of it)
    };
    Kind kind{Kind::None};
    Type type;
    std::variant<std::monostate, std::uint64_t, double, bool, std::string> value;
};

/// A value an rvalue or a call consumes.
struct Operand {
    enum class Kind : std::uint8_t { Copy, Move, Constant };
    Kind kind{Kind::Copy};
    Place place;       ///< Copy, Move
    Constant constant; ///< Constant
};

/// The right side of an assignment.
struct Rvalue {
    enum class Kind : std::uint8_t {
        Use,         ///< operands[0]
        AddressOf,   ///< `&place`
        Unary,       ///< unary_op operands[0]
        Binary,      ///< operands[0] binary_op operands[1]; `checked` for a checked `/`, `%`
        Cast,        ///< operands[0] as `type`
        Aggregate,   ///< a struct, tuple or array value: operands[i] goes to field `fields[i]`
                     ///< (`fields` empty: positional)
        Repeat,      ///< `[operands[0]; count]`
        Zero,        ///< a zeroed `type`; a class also gets its vptr
        Slice,       ///< `place[operands[0]..operands[1]]`; a `None` constant is a missing bound
        ToInterface, ///< operands[0] (a pointer to the object) as interface `type`, implemented by
                     ///< `name`
    };
    Kind kind{Kind::Use};
    Type type;
    Place place; ///< AddressOf, Slice
    std::vector<Operand> operands;
    std::vector<std::uint32_t> fields; ///< Aggregate
    std::uint64_t count{0};            ///< Repeat
    std::string name;                  ///< ToInterface
    UnaryOp unary_op{UnaryOp::Neg};
    BinaryOp binary_op{BinaryOp::Add};
    bool checked{false};
};

/// `place = value`; a statement never transfers control.
struct Statement {
    Place place;
    Rvalue value;
    Span span;
};

/// The operations the compiler provides itself; a call of one is a `Builtin` callee.
enum class Builtin : std::uint8_t {
    Alloc,          ///< the `register(alloc)` function for a `new` of `Callee::type`
    AtomicLoad,     ///< (pointer); Callee::index = MemoryOrder
    AtomicStore,    ///< (pointer, value)
    AtomicXchg,     ///< (pointer, value) -> old
    AtomicCas,      ///< (pointer, expected, desired) -> old
    AtomicAdd,      ///< (pointer, value) -> old
    Fence,          ///< Callee::index = MemoryOrder
    Asm,            ///< the inputs and the addresses of the outputs, in operand order
    UnwindPads,     ///< `__builtin_unwindPads()`
    UnwindPadCount, ///< `__builtin_unwindPadCount()`
};

/// What a `Call` terminator calls.
struct Callee {
    enum class Kind : std::uint8_t {
        Direct,    ///< name: the HIR function
        Indirect,  ///< value: the function pointer
        Virtual,   ///< index: the vtable slot; name: the statically resolved function
        Interface, ///< value: the interface; name: the method; index: its itable slot
        Syscall,   ///< args[0] is the number
        Builtin,   ///< builtin
    };
    Kind kind{Kind::Direct};
    std::string name;
    std::uint32_t index{0};
    Operand value;
    mir::Builtin builtin{mir::Builtin::Alloc};
    Type type; ///< Builtin::Alloc: the allocated type
};

/// The control transfer that ends a block.
struct Terminator {
    enum class Kind : std::uint8_t {
        Goto,        ///< to targets[0]
        Branch,      ///< operand true: targets[0], false: targets[1]
        Call,        ///< destination = callee(args); continues at targets[0], none when the call
                     ///< never returns
        Drop,        ///< runs the drop glue `glue` on `place`; continues at targets[0]
        Try,         ///< `try`: snapshot into `place`, the message buffer is `message`; the body
                     ///< is targets[0], the `catch` targets[1]
        Return,      ///< returns operand (a `None` constant in a `void` function)
        Unreachable, ///< no path reaches the end of the block
    };
    Kind kind{Kind::Unreachable};
    Operand operand; ///< Branch: the condition; Return: the value
    Callee callee;   ///< Call
    std::vector<Operand> args;          ///< Call
    std::optional<Place> destination;   ///< Call: where the result goes; none for `void`
    Place place;                        ///< Drop: the dropped place; Try: the snapshot buffer
    std::string glue;                   ///< Drop
    LocalId message{0};                 ///< Try
    std::vector<BlockId> targets;
    std::optional<BlockId> unwind;      ///< Call, Drop: the `catch` a panic in it jumps to
    Span span;

    /// The blocks control can go to next: `targets`, then `unwind`.
    [[nodiscard]] std::vector<BlockId> successors() const {
        std::vector<BlockId> out = targets;
        if (unwind) {
            out.push_back(*unwind);
        }
        return out;
    }
};

struct BasicBlock {
    std::vector<Statement> statements;
    Terminator terminator;
};

/// The MIR of one function. Block 0 is the entry.
struct Body {
    std::string name; ///< the HIR function's qualified name
    std::vector<Local> locals;
    std::uint32_t param_count{0};
    Type ret;
    std::vector<BasicBlock> blocks;
    Span span;
};

struct Module {
    std::vector<Body> bodies; ///< in the order of the HIR functions
};

void dumpModule(std::ostream& out, const Module& module);

} // namespace xlang::mir
