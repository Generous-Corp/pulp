// migration_runtime.cpp — Expression evaluator, hop filter, text/JSON
// renderers for the embedded migration index.
//
// The generated data table lives in `migration_index.cpp` (autogen).
// This TU owns all runtime logic so it can be unit-tested without the
// generated table — tests construct in-memory `MigrationEntry` vectors
// and call the helpers directly.

#include "migration_index.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace pulp::cli::migration {

namespace {

// ── Semver helpers (local — intentionally not reusing version_diag's
// parser to keep this TU link-free). ─────────────────────────────────────────

struct Semver {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool ok = false;
};

Semver parse_semver(std::string_view s) {
    // Accept optional leading 'v' / 'V'.
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) {
        s.remove_prefix(1);
    }
    Semver out;
    int parts[3] = {0, 0, 0};
    std::size_t idx = 0;
    std::size_t i = 0;
    while (i < s.size() && idx < 3) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return out;
        int n = 0;
        while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
            n = n * 10 + (s[i] - '0');
            ++i;
        }
        parts[idx++] = n;
        if (idx == 3) break;
        if (i >= s.size() || s[i] != '.') return out;
        ++i;  // consume '.'
    }
    // Trailing pre-release / build metadata tolerated but ignored.
    if (idx < 3) return out;
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    out.ok = true;
    return out;
}

int compare_semver(const Semver& a, const Semver& b) {
    if (std::tie(a.major, a.minor, a.patch) <
        std::tie(b.major, b.minor, b.patch)) return -1;
    if (std::tie(a.major, a.minor, a.patch) >
        std::tie(b.major, b.minor, b.patch)) return 1;
    return 0;
}

// ── Tokeniser for the applies_if expression language. ───────────────────────

enum class TokKind {
    End,
    Ident,      // "cli_version_from", "cli_version_to"
    Version,    // "0.27.0" (optionally prefixed with 'v')
    Op,         // <, <=, >, >=, ==, !=
    And,        // &&
    Or,         // ||
    LParen,
    RParen,
    Invalid,
};

struct Token {
    TokKind kind = TokKind::Invalid;
    std::string text;  // for Ident / Version / Op
};

struct Lexer {
    std::string src;
    std::size_t pos = 0;

    void skip_ws() {
        while (pos < src.size() &&
               std::isspace(static_cast<unsigned char>(src[pos]))) {
            ++pos;
        }
    }

    Token next() {
        skip_ws();
        Token t;
        if (pos >= src.size()) { t.kind = TokKind::End; return t; }
        char c = src[pos];

        if (c == '(') { ++pos; t.kind = TokKind::LParen; return t; }
        if (c == ')') { ++pos; t.kind = TokKind::RParen; return t; }

        if (c == '&' && pos + 1 < src.size() && src[pos + 1] == '&') {
            pos += 2; t.kind = TokKind::And; return t;
        }
        if (c == '|' && pos + 1 < src.size() && src[pos + 1] == '|') {
            pos += 2; t.kind = TokKind::Or; return t;
        }
        if (c == '<' || c == '>' || c == '=' || c == '!') {
            // Accept two-char ops (<=, >=, ==, !=) and one-char (<, >).
            t.kind = TokKind::Op;
            if (pos + 1 < src.size() && src[pos + 1] == '=') {
                t.text = src.substr(pos, 2);
                pos += 2;
            } else if (c == '<' || c == '>') {
                t.text = std::string(1, c);
                ++pos;
            } else {
                t.kind = TokKind::Invalid;
                ++pos;
            }
            return t;
        }

        // Version literal: optional 'v'/'V' followed by a digit, then M.N.P.
        // Checked BEFORE identifier so `v0.27.0` parses as Version, not
        // as an ident `v` trailed by a parse error on `0.27.0`.
        if ((c == 'v' || c == 'V') && pos + 1 < src.size() &&
            std::isdigit(static_cast<unsigned char>(src[pos + 1]))) {
            auto start = pos;
            ++pos;  // consume 'v'/'V'
            while (pos < src.size() &&
                   (std::isdigit(static_cast<unsigned char>(src[pos])) ||
                    src[pos] == '.')) {
                ++pos;
            }
            t.kind = TokKind::Version;
            t.text = src.substr(start, pos - start);
            return t;
        }

        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            auto start = pos;
            while (pos < src.size() &&
                   (std::isalnum(static_cast<unsigned char>(src[pos])) ||
                    src[pos] == '_')) {
                ++pos;
            }
            t.kind = TokKind::Ident;
            t.text = src.substr(start, pos - start);
            return t;
        }

