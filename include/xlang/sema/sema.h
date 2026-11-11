#pragma once

#include "xlang/hir/hir.h"
#include "xlang/lang/ast.h"
#include "xlang/lang/config.h"
#include "xlang/sema/layout.h"
#include "xlang/util/diagnostics.h"

#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace xlang {

namespace lower {
// The lowering pass (`src/sema/lower/lower.hpp`): one instance per module.
class Lowerer;
} // namespace lower

class Compilation;

/// Wide integer for compile-time values.
using Int128 = __int128;
using UInt128 = unsigned __int128;

/// Compile-time value (plan 3.6): literals, `define` constants, `-D` values, enum members and the
/// layout queries.
// `__int128` asks for 16-byte alignment, so the compiler copies a ConstValue with `movaps`, which
// faults on a 16-misaligned address. On Windows (clang-cl) `Sema::evalConst` crashed that way
// (access violation at `movaps [r12+10h]`, found with cdb). `pack(8)` caps the member alignment, so
// a ConstValue is 8-aligned and is copied with `movups`. (`aligned(8)` on a typedef does not lower
// it: alignof stays 16.)
#pragma pack(push, 8)
struct ConstValue {
    enum class Kind { Int, Float, Bool, String } kind{Kind::Int};
    Int128 int_value{0};
    double float_value{0.0};
    bool bool_value{false};
    std::string string_value;

    [[nodiscard]] static ConstValue makeInt(Int128 v);
    [[nodiscard]] static ConstValue makeFloat(double v);
    [[nodiscard]] static ConstValue makeBool(bool v);
    [[nodiscard]] static ConstValue makeString(std::string v);
    [[nodiscard]] std::string toString() const;
};
#pragma pack(pop)
static_assert(alignof(ConstValue) == 8, "ConstValue must not need 16-byte alignment (see above)");

/// Single-module semantic services that do not need function bodies typed (plan phases 2 and
/// 3.6/3.8): name table, type alias resolution, enums, constant evaluation, layouts,
/// `staticAssert`. Cross-module names (`alias.Name`) stay unresolved until module loading.
class Sema {
  public:
    Sema(const Program& program,
         const BuildConfig& config,
         DiagnosticEngine& diagnostics,
         const LayoutHistory* history = nullptr);

    /// Runs every check over the module: duplicate names, aliases, enums, array lengths in all
    /// types, layouts of all structs/classes (with `declare` warnings), module-level
    /// `staticAssert`s.
    void analyze();

    /// Follows aliases and turns `Named` into Struct/Class/Interface/Enum. Reports unknown
    /// names (E0706) and cyclic aliases (E0704); nullopt on failure.
    [[nodiscard]] std::optional<Type> resolve(const Type& type);

    [[nodiscard]] std::optional<ConstValue> evalConst(const Expr& expr);

    [[nodiscard]] std::optional<TypeLayout> layoutOf(const Type& type, const Span& at);
    /// Layout of the struct/class declared with `name`, nullopt when unknown or unsized.
    [[nodiscard]] std::optional<TypeLayout> layoutOfName(const std::string& name, const Span& at);

    /// Names of the struct and class declarations, in source order.
    [[nodiscard]] std::vector<std::string> aggregateNames() const;

    /// Value of `Enum.Member` (auto-numbering applied).
    [[nodiscard]] std::optional<ConstValue>
    enumMember(const std::string& enum_name, const std::string& member, const Span& at);

    /// Plan 4.1-4.3: scopes, name resolution and type checking of every function body, lowered to
    /// HIR in the same pass. Run `analyze()` first; diagnostics go to the engine, the returned
    /// module is only meaningful when no error was reported.
    [[nodiscard]] hir::Module lower();

    [[nodiscard]] const BuildConfig& config() const { return config_; }

    // --- module awareness (plan 4.6); a standalone Sema has no compilation and resolves imports
    // --- as external names.

    struct Symbol {
        enum class Kind {
            Struct,
            Class,
            Interface,
            Enum,
            Alias,
            Define,
            Global,
            Fn,
            FnAlias,
            Macro
        } kind;
        const Item* item{nullptr};
        /// Fn: every declaration of the name in declaration order (more than one = overloads).
        std::vector<const Item*> overloads;
    };

    /// An import binding made by the compilation: a name that stands for an entity of another
    /// module, or a whole module (`import "m" as a`).
    struct ImportBinding {
        enum class Kind { Entity, Namespace } kind{Kind::Entity};
        std::string module_key; ///< defining module (Entity) / the module itself (Namespace)
        std::string name;       ///< Entity: name inside that module
        Span span;
    };

    /// An entity found through scopes: the module that owns it and its local name there.
    struct EntityRef {
        Sema* owner{nullptr};
        const Symbol* sym{nullptr};
        std::string local;
        [[nodiscard]] std::string qname() const;
    };

