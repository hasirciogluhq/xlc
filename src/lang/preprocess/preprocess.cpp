#include "xlang/lang/preprocess.h"

#include <format>
#include <map>
#include <memory>
#include <string>

namespace xlang {

namespace {

struct Failure {};

/// Result of evaluating a condition.
using Value = ConfigValue;

struct CondNode {
    enum class Kind { Literal, Name, Defined, Not, Binary } kind{Kind::Literal};
    Value literal;
    std::string name;
    TokenKind op{TokenKind::End};
    std::unique_ptr<CondNode> lhs;
    std::unique_ptr<CondNode> rhs;
    Span span;
};

using CondPtr = std::unique_ptr<CondNode>;

class Preprocessor {
public:
    Preprocessor(const BuildConfig& config, DiagnosticEngine& diag) : config_(config), diag_(diag) {}

    std::vector<Token> run(std::vector<Token> tokens) {
        toks_ = std::move(tokens);
        std::vector<Token> out;
        if (!toks_.empty() && toks_.back().kind == TokenKind::End) {
            process(0, toks_.size() - 1, true, out);
            out.push_back(toks_.back());
        } else {
            process(0, toks_.size(), true, out);
        }
        return out;
    }

private:
    static bool isDirective(TokenKind k) { return k >= TokenKind::DirIf && k <= TokenKind::DirError; }
    static bool opensConditional(TokenKind k) {
        return k == TokenKind::DirIf || k == TokenKind::DirIfdef || k == TokenKind::DirIfndef;
    }

    void fail(const Span& span, std::string message, std::string label) {
        if (quiet_ > 0) {
            return;
        }
        diag_.error("E0003", span, std::move(message), std::move(label));
    }

    // --- condition parsing ----------------------------------------------------------------

    struct Cursor {
        std::size_t pos;
        std::size_t end;
        int paren_depth{0};
    };

    [[nodiscard]] const Token& at(const Cursor& c) const { return toks_[std::min(c.pos, c.end - 1)]; }
    [[nodiscard]] bool more(const Cursor& c) const { return c.pos < c.end; }
    /// An operator token may continue the condition: same line, or inside parentheses.
    [[nodiscard]] bool continues(const Cursor& c) const {
        return more(c) && (!toks_[c.pos].newline_before || c.paren_depth > 0);
    }

    CondPtr parseOr(Cursor& c) {
        CondPtr lhs = parseAnd(c);
        while (continues(c) && toks_[c.pos].kind == TokenKind::OrOr) {
            lhs = binary(TokenKind::OrOr, std::move(lhs), (++c.pos, parseAnd(c)));
        }
        return lhs;
    }

    CondPtr parseAnd(Cursor& c) {
        CondPtr lhs = parseCompare(c);
        while (continues(c) && toks_[c.pos].kind == TokenKind::AndAnd) {
            lhs = binary(TokenKind::AndAnd, std::move(lhs), (++c.pos, parseCompare(c)));
        }
        return lhs;
    }

    CondPtr parseCompare(Cursor& c) {
        CondPtr lhs = parseUnary(c);
        if (continues(c)) {
            const TokenKind k = toks_[c.pos].kind;
            if (k == TokenKind::EqEq || k == TokenKind::NotEq || k == TokenKind::Lt ||
                k == TokenKind::Le || k == TokenKind::Gt || k == TokenKind::Ge) {
                ++c.pos;
                lhs = binary(k, std::move(lhs), parseUnary(c));
            }
        }
        return lhs;
    }

    static CondPtr binary(TokenKind op, CondPtr lhs, CondPtr rhs) {
        auto n = std::make_unique<CondNode>();
        n->kind = CondNode::Kind::Binary;
        n->op = op;
        n->span = Span::merge(lhs->span, rhs->span);
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        return n;
    }

    CondPtr parseUnary(Cursor& c) {
        if (!more(c)) {
            fail(toks_[c.end - 1].span, "expected a condition", "condition ends here");
            throw Failure{};
        }
        const Token& t = toks_[c.pos];
        if (t.kind == TokenKind::Bang) {
            ++c.pos;
            auto n = std::make_unique<CondNode>();
            n->kind = CondNode::Kind::Not;
            n->span = t.span;
            n->lhs = parseUnary(c);
            return n;
        }
        return parsePrimary(c);
    }

