#pragma once

// The C boundary of the parser, which is written in xlang (src/lang/parser/parser.xl) and built
// into the compiler as an object file. This header is the C++ side of that seam:
// parseTokensFlat() in src/lang/parser/bridge.cpp reads the frozen result and rebuilds the
// `Program` tree of ast.h, and reports the recorded syntax errors as E0002.
//
// Every struct here mirrors a struct of src/lang/ast/ast.xl field for field (the comment there
// names the C++ AST node each one carries). The kind constants are the numbers of the C++ enums
// (`Expr::Kind`, `Stmt::Kind`, `TypeKind`, the declaration order of `Decl`, `UnaryOp`, `BinaryOp`);
// ast.xl must agree number for number.
//
// Ownership: everything the xlang side returns lives in one block that `xlcParseFree` releases
// through the `block` field. Strings are `(offset, length)` pairs into the result's byte buffer;
// an index field is int32_t, -1 for none; a child list is a (first, count) range.

#include <stdint.h>

#if defined(_WIN32)
#define XLC_PARSE_CALL __cdecl
#else
#define XLC_PARSE_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// `(offset, length)` into the result's byte buffer.
typedef struct XpStr {
    uint32_t off;
    uint32_t len;
} XpStr;

/// Field for field the C++ `Span` (the file id is stamped by the caller).
typedef struct XpSpan {
    uint32_t file;
    uint32_t line;
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
} XpSpan;

/// One field initializer of a struct literal or `new`.
typedef struct XpFieldInit {
    XpStr name;
    int32_t value;  ///< expr index
    XpSpan span;
} XpFieldInit;

/// The parameters and body of a closure literal; the C++ `ClosureData`.
typedef struct XpClosure {
    uint32_t params_first;  ///< XpParam arena
    uint32_t params_count;
    uint32_t has_ret;
    int32_t ret;  ///< type index
    uint32_t is_move;
    uint32_t expr_body;
    int32_t body;  ///< block index
} XpClosure;

/// One expression node; the fields of the C++ `Expr`, unused ones zero.
typedef struct XpExpr {
    int32_t kind;
    XpSpan span;
    uint64_t int_value;
    double float_value;
    uint32_t bool_value;
    XpStr text;
    int32_t unary_op;
    int32_t binary_op;
    int32_t lhs;  ///< expr index
    int32_t rhs;  ///< expr index
    uint32_t args_first;  ///< expr index arena
    uint32_t args_count;
    uint32_t fields_first;  ///< XpFieldInit arena
    uint32_t fields_count;
    int32_t type_idx;  ///< type index (Cast target, StructLiteral/New type)
    uint32_t has_call_args;
    uint32_t generic_args_first;  ///< type index arena
    uint32_t generic_args_count;
    uint32_t macro_args_first;  ///< XpTokList arena, one entry per argument
    uint32_t macro_args_count;
    int32_t closure;  ///< XpClosure index
} XpExpr;

/// One arm of a `switch`.
typedef struct XpSwitchCase {
    XpSpan span;
    uint32_t is_default;
    uint32_t values_first;  ///< expr index arena
    uint32_t values_count;
    int32_t body;  ///< block index
} XpSwitchCase;

/// One statement node; the fields of the C++ `Stmt`, unused ones zero.
typedef struct XpStmt {
    int32_t kind;
    XpSpan span;
    uint32_t attrs_first;  ///< XpAttr arena
    uint32_t attrs_count;
    XpStr name;
    uint32_t names_first;  ///< XpStr table (the tuple targets / the ForEach index)
    uint32_t names_count;
    XpStr label;
    uint32_t has_type;
    int32_t type_idx;  ///< type index
    int32_t target;  ///< expr index (Assign)
    int32_t expr;  ///< expr index
    int32_t expr2;  ///< expr index (ForRange upper bound)
    uint32_t has_compound_op;
    int32_t compound_op;
    uint32_t is_incdec;
    uint32_t range_inclusive;
    int32_t body;  ///< block index
    int32_t else_body;  ///< block index
    uint32_t else_is_if;
    int32_t init;  ///< stmt index (For initializer, Defer statement)
    int32_t step;  ///< stmt index (For step)
    uint32_t cases_first;  ///< XpSwitchCase arena
    uint32_t cases_count;
} XpStmt;

/// One block: a span and a run of top-level statement indices.
typedef struct XpBlock {
    XpSpan span;
    uint32_t stmts_first;  ///< statement index arena
    uint32_t stmts_count;
} XpBlock;

/// One type node; the fields of the C++ `Type`.
typedef struct XpType {
    int32_t kind;
    uint32_t bits;
    uint32_t is_signed;
    uint32_t is_mutable;
    uint32_t path_first;  ///< XpStr table (the qualified name parts)
    uint32_t path_count;
    int32_t elem;  ///< type index (Pointer/Array pointee, Enum underlying)
    uint32_t params_first;  ///< type index arena (FnPtr)
    uint32_t params_count;
    int32_t ret;  ///< type index (FnPtr)
    uint32_t args_first;  ///< type index arena (generic arguments, Tuple elements)
    uint32_t args_count;
    int32_t length;  ///< expr index (Array length)
    XpSpan span;
} XpType;

