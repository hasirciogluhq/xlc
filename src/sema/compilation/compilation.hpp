#pragma once

#include "xlang/sema/compilation.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace xlang {
class Sema;
}  // namespace xlang

// Internal helpers shared by the parts of the `Compilation` driver (module loading, exports,
// imports). The parts define members declared in `xlang/sema/compilation.h`; this header only
// carries the free helpers and the include set every part needs.

namespace xlang::compilation_detail {

/// Filesystem key of a path: lexically normal, `/`-separated. Used as the identity of a module
/// file in `Compilation::by_path_` and of a virtual file in `Compilation::virtual_files_`.
[[nodiscard]] inline std::string normalizedKey(const std::filesystem::path& p) {
    return p.lexically_normal().generic_string();
}

/// Last `/`-separated path of a module spec (`std/mem` -> `mem`): the default binding name of
/// `import "std/mem"`.
[[nodiscard]] inline std::string lastSegment(const std::string& spec) {
    const std::size_t slash = spec.find_last_of('/');
    return slash == std::string::npos ? spec : spec.substr(slash + 1);
}

/// Appends `m` after the modules of its own group it imports (depth-first, each once). Drives the
/// `@init` order: the library group runs before the program group, imports before importers.
inline void initPostOrder(ModuleInfo* m, std::set<const ModuleInfo*>& done, std::vector<Sema*>& out) {
    if (!done.insert(m).second) {
        return;
    }
    for (ModuleInfo* dep : m->deps) {
        if (dep->is_library == m->is_library) {
            initPostOrder(dep, done, out);
        }
    }
    out.push_back(m->sema.get());
}

}  // namespace xlang::compilation_detail
