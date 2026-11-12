#include "xlang/lang/ast.h"

#include <format>
#include <ostream>
#include <sstream>
#include <type_traits>

namespace xlang {

const char* unaryOpName(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg:
        return "-";
    case UnaryOp::Not:
        return "!";
    case UnaryOp::Deref:
        return "*";
    case UnaryOp::AddrOf:
        return "&";
    case UnaryOp::BitNot:
        return "~";
    }
    return "?";
}

const char* binaryOpName(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add:
        return "+";
    case BinaryOp::Sub:
        return "-";
    case BinaryOp::Mul:
        return "*";
    case BinaryOp::Div:
        return "/";
    case BinaryOp::Mod:
        return "%";
    case BinaryOp::Eq:
        return "==";
    case BinaryOp::Ne:
        return "!=";
    case BinaryOp::Lt:
        return "<";
    case BinaryOp::Le:
        return "<=";
    case BinaryOp::Gt:
        return ">";
    case BinaryOp::Ge:
        return ">=";
    case BinaryOp::And:
        return "&&";
    case BinaryOp::Or:
        return "||";
    case BinaryOp::BitAnd:
        return "&";
    case BinaryOp::BitOr:
        return "|";
    case BinaryOp::BitXor:
        return "^";
    case BinaryOp::Shl:
        return "<<";
    case BinaryOp::Shr:
        return ">>";
    }
    return "?";
}

const DeclBase& declBase(const Item& item) {
    return std::visit([](const auto& decl) -> const DeclBase& { return decl; }, item.decl);
}

const char* declKindName(const Decl& decl) {
    struct Visitor {
        const char* operator()(const FnDecl&) const { return "function"; }
        const char* operator()(const GlobalDecl& g) const {
            return g.is_const ? "const" : "global";
        }
        const char* operator()(const StructDecl&) const { return "struct"; }
        const char* operator()(const ClassDecl&) const { return "class"; }
        const char* operator()(const InterfaceDecl&) const { return "interface"; }
        const char* operator()(const ImplDecl&) const { return "impl"; }
        const char* operator()(const EnumDecl&) const { return "enum"; }
        const char* operator()(const TypeAliasDecl&) const { return "type alias"; }
        const char* operator()(const DefineConstDecl&) const { return "define"; }
        const char* operator()(const DefineMacroDecl&) const { return "macro"; }
        const char* operator()(const ImportDecl&) const { return "import"; }
        const char* operator()(const ExportFromDecl&) const { return "re-export"; }
        const char* operator()(const MacroCallDecl&) const { return "macro call"; }
        const char* operator()(const TopLevelCallDecl&) const { return "call"; }
        const char* operator()(const FnAliasDecl&) const { return "function alias"; }
    };
    return std::visit(Visitor{}, decl);
}

namespace {

class Dumper {
  public:
    explicit Dumper(std::ostream& out) : out_(out) {}

    void line(std::string_view text) { out_ << std::string(indent_ * 2, ' ') << text << '\n'; }

    struct Scope {
        Dumper& d;
        explicit Scope(Dumper& dumper) : d(dumper) { ++d.indent_; }
        ~Scope() { --d.indent_; }
    };

    static std::string modifiers(const Modifiers& m) {
        std::string out;
        if (m.export_c) {
            out += "export(c) ";
        } else if (m.exported) {
            out += "export ";
        }
        if (m.declared) {
            out += "declare ";
        }
        if (m.registered) {
            out += m.register_role.empty() ? "register "
                                           : std::format("register({}) ", m.register_role);
        }
        return out;
    }

    static std::string attributes(const std::vector<Attribute>& attrs) {
        std::string out;
        for (const Attribute& a : attrs) {
            out += "@" + a.name;
            if (!a.args.empty()) {
                out += "(";
                for (std::size_t i = 0; i < a.args.size(); ++i) {
                    const AttributeArg& arg = a.args[i];
                    if (i != 0) {
                        out += ", ";
                    }
                    if (!arg.key.empty()) {
                        out += arg.key + " = ";
                    }
                    switch (arg.kind) {
                    case AttributeArg::Kind::Ident:
                        out += arg.text;
                        break;
                    case AttributeArg::Kind::Int:
                        out += std::to_string(arg.number);
                        break;
                    case AttributeArg::Kind::String:
                        out += "\"" + arg.text + "\"";
                        break;
                    }
                }
                out += ")";
            }
            out += " ";
        }
        return out;
    }

