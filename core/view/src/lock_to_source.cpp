// lock_to_source.cpp — Phase 4a Lock-to-source, Path A engine.
//
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md,
//       "Phase 4a — Path A (generated TSX rewrite)".
//
// Definitions only; declarations live in pulp/view/lock_to_source.hpp.
//
// The engine rewrites the *generated* import artifact produced by
// generate_pulp_js() (web-compat mode). That artifact has a stable,
// machine-traceable shape per element:
//
//     // @pulp-anchor <id>
//     const var = document.createElement('div');
//     var.style.padding = '8px';
//     var.style.backgroundColor = '#888';
//     parent.appendChild(var);
//     setAnchor(var._id, '<id>');
//
// Lock-to-source finds the `// @pulp-anchor <id>` comment, walks the
// element block, and rewrites (or inserts) the `var.style.<prop>` line.

#include <pulp/view/lock_to_source.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <vector>

namespace pulp::view {

namespace {

// Trim ASCII whitespace from both ends.
std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Leading-whitespace prefix of a line (so an inserted line keeps the
// element block's indentation).
std::string indent_of(const std::string& line) {
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.substr(0, i);
}

// Split into lines, preserving content (no trailing newline kept per line).
// Tracks whether the source ended with a newline so we can re-join exactly.
std::vector<std::string> split_lines(const std::string& src, bool& trailing_nl) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : src) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    trailing_nl = src.empty() ? false : (src.back() == '\n');
    if (!cur.empty() || !trailing_nl) {
        // A non-newline-terminated final segment, or an empty trailing
        // segment that we should not synthesize. Push the remainder only
        // when there is genuine trailing content.
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

std::string join_lines(const std::vector<std::string>& lines, bool trailing_nl) {
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        out += lines[i];
        if (i + 1 < lines.size() || trailing_nl) out += '\n';
    }
    return out;
}

// Parse `// @pulp-anchor <id>` — returns the id, or empty if the line is
// not an anchor comment.
std::string anchor_comment_id(const std::string& line) {
    const std::string t = trim(line);
    constexpr const char* kPrefix = "// @pulp-anchor ";
    if (t.rfind(kPrefix, 0) != 0) return {};
    return trim(t.substr(std::char_traits<char>::length(kPrefix)));
}

// camelCase a hyphen- or snake-separated CSS property fragment. Pure
// fragment converter — "background-color" → "backgroundColor".
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

// Lowercase ASCII copy.
std::string lower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// Escape a value for emission inside a JS single-quoted literal. Handles
// quotes and backslashes, plus control characters: a pasted multi-line
// CSS value can carry newlines / tabs that would otherwise terminate the
// `'<value>'` literal and produce syntactically invalid generated JS.
std::string escape_js_single(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\'': out += "\\'"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    // Any remaining C0 control char as a `\xNN` escape so
                    // the resulting string literal is always valid JS.
                    static const char* kHex = "0123456789abcdef";
                    out += "\\x";
                    out += kHex[(c >> 4) & 0xF];
                    out += kHex[c & 0xF];
                } else {
                    out += static_cast<char>(c);
                }
                break;
        }
    }
    return out;
}

}  // namespace