/// One attribute argument.
typedef struct XpAttrArg {
    int32_t kind;  ///< 0 ident, 1 int, 2 string
    XpStr key;
    XpStr text;
    int64_t number;
    XpSpan span;
} XpAttrArg;

typedef struct XpAttr {
    XpStr name;
    uint32_t args_first;  ///< XpAttrArg arena
    uint32_t args_count;
    XpSpan span;
} XpAttr;

/// One parameter; the C++ `Param`.
typedef struct XpParam {
    XpStr name;
    int32_t type_idx;  ///< type index
    uint32_t is_self;
    uint32_t is_pack;
    uint32_t typed_self;
    uint32_t has_type;
    XpSpan span;
} XpParam;

/// One field of a struct or class.
typedef struct XpField {
    XpStr name;
    int32_t type_idx;  ///< type index
    uint32_t attrs_first;  ///< XpAttr arena
    uint32_t attrs_count;
    XpSpan span;
} XpField;

/// One variant of a payload enum.
typedef struct XpSumVariant {
    XpStr name;
    uint32_t payload_first;  ///< type index arena
    uint32_t payload_count;
} XpSumVariant;

/// One member of an enum.
typedef struct XpEnumMember {
    XpStr name;
    int32_t value;  ///< expr index, -1 = auto-numbered
    uint32_t has_payload;
    uint32_t payload_first;  ///< type index arena
    uint32_t payload_count;
    uint32_t attrs_first;  ///< XpAttr arena
    uint32_t attrs_count;
    XpSpan span;
} XpEnumMember;

/// One parameter of a `define` macro.
typedef struct XpMacroParam {
    int32_t kind;  ///< 0 expr, 1 type, 2 ident, 3 block, 4 items
    XpStr name;
    XpSpan span;
} XpMacroParam;

/// One imported (or re-exported) name.
typedef struct XpImportName {
    XpStr name;
    XpStr alias;
    XpSpan span;
} XpImportName;

/// One raw token kept in the tree (macro bodies and macro call arguments).
typedef struct XpTok {
    int32_t kind;
    XpStr text;
    uint64_t int_value;
    double float_value;
    uint32_t line;
    uint32_t column;
    uint32_t end_line;
    uint32_t end_column;
    uint32_t newline_before;
} XpTok;

/// One raw token list (a macro argument, or the whole body of a `define`).
typedef struct XpTokList {
    uint32_t first;  ///< XpTok arena
    uint32_t count;
} XpTokList;

/// One declaration record: the union of the fields the fifteen C++ declaration structs carry,
/// tagged by `kind` (the declaration order of the `Decl` variant). Methods of a class, interface
/// or impl are Fn records in the same declaration arena, ranged over by `methods_first/count`.
typedef struct XpDecl {
    uint32_t form;  ///< import: 0 module, 1 star, 2 star-as, 3 names; export-from: 0 star, 1 star-as, 2 names
    int32_t kind;
    XpStr name;
    uint32_t attrs_first;  ///< XpAttr arena
    uint32_t attrs_count;
    XpSpan span;
    XpSpan name_span;
    uint32_t generic_first;  ///< XpStr table (the generic parameter names)
    uint32_t generic_count;
    uint32_t params_first;  ///< XpParam arena (Fn)
    uint32_t params_count;
    uint32_t has_ret;
    int32_t ret;  ///< type index
    uint32_t has_body;
    int32_t body;  ///< block index
    uint32_t is_virtual;
    uint32_t is_override;
    int32_t type_idx;  ///< type index (Global/Const type, TypeAlias target)
    uint32_t has_type;
    uint32_t is_const;
    int32_t init;  ///< expr index (Global init, DefineConst value)
    uint32_t fields_first;  ///< XpField arena (Struct/Class)
    uint32_t fields_count;
    uint32_t opaque;
    uint32_t is_union;
    uint32_t is_payload;
    uint32_t variants_first;  ///< XpSumVariant arena (Struct of a payload enum)
    uint32_t variants_count;
    uint32_t methods_first;  ///< XpDecl arena, kind Fn (Class/Interface/Impl)
    uint32_t methods_count;
    uint32_t base_has;
    uint32_t base_first;  ///< XpStr table (Class base path)
    uint32_t base_count;
    uint32_t iface_path_first;  ///< XpStr table (Impl interface path)
    uint32_t iface_path_count;
    uint32_t iface_args_first;  ///< type index arena (Impl)
    uint32_t iface_args_count;
    int32_t impl_target;  ///< type index (Impl target type)
    uint32_t has_underlying;
    int32_t underlying;  ///< type index (Enum)
    uint32_t members_first;  ///< XpEnumMember arena (Enum)
    uint32_t members_count;
    uint32_t macro_params_first;  ///< XpMacroParam arena (DefineMacro)
    uint32_t macro_params_count;
    uint32_t macro_body_first;  ///< XpTok arena (DefineMacro)
    uint32_t macro_body_count;
    XpStr module;  ///< Import/ExportFrom
    XpSpan module_span;
    XpStr alias;
    uint32_t names_first;  ///< XpImportName arena (Import/ExportFrom)
    uint32_t names_count;
    uint32_t except_first;  ///< XpImportName arena (ExportFrom)
    uint32_t except_count;
    uint32_t macro_args_first;  ///< XpTokList arena (MacroCall item)
    uint32_t macro_args_count;
    int32_t call;  ///< expr index (TopLevelCall)
    uint32_t target_path_first;  ///< XpStr table (FnAlias)
    uint32_t target_path_count;
    XpSpan target_span;
} XpDecl;