    static std::string params(const std::vector<Param>& ps) {
        std::string out;
        for (std::size_t i = 0; i < ps.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            out += ps[i].is_self ? "self"
                                 : std::format("{}: {}", ps[i].name, typeToString(ps[i].type));
        }
        return out;
    }

    static std::string signature(const FnDecl& fn) {
        std::string out = std::format("fn {}({})", fn.name, params(fn.params));
        if (fn.has_return_type) {
            out += " -> " + typeToString(fn.return_type);
        }
        return out;
    }

    static std::string where(const Span& s) { return std::format("{}:{}", s.line, s.column); }

    std::string expr(const Expr& e) {
        switch (e.kind) {
        case Expr::Kind::IntLiteral:
            return std::to_string(e.int_value);
        case Expr::Kind::FloatLiteral: {
            std::string text = std::format("{}", e.float_value);
            if (text.find_first_of(".en") == std::string::npos) {
                text += ".0";
            }
            return text;
        }
        case Expr::Kind::BoolLiteral:
            return e.bool_value ? "true" : "false";
        case Expr::Kind::StringLiteral:
            return "\"" + escape(e.text) + "\"";
        case Expr::Kind::ByteLiteral:
            return std::format("b'\\x{:02x}'", e.int_value);
        case Expr::Kind::Name:
            return e.text;
        case Expr::Kind::Unary:
            return std::format("({}{})", unaryOpName(e.unary_op), expr(*e.lhs));
        case Expr::Kind::Binary:
            return std::format("({} {} {})", expr(*e.lhs), binaryOpName(e.binary_op), expr(*e.rhs));
        case Expr::Kind::Call: {
            std::string out = expr(*e.lhs) + "(";
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                out += (i != 0 ? ", " : "") + expr(*e.args[i]);
            }
            return out + ")";
        }
        case Expr::Kind::Ternary:
            return std::format("({} ? {} : {})", expr(*e.lhs), expr(*e.rhs), expr(*e.args[0]));
        case Expr::Kind::Try:
            return expr(*e.lhs) + "?";
        case Expr::Kind::Index:
            return std::format("{}[{}]", expr(*e.lhs), expr(*e.rhs));
        case Expr::Kind::Slice:
            return std::format("{}[{}..{}]",
                               expr(*e.lhs),
                               e.args.empty() ? std::string() : expr(*e.args[0]),
                               e.rhs ? expr(*e.rhs) : std::string());
        case Expr::Kind::Field:
            return std::format(
                "{}.{}", expr(*e.lhs), e.text.empty() ? std::to_string(e.int_value) : e.text);
        case Expr::Kind::Cast:
            return std::format("({} as {})", expr(*e.lhs), typeToString(e.type));
        case Expr::Kind::ArrayList: {
            std::string out = "[";
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                out += (i != 0 ? ", " : "") + expr(*e.args[i]);
            }
            return out + "]";
        }
        case Expr::Kind::ArrayRepeat:
            return std::format("[{}; {}]", expr(*e.lhs), expr(*e.rhs));
        case Expr::Kind::TupleLiteral: {
            std::string out = "(";
            for (std::size_t i = 0; i < e.args.size(); ++i) {
                out += (i != 0 ? ", " : "") + expr(*e.args[i]);
            }
            if (e.args.size() == 1) {
                out += ',';
            }
            return out + ")";
        }
        case Expr::Kind::StructLiteral:
            return typeToString(e.type) + fieldInits(e);
        case Expr::Kind::New:
            return std::format("new{} {}{}",
                               e.lhs ? "(" + expr(*e.lhs) + ")" : std::string(),
                               typeToString(e.type),
                               fieldInits(e));
        case Expr::Kind::Closure: {
            std::string out = e.closure->is_move ? "move |" : "|";
            for (std::size_t i = 0; i < e.closure->params.size(); ++i) {
                const Param& p = e.closure->params[i];
                out += (i != 0 ? ", " : "") + p.name;
                if (p.has_type) {
                    out += ": " + typeToString(p.type);
                }
            }
            out += "|";
            if (e.closure->has_return_type) {
                out += " -> " + typeToString(e.closure->return_type);
            }
            const Block& body = e.closure->body;
            return out + (e.closure->expr_body ? " " + expr(*body.statements.front().expr)
                                               : std::format(" {{ {} statement(s) }}",
                                                             body.statements.size()));
        }
        case Expr::Kind::Spread:
            return ".." + e.text;
        case Expr::Kind::AsmOperand:
            return std::format("{}({}) {}", e.text, expr(*e.lhs), expr(*e.rhs));
        case Expr::Kind::MacroCall: {
            std::string out = e.text + "!(";
            for (std::size_t i = 0; i < e.macro_args.size(); ++i) {
                out += (i != 0 ? ", " : "") + tokens(e.macro_args[i]);
            }
            return out + ")";
        }
        }
        return "?";
    }

    static std::string tokens(const std::vector<Token>& ts) {
        std::string out;
        for (std::size_t i = 0; i < ts.size(); ++i) {
            if (i != 0) {
                out += " ";
            }
            const Token& t = ts[i];
            out += t.kind == TokenKind::String ? "\"" + escape(t.text) + "\"" : tokenText(t);
        }
        return out;
    }

    static std::string tokenText(const Token& t) {
        switch (t.kind) {
        case TokenKind::Ident:
        case TokenKind::PrimType:
        case TokenKind::Int:
        case TokenKind::Float:
            return t.text;
        default:
            return tokenKindName(t.kind);
        }
    }

    std::string fieldInits(const Expr& e) {
        std::string out = " {";
        for (std::size_t i = 0; i < e.fields.size(); ++i) {
            out += (i != 0 ? ", " : " ") + e.fields[i].name + ": " + expr(*e.fields[i].value);
        }
        return out + (e.fields.empty() ? "}" : " }");
    }

    static std::string escape(const std::string& s) {
        std::string out;
        for (const char c : s) {
            switch (c) {
            case '\n':
                out += "\\n";
                break;
            case '\t':
                out += "\\t";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\0':
                out += "\\0";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\\':
                out += "\\\\";
                break;
            default:
                out += c;
            }
        }
        return out;
    }

    static std::string labelPrefix(const Stmt& s) {
        return s.label.empty() ? std::string() : s.label + ": ";
    }

    void stmt(const Stmt& s) {
        const std::string attrs = attributes(s.attributes);
        switch (s.kind) {
        case Stmt::Kind::Local:
            line(std::format("{}local {}{}{}",
                             attrs,
                             s.name,
                             s.has_type ? ": " + typeToString(s.type) : std::string(),
                             s.expr ? " = " + expr(*s.expr) : std::string()));
            break;
        case Stmt::Kind::Delete:
            line(std::format("{}delete {}", attrs, expr(*s.expr)));
            break;
        case Stmt::Kind::Const:
            line(std::format("{}const {}{}{}",
                             attrs,
                             s.name,
                             s.has_type ? ": " + typeToString(s.type) : std::string(),
                             s.expr ? " = " + expr(*s.expr) : std::string()));
            break;
        case Stmt::Kind::Assign:
            line(std::format("{}{} {}= {}",
                             attrs,
                             expr(*s.target),
                             s.has_compound_op ? binaryOpName(s.compound_op) : "",
                             expr(*s.expr)));
            break;
        case Stmt::Kind::Return:
            line(std::format("{}return{}", attrs, s.expr ? " " + expr(*s.expr) : std::string()));
            break;
        case Stmt::Kind::Expr:
            line(attrs + expr(*s.expr));
            break;
        case Stmt::Kind::If: {
            line(std::format("{}if {}", attrs, expr(*s.expr)));
            {
                Scope scope(*this);
                block(*s.body);
            }
            if (s.else_body) {
                if (s.else_is_if) {
                    line("else");
                    Scope scope(*this);
                    block(*s.else_body);
                } else {
                    line("else");
                    Scope scope(*this);
                    block(*s.else_body);
                }
            }
            break;
        }
        case Stmt::Kind::While: {
            line(std::format("{}{}while {}", attrs, labelPrefix(s), expr(*s.expr)));
            Scope scope(*this);
            block(*s.body);
            break;
        }
        case Stmt::Kind::DoWhile: {
            line(std::format("{}{}do-while {}", attrs, labelPrefix(s), expr(*s.expr)));
            Scope scope(*this);
            block(*s.body);
            break;
        }
        case Stmt::Kind::For: {
            line(std::format("{}{}for", attrs, labelPrefix(s)));
            Scope scope(*this);
            if (s.init) {
                line("init");
                Scope inner(*this);
                stmt(*s.init);
            }
            if (s.expr) {
                line("cond " + expr(*s.expr));
            }
            if (s.step) {
                line("step");
                Scope inner(*this);
                stmt(*s.step);
            }
            block(*s.body);
            break;
        }
        case Stmt::Kind::ForRange: {
            line(std::format("{}{}for {} in {} .. {}",
                             attrs,
                             labelPrefix(s),
                             s.name,
                             expr(*s.expr),
                             expr(*s.expr2)));
            Scope scope(*this);
            block(*s.body);
            break;
        }
        case Stmt::Kind::ForPack: {
            line(std::format("{}{}for {} in {}", attrs, labelPrefix(s), s.name, expr(*s.expr)));
            Scope scope(*this);
            block(*s.body);
            break;
        }
        case Stmt::Kind::ForEach: {
            const std::string var =
                s.names.empty() ? s.name : std::format("{}, {}", s.name, s.names[0]);
            line(std::format("{}{}for {} in {}", attrs, labelPrefix(s), var, expr(*s.expr)));
            Scope scope(*this);
            block(*s.body);
            break;
        }
        case Stmt::Kind::Switch: {
            line(s.expr ? std::format("{}switch {}", attrs, expr(*s.expr)) : attrs + "switch");
            Scope scope(*this);
            for (const SwitchCase& c : s.cases) {
                std::string head = c.is_default ? "default" : "case";
                for (std::size_t i = 0; i < c.values.size(); ++i) {
                    head += (i != 0 ? ", " : " ") + expr(*c.values[i]);
                }
                line(head);
                Scope inner(*this);
                block(*c.body);
            }
            break;
        }
        case Stmt::Kind::Defer: {
            line(attrs + "defer");
            Scope scope(*this);
            stmt(*s.step);
            break;
        }
        case Stmt::Kind::Try: {
            line(attrs + "try");
            {
                Scope scope(*this);
                block(*s.body);
            }
            line("catch" + (s.name.empty() ? std::string() : " " + s.name));
            Scope scope(*this);
            block(*s.else_body);
            break;
        }
        case Stmt::Kind::Continue:
            line(attrs + "continue" + (s.name.empty() ? std::string() : " " + s.name));
            break;
        case Stmt::Kind::Break:
            line(attrs + "break" + (s.name.empty() ? std::string() : " " + s.name));
            break;
        case Stmt::Kind::Unsafe: {
            line(attrs + "unsafe");
            Scope scope(*this);
            block(*s.body);
            break;
        }
        case Stmt::Kind::Block: {
            line(attrs + "block");
            Scope scope(*this);
            block(*s.body);
            break;
        }
        }
    }

    void block(const Block& b) {
        for (const Stmt& s : b.statements) {
            stmt(s);
        }
    }

    void fnBody(const FnDecl& fn) {
        if (fn.has_body) {
            Scope scope(*this);
            block(fn.body);
        }
    }

    void item(const Item& it) {
        const DeclBase& base = declBase(it);
        const std::string head = attributes(base.attributes) + modifiers(it.modifiers);
        struct Visitor {
            Dumper& d;
            const std::string& head;
            const DeclBase& base;

            void operator()(const FnDecl& fn) const {
                d.line(
                    std::format("{}{}{}", head, signature(fn), fn.has_body ? "" : "  [no body]"));
                d.fnBody(fn);
            }
            void operator()(const GlobalDecl& g) const {
                const std::string type = g.has_type ? ": " + typeToString(g.type) : std::string();
                d.line(std::format("{}{} {}{}{}",
                                   head,
                                   g.is_const ? "const" : "global",
                                   g.name,
                                   type,
                                   g.init ? " = " + d.expr(*g.init) : std::string()));
            }
            void operator()(const StructDecl& s) const {
                d.line(std::format("{}struct {}{}", head, s.name, s.opaque ? "  [opaque]" : ""));
                Scope scope(d);
                for (const FieldDecl& f : s.fields) {
                    d.line(std::format(
                        "{}{}: {}", attributes(f.attributes), f.name, typeToString(f.type)));
                }
            }
            void operator()(const ClassDecl& c) const {
                std::string base_name;
                for (std::size_t i = 0; i < c.base.size(); ++i) {
                    base_name += (i != 0 ? "." : "") + c.base[i];
                }
                d.line(std::format("{}class {}{}{}",
                                   head,
                                   c.name,
                                   c.has_base ? " : " + base_name : std::string(),
                                   c.opaque ? "  [opaque]" : ""));
                Scope scope(d);
                for (const FieldDecl& f : c.fields) {
                    d.line(std::format(
                        "{}{}: {}", attributes(f.attributes), f.name, typeToString(f.type)));
                }
                for (const FnDecl& m : c.methods) {
                    d.method(m);
                }
            }
            void operator()(const InterfaceDecl& i) const {
                d.line(std::format("{}interface {}{}", head, i.name, i.opaque ? "  [opaque]" : ""));
                Scope scope(d);
                for (const FnDecl& m : i.methods) {
                    d.method(m);
                }
            }
            void operator()(const ImplDecl& i) const {
                std::string iface;
                for (std::size_t k = 0; k < i.interface_path.size(); ++k) {
                    iface += (k != 0 ? "." : "") + i.interface_path[k];
                }
                d.line(std::format("{}impl {} for {}", head, iface, typeToString(i.target)));
                Scope scope(d);
                for (const FnDecl& m : i.methods) {
                    d.method(m);
                }
            }
            void operator()(const EnumDecl& e) const {
                d.line(std::format("{}enum {}: {}", head, e.name, typeToString(e.underlying)));
                Scope scope(d);
                for (const EnumMember& m : e.members) {
                    d.line(m.name + (m.value ? " = " + d.expr(*m.value) : std::string()));
                }
            }
            void operator()(const TypeAliasDecl& t) const {
                d.line(std::format("{}type {} = {}", head, t.name, typeToString(t.target)));
            }
            void operator()(const FnAliasDecl& a) const {
                std::string target;
                for (std::size_t i = 0; i < a.target.size(); ++i) {
                    target += (i != 0 ? "." : "") + a.target[i];
                }
                d.line(std::format("{}fn {} = {}", head, a.name, target));
            }
            void operator()(const DefineConstDecl& c) const {
                d.line(std::format("{}define {} = {}", head, c.name, d.expr(*c.value)));
            }
            void operator()(const DefineMacroDecl& m) const {
                std::string ps;
                for (std::size_t i = 0; i < m.params.size(); ++i) {
                    static constexpr const char* kinds[] = {
                        "expr", "type", "ident", "block", "items"};
                    ps += std::format("{}{}: {}",
                                      i != 0 ? ", " : "",
                                      m.params[i].name,
                                      kinds[static_cast<int>(m.params[i].kind)]);
                }
                d.line(std::format("{}define {}({}) {{ {} }}", head, m.name, ps, tokens(m.body)));
            }
            void operator()(const ImportDecl& i) const {
                switch (i.form) {
                case ImportDecl::Form::Module:
                    d.line(std::format(
                        "import \"{}\"{}", i.module, i.alias.empty() ? "" : " as " + i.alias));
                    break;
                case ImportDecl::Form::Star:
                    d.line(std::format("import * from \"{}\"", i.module));
                    break;
                case ImportDecl::Form::StarAs:
                    d.line(std::format("import * as {} from \"{}\"", i.alias, i.module));
                    break;
                case ImportDecl::Form::Names:
                    d.line(std::format("import {} from \"{}\"", names(i.names), i.module));
                    break;
                }
            }
            void operator()(const ExportFromDecl& e) const {
                switch (e.form) {
                case ExportFromDecl::Form::Star:
                    d.line(std::format("export * from \"{}\"{}",
                                       e.module,
                                       e.except.empty() ? "" : " except " + names(e.except)));
                    break;
                case ExportFromDecl::Form::StarAs:
                    d.line(std::format("export * as {} from \"{}\"", e.alias, e.module));
                    break;
                case ExportFromDecl::Form::Names:
                    d.line(std::format("export {{ {} }} from \"{}\"", names(e.names), e.module));
                    break;
                }
            }
            void operator()(const MacroCallDecl& m) const {
                std::string args;
                for (std::size_t i = 0; i < m.args.size(); ++i) {
                    args += (i != 0 ? ", " : "") + tokens(m.args[i]);
                }
                d.line(std::format("{}{}!({})", head, m.name, args));
            }
            void operator()(const TopLevelCallDecl& c) const { d.line(head + d.expr(*c.call)); }

            static std::string names(const std::vector<ImportName>& ns) {
                std::string out;
                for (std::size_t i = 0; i < ns.size(); ++i) {
                    out += (i != 0 ? ", " : "") + ns[i].name;
                    if (!ns[i].alias.empty()) {
                        out += " as " + ns[i].alias;
                    }
                }
                return out;
            }
        };
        std::visit(Visitor{*this, head, base}, it.decl);
    }

    void method(const FnDecl& m) {
        std::string prefix = attributes(m.attributes);
        if (m.is_virtual) {
            prefix += "virtual ";
        }
        if (m.is_override) {
            prefix += "override ";
        }
        line(std::format("{}{}{}", prefix, signature(m), m.has_body ? "" : "  [no body]"));
        fnBody(m);
    }

  private:
    std::ostream& out_;
    std::size_t indent_{0};
};

} // namespace

