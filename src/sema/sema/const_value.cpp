#include "sema/sema/sema.hpp"

#include <format>

namespace xlang {

// `ConstValue`: the compile-time value of a literal, a `define` constant, a `-D` flag, an enum
// member or a layout query. The evaluation itself lives in `const_eval.cpp`.

using sema_detail::int128ToString;

ConstValue ConstValue::makeInt(Int128 v) {
    ConstValue c;
    c.kind = Kind::Int;
    c.int_value = v;
    return c;
}

ConstValue ConstValue::makeFloat(double v) {
    ConstValue c;
    c.kind = Kind::Float;
    c.float_value = v;
    return c;
}

ConstValue ConstValue::makeBool(bool v) {
    ConstValue c;
    c.kind = Kind::Bool;
    c.bool_value = v;
    return c;
}

ConstValue ConstValue::makeString(std::string v) {
    ConstValue c;
    c.kind = Kind::String;
    c.string_value = std::move(v);
    return c;
}

std::string ConstValue::toString() const {
    switch (kind) {
    case Kind::Int:
        return int128ToString(int_value);
    case Kind::Float:
        return std::format("{}", float_value);
    case Kind::Bool:
        return bool_value ? "true" : "false";
    case Kind::String:
        return "\"" + string_value + "\"";
    }
    return {};
}

}  // namespace xlang
