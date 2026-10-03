// design_import_claude_css.cpp — pure Claude Design HTML/CSS scanning

#include <pulp/view/design_sources.hpp>
#include <choc/text/choc_JSON.h>
#include <choc/text/choc_StringUtilities.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace pulp::view {


// ── Claude Design classname extraction ───────────────────────────────────
//
// Mirrors Spectr's `tools/extract-html-bundle/extract.mjs` classname
// pass: pull every `<style>...</style>` block, parse the CSS rules, and
// emit `classname → { cssProp(camelCase): cssValue, ... }`. The
// `@pulp/css-adapt` layer downstream consumes this map to merge
// class-based styles into inline before forwarding to bridge calls.

namespace {

// Convert a CSS hyphen-cased property name (`font-family`) to a
// JS-friendly camelCase key (`fontFamily`). Mirrors Spectr's
// `parseDeclarationsToCamelCase` so the artifacts stay byte-compatible.
// Pure string-ops — no allocation beyond the result.
std::string css_prop_to_camel_case(const std::string& prop) {
    std::string out;
    out.reserve(prop.size());
    bool upper_next = false;
    for (char c : prop) {
        if (c == '-') {
            upper_next = true;
        } else if (upper_next) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            upper_next = false;
        } else {
            out += c;
        }
    }
    return out;
}

// Strip CSS `/* ... */` comments from a block. Multi-line safe. This
// runs before the rule walker so brace counting there never has to
// reason about braces that live inside a comment.
std::string strip_css_comments(const std::string& css) {
    std::string out;
    out.reserve(css.size());
    size_t i = 0;
    while (i < css.size()) {
        if (i + 1 < css.size() && css[i] == '/' && css[i + 1] == '*') {
            auto end = css.find("*/", i + 2);
            if (end == std::string::npos) break;  // unterminated — drop rest
            i = end + 2;
        } else {
            out += css[i++];
        }
    }
    return out;
}

// Parse a CSS declaration block body (the text between `{` and `}`)
// into camelCase prop → value pairs. Splits on `;`, then on the first
// `:` per declaration. Skips empty declarations and bare colons.
std::map<std::string, std::string> parse_css_declarations(const std::string& body) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < body.size()) {
        auto semi = body.find(';', i);
        std::string decl = body.substr(i, (semi == std::string::npos ? body.size() : semi) - i);
        i = (semi == std::string::npos) ? body.size() : semi + 1;
        auto colon = decl.find(':');
        if (colon == std::string::npos) continue;
        auto prop = std::string(choc::text::trim(std::string_view(decl).substr(0, colon)));
        auto value = std::string(choc::text::trim(std::string_view(decl).substr(colon + 1)));
        if (prop.empty() || value.empty()) continue;
        out[css_prop_to_camel_case(prop)] = value;
    }
    return out;
}

// Advance past a CSS string literal. `quote_at` indexes the opening
// `'` or `"`; returns the index just past the closing quote, or
// `css.size()` if the literal is unterminated. Backslash escapes are
// honored so `content: "\""` doesn't end the literal early.
size_t skip_css_string(const std::string& css, size_t quote_at) {
    const char quote = css[quote_at];
    for (size_t i = quote_at + 1; i < css.size(); ++i) {
        if (css[i] == '\\') {
            ++i;  // consume the escaped char
            continue;
        }
        if (css[i] == quote) return i + 1;
    }
    return css.size();
}

