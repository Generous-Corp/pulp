// design_adherence.cpp — lint UI JS against a compiled design contract.

#include <pulp/design/design_adherence.hpp>

#include <pulp/view/design_tokens.hpp>  // token_css_var — single owner of token → var(--x)

#include <choc/text/choc_StringUtilities.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstdlib>
#include <regex>
#include <unordered_map>
#include <unordered_set>

#if defined(_WIN32)
#include <clocale>
#else
#include <locale.h>
#endif

namespace pulp::design {

const char* adherence_kind_name(AdherenceKind kind) {
    switch (kind) {
        case AdherenceKind::raw_color: return "raw-color";
        case AdherenceKind::unknown_token: return "unknown-token";
        case AdherenceKind::raw_dimension: return "raw-dimension";
    }
    return "unknown";
}

namespace {

// Replace comment spans with spaces (preserving length, newlines, and every
// non-comment byte position) so line/column reporting stays exact and string
// literals — where style values live — are still scanned. A single-quote,
// double-quote, or backtick span is treated as a string; `//` and `/* */`
// outside a string become spaces.
std::string scrub_comments(const std::string& src) {
    std::string out = src;
    enum class St { code, line_comment, block_comment, str } st = St::code;
    char quote = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        char c = out[i];
        char n = (i + 1 < out.size()) ? out[i + 1] : '\0';
        switch (st) {
            case St::code:
                if (c == '/' && n == '/') { st = St::line_comment; out[i] = ' '; }
                else if (c == '/' && n == '*') { st = St::block_comment; out[i] = ' '; }
                else if (c == '\'' || c == '"' || c == '`') { st = St::str; quote = c; }
                break;
            case St::line_comment:
                if (c == '\n') st = St::code;
                else out[i] = ' ';
                break;
            case St::block_comment:
                if (c == '*' && n == '/') { out[i] = ' '; out[i + 1] = ' '; ++i; st = St::code; }
                else if (c != '\n') out[i] = ' ';
                break;
            case St::str:
                if (c == '\\') { ++i; }  // skip the escaped char
                else if (c == quote) st = St::code;
                break;
        }
    }
    return out;
}

// Expand a 3-digit hex ("#abc") to 6 ("#aabbcc"); pass 6/8 through. Input is the
// hex body without '#', already lowercased.
std::string expand_hex(const std::string& body) {
    if (body.size() == 3) {
        std::string out;
        for (char c : body) { out += c; out += c; }
        return out;
    }
    return body;
}

// Shortest round-trip decimal of a numeric string, so "8.0"/"8.00"/"8" all
// compare equal to a dimension token's stored "8".
//
// std::from_chars' floating-point overload is still a =deleted placeholder in
// the libc++ shipped with some toolchains Pulp builds on (the github-hosted
// macOS image behind the release sign lane), so parse against an explicit "C"
// locale via strtod instead. That keeps parsing locale-independent — a
// comma-decimal global locale cannot misparse "8.0" — without the fragile float
// overload. to_chars(float) is universally available and gives the shortest
// round-trip. (pulp::format::detail::parse_double_c_locale is the same idea in
// the format layer; kept local here to avoid a view→format detail dependency.)
std::string normalize_number(const std::string& num) {
    if (num.empty()) return num;
    errno = 0;
    char* end = nullptr;
#if defined(_WIN32)
    static _locale_t c_locale = ::_create_locale(LC_ALL, "C");
    double parsed = ::_strtod_l(num.c_str(), &end, c_locale);
#else
    static ::locale_t c_locale = ::newlocale(LC_ALL_MASK, "C", static_cast<::locale_t>(0));
    const ::locale_t prev = ::uselocale(c_locale);
    double parsed = std::strtod(num.c_str(), &end);
    ::uselocale(prev);
#endif
    if (end == num.c_str() || errno == ERANGE) return num;  // no number / out of range
    float v = static_cast<float>(parsed);
    char buf[32];
    auto [out_end, ec2] = std::to_chars(buf, buf + sizeof(buf), v);
    if (ec2 != std::errc{}) return num;
    return std::string(buf, out_end);
}

}  // namespace

