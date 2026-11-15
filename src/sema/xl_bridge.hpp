#pragma once

// The xlang lowering pass (src/lower/program.xl) as seen from C++: the sema and the lowering of
// one module, run on the frozen flat AST of the frontend (include/xlang/lang/frontend.h). The
// C ABI is at the top; `xlang::XlLowered` owns one session and its parse. Only this bridge and
// the unit test that compares the two lowerings use it: the C++ pass (src/sema/lower) stays the
// reference until the comparison is equal for every sample.

#include <cstdint>
#include <memory>
#include <string>

extern "C" {

/// The text `xlcLowerDump` returns: `len` bytes at `bytes`, owned by `block`.
struct XlLowerText {
    const std::uint8_t* bytes;
    std::uint32_t len;
    void* block;
};

/// Lowers the module whose frozen parse is `parse` (an `XlcParseResult`); diagnostics go to the
/// xlang engine `diag`. Returns a session that `xlcLowerFree` releases; the parse must outlive it.
void* xlcLowerRun(const void* parse, void* diag);
/// The text form of the lowered module, the same form `hir::dumpModule` writes.
XlLowerText xlcLowerDump(void* session);
/// Releases the text of `xlcLowerDump`.
void xlcLowerDumpFree(XlLowerText text);
/// Releases a session of `xlcLowerRun`.
void xlcLowerFree(void* session);
}

#include "xlang/lang/config.h"
#include "xlang/lang/frontend.h"
#include "xlang/util/diagnostics.h"
#include "xlang/util/source.h"

namespace xlang {

/// The flat HIR the xlang lowering pass built for one file. Movable, not copyable; the session and
/// the frozen parse it reads are released together with it.
class XlLowered {
  public:
    /// Parses `file` with the xlang frontend and lowers it. Syntax errors are not reported here
    /// (the C++ parse reports them); the xlang sema reports its own errors to `diagnostics`.
    [[nodiscard]] static XlLowered lower(const SourceManager& sources, FileId file,
                                         const BuildConfig& config,
                                         DiagnosticEngine& diagnostics);

    XlLowered(XlLowered&&) noexcept;
    XlLowered& operator=(XlLowered&&) noexcept;
    XlLowered(const XlLowered&) = delete;
    XlLowered& operator=(const XlLowered&) = delete;
    ~XlLowered();

    /// The text form of the module: the same text `hir::dumpModule` writes for the C++ pass.
    [[nodiscard]] std::string dump() const;

  private:
    struct Impl;
    explicit XlLowered(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace xlang