// Find the `}` that closes the block opening at `open`, counting
// nested braces so an at-rule body (`@media { .a {} .b {} }`) is
// consumed whole rather than stopping at its first inner `}`. Braces
// inside string literals don't count. Returns npos when unbalanced.
size_t find_matching_brace(const std::string& css, size_t open) {
    int depth = 0;
    for (size_t i = open; i < css.size(); ++i) {
        const char c = css[i];
        if (c == '"' || c == '\'') {
            i = skip_css_string(css, i) - 1;
            continue;
        }
        if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

// Walk a CSS source string (the inside of one `<style>` block) and
// merge every top-level `.classname { ... }` rule into `into`. Skips
// at-rules (anything that begins with `@`), `:root` blocks, descendant
// / pseudo-class selectors (anything with whitespace, `:`, `>` or `,`
// between the dot and the `{`), and `.scheme-*` selectors (those are
// already handled as theme-mode token overrides upstream).
//
// Rules nested inside an at-rule are deliberately NOT collected: a
// `@media` / `@supports` body is conditional, so hoisting its rules
// into the unconditional classname map would make a responsive or
// dark-mode override apply always. Consuming each at-rule body whole
// (balanced braces) is what keeps the walker in sync — stopping at the
// first inner `}` would both leak the at-rule's later rules and
// misparse the following top-level rule's selector.
//
// We hand-walk character-by-character rather than regex because:
//   1. Brace nesting is unbounded (`@media { @supports { … } }`), which
//      a regex cannot match.
//   2. The selector list can include commas, so we need to split on
//      `,` and apply the same body to every classname in the list.
void collect_classnames_from_css(const std::string& css_in,
                                 ClaudeClassNameRules& into) {
    auto css = strip_css_comments(css_in);
    size_t i = 0;
    while (i < css.size()) {
        // Skip whitespace.
        while (i < css.size() && std::isspace(static_cast<unsigned char>(css[i]))) ++i;
        if (i >= css.size()) break;

        // Scan the prelude for whichever comes first: the `{` that
        // opens a body, or the `;` that terminates a body-less
        // statement at-rule (`@import url(…);`, `@charset "utf-8";`).
        // Quoted spans are stepped over so a `{` or `;` inside a string
        // doesn't split the prelude.
        size_t scan = i;
        size_t open = std::string::npos;
        bool statement_at_rule = false;
        while (scan < css.size()) {
            const char c = css[scan];
            if (c == '"' || c == '\'') {
                scan = skip_css_string(css, scan);
                continue;
            }
            if (c == '{') { open = scan; break; }
            if (c == ';') { statement_at_rule = true; break; }
            ++scan;
        }

        // A body-less statement — consume it and move on. Without this
        // the `;` would be swallowed into the *next* rule's selector
        // list, disqualifying an otherwise plain classname rule.
        if (statement_at_rule) {
            i = scan + 1;
            continue;
        }
        if (open == std::string::npos) break;
        std::string selector_list = css.substr(i, open - i);

        // Find the matching `}`, counting nested braces.
        auto close = find_matching_brace(css, open);
        if (close == std::string::npos) break;
        std::string body = css.substr(open + 1, close - (open + 1));
        i = close + 1;

        // Skip at-rules (`@media`, `@font-face`, `@keyframes`, etc.).
        // The first non-whitespace char of the selector tells us. The
        // whole balanced body is already consumed above, so nested
        // at-rules are skipped along with it.
        auto first_non_ws = selector_list.find_first_not_of(" \t\r\n");
        if (first_non_ws == std::string::npos) continue;
        if (selector_list[first_non_ws] == '@') continue;

        // Split selector_list on top-level commas.
        std::vector<std::string> selectors;
        size_t s_start = 0;
        for (size_t k = 0; k <= selector_list.size(); ++k) {
            if (k == selector_list.size() || selector_list[k] == ',') {
                selectors.push_back(std::string(choc::text::trim(
                    std::string_view(selector_list).substr(s_start, k - s_start))));
                s_start = k + 1;
            }
        }

        // Parse the body once — every matching selector references the
        // same map.
        std::optional<std::map<std::string, std::string>> decls;

        for (auto& sel : selectors) {
            // Only accept simple `.classname` selectors. The classname
            // grammar matches Spectr's regex: `[a-zA-Z][a-zA-Z0-9_-]*`.
            // A trailing chained selector (`.foo .bar`, `.foo > .bar`,
            // `.foo:hover`, `.foo[data-x]`) means this isn't a plain
            // classname rule — skip it.
            if (sel.empty() || sel[0] != '.') continue;
            std::string name;
            size_t k = 1;
            if (k >= sel.size() || !(std::isalpha(static_cast<unsigned char>(sel[k])) || sel[k] == '_'))
                continue;
            while (k < sel.size() &&
                   (std::isalnum(static_cast<unsigned char>(sel[k])) ||
                    sel[k] == '_' || sel[k] == '-')) {
                name += sel[k++];
            }
            // Anything left over → not a plain classname selector.
            if (k != sel.size()) continue;
            if (name.empty()) continue;
            // Theme-scope selectors are handled upstream as token
            // overrides, not classname rules.
            if (name.rfind("scheme-", 0) == 0) continue;

            if (!decls) decls = parse_css_declarations(body);
            if (decls->empty()) continue;

            // Cascade: later blocks override earlier ones for the same
            // classname. Per-prop merge keeps unrelated declarations
            // from being lost when two blocks define the same class.
            auto& existing = into[name];
            for (auto& [prop, val] : *decls) {
                existing[prop] = val;
            }
        }
    }
}

// Pull every `<style>...</style>` block out of an HTML string, in
// document order. Skips `<style>` blocks whose first 200 chars contain
// `font-face` (matches Spectr's filter — those carry only `@font-face`
// rules, no classnames). Returns the inner CSS bodies.
std::vector<std::string> extract_html_style_blocks(const std::string& html) {
    std::vector<std::string> blocks;
    static const std::regex style_re(
        R"RX(<style\b[^>]*>([\s\S]*?)</style>)RX",
        std::regex::icase);
    auto begin = std::sregex_iterator(html.begin(), html.end(), style_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::string body = (*it)[1].str();
        // Spectr's filter: skip blocks whose head looks like @font-face.
        // The head check (first 200 chars) keeps a normal classname
        // block that happens to mention `font-face` later from being
        // dropped.
        std::string head = body.substr(0, std::min<size_t>(body.size(), 200));
        std::transform(head.begin(), head.end(), head.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (head.find("font-face") != std::string::npos) continue;
        blocks.push_back(std::move(body));
    }
    return blocks;
}

std::optional<std::string> extract_bundler_template_html(const std::string& html) {
    const std::string opener_dq = "<script type=\"__bundler/template\"";
    const std::string opener_sq = "<script type='__bundler/template'";
    size_t tag_start = html.find(opener_dq);
    size_t header_len = opener_dq.size();
    if (tag_start == std::string::npos) {
        tag_start = html.find(opener_sq);
        header_len = opener_sq.size();
        if (tag_start == std::string::npos) return std::nullopt;
    }

    const size_t open_end = html.find('>', tag_start + header_len);
    if (open_end == std::string::npos) return std::nullopt;

    const size_t close = html.find("</script>", open_end + 1);
    if (close == std::string::npos) return std::nullopt;

    try {
        auto value = choc::json::parseValue(html.substr(open_end + 1, close - (open_end + 1)));
        if (!value.isString()) return std::nullopt;
        return std::string(value.getString());
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace

ClaudeClassNameRules extract_claude_classnames(const std::string& html) {
    ClaudeClassNameRules rules;

    // Walk the raw HTML's <style> blocks first. For non-bundled
    // exports (the `--no-bundle` flow, or hand-written test fixtures)
    // this is the only source.
    for (auto& css : extract_html_style_blocks(html)) {
        collect_classnames_from_css(css, rules);
    }

    // For self-bundled Claude Design exports, the actual app CSS lives inside
    // the JSON-encoded `<script type="__bundler/template">` payload. Decode
    // only that template here so the static importer stays linkable from the
    // core view library without pulling in the runtime-import JS harness.
    if (auto template_html = extract_bundler_template_html(html)) {
        for (auto& css : extract_html_style_blocks(*template_html)) {
            collect_classnames_from_css(css, rules);
        }
    }

    return rules;
}

bool looks_like_bundler_entry(const std::string& html) {
    if (html.empty()) return false;

    auto contains = [&](const char* needle) {
        return html.find(needle) != std::string::npos;
    };

    // Standard mount points (React, Vue, Svelte, @pulp/react).
    const bool has_mount_root =
        contains("id=\"root\"")        || contains("id='root'") ||
        contains("id=\"app\"")         || contains("id='app'")  ||
        contains("id=\"__pulp_root\"") || contains("id='__pulp_root'");

    // Script tags that pull in a bundled JS entry. We don't try to
    // identify whether the script *is* a bundle — just that the page
    // is structured to load one.
    const bool has_script_src =
        contains("<script src=")                  ||
        contains("<script type=\"module\" src=")  ||
        contains("import(\"./")                   || contains("import('./");

    // Bundler-emitted markers (`__bundler_*`, "Unpacking..." status, the
    // @pulp/react runtime, React dev-tools hooks). These rarely show up
    // in hand-authored Claude Design HTML, so a single hit is enough.
    const bool has_bundler_hint =
        contains("__bundler")        || contains("Unpacking")     ||
        contains("data-reactroot")   || contains("@pulp/react");

    // Either (mount + script) — vanilla shell — or any unambiguous
    // bundler-specific marker.
    return (has_mount_root && has_script_src) || has_bundler_hint;
}

std::string serialize_claude_classnames(const ClaudeClassNameRules& rules) {
    // Use choc::value::createObject for stable, well-escaped JSON. The
    // outer map is a std::map so keys arrive in alphabetical order
    // already; per-class declaration maps are also std::map for the
    // same property-order guarantee. choc::json::toString preserves
    // insertion order, so the resulting JSON is deterministic.
    auto root = choc::value::createObject("");
    for (const auto& [name, decls] : rules) {
        auto obj = choc::value::createObject("");
        for (const auto& [prop, val] : decls) {
            obj.addMember(prop, val);
        }
        root.addMember(name, obj);
    }
    // Pretty-print with line breaks so the artifact is human-readable
    // and diff-friendly (matches Spectr's `JSON.stringify(_, null, 2)`
    // output shape for parity with the existing tooling).
    return choc::json::toString(root, /*useLineBreaks=*/true);
}

} // namespace pulp::view
