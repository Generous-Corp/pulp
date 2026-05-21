// jsx_lock.cpp — Phase 4b Lock-to-source, Path B engine.
//
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md,
//       "Phase 4b — Path B (JSX/TSX AST patch via element
//       instrumentation)". GitHub issue #1308.
//
// Definitions only; declarations live in pulp/view/jsx_lock.hpp.
//
// The engine patches *hand-authored* JSX/TSX. An element is located by
// its instrumentation marker:
//
//     <Knob
//       data-pulp-anchor="sha1:abc123"
//       style={{ padding: 8, background: '#888' }}
//       width={80}
//     />
//
// jsx_lock_tweak_into_source() finds the opening tag carrying the
// matching `data-pulp-anchor`, then either rewrites a property inside
// the inline `style={{…}}` object literal, or rewrites a bare attribute
// value. Anything that is not a plain rewritable literal — a spread, a
// computed key, an expression value — is rejected as `too_dynamic`
// rather than guessed at. See the header for the full design rationale.

#include <pulp/view/jsx_lock.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace pulp::view {

namespace {

// ── Small text helpers (mirror lock_to_source.cpp's local helpers) ──────

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// camelCase a hyphen-/snake-separated fragment: "background-color" →
// "backgroundColor". Identical contract to lock_to_source.cpp's to_camel.
std::string to_camel(const std::string& s) {
    std::string out;
    bool up = false;
    for (char c : s) {
        if (c == '-' || c == '_') {
            up = true;
        } else if (up) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            up = false;
        } else {
            out += c;
        }
    }
    return out;
}

// 1-based line number of byte offset `pos` within `s`.
int line_of(const std::string& s, std::size_t pos) {
    int line = 1;
    for (std::size_t i = 0; i < pos && i < s.size(); ++i)
        if (s[i] == '\n') ++line;
    return line;
}

// True for JS identifier characters (also covers `$`).
bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
}

// Does `value` look like a JS numeric literal (optionally signed,
// optionally fractional)? Used to decide whether to emit a value quoted
// or bare. A trailing CSS unit (`12px`, `1.5rem`) is NOT numeric — it
// must be quoted.
bool looks_numeric(const std::string& value) {
    const std::string t = trim(value);
    if (t.empty()) return false;
    std::size_t i = 0;
    if (t[i] == '+' || t[i] == '-') ++i;
    bool saw_digit = false, saw_dot = false;
    for (; i < t.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(t[i]))) {
            saw_digit = true;
        } else if (t[i] == '.' && !saw_dot) {
            saw_dot = true;
        } else {
            return false;  // a letter / unit / second dot — not a number.
        }
    }
    return saw_digit;
}

// Escape a value for emission inside a JS/JSX string literal body.
std::string escape_js_string_body(const std::string& s, char quote) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '\\' || c == quote) out += '\\';
        out += c;
    }
    return out;
}

// ── Anchor location ─────────────────────────────────────────────────────
//
// Find the opening JSX tag that carries
// `data-pulp-anchor="<anchor>"`. The engine scans for the attribute
// literal, then walks left to the `<` that opens the tag and right to
// the matching `>` so callers get the full opening-tag span.

struct TagSpan {
    bool found = false;
    bool ambiguous = false;
    std::size_t tag_begin = 0;   // index of '<'
    std::size_t tag_end = 0;     // index just past the closing '>'
};