    CondPtr parsePrimary(Cursor& c) {
        const Token& t = toks_[c.pos];
        auto n = std::make_unique<CondNode>();
        n->span = t.span;
        switch (t.kind) {
        case TokenKind::Int:
            n->literal = Value::makeInt(static_cast<std::int64_t>(t.int_value));
            ++c.pos;
            return n;
        case TokenKind::Minus:
            if (c.pos + 1 < c.end && toks_[c.pos + 1].kind == TokenKind::Int) {
                n->literal = Value::makeInt(-static_cast<std::int64_t>(toks_[c.pos + 1].int_value));
                c.pos += 2;
                return n;
            }
            break;
        case TokenKind::String:
            n->literal = Value::makeString(t.text);
            ++c.pos;
            return n;
        case TokenKind::True:
        case TokenKind::False:
            n->literal = Value::makeBool(t.kind == TokenKind::True);
            ++c.pos;
            return n;
        case TokenKind::LParen: {
            ++c.pos;
            ++c.paren_depth;
            CondPtr inner = parseOr(c);
            --c.paren_depth;
            if (!more(c) || toks_[c.pos].kind != TokenKind::RParen) {
                fail(more(c) ? toks_[c.pos].span : t.span, "expected `)` in the condition",
                     "unclosed parenthesis");
                throw Failure{};
            }
            ++c.pos;
            return inner;
        }
        case TokenKind::Ident:
            if (t.text == "defined" && c.pos + 3 < c.end + 0 &&
                toks_[c.pos + 1].kind == TokenKind::LParen &&
                toks_[c.pos + 2].kind == TokenKind::Ident &&
                toks_[c.pos + 3].kind == TokenKind::RParen) {
                n->kind = CondNode::Kind::Defined;
                n->name = toks_[c.pos + 2].text;
                c.pos += 4;
                return n;
            }
            n->kind = CondNode::Kind::Name;
            n->name = t.text;
            ++c.pos;
            return n;
        default: break;
        }
        fail(t.span, std::format("expected a condition, found {}", describeToken(t)),
             "not valid in a conditional-compilation condition");
        throw Failure{};
    }

    // --- evaluation ------------------------------------------------------------------------

    [[nodiscard]] std::optional<Value> nameValue(const std::string& name) const {
        if (auto v = config_.lookup(name)) {
            // `-D` wins over a file `define` of the same name, builtins lose to nothing here.
            return v;
        }
        if (const auto it = file_defines_.find(name); it != file_defines_.end()) {
            return it->second;
        }
        return std::nullopt;
    }

    [[nodiscard]] bool definedName(const std::string& name) const {
        return config_.isDefined(name) || file_defines_.contains(name);
    }

    static std::string kindName(const Value& v) {
        switch (v.kind) {
        case Value::Kind::Bool: return "bool";
        case Value::Kind::Int: return "integer";
        case Value::Kind::String: return "string";
        }
        return "?";
    }

    Value evaluate(const CondNode& n) {
        switch (n.kind) {
        case CondNode::Kind::Literal:
            return n.literal;
        case CondNode::Kind::Defined:
            return Value::makeBool(definedName(n.name));
        case CondNode::Kind::Name: {
            if (auto v = nameValue(n.name)) {
                return *v;
            }
            diag_.error("E0003", n.span, std::format("`{}` is not defined", n.name),
                        "unknown name in a condition");
            throw Failure{};
        }
        case CondNode::Kind::Not: {
            return Value::makeBool(!truth(evaluate(*n.lhs), n.lhs->span));
        }
        case CondNode::Kind::Binary:
            break;
        }

        if (n.op == TokenKind::AndAnd) {
            return Value::makeBool(truth(evaluate(*n.lhs), n.lhs->span) &&
                                   truth(evaluate(*n.rhs), n.rhs->span));
        }
        if (n.op == TokenKind::OrOr) {
            return Value::makeBool(truth(evaluate(*n.lhs), n.lhs->span) ||
                                   truth(evaluate(*n.rhs), n.rhs->span));
        }
        const Value l = evaluate(*n.lhs);
        const Value r = evaluate(*n.rhs);
        if (l.kind != r.kind) {
            fail(n.span, std::format("cannot compare {} with {}", kindName(l), kindName(r)),
                 "operands have different kinds");
            throw Failure{};
        }
        int cmp = 0;
        switch (l.kind) {
        case Value::Kind::Bool: cmp = static_cast<int>(l.bool_value) - static_cast<int>(r.bool_value); break;
        case Value::Kind::Int: cmp = l.int_value < r.int_value ? -1 : (l.int_value > r.int_value ? 1 : 0); break;
        case Value::Kind::String: cmp = l.string_value.compare(r.string_value); break;
        }
        switch (n.op) {
        case TokenKind::EqEq: return Value::makeBool(cmp == 0);
        case TokenKind::NotEq: return Value::makeBool(cmp != 0);
        case TokenKind::Lt: return Value::makeBool(cmp < 0);
        case TokenKind::Le: return Value::makeBool(cmp <= 0);
        case TokenKind::Gt: return Value::makeBool(cmp > 0);
        default: return Value::makeBool(cmp >= 0);
        }
    }

