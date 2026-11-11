#include "sema/compilation/compilation.hpp"

#include <format>
#include <map>

namespace xlang {

// `register(role)` names of the whole compilation. A program registration replaces a library one of
// the same name; a library one never replaces the program's; two registrations in the same group are
// `E0502`.

void Compilation::collectRegistered() {
    std::map<std::string, bool> from_library;
    for (ModuleInfo* m : order_) {
        for (const Item& item : m->program.items) {
            if (!item.modifiers.registered) {
                continue;
            }
            const DeclBase& base = declBase(item);
            const Sema::Symbol* sym = m->sema->findLocal(base.name);
            if (sym == nullptr) {
                continue;
            }
            const auto [it, inserted] =
                registered_.emplace(base.name, Sema::EntityRef{m->sema.get(), sym, base.name});
            bool& existing_library = from_library[base.name];
            if (inserted) {
                existing_library = m->is_library;
                continue;
            }
            if (existing_library != m->is_library) {
                if (!m->is_library) {
                    it->second = Sema::EntityRef{m->sema.get(), sym, base.name};
                    existing_library = false;
                }
                continue;
            }
            Diagnostic d;
            d.code = "E0502";
            d.severity = Severity::Error;
            d.message = std::format("`{}` is registered twice", base.name);
            d.labels.push_back({declBase(*it->second.sym->item).name_span, "first registration"});
            d.labels.push_back({base.name_span, "second registration"});
            d.fix = "register the name once per group: remove one registration, or, if a library "
                    "already registers it, register the name from your own code instead";
            diag_.report(std::move(d));
        }
    }
}

}  // namespace xlang