// Locate every `data-pulp-anchor="<anchor>"` occurrence; return the tag
// span when exactly one element matches.
TagSpan find_anchored_tag(const std::string& src, const std::string& anchor) {
    TagSpan span;
    if (anchor.empty()) return span;

    // The instrumentation attribute. Accept single or double quotes.
    const std::string attr = "data-pulp-anchor";
    std::size_t scan = 0;
    int matches = 0;
    std::size_t first_attr_pos = std::string::npos;

    while (true) {
        const std::size_t at = src.find(attr, scan);
        if (at == std::string::npos) break;
        scan = at + attr.size();

        // The match must be a standalone attribute name: preceded by
        // whitespace / `<` and followed (after optional spaces) by `=`.
        if (at > 0 && is_ident_char(src[at - 1])) continue;
        std::size_t i = at + attr.size();
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
        if (i >= src.size() || src[i] != '=') continue;
        ++i;
        while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
        if (i >= src.size() || (src[i] != '"' && src[i] != '\'')) continue;
        const char q = src[i];
        const std::size_t val_begin = i + 1;
        const std::size_t val_end = src.find(q, val_begin);
        if (val_end == std::string::npos) continue;
        const std::string val = src.substr(val_begin, val_end - val_begin);
        if (val != anchor) continue;

        ++matches;
        if (first_attr_pos == std::string::npos) first_attr_pos = at;
    }

    if (matches == 0) return span;
    if (matches > 1) {
        span.ambiguous = true;
        return span;
    }

    // Walk left to the '<' that opens this tag. A JSX opening tag has no
    // bare '>' inside it before the attribute (any `>` would close the
    // tag), so the nearest preceding '<' is the tag start.
    std::size_t lt = first_attr_pos;
    while (lt > 0 && src[lt] != '<') --lt;
    if (src[lt] != '<') return span;  // malformed — no opening bracket.

    // Walk right to the matching '>' that closes the opening tag, while
    // respecting string literals and `{…}` expression braces so a `>`
    // inside `{a > b}` or a quoted value does not end the tag early.
    std::size_t i = first_attr_pos;
    int brace_depth = 0;
    char in_str = 0;
    std::size_t gt = std::string::npos;
    for (; i < src.size(); ++i) {
        const char c = src[i];
        if (in_str) {
            if (c == in_str) in_str = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            in_str = c;
        } else if (c == '{') {
            ++brace_depth;
        } else if (c == '}') {
            if (brace_depth > 0) --brace_depth;
        } else if (c == '>' && brace_depth == 0) {
            gt = i;
            break;
        }
    }
    if (gt == std::string::npos) return span;

    span.found = true;
    span.tag_begin = lt;
    span.tag_end = gt + 1;
    return span;
}

// ── Prop / style-object location within an opening tag ──────────────────

// Result of locating a literal value span inside the opening tag.
struct ValueSpan {
    bool found = false;
    bool too_dynamic = false;
    std::string reason;       // populated when too_dynamic.
    std::size_t begin = 0;    // first byte of the value text to replace.
    std::size_t end = 0;      // one past the last byte to replace.
    bool quoted = false;      // the existing value was a quoted string.
    char quote = '\0';        // the delimiter for quoted values.
    bool expr_braces = false; // the prop used `{…}` (so a number stays bare).
};

// Find `name` as a top-level attribute inside the opening-tag text
// `tag` (already sliced to [<…>]). Returns the index just past the
// attribute name, or npos. Skips matches inside strings / nested braces.
std::size_t find_attr_name(const std::string& tag, const std::string& name) {
    std::size_t scan = 0;
    char in_str = 0;
    int brace_depth = 0;
    for (std::size_t i = 0; i < tag.size(); ++i) {
        const char c = tag[i];
        if (in_str) {
            if (c == in_str) in_str = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') {
            in_str = c;
            continue;
        }
        if (c == '{') { ++brace_depth; continue; }
        if (c == '}') { if (brace_depth > 0) --brace_depth; continue; }
        if (brace_depth != 0) continue;
        if (i != scan) {
            // only consider attribute starts after whitespace / '<'.
        }
        // Try to match `name` here as a standalone attribute.
        if (tag.compare(i, name.size(), name) == 0) {
            const bool left_ok =
                (i == 0) || tag[i - 1] == ' ' || tag[i - 1] == '\t' ||
                tag[i - 1] == '\n' || tag[i - 1] == '\r' || tag[i - 1] == '<';
            const std::size_t after = i + name.size();
            std::size_t j = after;
            while (j < tag.size() && (tag[j] == ' ' || tag[j] == '\t')) ++j;
            const bool right_ok = (j < tag.size() && tag[j] == '=');
            if (left_ok && right_ok) return after;
        }
    }
    return std::string::npos;
}