    bool truth(const Value& v, const Span& span) {
        switch (v.kind) {
        case Value::Kind::Bool: return v.bool_value;
        case Value::Kind::Int: return v.int_value != 0;
        case Value::Kind::String:
            fail(span, "a string is not a condition", "compare it with `==` or use `defined(...)`");
            throw Failure{};
        }
        return false;
    }

    // --- structure -------------------------------------------------------------------------

    /// End (exclusive) of the line holding `from` (first later token that starts a line).
    [[nodiscard]] std::size_t lineEnd(std::size_t from, std::size_t end) const {
        std::size_t i = from + 1;
        while (i < end && !toks_[i].newline_before) {
            ++i;
        }
        return i;
    }

    void skipLine(std::size_t& i, std::size_t end) const { i = lineEnd(i, end); }

    void process(std::size_t begin, std::size_t end, bool active, std::vector<Token>& out) {
        std::size_t i = begin;
        while (i < end) {
            const Token& t = toks_[i];
            if (!isDirective(t.kind)) {
                if (active) {
                    noteDefine(i, end);
                    out.push_back(t);
                }
                ++i;
                continue;
            }
            switch (t.kind) {
            case TokenKind::DirIf:
            case TokenKind::DirIfdef:
            case TokenKind::DirIfndef:
                i = conditional(i, end, active, out);
                break;
            case TokenKind::DirError:
                directiveError(i, end, active);
                skipLine(i, end);
                break;
            default:
                fail(t.span, std::format("`{}` without a matching `#if`", tokenKindName(t.kind)),
                     "no open conditional here");
                skipLine(i, end);
                break;
            }
        }
    }

    void directiveError(std::size_t i, std::size_t end, bool active) {
        if (!active) {
            return;
        }
        std::string message = "#error";
        if (i + 1 < end && toks_[i + 1].kind == TokenKind::String && !toks_[i + 1].newline_before) {
            message = toks_[i + 1].text;
        }
        diag_.error("E0003", toks_[i].span, message, "`#error` directive reached");
    }

    /// Records `define NAME = literal` (line start, active code) for later conditions.
    void noteDefine(std::size_t i, std::size_t end) {
        std::size_t k = i;
        if (toks_[k].kind == TokenKind::Export && k + 1 < end) {
            ++k;
        }
        if (toks_[k].kind != TokenKind::Define || k + 3 >= end + 0 ||
            toks_[k + 1].kind != TokenKind::Ident || toks_[k + 2].kind != TokenKind::Assign) {
            return;
        }
        const Token& v = toks_[k + 3];
        const bool single = k + 4 >= end || toks_[k + 4].newline_before;
        if (!single) {
            return;
        }
        const std::string& name = toks_[k + 1].text;
        switch (v.kind) {
        case TokenKind::Int: file_defines_[name] = Value::makeInt(static_cast<std::int64_t>(v.int_value)); break;
        case TokenKind::String: file_defines_[name] = Value::makeString(v.text); break;
        case TokenKind::True: file_defines_[name] = Value::makeBool(true); break;
        case TokenKind::False: file_defines_[name] = Value::makeBool(false); break;
        default: break;
        }
    }

    /// Parses the condition of the directive at `i`; returns the first token after it.
    CondPtr condition(std::size_t i, std::size_t end, std::size_t& next) {
        const Token& d = toks_[i];
        if (d.kind == TokenKind::DirIfdef || d.kind == TokenKind::DirIfndef) {
            if (i + 1 >= end || toks_[i + 1].kind != TokenKind::Ident || toks_[i + 1].newline_before) {
                fail(d.span, std::format("`{}` needs a name", tokenKindName(d.kind)),
                     "expected a name after the directive");
                throw Failure{};
            }
            auto n = std::make_unique<CondNode>();
            n->kind = CondNode::Kind::Defined;
            n->name = toks_[i + 1].text;
            n->span = toks_[i + 1].span;
            next = i + 2;
            if (d.kind == TokenKind::DirIfndef) {
                auto neg = std::make_unique<CondNode>();
                neg->kind = CondNode::Kind::Not;
                neg->span = n->span;
                neg->lhs = std::move(n);
                return neg;
            }
            return n;
        }
        if (i + 1 >= end || toks_[i + 1].newline_before) {
            fail(d.span, std::format("`{}` needs a condition", tokenKindName(d.kind)),
                 "expected a condition after the directive");
            throw Failure{};
        }
        Cursor c{i + 1, end, 0};
        CondPtr n = parseOr(c);
        next = c.pos;
        return n;
    }

