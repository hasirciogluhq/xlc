// The seam between the C++ compiler and the parser written in xlang (src/lang/parser/parser.xl).
//
// The parser itself lives on the xlang side; this file turns its frozen arena result into the
// `Program` tree of ast.h and reports the recorded syntax errors as E0002. The structs are the
// ones of include/xlang/lang/parser_c.h; the kind constants are the numbers of the C++ enums,
// which src/lang/ast/ast.xl mirrors number for number.
//
// `XLC_PARSER` picks the parser while the port settles: `xl` (the default once the port is
// proven), `cpp` (the reference parser of src/lang/parser/) and `diff` (runs both, compares the
// full dumps and aborts on the first difference).
#include "xlang/lang/ast.h"
#include "xlang/lang/lexer.h"
#include "xlang/lang/lexer_c.h"
#include "xlang/lang/parser_c.h"
#include "xlang/util/diagnostics.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace xlang {

namespace {

/// Reads the frozen arena result and rebuilds the AST tree from it.
class FlatReader {
  public:
    FlatReader(const XlcParseResult& result, FileId file, DiagnosticEngine& diagnostics)
        : r_(result), file_(file), diag_(diagnostics) {}

    std::string_view str(const XpStr& s) const {
        return std::string_view(r_.bytes + s.off, s.len);
    }

    std::string strCopy(const XpStr& s) const { return std::string(str(s)); }

    Span span(const XpSpan& s) const {
        Span span;
        span.file = file_;
        span.line = s.line;
        span.column = s.column;
        span.end_line = s.end_line;
        span.end_column = s.end_column;
        return span;
    }

    /// Registers the `@allow(...)` scopes of one node's attributes (the C++ parser does this
    /// while building: per statement, per function and per item, in that order).
    void registerAllowScopes(const std::vector<Attribute>& attrs, const Span& scope) {
        for (const Attribute& attr : attrs) {
            if (attr.name != "allow") {
                continue;
            }
            std::vector<std::string> codes;
            for (const AttributeArg& arg : attr.args) {
                if (arg.key.empty() && arg.kind == AttributeArg::Kind::Ident) {
                    codes.push_back(arg.text);
                }
            }
            diag_.addAllowScope(scope, codes);
        }
    }

    std::vector<Attribute> attributes(uint32_t first, uint32_t count) {
        std::vector<Attribute> attrs;
        attrs.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const XpAttr& raw = r_.attrs[first + i];
            Attribute attr;
            attr.name = strCopy(raw.name);
            attr.span = span(raw.span);
            attr.args.reserve(raw.args_count);
            for (uint32_t j = 0; j < raw.args_count; ++j) {
                const XpAttrArg& rawArg = r_.attr_args[raw.args_first + j];
                AttributeArg arg;
                switch (rawArg.kind) {
                case 1: arg.kind = AttributeArg::Kind::Int; break;
                case 2: arg.kind = AttributeArg::Kind::String; break;
                default: arg.kind = AttributeArg::Kind::Ident; break;
                }
                arg.key = strCopy(rawArg.key);
                arg.text = strCopy(rawArg.text);
                arg.number = rawArg.number;
                arg.span = span(rawArg.span);
                attr.args.push_back(std::move(arg));
            }
            attrs.push_back(std::move(attr));
        }
        return attrs;
    }