// Locate a bare attribute's literal value span. `tag` is the opening-tag
// text; `after_name` is the index just past the attribute name.
ValueSpan locate_bare_attr_value(const std::string& tag,
                                 std::size_t after_name) {
    ValueSpan vs;
    std::size_t i = after_name;
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
    if (i >= tag.size() || tag[i] != '=') return vs;
    ++i;
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t' ||
                              tag[i] == '\n' || tag[i] == '\r')) ++i;
    if (i >= tag.size()) return vs;

    if (tag[i] == '"' || tag[i] == '\'') {
        // Quoted string literal: width="80" / color="#888".
        const char q = tag[i];
        const std::size_t b = i + 1;
        const std::size_t e = tag.find(q, b);
        if (e == std::string::npos) return vs;
        vs.found = true;
        vs.quoted = true;
        vs.quote = q;
        vs.begin = b;
        vs.end = e;
        return vs;
    }

    if (tag[i] == '{') {
        // Expression container: width={80} / color={'#888'} / x={g*2}.
        const std::size_t inner_begin = i + 1;
        // Find the matching close brace.
        int depth = 1;
        char in_str = 0;
        std::size_t j = inner_begin;
        for (; j < tag.size(); ++j) {
            const char c = tag[j];
            if (in_str) {
                if (c == in_str) in_str = 0;
                continue;
            }
            if (c == '"' || c == '\'' || c == '`') in_str = c;
            else if (c == '{') ++depth;
            else if (c == '}') { if (--depth == 0) break; }
        }
        if (depth != 0) return vs;  // unbalanced — bail.
        const std::string inner = trim(tag.substr(inner_begin, j - inner_begin));
        if (inner.empty()) {
            vs.too_dynamic = true;
            vs.reason = "empty expression container";
            return vs;
        }
        // A quoted string inside the braces — patch the string literal.
        if ((inner.front() == '\'' || inner.front() == '"') &&
            inner.back() == inner.front() && inner.size() >= 2) {
            // Recover the absolute span of the string contents.
            const std::size_t s_open =
                tag.find(inner.front(), inner_begin);
            const std::size_t s_close = tag.find(inner.front(), s_open + 1);
            if (s_open == std::string::npos || s_close == std::string::npos)
                return vs;
            vs.found = true;
            vs.quoted = true;
            vs.quote = inner.front();
            vs.expr_braces = true;
            vs.begin = s_open + 1;
            vs.end = s_close;
            return vs;
        }
        // A bare numeric literal inside the braces — patch the number.
        if (looks_numeric(inner)) {
            const std::size_t b = tag.find_first_not_of(
                " \t\r\n", inner_begin);
            std::size_t e = j;
            while (e > b && (tag[e - 1] == ' ' || tag[e - 1] == '\t' ||
                             tag[e - 1] == '\r' || tag[e - 1] == '\n'))
                --e;
            vs.found = true;
            vs.quoted = false;
            vs.expr_braces = true;
            vs.begin = b;
            vs.end = e;
            return vs;
        }
        // Anything else inside the braces is an expression we will not
        // rewrite — identifier, member access, arithmetic, call, …
        vs.too_dynamic = true;
        vs.reason = "prop value is a non-literal expression — '" + inner + "'";
        return vs;
    }

    return vs;  // bare unquoted token without braces — not valid JSX.
}

