#pragma once

#include "xlang/hir/hir.h"

#include <string>

namespace xlang::mir {

/// The MIR of a program: the control-flow graph of each function, one body per HIR function that
/// has a body, in HIR order. The graph is an xlang value (src/mir/model.xl, built by
/// src/mir/builder); this class owns it and frees it. The C++ side reads it only through `dump`.
///
/// @invariant the MIR never refers back to the HIR it was built from.
class Module {
public:
    Module() = default;
    /// Takes ownership of an xlang module.
    explicit Module(void* handle) noexcept : handle_(handle) {}
    ~Module();
    Module(Module&& other) noexcept;
    Module& operator=(Module&& other) noexcept;
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;

    /// The xlang module behind this one, for the analyses.
    [[nodiscard]] void* handle() const noexcept { return handle_; }

    /// The `--dump-mir` text of every body, in order.
    [[nodiscard]] std::string dump() const;

private:
    void* handle_{nullptr};
};

/// Builds the MIR of every HIR function that has a body, in HIR order.
[[nodiscard]] Module build(const hir::Module& module);

} // namespace xlang::mir
