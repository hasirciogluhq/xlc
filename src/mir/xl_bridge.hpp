// The C ABI of the MIR builder written in xlang (src/mir/builder/module.xl). The builder takes the
// session's `hir::Module` (flattened by src/hir/flatten.xl) and returns an xlang MIR module. Only
// src/mir/xl_bridge.cpp calls these; the rest of the C++ side uses `xlang::mir::Module`
// (include/xlang/mir/build.h).
#pragma once

#include <cstdint>

extern "C" {

/// The text `xlcMirDump` returns: `len` bytes at `bytes`, owned by `block`.
struct XlcMirText {
    const std::uint8_t* bytes;
    std::uint32_t len;
    void* block;
};

/// The MIR of the C++ `hir::Module` at `raw`.
void* xlcMirBuildSession(const void* raw);
/// Releases a module from `xlcMirBuildSession`.
void xlcMirFree(void* module);
/// The text form of a module.
XlcMirText xlcMirDump(void* module);
/// Releases the text of `xlcMirDump`.
void xlcMirDumpFree(XlcMirText text);
}
