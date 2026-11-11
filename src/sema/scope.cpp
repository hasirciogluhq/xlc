#include "xlang/sema/scope.h"

namespace xlang {

void ScopeStack::push(ScopeKind kind) {
    scopes_.push_back(Scope{kind, {}});
}

void ScopeStack::pop() {
    scopes_.pop_back();
}

const ScopeEntry* ScopeStack::declare(ScopeEntry entry) {
    Scope& scope = scopes_.back();
    const auto it = scope.names.find(entry.name);
    if (it != scope.names.end()) {
        return &it->second;
    }
    const std::string key = entry.name;
    scope.names.emplace(key, std::move(entry));
    return nullptr;
}

const ScopeEntry* ScopeStack::lookup(std::string_view name) const {
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        if (const auto found = it->names.find(name); found != it->names.end()) {
            return &found->second;
        }
    }
    return nullptr;
}

const ScopeEntry* ScopeStack::lookupModule(std::string_view name) const {
    if (scopes_.empty()) {
        return nullptr;
    }
    const auto found = scopes_.front().names.find(name);
    return found == scopes_.front().names.end() ? nullptr : &found->second;
}

} // namespace xlang