    Type type(int32_t idx) {
        if (idx < 0) {
            return Type{};
        }
        const XpType& raw = r_.types[idx];
        Type out;
        out.span = span(raw.span);
        switch (raw.kind) {
        case 0: out.kind = TypeKind::Void; break;
        case 1: out.kind = TypeKind::Never; break;
        case 2: out.kind = TypeKind::Bool; break;
        case 3:
            out.kind = TypeKind::Int;
            out.bits = raw.bits;
            out.is_signed = raw.is_signed != 0;
            break;
        case 4:
            out.kind = TypeKind::Float;
            out.bits = raw.bits;
            break;
        case 5:
            out.kind = TypeKind::Pointer;
            out.elem = std::make_shared<const Type>(this->type(raw.elem));
            break;
        case 6: {
            out.kind = TypeKind::FnPtr;
            for (uint32_t i = 0; i < raw.params_count; ++i) {
                out.params.push_back(this->type(r_.type_refs[raw.params_first + i]));
            }
            out.ret = std::make_shared<const Type>(this->type(raw.ret));
            break;
        }
        case 7:
            out.kind = TypeKind::Array;
            out.elem = std::make_shared<const Type>(this->type(raw.elem));
            if (raw.length >= 0) {
                out.length = expr(raw.length);
            }
            break;
        case 8:
            out.kind = TypeKind::Enum;
            out.elem = std::make_shared<const Type>(this->type(raw.elem));
            break;
        case 9: out.kind = TypeKind::Struct; break;
        case 10: out.kind = TypeKind::Class; break;
        case 11: out.kind = TypeKind::Interface; break;
        case 12: out.kind = TypeKind::Str; break;
        case 13:
            out.kind = TypeKind::Slice;
            out.is_mutable = raw.is_mutable != 0;
            out.elem = std::make_shared<const Type>(this->type(raw.elem));
            break;
        case 14:
            out.kind = TypeKind::Tuple;
            for (uint32_t i = 0; i < raw.args_count; ++i) {
                out.args.push_back(this->type(r_.type_refs[raw.args_first + i]));
            }
            break;
        default: {
            out.kind = TypeKind::Named;
            for (uint32_t i = 0; i < raw.path_count; ++i) {
                out.path.push_back(strCopy(r_.strs[raw.path_first + i]));
            }
            for (uint32_t i = 0; i < raw.args_count; ++i) {
                out.args.push_back(this->type(r_.type_refs[raw.args_first + i]));
            }
            break;
        }
        }
        // Struct/Class/Interface carry a name and generic arguments like `Named`.
        if (raw.kind == 9 || raw.kind == 10 || raw.kind == 11) {
            for (uint32_t i = 0; i < raw.path_count; ++i) {
                out.path.push_back(strCopy(r_.strs[raw.path_first + i]));
            }
            for (uint32_t i = 0; i < raw.args_count; ++i) {
                out.args.push_back(this->type(r_.type_refs[raw.args_first + i]));
            }
        }
        return out;
    }

    Token token(const XpTok& raw) const {
        Token token;
        token.kind = static_cast<TokenKind>(raw.kind);
        token.text = strCopy(raw.text);
        token.int_value = raw.int_value;
        token.float_value = raw.float_value;
        token.span = spanOf(raw);
        token.newline_before = raw.newline_before != 0;
        return token;
    }

    Span spanOf(const XpTok& raw) const {
        Span span;
        span.file = file_;
        span.line = raw.line;
        span.column = raw.column;
        span.end_line = raw.end_line;
        span.end_column = raw.end_column;
        return span;
    }