/// The modifiers of one item.
typedef struct XpItem {
    uint32_t exported;
    uint32_t export_c;
    uint32_t declared;
    uint32_t registered;
    XpStr register_role;
    int32_t decl;  ///< XpDecl index
} XpItem;

/// One label of a syntax error: a span and its text.
typedef struct XpErrLabel {
    XpSpan span;
    XpStr text;
} XpErrLabel;

/// One syntax error: the message, the labels (the first is primary) and the detail and fix hints.
typedef struct XpErr {
    XpStr code;  ///< empty for the plain syntax error E0002
    XpStr message;
    XpStr detail;
    XpStr fix;
    uint32_t labels_first;  ///< XpErrLabel arena
    uint32_t labels_count;
} XpErr;

/// The whole program: the file, its `@file(...)` attributes and the items.
typedef struct XpProgram {
    uint32_t file;
    uint32_t file_attrs_first;  ///< XpAttr arena
    uint32_t file_attrs_count;
    uint32_t items_first;  ///< XpItem arena
    uint32_t items_count;
} XpProgram;

/// The frozen outcome of parsing one file: the program header, one pointer and count per arena,
/// and the syntax errors. `block` is the opaque handle `xlcParseFree` releases.
typedef struct XlcParseResult {
    // The program header, flattened (an embedded imported struct shifted the offsets once).
    uint32_t program_file;
    uint32_t file_attrs_first;  ///< XpAttr arena
    uint32_t file_attrs_count;
    uint32_t items_first;  ///< XpItem arena
    uint32_t items_count;
    const XpExpr* exprs;
    uint32_t expr_count;
    const XpStmt* stmts;
    uint32_t stmt_count;
    const XpBlock* blocks;
    uint32_t block_count;
    const XpType* types;
    uint32_t type_count;
    const int32_t* type_refs;
    uint32_t type_ref_count;
    const int32_t* expr_refs;
    uint32_t expr_ref_count;
    const int32_t* stmt_refs;
    uint32_t stmt_ref_count;
    const XpDecl* decls;
    uint32_t decl_count;
    const XpItem* items;
    uint32_t item_count;
    const XpAttr* attrs;
    uint32_t attr_count;
    const XpAttrArg* attr_args;
    uint32_t attr_arg_count;
    const XpParam* params;
    uint32_t param_count;
    const XpClosure* closures;
    uint32_t closure_count;
    const XpFieldInit* field_inits;
    uint32_t field_init_count;
    const XpField* fields;
    uint32_t field_count;
    const XpSumVariant* variants;
    uint32_t variant_count;
    const XpEnumMember* enum_members;
    uint32_t enum_member_count;
    const XpMacroParam* macro_params;
    uint32_t macro_param_count;
    const XpImportName* import_names;
    uint32_t import_name_count;
    const XpTokList* tok_lists;
    uint32_t tok_list_count;
    const XpTok* toks;
    uint32_t tok_count;
    const XpSwitchCase* cases;
    uint32_t case_count;
    const XpStr* strs;
    uint32_t str_count;
    const char* bytes;
    uint32_t byte_count;
    const XpErr* errs;
    uint32_t err_count;
    const XpErrLabel* err_labels;
    uint32_t err_label_count;
    void* block;
} XlcParseResult;

/// Parses `count` tokens at `toks` (the `XlcLexToken` layout; the array ends with an end-of-file
/// token) and hands back the flat AST. `file` is stamped into every span. Syntax errors are
/// recorded in the `errs` arena, not thrown across the boundary.
XlcParseResult XLC_PARSE_CALL xlcParse(const void* toks, uint32_t count, uint32_t file);

/// Releases a result returned by `xlcParse`. A default-constructed result is a no-op.
void XLC_PARSE_CALL xlcParseFree(XlcParseResult result);

#ifdef __cplusplus
}  // extern "C"
#endif
