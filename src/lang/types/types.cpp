#include "xlang/lang/types.h"

#include "xlang/lang/ast.h"

#include <algorithm>
#include <format>

namespace xlang {

Type Type::makeVoid() {
    return Type{};
}

Type Type::makeBool() {
    Type t;
    t.kind = TypeKind::Bool;
    return t;
}

Type Type::makeInt(unsigned bits, bool is_signed) {
    Type t;
    t.kind = TypeKind::Int;
    t.bits = bits;
    t.is_signed = is_signed;
    return t;
}

Type Type::makeFloat(unsigned bits) {
    Type t;
    t.kind = TypeKind::Float;
    t.bits = bits;
    t.is_signed = true;  // floats count as signed (E0601 rules)
    return t;
}

Type Type::makePointer(Type pointee) {
    Type t;
    t.kind = TypeKind::Pointer;
    t.elem = std::make_shared<const Type>(std::move(pointee));
    return t;
}

Type Type::makeFnPtr(std::vector<Type> params, Type ret) {
    Type t;
    t.kind = TypeKind::FnPtr;
    t.params = std::move(params);
    t.ret = std::make_shared<const Type>(std::move(ret));
    return t;
}

Type Type::makeArray(Type element, std::shared_ptr<const Expr> length) {
    Type t;
    t.kind = TypeKind::Array;
    t.elem = std::make_shared<const Type>(std::move(element));
    t.length = std::move(length);
    return t;
}

Type Type::makeNamed(std::vector<std::string> path) {
    Type t;
    t.kind = TypeKind::Named;
    t.path = std::move(path);
    return t;
}

Type Type::makeStr() {
    Type t;
    t.kind = TypeKind::Str;
    return t;
}

Type Type::makeSlice(Type element, bool is_mutable) {
    Type t;
    t.kind = TypeKind::Slice;
    t.elem = std::make_shared<const Type>(std::move(element));
    t.is_mutable = is_mutable;
    return t;
}

Type Type::makeTuple(std::vector<Type> elements) {
    Type t;
    t.kind = TypeKind::Tuple;
    t.args = std::move(elements);
    return t;
}

std::optional<Type> primitiveFromName(std::string_view name) {
    struct Entry {
        std::string_view name;
        TypeKind kind;
        unsigned bits;
        bool is_signed;
    };
    static constexpr Entry table[] = {
        {"i8", TypeKind::Int, 8, true},     {"i16", TypeKind::Int, 16, true},
        {"i32", TypeKind::Int, 32, true},   {"i64", TypeKind::Int, 64, true},
        {"isize", TypeKind::Int, 0, true},  {"u8", TypeKind::Int, 8, false},
        {"u16", TypeKind::Int, 16, false}, {"u32", TypeKind::Int, 32, false},
        {"u64", TypeKind::Int, 64, false}, {"usize", TypeKind::Int, 0, false},
        {"f32", TypeKind::Float, 32, true}, {"f64", TypeKind::Float, 64, true},
        {"bool", TypeKind::Bool, 0, false}, {"void", TypeKind::Void, 0, false},
        {"str", TypeKind::Str, 0, false},
    };
    for (const Entry& e : table) {
        if (e.name != name) {
            continue;
        }
        switch (e.kind) {
        case TypeKind::Int: return Type::makeInt(e.bits, e.is_signed);
        case TypeKind::Float: return Type::makeFloat(e.bits);
        case TypeKind::Bool: return Type::makeBool();
        case TypeKind::Str: return Type::makeStr();
        default: return Type::makeVoid();
        }
    }
    return std::nullopt;
}

bool isPrimitiveTypeName(std::string_view name) {
    return primitiveFromName(name).has_value();
}

namespace {

[[nodiscard]] std::string joinPath(const std::vector<std::string>& path) {
    std::string out;
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (i != 0) {
            out += '.';
        }
        out += path[i];
    }
    return out;
}

[[nodiscard]] std::string lengthToString(const Expr* length) {
    if (length == nullptr) {
        return "?";
    }
    if (length->kind == Expr::Kind::IntLiteral) {
        return std::to_string(length->int_value);
    }
    if (length->kind == Expr::Kind::Name) {
        return length->text;
    }
    return "<const expr>";
}

}  // namespace

