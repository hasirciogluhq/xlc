#pragma once

#include "xlang/lang/ast.h"

#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>

namespace xlang {

class Sema;

enum class ScopeKind { Module, Function, Block };

/// What a name is bound to (plan 4.1). `Namespace` is `import "m" as a` / `import * as a`;
/// `External` is a name that arrives from a module that is not loaded yet (plan 4.6): its type is
/// unknown and uses of it are typed as unknown without diagnostics.
struct ScopeEntry {
    enum class Kind { Local, Global, Function, Constant, Type, Namespace, External } kind{Kind::Local};
    std::string name;
    Type type;                       ///< Local/Global: the value type
    Span span;                       ///< where the name was bound
    std::uint32_t local{0};          ///< Local: index into the function's locals
    const Item* item{nullptr};       ///< Global/Function/Constant/Type: the declaration
    std::string module;              ///< Namespace: module key; External: module path as written
    Sema* owner{nullptr};            ///< module that owns the declaration (item)
    std::string qname;               ///< qualified name of the entity
    std::string member;              ///< External: name inside `module` (empty for a whole module)
    bool readonly{false};            ///< Global: a `const` (its places cannot be written)
    std::vector<const Item*> overloads;  ///< Function: every overload of the name (else empty)
};

/// Lexical scopes: module -> function -> nested blocks. Lookup walks from the innermost scope
/// outwards; a name can be bound once per scope, inner scopes shadow outer ones.
class ScopeStack {
public:
    void push(ScopeKind kind);
    void pop();

    /// Binds `entry` in the innermost scope. Returns the earlier binding of the same name in that
    /// scope (and binds nothing) when there is one, nullptr on success.
    [[nodiscard]] const ScopeEntry* declare(ScopeEntry entry);

    [[nodiscard]] const ScopeEntry* lookup(std::string_view name) const;
    /// Lookup restricted to the module scope (the outermost one).
    [[nodiscard]] const ScopeEntry* lookupModule(std::string_view name) const;

    [[nodiscard]] std::size_t depth() const { return scopes_.size(); }
    [[nodiscard]] ScopeKind innermost() const { return scopes_.back().kind; }

private:
    struct Scope {
        ScopeKind kind;
        std::map<std::string, ScopeEntry, std::less<>> names;
    };
    std::deque<Scope> scopes_;
};

}  // namespace xlang
