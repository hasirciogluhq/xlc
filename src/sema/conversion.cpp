#include "xlang/sema/conversion.h"

namespace xlang {

namespace {

[[nodiscard]] unsigned widthOf(const Type& t, unsigned pointer_bits) {
    return t.bits == 0 ? pointer_bits : t.bits;
}

[[nodiscard]] bool sameShape(const Type& a, const Type& b) {
    if (a.kind != b.kind) {
        return false;
    }
    switch (a.kind) {
    case TypeKind::Int:
        return a.bits == b.bits && a.is_signed == b.is_signed;
    case TypeKind::Float:
        return a.bits == b.bits;
    case TypeKind::Void:
    case TypeKind::Bool:
        return true;
    default:
        return typeToString(a) == typeToString(b);
    }
}

} // namespace

ConversionResult
implicitConversion(const Type& from, const Type& to, bool in_unsafe, unsigned pointer_bits) {
    ConversionResult r;
    if (sameShape(from, to)) {
        r.status = ConversionStatus::Ok;
        return r;
    }

    const bool from_num = from.kind == TypeKind::Int || from.kind == TypeKind::Float;
    const bool to_num = to.kind == TypeKind::Int || to.kind == TypeKind::Float;
    if (!from_num || !to_num) {
        // bool, pointers, enums, aggregates: only an explicit `as` can convert (when anything can).
        const bool castable =
            (from_num || from.kind == TypeKind::Bool || from.isPointer() ||
             from.kind == TypeKind::Enum) &&
            (to_num || to.kind == TypeKind::Bool || to.isPointer() || to.kind == TypeKind::Enum);
        r.status = castable ? ConversionStatus::NeedsCast : ConversionStatus::Invalid;
        return r;
    }

    const unsigned fw = from.kind == TypeKind::Int ? widthOf(from, pointer_bits) : from.bits;
    const unsigned tw = to.kind == TypeKind::Int ? widthOf(to, pointer_bits) : to.bits;

    if (from.kind == TypeKind::Int && to.kind == TypeKind::Int) {
        if (from.is_signed == to.is_signed) {
            r.status = tw >= fw ? ConversionStatus::Ok : ConversionStatus::NeedsCast;
            return r;
        }
        // Signedness changes: E0601, accepted in `unsafe` only for the same or a wider target.
        if (tw < fw) {
            r.status = ConversionStatus::NeedsCast;
            r.note = "narrowing needs `as`, even inside `unsafe`";
            return r;
        }
        if (in_unsafe) {
            r.status = ConversionStatus::Ok;
            r.used_unsafe = true;
            return r;
        }
        r.status = ConversionStatus::Signedness;
        return r;
    }

    if (from.kind == TypeKind::Int && to.kind == TypeKind::Float) {
        if (from.is_signed) {
            r.status = ConversionStatus::Warn; // W0601
            return r;
        }
        // Floats count as signed: `ui -> f` is E0601; `unsafe` accepts it (W0601 still applies).
        if (in_unsafe) {
            r.status = ConversionStatus::Warn;
            r.used_unsafe = true;
            return r;
        }
        r.status = ConversionStatus::Signedness;
        return r;
    }

    if (from.kind == TypeKind::Float && to.kind == TypeKind::Float) {
        r.status = tw >= fw ? ConversionStatus::Ok : ConversionStatus::NeedsCast;
        return r;
    }

    r.status = ConversionStatus::NeedsCast; // float -> int
    return r;
}

CommonTypeResult commonBinaryType(const Type& a, const Type& b, unsigned pointer_bits) {
    CommonTypeResult r;
    const bool a_num = a.kind == TypeKind::Int || a.kind == TypeKind::Float;
    const bool b_num = b.kind == TypeKind::Int || b.kind == TypeKind::Float;
    if (!a_num || !b_num) {
        if (sameShape(a, b)) {
            r.status = ConversionStatus::Ok;
            r.type = a;
        }
        return r;
    }
    if (a.kind == TypeKind::Int && b.kind == TypeKind::Int) {
        if (a.is_signed != b.is_signed) {
            r.status = ConversionStatus::Signedness;
            return r;
        }
        r.status = ConversionStatus::Ok;
        r.type = widthOf(a, pointer_bits) >= widthOf(b, pointer_bits) ? a : b;
        return r;
    }
    if (a.kind == TypeKind::Float && b.kind == TypeKind::Float) {
        r.status = ConversionStatus::Ok;
        r.type = a.bits >= b.bits ? a : b;
        return r;
    }
    // Integer with float: the integer converts to the float type.
    const Type& integer = a.kind == TypeKind::Int ? a : b;
    const Type& real = a.kind == TypeKind::Float ? a : b;
    if (!integer.is_signed) {
        r.status = ConversionStatus::Signedness;
        return r;
    }
    r.status = ConversionStatus::Ok;
    r.type = real;
    r.warn = true;
    return r;
}

} // namespace xlang