    std::vector<std::vector<Token>> macroArgs(uint32_t first, uint32_t count) {
        std::vector<std::vector<Token>> args;
        args.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const XpTokList& list = r_.tok_lists[first + i];
            std::vector<Token> tokens;
            tokens.reserve(list.count);
            for (uint32_t j = 0; j < list.count; ++j) {
                tokens.push_back(token(r_.toks[list.first + j]));
            }
            args.push_back(std::move(tokens));
        }
        return args;
    }

    ExprPtr expr(int32_t idx) {
        if (idx < 0) {
            return nullptr;
        }
        const XpExpr& raw = r_.exprs[idx];
        auto e = std::make_unique<Expr>();
        e->span = span(raw.span);
        e->int_value = raw.int_value;
        e->float_value = raw.float_value;
        e->bool_value = raw.bool_value != 0;
        e->text = strCopy(raw.text);
        switch (raw.kind) {
        case 0: e->kind = Expr::Kind::IntLiteral; break;
        case 1: e->kind = Expr::Kind::FloatLiteral; break;
        case 2: e->kind = Expr::Kind::BoolLiteral; break;
        case 3: e->kind = Expr::Kind::StringLiteral; break;
        case 4: e->kind = Expr::Kind::ByteLiteral; break;
        case 5: e->kind = Expr::Kind::Name; break;
        case 6:
            e->kind = Expr::Kind::Unary;
            e->unary_op = static_cast<UnaryOp>(raw.unary_op);
            e->lhs = expr(raw.lhs);
            break;
        case 7:
            e->kind = Expr::Kind::Binary;
            e->binary_op = static_cast<BinaryOp>(raw.binary_op);
            e->lhs = expr(raw.lhs);
            e->rhs = expr(raw.rhs);
            break;
        case 8:
            e->kind = Expr::Kind::Call;
            e->lhs = expr(raw.lhs);
            for (uint32_t i = 0; i < raw.args_count; ++i) {
                e->args.push_back(expr(r_.expr_refs[raw.args_first + i]));
            }
            for (uint32_t i = 0; i < raw.generic_args_count; ++i) {
                e->generic_args.push_back(type(r_.type_refs[raw.generic_args_first + i]));
            }
            break;
        case 9:
            e->kind = Expr::Kind::Index;
            e->lhs = expr(raw.lhs);
            e->rhs = expr(raw.rhs);
            break;
        case 10:
            e->kind = Expr::Kind::Slice;
            e->lhs = expr(raw.lhs);
            for (uint32_t i = 0; i < raw.args_count; ++i) {
                e->args.push_back(expr(r_.expr_refs[raw.args_first + i]));
            }
            e->rhs = expr(raw.rhs);
            break;
        case 11:
            e->kind = Expr::Kind::Field;
            e->lhs = expr(raw.lhs);
            break;
        case 12:
            e->kind = Expr::Kind::Cast;
            e->lhs = expr(raw.lhs);
            e->type = type(raw.type_idx);
            break;
        case 13:
            e->kind = Expr::Kind::ArrayList;
            for (uint32_t i = 0; i < raw.args_count; ++i) {
                e->args.push_back(expr(r_.expr_refs[raw.args_first + i]));
            }
            break;
        case 14:
            e->kind = Expr::Kind::ArrayRepeat;
            e->lhs = expr(raw.lhs);
            e->rhs = expr(raw.rhs);
            break;
        case 15:
            e->kind = Expr::Kind::TupleLiteral;
            for (uint32_t i = 0; i < raw.args_count; ++i) {
                e->args.push_back(expr(r_.expr_refs[raw.args_first + i]));
            }
            break;
        case 16:
            e->kind = Expr::Kind::StructLiteral;
            e->type = type(raw.type_idx);
            for (uint32_t i = 0; i < raw.fields_count; ++i) {
                const XpFieldInit& f = r_.field_inits[raw.fields_first + i];
                FieldInit init;
                init.name = strCopy(f.name);
                init.value = expr(f.value);
                init.span = span(f.span);
                e->fields.push_back(std::move(init));
            }
            for (uint32_t i = 0; i < raw.generic_args_count; ++i) {
                e->generic_args.push_back(type(r_.type_refs[raw.generic_args_first + i]));
            }
            break;
        case 17:
            e->kind = Expr::Kind::New;
            e->type = type(raw.type_idx);
            e->has_call_args = raw.has_call_args != 0;
            e->lhs = expr(raw.lhs);
            for (uint32_t i = 0; i < raw.args_count; ++i) {
                e->args.push_back(expr(r_.expr_refs[raw.args_first + i]));
            }
            for (uint32_t i = 0; i < raw.fields_count; ++i) {
                const XpFieldInit& f = r_.field_inits[raw.fields_first + i];
                FieldInit init;
                init.name = strCopy(f.name);
                init.value = expr(f.value);
                init.span = span(f.span);
                e->fields.push_back(std::move(init));
            }
            break;
        case 18:
            e->kind = Expr::Kind::Ternary;
            e->lhs = expr(raw.lhs);
            e->rhs = expr(raw.rhs);
            if (raw.args_count > 0) {
                e->args.push_back(expr(r_.expr_refs[raw.args_first]));
            }
            break;
        case 19:
            e->kind = Expr::Kind::Try;
            e->lhs = expr(raw.lhs);
            break;
        case 20:
            e->kind = Expr::Kind::MacroCall;
            e->macro_args = macroArgs(raw.macro_args_first, raw.macro_args_count);
            break;
        case 21: e->kind = Expr::Kind::Spread; break;
        case 22:
            e->kind = Expr::Kind::AsmOperand;
            e->lhs = expr(raw.lhs);
            e->rhs = expr(raw.rhs);
            break;
        case 23:
            e->kind = Expr::Kind::TypeOperand;
            e->type = type(raw.type_idx);
            break;
        default: {
            e->kind = Expr::Kind::Closure;
            const XpClosure& c = r_.closures[raw.closure];
            auto data = std::make_shared<ClosureData>();
            for (uint32_t i = 0; i < c.params_count; ++i) {
                data->params.push_back(param(r_.params[c.params_first + i]));
            }
            data->has_return_type = c.has_ret != 0;
            if (c.has_ret != 0) {
                data->return_type = type(c.ret);
            }
            data->is_move = c.is_move != 0;
            data->expr_body = c.expr_body != 0;
            data->body = block(c.body);
            e->closure = std::move(data);
            break;
        }
        }
        return e;
    }

    Param param(const XpParam& raw) {
        Param param;
        param.name = strCopy(raw.name);
        param.type = type(raw.type_idx);
        param.is_self = raw.is_self != 0;
        param.is_pack = raw.is_pack != 0;
        param.typed_self = raw.typed_self != 0;
        param.has_type = raw.has_type != 0;
        param.span = span(raw.span);
        return param;
    }

    Block block(int32_t idx) {
        Block block;
        if (idx < 0) {
            return block;
        }
        const XpBlock& raw = r_.blocks[idx];
        block.span = span(raw.span);
        block.statements.reserve(raw.stmts_count);
        for (uint32_t i = 0; i < raw.stmts_count; ++i) {
            block.statements.push_back(stmt(r_.stmt_refs[raw.stmts_first + i]));
        }
        return block;
    }

    Stmt stmt(int32_t idx) {
        Stmt stmt;
        const XpStmt& raw = r_.stmts[idx];
        stmt.span = span(raw.span);
        stmt.attributes = attributes(raw.attrs_first, raw.attrs_count);
        stmt.name = strCopy(raw.name);
        for (uint32_t i = 0; i < raw.names_count; ++i) {
            stmt.names.push_back(strCopy(r_.strs[raw.names_first + i]));
        }
        stmt.label = strCopy(raw.label);
        stmt.has_type = raw.has_type != 0;
        stmt.type = type(raw.type_idx);
        stmt.target = expr(raw.target);
        stmt.expr = expr(raw.expr);
        stmt.expr2 = expr(raw.expr2);
        stmt.has_compound_op = raw.has_compound_op != 0;
        stmt.compound_op = static_cast<BinaryOp>(raw.compound_op);
        stmt.is_incdec = raw.is_incdec != 0;
        stmt.range_inclusive = raw.range_inclusive != 0;
        if (raw.body >= 0) {
            stmt.body = std::make_unique<Block>(block(raw.body));
        }
        if (raw.else_body >= 0) {
            stmt.else_body = std::make_unique<Block>(block(raw.else_body));
        }
        stmt.else_is_if = raw.else_is_if != 0;
        if (raw.init >= 0) {
            stmt.init = std::make_unique<Stmt>(this->stmt(raw.init));
        }
        if (raw.step >= 0) {
            stmt.step = std::make_unique<Stmt>(this->stmt(raw.step));
        }
        for (uint32_t i = 0; i < raw.cases_count; ++i) {
            const XpSwitchCase& c = r_.cases[raw.cases_first + i];
            SwitchCase arm;
            arm.span = span(c.span);
            arm.is_default = c.is_default != 0;
            for (uint32_t j = 0; j < c.values_count; ++j) {
                arm.values.push_back(expr(r_.expr_refs[c.values_first + j]));
            }
            if (c.body >= 0) {
                arm.body = std::make_unique<Block>(block(c.body));
            }
            stmt.cases.push_back(std::move(arm));
        }
        switch (raw.kind) {
        case 0: stmt.kind = Stmt::Kind::Local; break;
        case 1: stmt.kind = Stmt::Kind::Delete; break;
        case 2: stmt.kind = Stmt::Kind::Const; break;
        case 3: stmt.kind = Stmt::Kind::Assign; break;
        case 4: stmt.kind = Stmt::Kind::Return; break;
        case 5: stmt.kind = Stmt::Kind::Expr; break;
        case 6: stmt.kind = Stmt::Kind::If; break;
        case 7: stmt.kind = Stmt::Kind::While; break;
        case 8: stmt.kind = Stmt::Kind::DoWhile; break;
        case 9: stmt.kind = Stmt::Kind::For; break;
        case 10: stmt.kind = Stmt::Kind::ForRange; break;
        case 11: stmt.kind = Stmt::Kind::ForPack; break;
        case 12: stmt.kind = Stmt::Kind::ForEach; break;
        case 13: stmt.kind = Stmt::Kind::Switch; break;
        case 14: stmt.kind = Stmt::Kind::Defer; break;
        case 15: stmt.kind = Stmt::Kind::Try; break;
        case 16: stmt.kind = Stmt::Kind::Continue; break;
        case 17: stmt.kind = Stmt::Kind::Break; break;
        case 18: stmt.kind = Stmt::Kind::Unsafe; break;
        default: stmt.kind = Stmt::Kind::Block; break;
        }
        registerAllowScopes(stmt.attributes, stmt.span);
        return stmt;
    }

    std::vector<std::string> strings(uint32_t first, uint32_t count) {
        std::vector<std::string> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            out.push_back(strCopy(r_.strs[first + i]));
        }
        return out;
    }

    std::vector<Param> params(uint32_t first, uint32_t count) {
        std::vector<Param> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            out.push_back(param(r_.params[first + i]));
        }
        return out;
    }

    std::vector<FieldDecl> fields(uint32_t first, uint32_t count) {
        std::vector<FieldDecl> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const XpField& raw = r_.fields[first + i];
            FieldDecl field;
            field.name = strCopy(raw.name);
            field.type = type(raw.type_idx);
            field.attributes = attributes(raw.attrs_first, raw.attrs_count);
            field.span = span(raw.span);
            out.push_back(std::move(field));
        }
        return out;
    }

    std::vector<FnDecl> methods(uint32_t first, uint32_t count) {
        std::vector<FnDecl> out;
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            Decl decl = declOf(static_cast<int32_t>(first + i));
            FnDecl& fn = std::get<FnDecl>(decl);
            registerAllowScopes(fn.attributes, fn.span);
            out.push_back(std::move(fn));
        }
        return out;
    }

    Decl declOf(int32_t idx) {
        const XpDecl& raw = r_.decls[idx];
        switch (raw.kind) {
        case 0: {  // FnDecl
            FnDecl d;
            fillBase(d, raw);
            d.generic_params = strings(raw.generic_first, raw.generic_count);
            d.params = params(raw.params_first, raw.params_count);
            d.has_return_type = raw.has_ret != 0;
            if (raw.has_ret != 0) {
                d.return_type = type(raw.ret);
            }
            d.has_body = raw.has_body != 0;
            if (raw.body >= 0) {
                d.body = block(raw.body);
            }
            d.is_virtual = raw.is_virtual != 0;
            d.is_override = raw.is_override != 0;
            return d;
        }
        case 1: {  // GlobalDecl
            GlobalDecl d;
            fillBase(d, raw);
            d.has_type = raw.has_type != 0;
            d.type = type(raw.type_idx);
            d.is_const = raw.is_const != 0;
            if (raw.init >= 0) {
                d.init = expr(raw.init);
            }
            return d;
        }
        case 2: {  // StructDecl
            StructDecl d;
            fillBase(d, raw);
            d.generic_params = strings(raw.generic_first, raw.generic_count);
            d.fields = fields(raw.fields_first, raw.fields_count);
            d.opaque = raw.opaque != 0;
            d.is_union = raw.is_union != 0;
            d.is_payload = raw.is_payload != 0;
            for (uint32_t i = 0; i < raw.variants_count; ++i) {
                const XpSumVariant& v = r_.variants[raw.variants_first + i];
                SumVariant variant;
                variant.name = strCopy(v.name);
                for (uint32_t j = 0; j < v.payload_count; ++j) {
                    variant.payload.push_back(type(r_.type_refs[v.payload_first + j]));
                }
                d.variants.push_back(std::move(variant));
            }
            return d;
        }
        case 3: {  // ClassDecl
            ClassDecl d;
            fillBase(d, raw);
            d.generic_params = strings(raw.generic_first, raw.generic_count);
            d.has_base = raw.base_has != 0;
            d.base = strings(raw.base_first, raw.base_count);
            d.fields = fields(raw.fields_first, raw.fields_count);
            d.methods = methods(raw.methods_first, raw.methods_count);
            d.opaque = raw.opaque != 0;
            return d;
        }
        case 4: {  // InterfaceDecl
            InterfaceDecl d;
            fillBase(d, raw);
            d.generic_params = strings(raw.generic_first, raw.generic_count);
            d.methods = methods(raw.methods_first, raw.methods_count);
            d.opaque = raw.opaque != 0;
            return d;
        }
        case 5: {  // ImplDecl
            ImplDecl d;
            fillBase(d, raw);
            d.generic_params = strings(raw.generic_first, raw.generic_count);
            d.interface_path = strings(raw.iface_path_first, raw.iface_path_count);
            for (uint32_t i = 0; i < raw.iface_args_count; ++i) {
                d.interface_args.push_back(type(r_.type_refs[raw.iface_args_first + i]));
            }
            d.target = type(raw.impl_target);
            d.methods = methods(raw.methods_first, raw.methods_count);
            return d;
        }
        case 6: {  // EnumDecl
            EnumDecl d;
            fillBase(d, raw);
            d.generic_params = strings(raw.generic_first, raw.generic_count);
            d.has_underlying = raw.has_underlying != 0;
            // The parser gives a value enum the type `i32` even when it writes no `: T`, and the
            // declaration carries it either way.
            if (raw.underlying >= 0) {
                d.underlying = type(raw.underlying);
            }
            for (uint32_t i = 0; i < raw.members_count; ++i) {
                const XpEnumMember& m = r_.enum_members[raw.members_first + i];
                EnumMember member;
                member.name = strCopy(m.name);
                if (m.value >= 0) {
                    member.value = expr(m.value);
                }
                member.has_payload = m.has_payload != 0;
                for (uint32_t j = 0; j < m.payload_count; ++j) {
                    member.payload.push_back(type(r_.type_refs[m.payload_first + j]));
                }
                member.attributes = attributes(m.attrs_first, m.attrs_count);
                member.span = span(m.span);
                d.members.push_back(std::move(member));
            }
            return d;
        }
        case 7: {  // TypeAliasDecl
            TypeAliasDecl d;
            fillBase(d, raw);
            d.generic_params = strings(raw.generic_first, raw.generic_count);
            d.target = type(raw.type_idx);
            return d;
        }
        case 8: {  // DefineConstDecl
            DefineConstDecl d;
            fillBase(d, raw);
            if (raw.init >= 0) {
                d.value = expr(raw.init);
            }
            return d;
        }
        case 9: {  // DefineMacroDecl
            DefineMacroDecl d;
            fillBase(d, raw);
            for (uint32_t i = 0; i < raw.macro_params_count; ++i) {
                const XpMacroParam& p = r_.macro_params[raw.macro_params_first + i];
                MacroParam param;
                switch (p.kind) {
                case 1: param.kind = MacroParam::Kind::Type; break;
                case 2: param.kind = MacroParam::Kind::Ident; break;
                case 3: param.kind = MacroParam::Kind::Block; break;
                case 4: param.kind = MacroParam::Kind::Items; break;
                default: param.kind = MacroParam::Kind::Expr; break;
                }
                param.name = strCopy(p.name);
                param.span = span(p.span);
                d.params.push_back(std::move(param));
            }
            for (uint32_t i = 0; i < raw.macro_body_count; ++i) {
                d.body.push_back(token(r_.toks[raw.macro_body_first + i]));
            }
            return d;
        }
        case 10: {  // ImportDecl
            ImportDecl d;
            fillBase(d, raw);
            switch (raw.form) {
            case 1: d.form = ImportDecl::Form::Star; break;
            case 2: d.form = ImportDecl::Form::StarAs; break;
            case 3: d.form = ImportDecl::Form::Names; break;
            default: d.form = ImportDecl::Form::Module; break;
            }
            d.module = strCopy(raw.module);
            d.module_span = span(raw.module_span);
            d.alias = strCopy(raw.alias);
            importNames(d.names, raw.names_first, raw.names_count);
            return d;
        }
        case 11: {  // ExportFromDecl
            ExportFromDecl d;
            fillBase(d, raw);
            switch (raw.form) {
            case 1: d.form = ExportFromDecl::Form::StarAs; break;
            case 2: d.form = ExportFromDecl::Form::Names; break;
            default: d.form = ExportFromDecl::Form::Star; break;
            }
            d.module = strCopy(raw.module);
            d.module_span = span(raw.module_span);
            d.alias = strCopy(raw.alias);
            importNames(d.names, raw.names_first, raw.names_count);
            importNames(d.except, raw.except_first, raw.except_count);
            return d;
        }
        case 12: {  // MacroCallDecl
            MacroCallDecl d;
            fillBase(d, raw);
            d.args = macroArgs(raw.macro_args_first, raw.macro_args_count);
            return d;
        }
        case 13: {  // TopLevelCallDecl
            TopLevelCallDecl d;
            fillBase(d, raw);
            if (raw.call >= 0) {
                d.call = expr(raw.call);
            }
            return d;
        }
        default: {  // FnAliasDecl
            FnAliasDecl d;
            fillBase(d, raw);
            d.target = strings(raw.target_path_first, raw.target_path_count);
            d.target_span = span(raw.target_span);
            return d;
        }
        }
    }

    void importNames(std::vector<ImportName>& out, uint32_t first, uint32_t count) {
        out.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const XpImportName& raw = r_.import_names[first + i];
            ImportName name;
            name.name = strCopy(raw.name);
            name.alias = strCopy(raw.alias);
            name.span = span(raw.span);
            out.push_back(std::move(name));
        }
    }

    template <typename D>
    void fillBase(D& d, const XpDecl& raw) {
        d.name = strCopy(raw.name);
        d.attributes = attributes(raw.attrs_first, raw.attrs_count);
        d.span = span(raw.span);
        d.name_span = span(raw.name_span);
    }

  private:
    const XlcParseResult& r_;
    FileId file_;
    DiagnosticEngine& diag_;
};

