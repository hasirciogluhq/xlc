// Lowering: which types C can express (`export(c)`, `declare`).

#include "sema/lower/lower.hpp"

#include <utility>

namespace xlang::lower {

// ---------------------------------------------------------------------------------------------
// Items

bool Lowerer::cCompatible(Sema& sema, const Type& t) {
    switch (t.kind) {
    case TypeKind::Void:
    case TypeKind::Never: // C11 `_Noreturn void`
    case TypeKind::Bool:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::FnPtr:
    case TypeKind::Pointer:
        return true;
    case TypeKind::Enum:
        return t.elem && t.elem->kind == TypeKind::Int;
    case TypeKind::Struct: {
        const auto ref = sema.entityByQName(t.path.front());
        if (!ref) {
            return false;
        }
        const Attribute* repr = lower::findAttribute(declBase(*ref->sym->item).attributes, "repr");
        return repr != nullptr &&
               std::ranges::any_of(repr->args, [](const AttributeArg& a) { return a.text == "c"; });
    }
    default:
        return false;
    }
}

void Lowerer::assignSymbol(const std::vector<Attribute>& attrs,
                           bool export_c,
                           bool declared,
                           const std::string& local_name,
                           const std::vector<Type>& params,
                           bool overloaded,
                           std::string& symbol,
                           const Span& at) {
    if (!overloaded && !export_c) {
        if (const Attribute* m = lower::findAttribute(attrs, "mangle");
            m != nullptr && !m->args.empty() && m->args[0].kind == AttributeArg::Kind::Ident &&
            m->args[0].text != "none") {
            err("E0705", m->args[0].span, "`@mangle` takes `none` or a string", "invalid argument");
        }
    }
    symbol = s_.linkName(attrs, export_c, declared, local_name, params, overloaded);
    if (symbol == "_start" || symbol == "mainCRTStartup" || symbol == "_xl_initAll") {
        err("E0726",
            at,
            std::format("symbol `{}` is reserved for the compiler's entry point", symbol),
            "rename it: the compiler generates this symbol to start the program");
    }
    const auto [it, inserted] = sh_.link_names.emplace(symbol, at);
    // Plain names must be unique; mangled names are unique by their parameter types.
    if (!inserted && !declared && symbol.find("_xl$") != 0) {
        Diagnostic d;
        d.code = "E0401";
        d.severity = Severity::Error;
        d.message = std::format("symbol `{}` is defined twice", symbol);
        d.labels.push_back({it->second, "first definition"});
        d.labels.push_back({at, "second definition"});
        d.fix = "enable name mangling or rename one with `@mangle(\"...\")`";
        s_.diag_.report(std::move(d));
    }
}

} // namespace xlang::lower
