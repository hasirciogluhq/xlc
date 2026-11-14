#include "xlang/mir/build.h"

#include "mir/builder/body.hpp"

namespace xlang::mir {

Module build(const hir::Module& module) {
    Module out;
    for (const hir::Function& f : module.functions) {
        if (f.has_body) {
            out.bodies.push_back(builder::BodyBuilder(f).finish());
        }
    }
    return out;
}

} // namespace xlang::mir
