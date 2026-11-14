// Plan 5.8: the in-process archiver. The linker driver is xlang code (src/driver/link.xl); the
// archive stays here because it is LLVM's writer (llvm/Object/ArchiveWriter.h).

#include "xlang/codegen/archive.h"

#include "xlang/util/error.h"

#include <llvm/Object/ArchiveWriter.h>
#include <llvm/Support/Error.h>

namespace xlang::codegen {

void archive(const std::vector<std::filesystem::path>& objects, const std::filesystem::path& output,
             const TargetInfo& target) {
    std::vector<llvm::NewArchiveMember> members;
    for (const std::filesystem::path& o : objects) {
        auto m = llvm::NewArchiveMember::getFile(o.string(), true);
        if (!m) {
            throw XlangError("cannot read `" + o.string() + "`: " + llvm::toString(m.takeError()));
        }
        members.push_back(std::move(*m));
    }
    // ld64 wants the BSD (Darwin) member table; every ELF and COFF linker reads the GNU one.
    const auto kind = target.os == "macos" ? llvm::object::Archive::K_DARWIN : llvm::object::Archive::K_GNU;
    if (llvm::Error e = llvm::writeArchive(output.string(), members, llvm::SymtabWritingMode::NormalSymtab, kind, true,
                                           false)) {
        throw XlangError("cannot write archive: " + llvm::toString(std::move(e)));
    }
}

}  // namespace xlang::codegen
