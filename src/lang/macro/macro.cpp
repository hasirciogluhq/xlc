#include "xlang/lang/macro.h"

#include <format>
#include <set>

namespace xlang {

namespace {

[[nodiscard]] bool adjacent(const Token& a, const Token& b) {
    return a.span.end_line == b.span.line && a.span.end_column == b.span.column;
}

[[nodiscard]] bool opens(TokenKind k) {
    return k == TokenKind::LParen || k == TokenKind::LBracket || k == TokenKind::LBrace;
}

[[nodiscard]] bool closes(TokenKind k) {
    return k == TokenKind::RParen || k == TokenKind::RBracket || k == TokenKind::RBrace;
}

class Expander {
public:
    Expander(MacroTable& table, DiagnosticEngine& diag) : table_(table), diag_(diag) {}

    std::vector<Token> run(std::vector<Token> tokens) {
        collectDefinitions(tokens);
        std::vector<Token> out;
        expandRange(tokens, 0, tokens.size(), 0, {}, out);
        return out;
    }

private:
    struct Origin {
        Span call;
        std::string name;
    };

    void error(const Span& span, std::string message, std::string label,
               const std::vector<Origin>& chain) {
        Diagnostic d;
        d.code = "E0004";
        d.severity = Severity::Error;
        d.message = std::move(message);
        d.labels.push_back({span, std::move(label)});
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            d.labels.push_back({it->call, std::format("in the expansion of `{}!`", it->name)});
        }
        diag_.report(std::move(d));
    }

    /// Index just past the token that closes the group opened at `open_index`.
    [[nodiscard]] static std::size_t matching(const std::vector<Token>& t, std::size_t open_index,
                                              std::size_t end) {
        int depth = 0;
        for (std::size_t i = open_index; i < end; ++i) {
            if (opens(t[i].kind)) {
                ++depth;
            } else if (closes(t[i].kind)) {
                if (--depth == 0) {
                    return i + 1;
                }
            }
        }
        return end;
    }

    // --- definitions -----------------------------------------------------------------------

    void collectDefinitions(const std::vector<Token>& t) {
        for (std::size_t i = 0; i + 2 < t.size(); ++i) {
            if (t[i].kind != TokenKind::Define || t[i + 1].kind != TokenKind::Ident ||
                t[i + 2].kind != TokenKind::LParen) {
                continue;
            }
            MacroDef def;
            def.name = t[i + 1].text;
            def.span = t[i + 1].span;
            const std::size_t params_end = matching(t, i + 2, t.size());
            std::size_t p = i + 3;
            while (p + 1 < params_end) {
                if (t[p].kind != TokenKind::Ident || t[p + 1].kind != TokenKind::Colon) {
                    break;
                }
                MacroParam param;
                param.name = t[p].text;
                param.span = t[p].span;
                const Token& kind = t[p + 2 < params_end ? p + 2 : p];
                if (kind.kind == TokenKind::Type) {
                    param.kind = MacroParam::Kind::Type;
                } else if (kind.text == "ident") {
                    param.kind = MacroParam::Kind::Ident;
                } else if (kind.text == "block") {
                    param.kind = MacroParam::Kind::Block;
                } else if (kind.text == "items") {
                    param.kind = MacroParam::Kind::Items;
                } else {
                    param.kind = MacroParam::Kind::Expr;
                }
                def.params.push_back(std::move(param));
                p += 3;
                if (p < params_end && t[p].kind == TokenKind::Comma) {
                    ++p;
                }
            }
            if (params_end >= t.size() || t[params_end].kind != TokenKind::LBrace) {
                continue;  // malformed: the parser reports it
            }
            const std::size_t body_end = matching(t, params_end, t.size());
            def.body.assign(t.begin() + static_cast<std::ptrdiff_t>(params_end) + 1,
                            t.begin() + static_cast<std::ptrdiff_t>(body_end) - 1);
            if (table_.macros.contains(def.name)) {
                diag_.error("E0004", def.span, std::format("macro `{}` is defined twice", def.name),
                            "second definition");
            } else {
                table_.macros.emplace(def.name, std::move(def));
            }
            i = body_end - 1;
        }
    }

    // --- expansion -------------------------------------------------------------------------

    struct CallArgs {
        std::vector<std::vector<Token>> args;
        std::size_t end{0};  // index past the closing `)`
    };