// Locate a property's value span inside an inline `style={{…}}` object
// literal. `tag` is the opening-tag text; `key` is the camelCase style
// key. Returns a too_dynamic ValueSpan when the style object uses a
// spread, or the matched property's value is a non-literal expression.
ValueSpan locate_style_property_value(const std::string& tag,
                                       const std::string& key) {
    ValueSpan vs;
    const std::size_t after_name = find_attr_name(tag, "style");
    if (after_name == std::string::npos) return vs;  // no inline style.

    // After `style` expect `={{`.
    std::size_t i = after_name;
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
    if (i >= tag.size() || tag[i] != '=') return vs;
    ++i;
    while (i < tag.size() && (tag[i] == ' ' || tag[i] == '\t')) ++i;
    if (i + 1 >= tag.size() || tag[i] != '{' || tag[i + 1] != '{') {
        // style={someVar} — not an inline object literal.
        vs.too_dynamic = true;
        vs.reason = "style prop is not an inline object literal";
        return vs;
    }
    // Object-literal body starts after the inner '{'.
    const std::size_t obj_begin = i + 2;
    // Find the inner `}` that closes the object literal. The outer `{`
    // opens the JSX expression container and the inner `{` opens the
    // object; `depth` tracks nesting *inside* the object. The object
    // literal ends at the first `}` that brings `depth` back to 0 — the
    // second `}` of the `}}` pair closes the expression container.
    int depth = 0;
    char in_str = 0;
    std::size_t obj_end = std::string::npos;
    for (std::size_t j = obj_begin; j < tag.size(); ++j) {
        const char c = tag[j];
        if (in_str) {
            if (c == in_str) in_str = 0;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`') in_str = c;
        else if (c == '{' || c == '[' || c == '(') ++depth;
        else if (c == ']' || c == ')') { if (depth > 0) --depth; }
        else if (c == '}') {
            if (depth == 0) { obj_end = j; break; }
            --depth;
        }
    }
    if (obj_end == std::string::npos) return vs;  // unbalanced.

    const std::string obj = tag.substr(obj_begin, obj_end - obj_begin);

    // Reject a style object that uses a spread — its resolved value for
    // `key` may come from the spread source, which we cannot rewrite.
    {
        char s = 0;
        int d = 0;
        for (std::size_t j = 0; j + 2 < obj.size(); ++j) {
            const char c = obj[j];
            if (s) { if (c == s) s = 0; continue; }
            if (c == '"' || c == '\'' || c == '`') { s = c; continue; }
            if (c == '{' || c == '[' || c == '(') ++d;
            else if (c == '}' || c == ']' || c == ')') --d;
            else if (d == 0 && c == '.' && obj[j + 1] == '.' &&
                     obj[j + 2] == '.') {
                vs.too_dynamic = true;
                vs.reason =
                    "style object uses a spread — resolved value of '" +
                    key + "' may not be a local literal";
                return vs;
            }
        }
    }

    // Walk top-level `key: value` pairs in the object literal. We only
    // accept a plain identifier or quoted-string key that equals `key`.
    std::size_t scan = 0;
    char in_str2 = 0;
    int d2 = 0;
    while (scan < obj.size()) {
        // Skip whitespace / commas at depth 0.
        while (scan < obj.size() &&
               (obj[scan] == ' ' || obj[scan] == '\t' || obj[scan] == '\n' ||
                obj[scan] == '\r' || obj[scan] == ',')) ++scan;
        if (scan >= obj.size()) break;

        // Read the key token.
        std::string this_key;
        bool computed_key = false;
        if (obj[scan] == '[') {
            computed_key = true;  // [expr]: … — not addressable by name.
        } else if (obj[scan] == '\'' || obj[scan] == '"') {
            const char q = obj[scan];
            const std::size_t ke = obj.find(q, scan + 1);
            if (ke == std::string::npos) break;
            this_key = obj.substr(scan + 1, ke - scan - 1);
            scan = ke + 1;
        } else {
            std::size_t ke = scan;
            while (ke < obj.size() && is_ident_char(obj[ke])) ++ke;
            this_key = obj.substr(scan, ke - scan);
            scan = ke;
        }
        // Skip to the ':' separating key and value.
        while (scan < obj.size() &&
               (obj[scan] == ' ' || obj[scan] == '\t')) ++scan;
        if (computed_key) {
            // Skip the `[…]` we never indexed past; find the colon
            // after the closing bracket.
            int bd = 0;
            while (scan < obj.size()) {
                if (obj[scan] == '[') ++bd;
                else if (obj[scan] == ']') { --bd; if (bd == 0) { ++scan; break; } }
                ++scan;
            }
            while (scan < obj.size() &&
                   (obj[scan] == ' ' || obj[scan] == '\t')) ++scan;
        }
        if (scan >= obj.size() || obj[scan] != ':') {
            // Shorthand `{ padding }` or malformed — advance to the next
            // top-level comma and continue.
            in_str2 = 0; d2 = 0;
            while (scan < obj.size()) {
                const char c = obj[scan];
                if (in_str2) { if (c == in_str2) in_str2 = 0; }
                else if (c == '"' || c == '\'' || c == '`') in_str2 = c;
                else if (c == '{' || c == '[' || c == '(') ++d2;
                else if (c == '}' || c == ']' || c == ')') --d2;
                else if (c == ',' && d2 == 0) break;
                ++scan;
            }
            continue;
        }
        ++scan;  // past ':'.
        while (scan < obj.size() &&
               (obj[scan] == ' ' || obj[scan] == '\t' ||
                obj[scan] == '\n' || obj[scan] == '\r')) ++scan;

        // Find the end of this value: next top-level comma or end.
        const std::size_t val_begin = scan;
        in_str2 = 0;
        d2 = 0;
        std::size_t val_end = obj.size();
        for (std::size_t j = scan; j < obj.size(); ++j) {
            const char c = obj[j];
            if (in_str2) { if (c == in_str2) in_str2 = 0; continue; }
            if (c == '"' || c == '\'' || c == '`') { in_str2 = c; continue; }
            if (c == '{' || c == '[' || c == '(') ++d2;
            else if (c == '}' || c == ']' || c == ')') --d2;
            else if (c == ',' && d2 == 0) { val_end = j; break; }
        }
        // Trim trailing whitespace from the value.
        std::size_t ve = val_end;
        while (ve > val_begin && (obj[ve - 1] == ' ' || obj[ve - 1] == '\t' ||
                                  obj[ve - 1] == '\r' || obj[ve - 1] == '\n'))
            --ve;
        scan = val_end;

        const std::string this_key_camel = to_camel(this_key);
        if (this_key_camel != key) continue;  // not our property.

        // Matched the property. Classify its value.
        const std::string val = obj.substr(val_begin, ve - val_begin);
        // Absolute offsets back into `tag`.
        const std::size_t abs_val_begin = obj_begin + val_begin;
        if (val.empty()) {
            vs.too_dynamic = true;
            vs.reason = "style property '" + key + "' has an empty value";
            return vs;
        }
        if ((val.front() == '\'' || val.front() == '"') &&
            val.back() == val.front() && val.size() >= 2) {
            // Quoted string literal value.
            vs.found = true;
            vs.quoted = true;
            vs.quote = val.front();
            vs.begin = abs_val_begin + 1;
            vs.end = abs_val_begin + val.size() - 1;
            return vs;
        }
        if (looks_numeric(val)) {
            // Bare numeric literal value.
            vs.found = true;
            vs.quoted = false;
            vs.begin = abs_val_begin;
            vs.end = abs_val_begin + val.size();
            return vs;
        }
        // Non-literal value — identifier, member access, template,
        // arithmetic, call, ternary, …
        vs.too_dynamic = true;
        vs.reason = "style property '" + key +
                    "' value is a non-literal expression — '" + val + "'";
        return vs;
    }

    return vs;  // property not present in the style object.
}