std::vector<AdherenceFinding> lint_adherence(const std::string& js_source,
                                             const DesignManifest& manifest) {
    // Build lookups from the contract.
    std::unordered_set<std::string> valid_css_vars;
    std::unordered_map<std::string, std::string> color_value_to_token;  // "#14171c" → name
    std::unordered_map<std::string, std::string> dim_value_to_token;    // "8" → name
    for (const auto& t : manifest.tokens) {
        valid_css_vars.insert(pulp::view::token_css_var(t.name));
        if (t.kind == "color") color_value_to_token.emplace(choc::text::toLowerCase(t.value), t.name);
        else if (t.kind == "dimension") dim_value_to_token.emplace(t.value, t.name);
    }

    static const std::regex hex_re(R"(#([0-9a-fA-F]+))");
    static const std::regex var_re(R"(var\(\s*(--[A-Za-z0-9_-]+))");
    static const std::regex px_re(R"(\b([0-9]+(?:\.[0-9]+)?)px\b)");

    const std::string scrubbed = scrub_comments(js_source);

    std::vector<AdherenceFinding> findings;
    int line_no = 0;
    size_t pos = 0;
    while (pos <= scrubbed.size()) {
        size_t nl = scrubbed.find('\n', pos);
        std::string line = scrubbed.substr(pos, (nl == std::string::npos ? scrubbed.size() : nl) - pos);
        ++line_no;

        // raw hex colors
        for (auto it = std::sregex_iterator(line.begin(), line.end(), hex_re);
             it != std::sregex_iterator(); ++it) {
            const std::string body = it->str(1);
            if (body.size() != 3 && body.size() != 6 && body.size() != 8) continue;  // not a color
            const int col = static_cast<int>(it->position(0)) + 1;
            const std::string value = "#" + expand_hex(choc::text::toLowerCase(body));
            std::string msg = "raw color " + it->str(0) + " — bind to the theme via var(--token)";
            auto known = color_value_to_token.find(value);
            if (known != color_value_to_token.end())
                msg += "; this value is token `" + known->second + "`";
            findings.push_back({AdherenceKind::raw_color, AdherenceSeverity::error, line_no, col,
                                it->str(0), msg});
        }

        // var(--x) references to unknown tokens
        for (auto it = std::sregex_iterator(line.begin(), line.end(), var_re);
             it != std::sregex_iterator(); ++it) {
            const std::string var_name = it->str(1);
            if (valid_css_vars.count(var_name)) continue;
            const int col = static_cast<int>(it->position(0)) + 1;
            findings.push_back({AdherenceKind::unknown_token, AdherenceSeverity::error, line_no, col,
                                "var(" + var_name + ")",
                                "`" + var_name + "` is not a token in the design contract — if it is "
                                "not defined elsewhere it falls back to the CSS default at runtime"});
        }

        // <n>px literals matching a dimension token's value
        for (auto it = std::sregex_iterator(line.begin(), line.end(), px_re);
             it != std::sregex_iterator(); ++it) {
            auto known = dim_value_to_token.find(normalize_number(it->str(1)));
            if (known == dim_value_to_token.end()) continue;  // only flag values the system names
            const int col = static_cast<int>(it->position(0)) + 1;
            findings.push_back({AdherenceKind::raw_dimension, AdherenceSeverity::info, line_no, col,
                                it->str(0),
                                "raw " + it->str(0) + " matches token `" + known->second +
                                    "` — consider binding to it"});
        }

        if (nl == std::string::npos) break;
        pos = nl + 1;
    }

    std::sort(findings.begin(), findings.end(), [](const AdherenceFinding& a, const AdherenceFinding& b) {
        if (a.line != b.line) return a.line < b.line;
        return a.column < b.column;
    });
    return findings;
}

}  // namespace pulp::design