    std::size_t conditional(std::size_t i, std::size_t end, bool active, std::vector<Token>& out) {
        const Token& opener = toks_[i];
        std::size_t body = 0;
        CondPtr cond;
        try {
            cond = condition(i, end, body);
        } catch (const Failure&) {
            // Malformed condition: drop the line and treat the body as inactive.
            std::size_t next = lineEnd(i, end);
            if (next < end && toks_[next].newline_before) {
                return skipBlock(next, end);
            }
            return next;
        }

        const bool single_line = body < end && !toks_[body].newline_before;
        if (single_line) {
            const std::size_t line_end = lineEnd(body, end);
            bool take = false;
            if (active) {
                try {
                    take = truth(evaluate(*cond), cond->span);
                } catch (const Failure&) {
                    take = false;
                }
            }
            if (take) {
                const std::size_t first = out.size();
                process(body, line_end, true, out);
                if (out.size() > first) {
                    out[first].newline_before = true;
                }
            }
            return line_end;
        }

        // Block form: split into branches at depth 0.
        struct Branch {
            CondPtr cond;  // null = #else
            std::size_t begin;
            std::size_t end;
            Span span;
        };
        std::vector<Branch> branches;
        branches.push_back({std::move(cond), body, body, opener.span});
        std::size_t j = body;
        int depth = 0;
        bool closed = false;
        while (j < end) {
            const TokenKind k = toks_[j].kind;
            if (opensConditional(k)) {
                // A single-line nested conditional has no `#endif`.
                std::size_t nested_next = 0;
                bool nested_single = false;
                ++quiet_;
                try {
                    CondPtr nested = condition(j, end, nested_next);
                    nested_single = nested_next < end && !toks_[nested_next].newline_before;
                } catch (const Failure&) {
                    nested_single = true;
                }
                --quiet_;
                if (!nested_single) {
                    ++depth;
                }
                ++j;
                continue;
            }
            if (k == TokenKind::DirEndif) {
                if (depth == 0) {
                    branches.back().end = j;
                    closed = true;
                    ++j;
                    break;
                }
                --depth;
                ++j;
                continue;
            }
            if ((k == TokenKind::DirElif || k == TokenKind::DirElse) && depth == 0) {
                branches.back().end = j;
                Branch next;
                next.span = toks_[j].span;
                std::size_t after = j + 1;
                if (k == TokenKind::DirElif) {
                    try {
                        next.cond = condition(j, end, after);
                    } catch (const Failure&) {
                        next.cond = std::make_unique<CondNode>();  // literal false
                        next.cond->kind = CondNode::Kind::Literal;
                        next.cond->literal = Value::makeBool(false);
                        after = lineEnd(j, end);
                    }
                }
                next.begin = after;
                next.end = after;
                branches.push_back(std::move(next));
                j = after;
                continue;
            }
            ++j;
        }
        if (!closed) {
            fail(opener.span, "`#if` is never closed", "no matching `#endif`");
            branches.back().end = end;
        }

        bool taken = false;
        for (Branch& b : branches) {
            bool take = false;
            if (!taken) {
                if (!b.cond) {
                    take = true;
                } else if (active) {
                    try {
                        take = truth(evaluate(*b.cond), b.cond->span);
                    } catch (const Failure&) {
                        take = false;
                    }
                }
            }
            if (take) {
                taken = true;
            }
            process(b.begin, b.end, active && take, out);
        }
        return j;
    }

    /// Skips a block-form conditional starting at `i` (used after a malformed condition).
    std::size_t skipBlock(std::size_t i, std::size_t end) const {
        int depth = 0;
        for (std::size_t j = i; j < end; ++j) {
            if (opensConditional(toks_[j].kind)) {
                ++depth;
            } else if (toks_[j].kind == TokenKind::DirEndif) {
                if (depth == 0) {
                    return j + 1;
                }
                --depth;
            }
        }
        return end;
    }

    const BuildConfig& config_;
    DiagnosticEngine& diag_;
    std::vector<Token> toks_;
    std::map<std::string, Value> file_defines_;
    int quiet_{0};
};

}  // namespace

std::vector<Token> preprocess(std::vector<Token> tokens, const BuildConfig& config,
                              DiagnosticEngine& diagnostics) {
    Preprocessor pp(config, diagnostics);
    return pp.run(std::move(tokens));
}

}  // namespace xlang