std::string typeToString(const Type& type) {
    switch (type.kind) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "bool";
    case TypeKind::Str: return "str";
    case TypeKind::Slice:
        return std::format("[]{}{}", type.is_mutable ? "mut " : "",
                           type.elem ? typeToString(*type.elem) : "?");
    case TypeKind::Int:
        if (type.bits == 0) {
            return type.is_signed ? "isize" : "usize";
        }
        return std::format("{}{}", type.is_signed ? "i" : "u", type.bits);
    case TypeKind::Float: return std::format("f{}", type.bits);
    case TypeKind::Pointer: return "*" + (type.elem ? typeToString(*type.elem) : "?");
    case TypeKind::FnPtr: {
        std::string out = "fn(";
        for (std::size_t i = 0; i < type.params.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += typeToString(type.params[i]);
        }
        out += ")";
        if (type.ret && !type.ret->isVoid()) {
            out += " -> " + typeToString(*type.ret);
        }
        return out;
    }
    case TypeKind::Array:
        return std::format("[{}; {}]", type.elem ? typeToString(*type.elem) : "?",
                           lengthToString(type.length.get()));
    case TypeKind::Tuple: {
        // `()` is the empty tuple; a one-element tuple keeps its trailing comma so that it is not
        // read as a parenthesized type.
        std::string out = "(";
        for (std::size_t i = 0; i < type.args.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += typeToString(type.args[i]);
        }
        if (type.args.size() == 1) {
            out += ',';
        }
        return out + ")";
    }
    case TypeKind::Enum:
    case TypeKind::Struct:
    case TypeKind::Class:
    case TypeKind::Interface:
    case TypeKind::Named: return joinPath(type.path);
    }
    return "?";
}

std::string mangleType(const Type& type) {
    switch (type.kind) {
    case TypeKind::Void: return "void";
    case TypeKind::Bool: return "bool";
    case TypeKind::Str: return "str";
    case TypeKind::Slice:
        // `[]T` is `S<T>`, `[]mut T` is `M<T>`: distinct link names, the same element mangling.
        return (type.is_mutable ? "M" : "S") +
               (type.elem ? mangleType(*type.elem) : std::string("void"));
    case TypeKind::Int:
        if (type.bits == 0) {
            return type.is_signed ? "isize" : "usize";
        }
        return std::format("{}{}", type.is_signed ? "i" : "u", type.bits);
    case TypeKind::Float: return std::format("f{}", type.bits);
    case TypeKind::Pointer: return "P" + (type.elem ? mangleType(*type.elem) : "void");
    case TypeKind::FnPtr: {
        std::string out = "F";
        for (const Type& p : type.params) {
            out += mangleType(p);
        }
        out += "R" + (type.ret ? mangleType(*type.ret) : "void") + "E";
        return out;
    }
    case TypeKind::Array: {
        const std::uint64_t len =
            type.length && type.length->kind == Expr::Kind::IntLiteral ? type.length->int_value : 0;
        return std::format("A{}_", len) + (type.elem ? mangleType(*type.elem) : "void");
    }
    case TypeKind::Tuple: {
        std::string out = "T";
        for (const Type& a : type.args) {
            out += mangleType(a);
        }
        return out + "E";
    }
    case TypeKind::Enum:
    case TypeKind::Struct:
    case TypeKind::Class:
    case TypeKind::Interface:
    case TypeKind::Named: {
        // A generic instance is named `Base<args>` in its path; its mangled name is the base name
        // followed by `G<args>E`.
        std::string name = joinPath(type.path);
        std::string generic;
        if (const auto open = name.find('<'); open != std::string::npos) {
            name.resize(open);
        }
        for (std::size_t p = name.find("::"); p != std::string::npos; p = name.find("::", p)) {
            name.replace(p, 2, "_");
        }
        std::replace(name.begin(), name.end(), '.', '_');
        std::replace(name.begin(), name.end(), '/', '_');
        if (!type.args.empty()) {
            generic = "G";
            for (const Type& a : type.args) {
                generic += mangleType(a);
            }
            generic += "E";
        }
        return std::format("N{}{}{}", name.size(), name, generic);
    }
    }
    return "void";
}

}  // namespace xlang