/// Packs the token stream into the C array the xlang parser reads (the `XlcLexToken` layout with
/// one shared byte buffer for the spellings) and runs the xlang parser.
Program parseTokensXl(std::vector<Token>& tokens, FileId file, DiagnosticEngine& diagnostics) {
    // The spellings of all tokens, copied into one buffer that outlives the call.
    std::string bytes;
    std::uint32_t total = 0;
    for (const Token& t : tokens) {
        bytes.append(t.text);
        total += 1;
    }
    std::vector<std::uint32_t> offsets;
    offsets.reserve(total + 1);
    std::uint32_t off = 0;
    for (const Token& t : tokens) {
        offsets.push_back(off);
        off += static_cast<std::uint32_t>(t.text.size());
    }

    std::vector<XlcLexToken> raw;
    raw.reserve(total);
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const Token& t = tokens[i];
        XlcLexToken rt{};
        rt.kind = static_cast<std::int32_t>(t.kind);
        rt.text = bytes.data() + offsets[i];
        rt.text_len = static_cast<std::uint32_t>(t.text.size());
        rt.int_value = t.int_value;
        rt.float_value = t.float_value;
        rt.line = t.span.line;
        rt.column = t.span.column;
        rt.end_line = t.span.end_line;
        rt.end_column = t.span.end_column;
        rt.newline_before = t.newline_before ? 1 : 0;
        raw.push_back(rt);
    }

    const XlcParseResult result = xlcParse(raw.data(), static_cast<std::uint32_t>(raw.size()),
                                           file);
    FlatReader reader(result, file, diagnostics);
    Program program;
    program.file = file;
    program.file_attributes = reader.attributes(result.file_attrs_first,
                                                result.file_attrs_count);
    program.items.reserve(result.items_count);
    for (std::uint32_t i = 0; i < result.items_count; ++i) {
        const XpItem& rawItem = result.items[result.items_first + i];
        Item item;
        item.modifiers.exported = rawItem.exported != 0;
        item.modifiers.export_c = rawItem.export_c != 0;
        item.modifiers.declared = rawItem.declared != 0;
        item.modifiers.registered = rawItem.registered != 0;
        item.modifiers.register_role = reader.strCopy(rawItem.register_role);
        item.decl = reader.declOf(rawItem.decl);
        // The item-level `@allow` scopes (the C++ `addItem` registers them after the declaration,
        // so a scope inside a method body is registered before its item's own).
        {
            const XpDecl& rawDecl = result.decls[rawItem.decl];
            reader.registerAllowScopes(reader.attributes(rawDecl.attrs_first, rawDecl.attrs_count),
                                       reader.span(rawDecl.span));
        }
        program.items.push_back(std::move(item));
    }
    // The syntax errors the xlang side recorded.
    for (std::uint32_t i = 0; i < result.err_count; ++i) {
        const XpErr& e = result.errs[i];
        Diagnostic d;
        d.code = "E0002";
        d.severity = Severity::Error;
        d.message = reader.strCopy(e.message);
        for (std::uint32_t j = 0; j < e.labels_count; ++j) {
            const XpErrLabel& l = result.err_labels[e.labels_first + j];
            d.labels.push_back({reader.span(l.span), reader.strCopy(l.text)});
        }
        d.detail = reader.strCopy(e.detail);
        d.fix = reader.strCopy(e.fix);
        diagnostics.report(std::move(d));
    }
    xlcParseFree(result);
    return program;
}

/// The full dump of one program as a string (the differential test compares these).
std::string dumpToString(const Program& program) {
    std::ostringstream out;
    dumpProgram(out, program, false);
    return out.str();
}

}  // namespace

Program parseTokensFlat(std::vector<Token> tokens, FileId file, DiagnosticEngine& diagnostics) {
    return parseTokensXl(tokens, file, diagnostics);
}

}  // namespace xlang
