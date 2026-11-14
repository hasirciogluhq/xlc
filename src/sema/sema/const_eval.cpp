#include "sema/sema/sema.hpp"

#include "xlang/sema/compilation.h"

#include <algorithm>
#include <format>
#include <functional>
#include <numeric>
#include <random>

namespace xlang {

using sema_detail::wrapInt;

// Compile-time evaluation (plan 3.6): literals, `-D` values, `define` constants, enum
// members, operators, casts and the `sizeOf`/`alignOf`/`offsetOf` queries.


// ---------------------------------------------------------------------------------------------
// Constant evaluation

std::optional<ConstValue> Sema::fromConfig(const ConfigValue& v) {
    switch (v.kind) {
    case ConfigValue::Kind::Bool:
        return ConstValue::makeBool(v.bool_value);
    case ConfigValue::Kind::Int:
        return ConstValue::makeInt(v.int_value);
    case ConfigValue::Kind::String:
        return ConstValue::makeString(v.string_value);
    }
    return std::nullopt;
}

std::optional<ConstValue> Sema::evalConst(const Expr& expr) {
    switch (expr.kind) {
    case Expr::Kind::IntLiteral:
        return ConstValue::makeInt(static_cast<Int128>(expr.int_value));
    case Expr::Kind::FloatLiteral:
        return ConstValue::makeFloat(expr.float_value);
    case Expr::Kind::BoolLiteral:
        return ConstValue::makeBool(expr.bool_value);
    case Expr::Kind::StringLiteral:
        return ConstValue::makeString(expr.text);
    case Expr::Kind::ByteLiteral:
        return ConstValue::makeInt(static_cast<Int128>(expr.int_value));
    case Expr::Kind::Name: {
        // `-D NAME=...` and builtins (`os`, `pointerWidth`, ...) come first: CLI wins over
        // `define`.
        if (const auto cfg = config_.lookup(expr.text)) {
            return fromConfig(*cfg);
        }
        const Symbol* sym = find(expr.text);
        if (sym == nullptr) {
            if (const auto ref = lookupEntity(expr.text);
                ref && ref->owner != this && ref->sym->kind == Symbol::Kind::Define) {
                Expr remote;
                remote.kind = Expr::Kind::Name;
                remote.text = ref->local;
                remote.span = expr.span;
                return ref->owner->evalConst(remote);
            }
        }
        if (sym == nullptr || sym->kind != Symbol::Kind::Define) {
            notConstant(expr.span, std::format("`{}` is not a `define` constant", expr.text));
            return std::nullopt;
        }
        if (const auto it = define_cache_.find(expr.text); it != define_cache_.end()) {
            return it->second;
        }
        if (!define_in_progress_.insert(expr.text).second) {
            error("E0707",
                  expr.span,
                  std::format("`define {}` depends on itself", expr.text),
                  "cyclic constant");
            return std::nullopt;
        }
        const auto& decl = std::get<DefineConstDecl>(sym->item->decl);
        std::optional<ConstValue> value = evalConst(*decl.value);
        define_in_progress_.erase(expr.text);
        define_cache_[expr.text] = value;
        return value;
    }
    case Expr::Kind::Field: {
        if (const auto ref = entityOfExpr(*expr.lhs); ref && ref->sym->kind == Symbol::Kind::Enum) {
            return ref->owner->enumMember(ref->local, expr.text, expr.span);
        }
        // `ns.name` naming a define of another module
        if (expr.lhs->kind == Expr::Kind::Name) {
            if (const auto ref = lookupQualified({expr.lhs->text, expr.text});
                ref && ref->sym->kind == Symbol::Kind::Define) {
                Expr remote;
                remote.kind = Expr::Kind::Name;
                remote.text = ref->local;
                remote.span = expr.span;
                return ref->owner->evalConst(remote);
            }
        }
        notConstant(expr.span, "only enum members can be read as constants");
        return std::nullopt;
    }
    case Expr::Kind::Unary: {
        auto v = evalConst(*expr.lhs);
        if (!v) {
            return std::nullopt;
        }
        if (expr.unary_op == UnaryOp::Neg) {
            if (v->kind == ConstValue::Kind::Int) {
                return ConstValue::makeInt(-v->int_value);
            }
            if (v->kind == ConstValue::Kind::Float) {
                return ConstValue::makeFloat(-v->float_value);
            }
        } else if (expr.unary_op == UnaryOp::BitNot && v->kind == ConstValue::Kind::Int) {
            return ConstValue::makeInt(~v->int_value);
        } else if (expr.unary_op == UnaryOp::Not && v->kind == ConstValue::Kind::Bool) {
            return ConstValue::makeBool(!v->bool_value);
        }
        notConstant(expr.span, "operator does not apply to this constant");
        return std::nullopt;
    }
    case Expr::Kind::Binary:
        return evalBinary(expr);
    case Expr::Kind::Cast:
        return evalCast(expr);
    case Expr::Kind::Call:
        return evalCall(expr);
    default:
        notConstant(expr.span, "this form cannot be evaluated at compile time");
        return std::nullopt;
    }
}

std::optional<ConstValue> Sema::evalBinary(const Expr& expr) {
    auto l = evalConst(*expr.lhs);
    if (!l) {
        return std::nullopt;
    }
    if (expr.binary_op == BinaryOp::And || expr.binary_op == BinaryOp::Or) {
        if (l->kind != ConstValue::Kind::Bool) {
            notConstant(expr.lhs->span, "`&&`/`||` need bool operands");
            return std::nullopt;
        }
        // Short-circuit: the unevaluated side may be non-constant.
        if (expr.binary_op == BinaryOp::And && !l->bool_value) {
            return ConstValue::makeBool(false);
        }
        if (expr.binary_op == BinaryOp::Or && l->bool_value) {
            return ConstValue::makeBool(true);
        }
        auto r = evalConst(*expr.rhs);
        if (!r) {
            return std::nullopt;
        }
        if (r->kind != ConstValue::Kind::Bool) {
            notConstant(expr.rhs->span, "`&&`/`||` need bool operands");
            return std::nullopt;
        }
        return ConstValue::makeBool(r->bool_value);
    }
    auto r = evalConst(*expr.rhs);
    if (!r) {
        return std::nullopt;
    }

    const auto cmpResult = [&](int c) {
        switch (expr.binary_op) {
        case BinaryOp::Eq:
            return c == 0;
        case BinaryOp::Ne:
            return c != 0;
        case BinaryOp::Lt:
            return c < 0;
        case BinaryOp::Le:
            return c <= 0;
        case BinaryOp::Gt:
            return c > 0;
        default:
            return c >= 0;
        }
    };
    const bool is_cmp = expr.binary_op >= BinaryOp::Eq && expr.binary_op <= BinaryOp::Ge;

    if (l->kind == ConstValue::Kind::String || r->kind == ConstValue::Kind::String) {
        if (l->kind == r->kind && is_cmp) {
            return ConstValue::makeBool(cmpResult(l->string_value.compare(r->string_value)));
        }
        notConstant(expr.span, "strings only support comparison with another string");
        return std::nullopt;
    }
    if (l->kind == ConstValue::Kind::Bool && r->kind == ConstValue::Kind::Bool && is_cmp) {
        return ConstValue::makeBool(
            cmpResult(static_cast<int>(l->bool_value) - static_cast<int>(r->bool_value)));
    }
    if (l->kind == ConstValue::Kind::Bool || r->kind == ConstValue::Kind::Bool) {
        notConstant(expr.span, "bool operands only support `==`, `!=`, `&&`, `||`");
        return std::nullopt;
    }

    if (l->kind == ConstValue::Kind::Int && r->kind == ConstValue::Kind::Int) {
        const Int128 a = l->int_value;
        const Int128 b = r->int_value;
        if (is_cmp) {
            return ConstValue::makeBool(cmpResult(a < b ? -1 : (a > b ? 1 : 0)));
        }
        switch (expr.binary_op) {
        case BinaryOp::Add:
            return ConstValue::makeInt(a + b);
        case BinaryOp::Sub:
            return ConstValue::makeInt(a - b);
        case BinaryOp::Mul:
            return ConstValue::makeInt(a * b);
        case BinaryOp::Div:
        case BinaryOp::Mod:
            if (b == 0) {
                error("E0707",
                      expr.span,
                      "division by zero in a constant expression",
                      "divisor is 0");
                return std::nullopt;
            }
            return ConstValue::makeInt(expr.binary_op == BinaryOp::Div ? a / b : a % b);
        case BinaryOp::BitAnd:
            return ConstValue::makeInt(a & b);
        case BinaryOp::BitOr:
            return ConstValue::makeInt(a | b);
        case BinaryOp::BitXor:
            return ConstValue::makeInt(a ^ b);
        case BinaryOp::Shl:
        case BinaryOp::Shr:
            if (b < 0 || b > 126) {
                error("E0707",
                      expr.span,
                      "shift count out of range in a constant expression",
                      "the count must be between 0 and 126");
                return std::nullopt;
            }
            return ConstValue::makeInt(expr.binary_op == BinaryOp::Shl ? a << static_cast<int>(b)
                                                                       : a >> static_cast<int>(b));
        default:
            break;
        }
    }

    const auto toDouble = [](const ConstValue& v) {
        return v.kind == ConstValue::Kind::Int ? static_cast<double>(v.int_value) : v.float_value;
    };
    const double a = toDouble(*l);
    const double b = toDouble(*r);
    if (is_cmp) {
        return ConstValue::makeBool(cmpResult(a < b ? -1 : (a > b ? 1 : 0)));
    }
    switch (expr.binary_op) {
    case BinaryOp::Add:
        return ConstValue::makeFloat(a + b);
    case BinaryOp::Sub:
        return ConstValue::makeFloat(a - b);
    case BinaryOp::Mul:
        return ConstValue::makeFloat(a * b);
    case BinaryOp::Div:
        return ConstValue::makeFloat(a / b);
    default:
        notConstant(expr.span, "`%` needs integer operands");
        return std::nullopt;
    }
}

std::optional<ConstValue> Sema::evalCast(const Expr& expr) {
    auto v = evalConst(*expr.lhs);
    if (!v) {
        return std::nullopt;
    }
    auto target = resolve(expr.type);
    if (!target) {
        return std::nullopt;
    }
    if (target->kind == TypeKind::Enum) {
        return v->kind == ConstValue::Kind::Int ? v : std::optional<ConstValue>{};
    }
    switch (target->kind) {
    case TypeKind::Int: {
        Int128 raw = 0;
        switch (v->kind) {
        case ConstValue::Kind::Int:
            raw = v->int_value;
            break;
        case ConstValue::Kind::Float:
            raw = static_cast<Int128>(v->float_value);
            break;
        case ConstValue::Kind::Bool:
            raw = v->bool_value ? 1 : 0;
            break;
        case ConstValue::Kind::String:
            notConstant(expr.span, "a string address is not a constant integer");
            return std::nullopt;
        }
        const unsigned bits = target->bits == 0 ? config_.target.pointer_width : target->bits;
        return ConstValue::makeInt(wrapInt(raw, bits, target->is_signed));
    }
    case TypeKind::Float:
        if (v->kind == ConstValue::Kind::Int) {
            return ConstValue::makeFloat(static_cast<double>(v->int_value));
        }
        if (v->kind == ConstValue::Kind::Float) {
            return v->float_value == v->float_value && target->bits == 32
                       ? ConstValue::makeFloat(
                             static_cast<double>(static_cast<float>(v->float_value)))
                       : v;
        }
        break;
    case TypeKind::Bool:
        if (v->kind == ConstValue::Kind::Bool) {
            return v;
        }
        if (v->kind == ConstValue::Kind::Int) {
            return ConstValue::makeBool(v->int_value != 0);
        }
        break;
    default:
        break;
    }
    notConstant(expr.span, std::format("cast to `{}` is not a constant", typeToString(*target)));
    return std::nullopt;
}

/// @grammar `sizeOf(T)`, `alignOf(T)` and `offsetOf(T, field)` are the only calls that are compile-time constants; every other call in a constant position is E0707.
std::optional<ConstValue> Sema::evalCall(const Expr& expr) {
    if (expr.lhs->kind != Expr::Kind::Name) {
        notConstant(expr.span, "only `sizeOf`, `alignOf` and `offsetOf` calls are constants");
        return std::nullopt;
    }
    const std::string& callee = expr.lhs->text;
    if (callee == "sizeOf" || callee == "alignOf") {
        if (expr.args.size() != 1) {
            error("E0707",
                  expr.span,
                  std::format("`{}` takes one type argument", callee),
                  "wrong argument count");
            return std::nullopt;
        }
        auto type = exprToType(*expr.args[0]);
        if (!type) {
            notConstant(expr.args[0]->span, "expected a type");
            return std::nullopt;
        }
        auto layout = layoutOf(*type, expr.args[0]->span);
        if (!layout) {
            return std::nullopt;
        }
        return ConstValue::makeInt(callee == "sizeOf" ? layout->size : layout->align);
    }
    if (callee == "offsetOf") {
        if (expr.args.size() != 2 || expr.args[1]->kind != Expr::Kind::Name) {
            error(
                "E0707", expr.span, "`offsetOf` takes a type and a field name", "wrong arguments");
            return std::nullopt;
        }
        auto type = exprToType(*expr.args[0]);
        auto resolved = type ? resolve(*type) : std::nullopt;
        if (!resolved ||
            (resolved->kind != TypeKind::Struct && resolved->kind != TypeKind::Class)) {
            error("E0707",
                  expr.args[0]->span,
                  "`offsetOf` needs a struct or class",
                  "not an aggregate");
            return std::nullopt;
        }
        auto layout = layoutOf(*resolved, expr.args[0]->span);
        if (!layout) {
            return std::nullopt;
        }
        for (const FieldLayout& f : layout->fields) {
            if (f.name == expr.args[1]->text) {
                return ConstValue::makeInt(f.offset);
            }
        }
        error("E0707",
              expr.args[1]->span,
              std::format("`{}` has no field `{}`", typeToString(*resolved), expr.args[1]->text),
              "unknown field");
        return std::nullopt;
    }
    notConstant(expr.span, std::format("call of `{}` cannot be evaluated at compile time", callee));
    return std::nullopt;
}


}  // namespace xlang