// Emit the replacement text for a value span: a quoted string keeps its
// quote style implicitly (we always replace the *contents* between the
// quotes); a bare numeric span is replaced wholesale by the new literal.
std::string render_value(const ValueSpan& vs, const std::string& value) {
    if (vs.quoted) {
        // Contents-only replacement — escape for the original delimiter.
        return escape_js_string_body(value, vs.quote);
    }
    // Bare numeric span. If the new value is itself numeric, emit it
    // bare; otherwise the prop must become a quoted string, so emit
    // quotes explicitly (the span replacement includes them).
    if (looks_numeric(value)) return trim(value);
    return "'" + escape_js_string_body(value, '\'') + "'";
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────

std::optional<std::string>
jsx_lock_property_to_key(const std::string& property_path) {
    std::string leaf = property_path;
    const auto dot = property_path.find('.');
    if (dot != std::string::npos) {
        const std::string head = lower(property_path.substr(0, dot));
        if (head == "paint" || head == "style" || head == "layout") {
            leaf = property_path.substr(dot + 1);
        } else {
            return std::nullopt;  // unknown namespace.
        }
    }
    leaf = trim(leaf);
    if (leaf.empty()) return std::nullopt;
    if (leaf.find('.') != std::string::npos) return std::nullopt;  // nested.

    const std::string camel = to_camel(leaf);

    // Allow-list of lockable JSX style / prop keys. Mirrors
    // lock_to_source.cpp's kKnown so Path A and Path B agree on what a
    // tweak can target.
    static const std::vector<std::string> kKnown = {
        // Visual / paint
        "backgroundColor", "background", "color", "opacity",
        "borderRadius", "border", "boxShadow", "filter",
        "fontFamily", "fontSize", "fontWeight", "fontStyle",
        "textAlign", "letterSpacing", "lineHeight", "textTransform",
        "overflow", "cursor", "transform",
        // Layout / box-model
        "display", "flexDirection", "gap",
        "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
        "margin", "marginTop", "marginRight", "marginBottom", "marginLeft",
        "justifyContent", "alignItems", "flexWrap", "flexGrow",
        "position", "top", "left", "right", "bottom", "zIndex",
        "width", "height",
        "minWidth", "minHeight", "maxWidth", "maxHeight",
    };
    if (std::find(kKnown.begin(), kKnown.end(), camel) == kKnown.end())
        return std::nullopt;
    return camel;
}

bool is_authored_jsx_source(const std::string& source) {
    // A file carrying the Pulp codegen banner or an @generated marker in
    // its first lines is a generated artifact — Phase 4a's domain.
    std::size_t pos = 0;
    int scanned = 0;
    while (pos < source.size() && scanned < 8) {
        const auto nl = source.find('\n', pos);
        const std::string line = trim(
            source.substr(pos, nl == std::string::npos ? std::string::npos
                                                       : nl - pos));
        if (line.find("@generated") != std::string::npos) return false;
        if (line.find("Generated by Pulp import-design") != std::string::npos)
            return false;
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++scanned;
    }
    // It must contain JSX: a `<Tag` opening followed somewhere by `>` or
    // a self-closing `/>`. Cheap structural check.
    for (std::size_t i = 0; i + 1 < source.size(); ++i) {
        if (source[i] == '<' &&
            (std::isalpha(static_cast<unsigned char>(source[i + 1])) ||
             source[i + 1] == '/' || source[i + 1] == '>')) {
            return true;
        }
    }
    return false;
}

JsxLockResult jsx_lock_tweak_into_source(const std::string& source,
                                         const JsxLockTweak& tweak) {
    JsxLockResult result;
    result.source = source;

    // Resolve the property path first — an unsupported path is a clean
    // early-out that never touches the text.
    const auto key = jsx_lock_property_to_key(tweak.property_path);
    if (!key) {
        result.status = JsxLockStatus::unsupported_property;
        result.message = "property path '" + tweak.property_path +
                          "' does not map to a lockable JSX prop";
        return result;
    }
    result.property = *key;

    // Locate the anchored element's opening tag.
    const TagSpan tag = find_anchored_tag(source, tweak.anchor_id);
    if (tag.ambiguous) {
        result.status = JsxLockStatus::anchor_ambiguous;
        result.message = "data-pulp-anchor=\"" + tweak.anchor_id +
                          "\" appears on more than one element — refusing "
                          "to guess which to patch";
        return result;
    }
    if (!tag.found) {
        result.status = JsxLockStatus::anchor_not_found;
        result.message = "no element carries data-pulp-anchor=\"" +
                          tweak.anchor_id + "\" in authored JSX/TSX source";
        return result;
    }

    const std::string tag_text =
        source.substr(tag.tag_begin, tag.tag_end - tag.tag_begin);

    // Prefer an inline `style={{…}}` property; fall back to a bare
    // attribute of the same name.
    ValueSpan vs = locate_style_property_value(tag_text, *key);
    bool from_style = vs.found || vs.too_dynamic;
    if (!vs.found && !vs.too_dynamic) {
        // No style match — try a bare attribute.
        const std::size_t after = find_attr_name(tag_text, *key);
        if (after != std::string::npos) {
            vs = locate_bare_attr_value(tag_text, after);
            from_style = false;
        }
    }

    if (vs.too_dynamic) {
        result.status = JsxLockStatus::too_dynamic;
        result.message = "cannot lock '" + tweak.property_path +
                         "' on anchor '" + tweak.anchor_id + "': " + vs.reason +
                         " — keep this tweak in the sidecar";
        return result;
    }
    if (!vs.found) {
        // The element exists but neither a style property nor a bare
        // attribute of this name is present. Phase 4b does not *insert*
        // props into authored JSX (an insert would have to pick a
        // formatting and risk a malformed tag) — it only patches what
        // the author already wrote. Report as too_dynamic's calmer
        // sibling: unsupported for this element.
        result.status = JsxLockStatus::too_dynamic;
        result.message = "anchor '" + tweak.anchor_id +
                         "' has no '" + *key +
                         "' style property or attribute to patch — Phase 4b "
                         "patches existing props only; keep in the sidecar";
        return result;
    }

    // Absolute offsets into the full source.
    const std::size_t abs_begin = tag.tag_begin + vs.begin;
    const std::size_t abs_end = tag.tag_begin + vs.end;
    const std::string old_value = source.substr(abs_begin, abs_end - abs_begin);
    const std::string new_value = render_value(vs, tweak.value);

    result.line = line_of(source, abs_begin);

    if (old_value == new_value) {
        result.status = JsxLockStatus::already_current;
        result.message = "anchor '" + tweak.anchor_id + "' already has " +
                          *key + " = " + tweak.value +
                          (from_style ? " (inline style)" : " (attribute)");
        return result;
    }

    result.source = source.substr(0, abs_begin) + new_value +
                    source.substr(abs_end);
    result.status = JsxLockStatus::patched;
    result.message = "patched " + *key + " on anchor '" + tweak.anchor_id +
                     "' to '" + tweak.value + "'" +
                     (from_style ? " (inline style)" : " (attribute)");
    return result;
}

std::vector<JsxLockResult>
jsx_lock_tweaks_into_source(const std::string& source,
                            const std::vector<JsxLockTweak>& tweaks) {
    std::vector<JsxLockResult> results;
    results.reserve(tweaks.size());
    std::string running = source;
    for (const auto& tw : tweaks) {
        JsxLockResult r = jsx_lock_tweak_into_source(running, tw);
        running = r.source;  // chain so later tweaks see earlier patches.
        results.push_back(std::move(r));
    }
    for (auto& r : results) r.source = running;
    return results;
}

}  // namespace pulp::view