    static CallArgs splitArgs(const std::vector<Token>& t, std::size_t open_paren, std::size_t limit) {
        CallArgs call;
        const std::size_t close = matching(t, open_paren, limit);
        call.end = close;
        std::vector<Token> cur;
        int depth = 0;
        bool any = false;
        for (std::size_t i = open_paren + 1; i + 1 < close; ++i) {
            const TokenKind k = t[i].kind;
            if (depth == 0 && k == TokenKind::Comma) {
                call.args.push_back(std::move(cur));
                cur.clear();
                continue;
            }
            if (opens(k)) {
                ++depth;
            } else if (closes(k)) {
                --depth;
            }
            any = true;
            cur.push_back(t[i]);
        }
        if (any || !call.args.empty()) {
            call.args.push_back(std::move(cur));
        }
        return call;
    }

    [[nodiscard]] static bool startsType(const Token& t) {
        switch (t.kind) {
        case TokenKind::PrimType:
        case TokenKind::Ident:
        case TokenKind::Star:
        case TokenKind::Fn:
        case TokenKind::LBracket:
            return true;
        default:
            return false;
        }
    }

    bool checkArg(const MacroDef& def, std::size_t index, const std::vector<Token>& arg,
                  const Span& call_span, const std::vector<Origin>& chain) {
        const MacroParam& p = def.params[index];
        const auto bad = [&](std::string what) {
            error(arg.empty() ? call_span : arg.front().span,
                  std::format("argument {} of `{}!` ({}) {}", index + 1, def.name, p.name, what),
                  std::format("`{}` is declared `{}`", p.name, kindName(p.kind)), chain);
            return false;
        };
        switch (p.kind) {
        case MacroParam::Kind::Expr:
            return arg.empty() ? bad("is empty, an expression is expected") : true;
        case MacroParam::Kind::Type:
            return (!arg.empty() && startsType(arg.front())) ? true : bad("is not a type");
        case MacroParam::Kind::Ident:
            return (arg.size() == 1 && arg.front().kind == TokenKind::Ident) ? true
                                                                              : bad("is not a single identifier");
        case MacroParam::Kind::Block:
            return (arg.size() >= 2 && arg.front().kind == TokenKind::LBrace &&
                    arg.back().kind == TokenKind::RBrace)
                       ? true
                       : bad("is not a `{ ... }` block");
        case MacroParam::Kind::Items:
            return true;
        }
        return true;
    }

    static const char* kindName(MacroParam::Kind k) {
        switch (k) {
        case MacroParam::Kind::Expr: return "expr";
        case MacroParam::Kind::Type: return "type";
        case MacroParam::Kind::Ident: return "ident";
        case MacroParam::Kind::Block: return "block";
        case MacroParam::Kind::Items: return "items";
        }
        return "?";
    }

    std::vector<Token> substitute(const MacroDef& def, const std::vector<std::vector<Token>>& args,
                                  const Token& call) {
        // Hygiene: names bound by `local` inside the body (and not parameters) get fresh names.
        std::set<std::string> params;
        for (const MacroParam& p : def.params) {
            params.insert(p.name);
        }
        std::map<std::string, std::string> renamed;
        ++expansion_counter_;
        for (std::size_t i = 0; i + 1 < def.body.size(); ++i) {
            if (def.body[i].kind == TokenKind::Local && def.body[i + 1].kind == TokenKind::Ident &&
                !params.contains(def.body[i + 1].text)) {
                const std::string& n = def.body[i + 1].text;
                renamed.emplace(n, std::format("{}__m{}", n, expansion_counter_));
            }
        }

        std::vector<Token> out;
        for (std::size_t i = 0; i < def.body.size(); ++i) {
            const Token& t = def.body[i];
            if (t.kind == TokenKind::Ident) {
                const bool after_dot = i > 0 && def.body[i - 1].kind == TokenKind::Dot;
                if (!after_dot) {
                    std::size_t index = def.params.size();
                    for (std::size_t k = 0; k < def.params.size(); ++k) {
                        if (def.params[k].name == t.text) {
                            index = k;
                            break;
                        }
                    }
                    if (index < def.params.size() && index < args.size()) {
                        const std::vector<Token>& arg = args[index];
                        const bool group = def.params[index].kind == MacroParam::Kind::Expr && arg.size() > 1;
                        if (group) {
                            Token open = t;
                            open.kind = TokenKind::LParen;
                            open.text.clear();
                            out.push_back(open);
                        }
                        for (std::size_t a = 0; a < arg.size(); ++a) {
                            Token copy = arg[a];
                            if (a == 0 && !group) {
                                copy.newline_before = t.newline_before;
                            } else if (a == 0) {
                                copy.newline_before = false;
                            }
                            out.push_back(std::move(copy));
                        }
                        if (group) {
                            Token close = t;
                            close.kind = TokenKind::RParen;
                            close.text.clear();
                            close.newline_before = false;
                            out.push_back(close);
                        }
                        continue;
                    }
                    if (const auto it = renamed.find(t.text); it != renamed.end()) {
                        Token copy = t;
                        copy.text = it->second;
                        out.push_back(std::move(copy));
                        continue;
                    }
                }
            }
            out.push_back(t);
        }
        if (!out.empty()) {
            out.front().newline_before = call.newline_before;
        }
        return out;
    }