    void attach(Compilation* compilation, std::string module_key);
    void bindImport(const std::string& local_name, ImportBinding binding);
    [[nodiscard]] const std::map<std::string, ImportBinding>& imports() const { return imports_; }
    [[nodiscard]] const std::string& moduleKey() const { return key_; }
    [[nodiscard]] Compilation* compilation() const { return comp_; }
    [[nodiscard]] const Program& program() const { return program_; }
    /// `key::name` for imported modules, plain `name` for the root module and builtins.
    [[nodiscard]] std::string qualify(const std::string& local) const;
    /// Local definition, import, or registered global, in that order.
    [[nodiscard]] std::optional<EntityRef> lookupEntity(const std::string& name);
    /// `ns.Name`
    [[nodiscard]] std::optional<EntityRef> lookupQualified(const std::vector<std::string>& path);
    /// The function a `fn alias = path` names, following alias chains (E0704 on a cycle, E0706
    /// when the target is missing or not a function).
    [[nodiscard]] std::optional<EntityRef> resolveFnAlias(const std::vector<std::string>& path,
                                                          const Span& at);

    /// A concrete instance of a generic declaration: its origin and type arguments.
    struct InstanceInfo {
        std::string generic; ///< qualified name of the generic declaration
        std::vector<Type> args;
        Span requested_at;                 ///< where the instance was first requested
        std::string parent;                ///< local name of the instance that requested it, if any
        const Sema* parent_owner{nullptr}; ///< the module that owns `parent`
    };
    /// The instance of the generic declaration `generic` for `args` (created on first use, cached;
    /// E0724 on a wrong argument count, E0725 when the nesting of arguments is too deep).
    [[nodiscard]] std::optional<EntityRef>
    instantiate(const EntityRef& generic, const std::vector<Type>& args, const Span& at);
    [[nodiscard]] EntityRef
    instanceOf(const EntityRef& generic, const std::vector<Type>& args, const Span& at);
    /// Interns `item` as the instance named `local` (`item_instance_`, `instance_info_`, the
    /// pending queue) and returns its symbol. The caller has already pushed `item` into
    /// `instances_`; `instantiate` uses it for every item of a payload enum, so that an enum
    /// instance and the instances of its tag, union and payload structs are made as a unit.
    [[nodiscard]] const Symbol* internInstance(const Item& item,
                                               const std::string& local,
                                               const std::string& origin,
                                               const std::vector<Type>& args,
                                               const Span& at);
    /// The module key of the generic declaration that `local` instantiates (`key()` itself when
    /// `local` is not an instance).
    [[nodiscard]] std::string genericOwnerKey(const std::string& local) const;
    /// Instances created since the last call, for lowering.
    [[nodiscard]] std::vector<const Item*> takePendingInstances();
    /// Origin of an instance by its local name (nullptr when `local` is not an instance).
    [[nodiscard]] const InstanceInfo* instanceInfo(const std::string& local) const;
    /// Every instance of this module by local name.
    [[nodiscard]] const std::map<std::string, InstanceInfo>& instances() const {
        return instance_info_;
    }
    /// Adds a generic `impl` instance (its target and interface are concrete) and queues it for
    /// lowering. The returned item lives as long as the module.
    [[nodiscard]] const Item* addImplInstance(Item item);

    /// A method with its own type parameters, instantiated for one set of type arguments.
    struct MethodInstance {
        FnDecl decl;       ///< the method with its type parameters substituted
        std::string key;   ///< link name: `Class.method<args>`
        std::string cls;   ///< local name of the class
        std::string cls_q; ///< qualified name of the class
    };
    /// The instance of the generic method `generic` of class `cls` for `args` (created on first
    /// use, cached, queued for lowering by this module).
    [[nodiscard]] const MethodInstance* instantiateMethod(const FnDecl& generic,
                                                          const std::string& cls,
                                                          const std::string& cls_q,
                                                          const std::vector<Type>& args);
    /// Method instances created since the last call, for lowering.
    [[nodiscard]] std::vector<const MethodInstance*> takePendingMethods();
    /// Marks which instance's body is being lowered (empty for other code), so that diagnostics
    /// can name the instantiation chain.
    void setCurrentInstance(const Item* item);
    /// Names of the type parameters of a `fn`, `struct` or `class` (empty when not generic).
    [[nodiscard]] std::vector<std::string> genericParamsOf(const Item& item) const;
    /// `fn f(a: T, ..args)`: a function with an untyped trailing parameter pack. It has no code of
    /// its own; each call instantiates a copy for the pack's argument types (see variadic.cpp).
    [[nodiscard]] static bool isPackFn(const Item& item);
    /// Reports E0734 to E0736 for a pack function's declaration and E0735, E0737 and E0738 for
    /// the uses of its pack in the body. The body itself is checked per instance.
    void checkPackFn(const Item& item);
    /// E0736 when a method (of a class, impl or interface) takes a parameter pack.
    void checkNoPackMethod(const FnDecl& method);
    /// The instance of pack function `generic` (named `local`) for pack elements of types
    /// `elements`; cached like `instantiate`.
    [[nodiscard]] std::optional<EntityRef> instantiatePack(const Item& generic,
                                                           const std::string& local,
                                                           const std::vector<Type>& elements,
                                                           const Span& at);
    /// By qualified name (as stored in resolved types).
    [[nodiscard]] std::optional<EntityRef> entityByQName(const std::string& qname);
    [[nodiscard]] const Symbol* findLocal(const std::string& name) const { return find(name); }