        if (std::isdigit(static_cast<unsigned char>(c))) {
            auto start = pos;
            while (pos < src.size() &&
                   (std::isdigit(static_cast<unsigned char>(src[pos])) ||
                    src[pos] == '.')) {
                ++pos;
            }
            t.kind = TokKind::Version;
            t.text = src.substr(start, pos - start);
            return t;
        }

        t.kind = TokKind::Invalid;
        ++pos;
        return t;
    }
};

// ── Parser — recursive descent, fail-closed. ────────────────────────────────

struct Parser {
    Lexer& lx;
    Token cur;
    bool failed = false;
    const EvalContext& ctx;

    Parser(Lexer& l, const EvalContext& c) : lx(l), ctx(c) { cur = lx.next(); }

    void advance() { cur = lx.next(); }

    bool parse_expr(bool& out) {
        if (!parse_or(out)) return false;
        return cur.kind == TokKind::End;
    }

    bool parse_or(bool& out) {
        bool lhs;
        if (!parse_and(lhs)) return false;
        while (cur.kind == TokKind::Or) {
            advance();
            bool rhs;
            if (!parse_and(rhs)) return false;
            lhs = lhs || rhs;
        }
        out = lhs;
        return true;
    }

    bool parse_and(bool& out) {
        bool lhs;
        if (!parse_cmp(lhs)) return false;
        while (cur.kind == TokKind::And) {
            advance();
            bool rhs;
            if (!parse_cmp(rhs)) return false;
            lhs = lhs && rhs;
        }
        out = lhs;
        return true;
    }

    bool parse_cmp(bool& out) {
        if (cur.kind == TokKind::LParen) {
            advance();
            if (!parse_or(out)) return false;
            if (cur.kind != TokKind::RParen) return false;
            advance();
            return true;
        }
        if (cur.kind != TokKind::Ident) return false;
        const std::string ident = cur.text;
        advance();
        if (cur.kind != TokKind::Op) return false;
        const std::string op = cur.text;
        advance();
        if (cur.kind != TokKind::Version) return false;
        Semver rhs = parse_semver(cur.text);
        advance();
        if (!rhs.ok) return false;

        std::string lhs_str;
        if (ident == "cli_version_from") lhs_str = ctx.version_from;
        else if (ident == "cli_version_to") lhs_str = ctx.version_to;
        else return false;

        Semver lhs = parse_semver(lhs_str);
        if (!lhs.ok) {
            // Missing/unparseable context — fail closed.
            out = false;
            return true;
        }

        int c = compare_semver(lhs, rhs);
        if (op == "<")  out = c <  0;
        else if (op == "<=") out = c <= 0;
        else if (op == ">")  out = c >  0;
        else if (op == ">=") out = c >= 0;
        else if (op == "==") out = c == 0;
        else if (op == "!=") out = c != 0;
        else return false;
        return true;
    }
};