    void expandRange(const std::vector<Token>& t, std::size_t begin, std::size_t end, int depth,
                     const std::vector<Origin>& chain, std::vector<Token>& out) {
        std::size_t i = begin;
        while (i < end) {
            // Do not expand inside definitions: they expand where they are used.
            if (t[i].kind == TokenKind::Define && i + 2 < end && t[i + 1].kind == TokenKind::Ident &&
                t[i + 2].kind == TokenKind::LParen) {
                const std::size_t params_end = matching(t, i + 2, end);
                const std::size_t body_end =
                    (params_end < end && t[params_end].kind == TokenKind::LBrace)
                        ? matching(t, params_end, end)
                        : params_end;
                for (std::size_t k = i; k < body_end; ++k) {
                    out.push_back(t[k]);
                }
                i = body_end;
                continue;
            }

            const bool call = t[i].kind == TokenKind::Ident && i + 2 < end &&
                              t[i + 1].kind == TokenKind::Bang && t[i + 2].kind == TokenKind::LParen &&
                              adjacent(t[i], t[i + 1]) && adjacent(t[i + 1], t[i + 2]);
            const auto found = call ? table_.macros.find(t[i].text) : table_.macros.end();
            if (!call || found == table_.macros.end()) {
                out.push_back(t[i]);
                ++i;
                continue;
            }

            const MacroDef& def = found->second;
            const Token& head = t[i];
            std::vector<Origin> next_chain = chain;
            next_chain.push_back({head.span, def.name});

            const CallArgs parsed = splitArgs(t, i + 2, end);
            i = parsed.end;

            if (depth >= kMacroDepthLimit) {
                error(head.span, std::format("macro expansion is deeper than {}", kMacroDepthLimit),
                      "recursion limit reached", chain);
                continue;
            }
            if (parsed.args.size() != def.params.size()) {
                error(head.span,
                      std::format("`{}!` takes {} argument(s), {} given", def.name, def.params.size(),
                                  parsed.args.size()),
                      "wrong number of arguments", chain);
                continue;
            }
            bool ok = true;
            for (std::size_t a = 0; a < parsed.args.size(); ++a) {
                ok = checkArg(def, a, parsed.args[a], head.span, chain) && ok;
            }
            if (!ok) {
                continue;
            }

            std::vector<Token> body = substitute(def, parsed.args, head);
            std::vector<Token> expanded;
            expandRange(body, 0, body.size(), depth + 1, next_chain, expanded);

            // Expression position: keep the result a single operand.
            const bool statement_position =
                head.newline_before || out.empty() || out.back().kind == TokenKind::LBrace ||
                out.back().kind == TokenKind::RBrace;
            if (!statement_position) {
                Token open = head;
                open.kind = TokenKind::LParen;
                open.text.clear();
                Token close = head;
                close.kind = TokenKind::RParen;
                close.text.clear();
                close.newline_before = false;
                out.push_back(open);
                for (Token& e : expanded) {
                    out.push_back(std::move(e));
                }
                out.push_back(close);
            } else {
                for (Token& e : expanded) {
                    out.push_back(std::move(e));
                }
            }
        }
    }

    MacroTable& table_;
    DiagnosticEngine& diag_;
    int expansion_counter_{0};
};

}  // namespace

std::vector<Token> expandMacros(std::vector<Token> tokens, MacroTable& table,
                                DiagnosticEngine& diagnostics) {
    Expander expander(table, diagnostics);
    // Keep the End token out of the scan.
    Token end;
    bool has_end = !tokens.empty() && tokens.back().kind == TokenKind::End;
    if (has_end) {
        end = tokens.back();
        tokens.pop_back();
    }
    std::vector<Token> out = expander.run(std::move(tokens));
    if (has_end) {
        out.push_back(std::move(end));
    }
    return out;
}

}  // namespace xlang