std::optional<std::string>
lock_property_to_style_name(const std::string& property_path) {
    // Strip a recognized leading namespace segment. paint / style /
    // layout all collapse onto the web-compat `el.style.<name>` surface.
    std::string leaf = property_path;
    const auto dot = property_path.find('.');
    if (dot != std::string::npos) {
        const std::string head = lower(property_path.substr(0, dot));
        if (head == "paint" || head == "style" || head == "layout") {
            leaf = property_path.substr(dot + 1);
        } else {
            // Unknown namespace — not a generated-style path.
            return std::nullopt;
        }
    }
    leaf = trim(leaf);
    if (leaf.empty()) return std::nullopt;
    // A nested path like `layout.padding.top` is out of scope for Path A's
    // flat `el.style.<name>` rewrite.
    if (leaf.find('.') != std::string::npos) return std::nullopt;

    std::string camel = to_camel(leaf);

    // A few token-path leaves alias onto a different `el.style.<name>`
    // surface than their camelCase spelling. The web-compat codegen
    // represents gradient fills as `el.style.background`, so a
    // `backgroundGradient` tweak path is valid and maps to `background`.
    if (camel == "backgroundGradient") camel = "background";

    // Allow-list of style properties the web-compat codegen actually
    // emits as `el.style.<name>` (design_codegen.cpp generate_node()).
    // Keeping this explicit means an unknown / mistyped path reports
    // `unsupported_property` instead of writing a no-op assignment.
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

bool is_generated_source(const std::string& source) {
    // Cheap scan of the first few lines for the Pulp codegen banner or a
    // conventional `@generated` marker.
    std::size_t scanned = 0;
    std::size_t pos = 0;
    while (pos < source.size() && scanned < 8) {
        const auto nl = source.find('\n', pos);
        const std::string line =
            source.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        const std::string t = trim(line);
        if (t.find("@generated") != std::string::npos) return true;
        if (t.find("Generated by Pulp import-design") != std::string::npos)
            return true;
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++scanned;
    }
    return false;
}

LockResult lock_tweak_into_source(const std::string& source,
                                  const LockToSourceTweak& tweak) {
    LockResult result;
    result.source = source;

    // Resolve the property path before touching the text — an unsupported
    // path is a clean early-out that leaves the source untouched.
    const auto style_name = lock_property_to_style_name(tweak.property_path);
    if (!style_name) {
        result.status = LockStatus::unsupported_property;
        result.message = "property path '" + tweak.property_path +
                          "' does not map to a generated style property";
        return result;
    }
    result.style_property = *style_name;

    bool trailing_nl = true;
    std::vector<std::string> lines = split_lines(source, trailing_nl);

    // Find the element block whose `// @pulp-anchor <id>` comment matches.
    int anchor_line = -1;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (anchor_comment_id(lines[i]) == tweak.anchor_id &&
            !tweak.anchor_id.empty()) {
            anchor_line = static_cast<int>(i);
            break;
        }
    }
    if (anchor_line < 0) {
        result.status = LockStatus::anchor_not_found;
        result.message = "no // @pulp-anchor comment for anchor '" +
                          tweak.anchor_id + "' in generated source";
        return result;
    }

    // The element block runs from the line after the anchor comment up to
    // (but not including) the next `// @pulp-anchor` comment or the first
    // blank line — whichever comes first. generate_node() emits exactly
    // one blank line between elements, so a blank line is the block end.
    std::size_t block_begin = static_cast<std::size_t>(anchor_line) + 1;
    std::size_t block_end = lines.size();
    for (std::size_t i = block_begin; i < lines.size(); ++i) {
        if (!anchor_comment_id(lines[i]).empty()) {
            block_end = i;
            break;
        }
        if (trim(lines[i]).empty()) {
            block_end = i;
            break;
        }
    }

    // Determine the element's JS variable name from its `const <var> =`
    // declaration so an inserted line addresses the right element.
    std::string var_name;
    const std::string style_dot = ".style." + *style_name;
    int existing_prop_line = -1;
    int append_or_anchor_line = -1;
    for (std::size_t i = block_begin; i < block_end; ++i) {
        const std::string t = trim(lines[i]);
        if (var_name.empty() && t.rfind("const ", 0) == 0) {
            const auto eq = t.find(" = ");
            if (eq != std::string::npos)
                var_name = trim(t.substr(6, eq - 6));
        }
        // `<var>.style.<name> = ...` — the assignment we want to rewrite.
        if (existing_prop_line < 0) {
            const auto sp = t.find(style_dot);
            if (sp != std::string::npos) {
                // Guard: the match must be immediately followed by ` =`
                // or `=` so `.style.padding` does not match
                // `.style.paddingTop`.
                const auto after = sp + style_dot.size();
                if (after <= t.size()) {
                    std::size_t j = after;
                    while (j < t.size() && t[j] == ' ') ++j;
                    if (j < t.size() && t[j] == '=')
                        existing_prop_line = static_cast<int>(i);
                }
            }
        }
        if (append_or_anchor_line < 0 &&
            (t.find(".appendChild(") != std::string::npos ||
             t.rfind("setAnchor(", 0) == 0)) {
            append_or_anchor_line = static_cast<int>(i);
        }
    }

    const std::string escaped = escape_js_single(tweak.value);

    if (existing_prop_line >= 0) {
        // Rewrite the value inside the existing assignment's literal.
        std::string& ln = lines[static_cast<std::size_t>(existing_prop_line)];
        const std::string ind = indent_of(ln);
        // Recover the LHS (`<var>.style.<name>`) so we re-emit verbatim.
        const std::string t = trim(ln);
        const auto eq = t.find('=');
        std::string lhs = trim(t.substr(0, eq));
        const std::string rebuilt =
            ind + lhs + " = '" + escaped + "';";
        if (ln == rebuilt) {
            result.status = LockStatus::already_current;
            result.message = "anchor '" + tweak.anchor_id + "' already has " +
                              *style_name + " = '" + tweak.value + "'";
        } else {
            ln = rebuilt;
            result.status = LockStatus::rewritten;
            result.message = "rewrote " + *style_name + " for anchor '" +
                             tweak.anchor_id + "' to '" + tweak.value + "'";
        }
        result.line = existing_prop_line + 1;
        result.source = join_lines(lines, trailing_nl);
        return result;
    }

    // No existing assignment — insert one. Address it by the element's
    // variable name; fall back to `el` only if the declaration was
    // unreadable (defensive — generate_node always emits `const <var> =`).
    if (var_name.empty()) var_name = "el";

    // Insert just before the appendChild / setAnchor tail when present so
    // the new style line stays grouped with the element's other styles;
    // otherwise append at the end of the block.
    std::size_t insert_at =
        (append_or_anchor_line >= 0)
            ? static_cast<std::size_t>(append_or_anchor_line)
            : block_end;
    // Indentation: copy from a sibling line in the block.
    std::string ind;
    if (insert_at > block_begin && insert_at <= lines.size())
        ind = indent_of(lines[insert_at - 1]);
    else if (block_begin < lines.size())
        ind = indent_of(lines[block_begin]);

    const std::string new_line =
        ind + var_name + ".style." + *style_name + " = '" + escaped + "';";
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), new_line);

    result.status = LockStatus::inserted;
    result.line = static_cast<int>(insert_at) + 1;
    result.message = "inserted " + *style_name + " = '" + tweak.value +
                     "' for anchor '" + tweak.anchor_id + "'";
    result.source = join_lines(lines, trailing_nl);
    return result;
}

std::vector<LockResult>
lock_tweaks_into_source(const std::string& source,
                        const std::vector<LockToSourceTweak>& tweaks) {
    std::vector<LockResult> results;
    results.reserve(tweaks.size());
    std::string running = source;
    for (const auto& tw : tweaks) {
        LockResult r = lock_tweak_into_source(running, tw);
        running = r.source;  // chain so later tweaks see earlier rewrites
        results.push_back(std::move(r));
    }
    return results;
}

}  // namespace pulp::view