std::string escape_json(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(
                                      static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────────

bool evaluate_applies_if(const std::string& expr, const EvalContext& ctx) {
    // Empty expression: vacuously applicable.
    bool only_ws = true;
    for (char c : expr) {
        if (!std::isspace(static_cast<unsigned char>(c))) { only_ws = false; break; }
    }
    if (only_ws) return true;

    Lexer lx{expr, 0};
    Parser p(lx, ctx);
    bool out = false;
    if (!p.parse_expr(out)) return false;  // fail closed
    return out;
}

std::vector<const MigrationEntry*> entries_for_hop(const std::string& from,
                                                   const std::string& to) {
    std::vector<const MigrationEntry*> hits;
    Semver sf = parse_semver(from);
    Semver st = parse_semver(to);
    if (!sf.ok || !st.ok) return hits;
    if (compare_semver(sf, st) >= 0) return hits;  // from >= to → no hop

    for (std::size_t i = 0; i < kMigrationIndexSize; ++i) {
        const auto& e = kMigrationIndex[i];
        Semver sv = parse_semver(e.version);
        if (!sv.ok) continue;
        if (compare_semver(sv, sf) <= 0) continue;  // strictly newer than from
        if (compare_semver(sv, st) >  0) continue;  // <= to
        hits.push_back(&e);
    }
    std::sort(hits.begin(), hits.end(),
              [](const MigrationEntry* a, const MigrationEntry* b) {
                  return compare_semver(parse_semver(a->version),
                                        parse_semver(b->version)) < 0;
              });
    return hits;
}

std::vector<const MigrationEntry*> applicable_entries(const std::string& from,
                                                      const std::string& to) {
    auto hits = entries_for_hop(from, to);
    std::vector<const MigrationEntry*> kept;
    kept.reserve(hits.size());
    EvalContext ctx{from, to};
    for (const auto* e : hits) {
        std::string expr(e->applies_if);
        if (evaluate_applies_if(expr, ctx)) kept.push_back(e);
    }
    return kept;
}

std::string render_notes_text(const std::vector<const MigrationEntry*>& entries,
                              const std::string& from,
                              const std::string& to) {
    std::ostringstream os;
    os << "Pulp migration notes: " << from << " -> " << to << "\n";
    if (entries.empty()) {
        os << "\n  No migration notes apply to this upgrade.\n";
        return os.str();
    }
    for (const auto* e : entries) {
        os << "\n── v" << e->version;
        if (e->breaking) os << "  [breaking]";
        os << " ──\n";
        if (!e->summary.empty()) os << e->summary << "\n";
        if (!e->body_markdown.empty()) {
            os << "\n" << e->body_markdown;
            if (!e->body_markdown.empty() && e->body_markdown.back() != '\n')
                os << "\n";
        }
    }
    return os.str();
}

std::string render_notes_json(const std::vector<const MigrationEntry*>& entries,
                              const std::string& from,
                              const std::string& to) {
    // Top-level breaking summary so tooling and agents can branch on a single
    // boolean / count without parsing every entry — the signal to "inherit"
    // breaking-change knowledge for a pin hop before touching code.
    std::size_t breaking_count = 0;
    for (const auto* e : entries)
        if (e->breaking) ++breaking_count;

    std::ostringstream os;
    os << "{\n";
    os << "  \"from\": \"" << escape_json(from) << "\",\n";
    os << "  \"to\": \""   << escape_json(to)   << "\",\n";
    os << "  \"has_breaking\": " << (breaking_count > 0 ? "true" : "false") << ",\n";
    os << "  \"breaking_count\": " << breaking_count << ",\n";
    os << "  \"entries\": [";
    bool first = true;
    for (const auto* e : entries) {
        os << (first ? "\n" : ",\n");
        first = false;
        os << "    {\n";
        os << "      \"version\": \""   << escape_json(e->version)     << "\",\n";
        os << "      \"breaking\": "    << (e->breaking ? "true" : "false") << ",\n";
        os << "      \"summary\": \""   << escape_json(e->summary)     << "\",\n";
        os << "      \"applies_if\": \""<< escape_json(e->applies_if)  << "\",\n";
        os << "      \"body\": \""      << escape_json(e->body_markdown)<< "\"\n";
        os << "    }";
    }
    if (!first) os << "\n  ";
    os << "]\n";
    os << "}\n";
    return os.str();
}

}  // namespace pulp::cli::migration
