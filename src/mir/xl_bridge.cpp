#include "xlang/mir/build.h"

#include "mir/xl_bridge.hpp"

#include <string>
#include <utility>

namespace xlang::mir {

Module::~Module() {
    if (handle_ != nullptr) {
        xlcMirFree(handle_);
    }
}

Module::Module(Module&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

Module& Module::operator=(Module&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            xlcMirFree(handle_);
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

std::string Module::dump() const {
    const XlcMirText text = xlcMirDump(handle_);
    std::string out(reinterpret_cast<const char*>(text.bytes), text.len);
    xlcMirDumpFree(text);
    return out;
}

Module build(const hir::Module& module) {
    // The flatten only reads the C++ tree; the C ABI takes a plain pointer.
    return Module(xlcMirBuildSession(&module));
}

} // namespace xlang::mir