void dumpProgram(std::ostream& out, const Program& program, bool summary) {
    Dumper d(out);
    for (const Attribute& a : program.file_attributes) {
        d.line(Dumper::attributes({a}));
    }
    if (!summary) {
        for (const Item& item : program.items) {
            d.item(item);
        }
        return;
    }
    for (const Item& item : program.items) {
        const DeclBase& base = declBase(item);
        if (std::holds_alternative<ImportDecl>(item.decl) ||
            std::holds_alternative<ExportFromDecl>(item.decl)) {
            std::ostringstream tmp;
            Dumper(tmp).item(item);
            std::string text = tmp.str();
            while (!text.empty() && text.back() == '\n') {
                text.pop_back();
            }
            d.line(std::format("{}  @{}:{}", text, base.span.line, base.span.column));
            continue;
        }
        std::string line = std::format("{:<10} {}", declKindName(item.decl), base.name);
        const std::string mods = Dumper::modifiers(item.modifiers);
        if (!mods.empty()) {
            line += "  [" + mods.substr(0, mods.size() - 1) + "]";
        }
        if (const auto* fn = std::get_if<FnDecl>(&item.decl)) {
            line += std::format("  params={}", fn->params.size());
            if (fn->has_body) {
                line += std::format(" stmts={}", fn->body.statements.size());
            }
        }
        line += std::format("  @{}:{}", base.span.line, base.span.column);
        d.line(line);
    }
}

} // namespace xlang