  private:
    friend class lower::Lowerer;
    friend class Compilation;

    void collect();
    void checkType(const Type& type, const Span& at);
    void checkStatements(const Block& block);
    void checkExpr(const Expr& expr);
    void checkEnum(const EnumDecl& decl);
    void checkAggregate(const Item& item);
    void checkStaticAssert(const TopLevelCallDecl& call);
    void checkStaticAssertCall(const Expr& call);
    /// The generic payload enum whose generated item `name` is (`Result$Ok` and `Result$Data`
    /// both belong to `enum Result<T, E>`); `some_decl` itself when it is not such a part.
    [[nodiscard]] const Item& genericEnumOf(const std::string& name) const;

    [[nodiscard]] const Symbol* find(const std::string& name) const;

    /// Link name of a function or global: the plain name for `export(c)`, `@mangle("x")`,
    /// `declare`, and `--mangle=none` (an overloaded name is never plain), otherwise
    /// `_xl$<module>$<name>$<mangled parameter types>`.
    [[nodiscard]] std::string linkName(const std::vector<Attribute>& attrs,
                                       bool export_c,
                                       bool declared,
                                       const std::string& local_name,
                                       const std::vector<Type>& params,
                                       bool overloaded) const;
    [[nodiscard]] std::optional<Type> exprToType(const Expr& expr);
    [[nodiscard]] std::optional<EntityRef> entityOfExpr(const Expr& expr);
    [[nodiscard]] std::optional<ConstValue> evalBinary(const Expr& expr);
    [[nodiscard]] std::optional<ConstValue> evalCast(const Expr& expr);
    [[nodiscard]] std::optional<ConstValue> evalCall(const Expr& expr);
    [[nodiscard]] std::optional<ConstValue> fromConfig(const ConfigValue& v);
    [[nodiscard]] std::optional<std::uint64_t> primitiveSize(const Type& t, std::uint64_t& align);
    [[nodiscard]] std::optional<TypeLayout>
    aggregateLayout(const std::string& name, const Item& item, const Span& at);
    void notConstant(const Span& at, std::string why);
    void error(const char* code, const Span& at, std::string message, std::string label);
    /// Notes for the instantiation chain of the instance being lowered (at most 8 levels).
    [[nodiscard]] std::vector<Note> instantiationNotes() const;

    const Program& program_;
    const BuildConfig& config_;
    DiagnosticEngine& diag_;
    const LayoutHistory* history_;

    Item memory_order_item_;
    std::map<std::string, Symbol> symbols_;
    std::vector<std::string> aggregate_order_;

    // Generic instances (core-language phase 6). Instances of this module's generic declarations
    // are stored here; `pending_instances_` are the ones not lowered yet.
    std::deque<Item> instances_;
    /// Per pack function and element count: the copy of the function that `instantiatePack` makes
    /// instances from. The item and symbol keep their addresses while the map lives.
    struct PackTemplate {
        Item item;
        Symbol sym;
    };
    std::map<std::pair<const Item*, std::size_t>, std::unique_ptr<PackTemplate>> pack_templates_;
    [[nodiscard]] const PackTemplate& packTemplate(const Item& generic, std::size_t count);
    std::map<std::string, InstanceInfo> instance_info_; ///< instance local name -> origin
    /// Parts of a payload enum written with the declaration's arguments (`Result$Ok<T, E>`,
    /// which reads `Result<T, E>$Ok`): they have no symbol of their own, so the enum they belong
    /// to answers. The entries keep their addresses while the map lives.
    std::map<std::string, EntityRef> instance_aliases_;
    std::map<const Item*, std::string> item_instance_;  ///< instance item -> its local name
    std::string current_instance_; ///< local name of the instance whose body is lowered
    std::vector<const Item*> pending_instances_;
    std::deque<MethodInstance> method_instances_; ///< stable addresses for the lowering queue
    std::map<std::string, const MethodInstance*> method_index_; ///< link name -> instance
    std::vector<const MethodInstance*> pending_methods_;
    std::map<std::string, std::optional<ConstValue>> define_cache_;
    std::set<std::string> define_in_progress_;
    std::set<std::string> alias_in_progress_;
    std::map<std::string, std::optional<TypeLayout>> layout_cache_;
    std::set<std::string> layout_in_progress_;
    std::map<std::string, std::vector<std::optional<ConstValue>>> enum_values_;
    std::set<std::string> enum_in_progress_;
    std::set<std::string> reported_; ///< dedup key for repeated diagnostics
    Compilation* comp_{nullptr};
    std::string key_;
    std::map<std::string, ImportBinding> imports_;
};

} // namespace xlang
