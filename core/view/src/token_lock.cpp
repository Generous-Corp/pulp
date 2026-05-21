// token_lock.cpp — Phase 4c token lock-to-source via DESIGN.md export.
//
// See token_lock.hpp for the design rationale. Summary: locking a
// token-typed inspector tweak rewrites exactly one token value in the
// DESIGN.md YAML frontmatter, preserving every other byte of the file.
//
// Implementation strategy:
//   1. Split the `---` … `---` frontmatter from the markdown body, the
//      same way design_import_designmd.cpp's parser does (so the two
//      stay consistent).
//   2. Parse the frontmatter with yaml-cpp purely to *locate* the token:
//      confirm the group exists, the token exists, and obtain the YAML
//      Mark (line) of the value node.
//   3. Perform a minimal value-only edit on the ORIGINAL text at that
//      line — never re-serialize the YAML. Re-serialization would drop
//      comments, reorder keys, and re-spell scalars.
//
// Conservatism: any ambiguity (missing group/token/field, an
// un-locatable line) fails the lock and returns the input unchanged.

#include <pulp/view/token_lock.hpp>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::view {

namespace {

// ── Frontmatter extraction ──────────────────────────────────────────────
//
// Mirrors design_import_designmd.cpp's split_frontmatter, but also
// returns the byte offset at which the frontmatter starts inside the
// original markdown so a yaml-cpp Mark (relative to the YAML text) can
// be mapped back to an absolute line in the file.

struct Frontmatter {
    bool present = false;
    std::string yaml_text;
    // Byte offset in the original markdown where `yaml_text` begins.
    std::size_t yaml_offset = 0;
    // 1-based line in the original markdown where `yaml_text` begins.
    // The opening `---` is line 1, so the YAML body starts on line 2.
    int yaml_first_line = 2;
};

Frontmatter split_frontmatter(const std::string& markdown) {
    Frontmatter out;
    if (markdown.size() < 4 || markdown.substr(0, 3) != "---" ||
        (markdown[3] != '\n' && markdown[3] != '\r')) {
        return out;
    }
    std::size_t start =
        (markdown[3] == '\r' && markdown.size() > 4 && markdown[4] == '\n') ? 5
                                                                            : 4;
    std::regex closing(R"((?:^|\n)---[ \t]*(?:\r?\n|$))");
    std::smatch m;
    std::string tail(markdown.begin() + static_cast<std::ptrdiff_t>(start),
                     markdown.end());
    if (!std::regex_search(tail, m, closing)) {
        return out;
    }
    out.present = true;
    out.yaml_text = tail.substr(0, static_cast<std::size_t>(m.position(0)));
    out.yaml_offset = start;
    out.yaml_first_line = 2;
    return out;
}

// Split text into lines, keeping track of where each begins. Newlines
// are NOT included in the returned slices; the rebuild step re-inserts
// them so the original line terminators survive untouched.
struct LineIndex {
    // Byte offset of the start of each line.
    std::vector<std::size_t> starts;
    // Byte offset just past the end of each line's content (before the
    // line terminator, if any).
    std::vector<std::size_t> ends;
};

LineIndex index_lines(const std::string& text) {
    LineIndex idx;
    std::size_t i = 0;
    idx.starts.push_back(0);
    while (i < text.size()) {
        if (text[i] == '\n') {
            std::size_t content_end = i;
            if (content_end > idx.starts.back() &&
                text[content_end - 1] == '\r') {
                --content_end;
            }
            idx.ends.push_back(content_end);
            idx.starts.push_back(i + 1);
        }
        ++i;
    }
    // Final line (may be empty if the file ends in a newline).
    std::size_t content_end = text.size();
    if (content_end > idx.starts.back() && text[content_end - 1] == '\r') {
        --content_end;
    }
    idx.ends.push_back(content_end);
    return idx;
}

// ── YAML scalar helpers ─────────────────────────────────────────────────

// Render a YAML scalar value for embedding back into the frontmatter,
// preserving the original delimiter when the source value was quoted.
std::string render_scalar(const std::string& value, char quote) {
    if (quote == '\0') return value;
    std::string out;
    out += quote;
    for (char c : value) {
        if (quote == '\'' && c == '\'') {
            out += "''";
            continue;
        }
        if (quote == '"' && (c == '"' || c == '\\')) out += '\\';
        out += c;
    }
    out += quote;
    return out;
}

// Quote delimiter used by the value as written in YAML source, if any.
char source_value_quote(std::string_view trimmed_value) {
    if (trimmed_value.empty()) return '\0';
    const char c = trimmed_value.front();
    return (c == '"' || c == '\'') ? c : '\0';
}

// Given a `key: value` source line, return the [start, end) byte range
// (relative to the line) covering the *value* portion — everything after
// the `key:` and its following whitespace, up to any trailing `#`
// comment or trailing whitespace. Returns false when the line is not a
// `key: value` line at all.
bool find_value_span(const std::string& line, const std::string& key,
                      std::size_t& value_start, std::size_t& value_end) {
    // Skip leading whitespace.
    std::size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    // Match the key (allowing it to be quoted is unnecessary for the
    // canonical DESIGN.md token keys, which are bare identifiers).
    if (line.compare(i, key.size(), key) != 0) return false;
    std::size_t after = i + key.size();
    // Expect a colon directly after the key.
    if (after >= line.size() || line[after] != ':') return false;
    ++after;
    // Skip whitespace between the colon and the value.
    while (after < line.size() && (line[after] == ' ' || line[after] == '\t')) {
        ++after;
    }
    if (after >= line.size()) return false;  // key with no inline value
    value_start = after;
    // The value runs to end-of-line. A trailing `#` comment is only a
    // comment when preceded by whitespace and outside quotes; the
    // canonical token values never contain a bare `#`-after-space, and
    // colors are quoted, so a simple scan suffices.
    std::size_t end = line.size();
    bool in_quote = false;
    char quote = '\0';
    for (std::size_t j = value_start; j < line.size(); ++j) {
        char c = line[j];
        if (in_quote) {
            if (c == quote) in_quote = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            in_quote = true;
            quote = c;
        } else if (c == '#' && j > value_start &&
                   (line[j - 1] == ' ' || line[j - 1] == '\t')) {
            end = j;
            break;
        }
    }
    // Trim trailing whitespace from the value span.
    while (end > value_start &&
           (line[end - 1] == ' ' || line[end - 1] == '\t')) {
        --end;
    }
    value_end = end;
    return value_end > value_start;
}

// ── Token location via yaml-cpp ─────────────────────────────────────────

// Result of locating a token in the parsed YAML. `value_line` is 0-based
// within the YAML text (yaml-cpp Mark convention).
struct TokenLocation {
    bool found = false;
    std::string error;
    int value_line = -1;     // 0-based, within the YAML text
    std::string leaf_key;    // the YAML key whose value gets rewritten
    std::string previous_value;
};

TokenLocation locate_token(const YAML::Node& root, const TokenTarget& target) {
    TokenLocation loc;
    const char* group_key = token_group_key(target.group);

    YAML::Node group = root[group_key];
    if (!group || !group.IsMap()) {
        loc.error = std::string("DESIGN.md has no `") + group_key +
                    "` token group";
        return loc;
    }

    YAML::Node token = group[target.name];
    if (!token) {
        loc.error = std::string("token `") + group_key + "." + target.name +
                    "` not found in DESIGN.md";
        return loc;
    }

    if (target.group == TokenGroup::typography) {
        // Typography tokens are nested: typography.<level>.<field>.
        if (target.field.empty()) {
            loc.error = "typography token lock requires a field "
                        "(fontSize / fontWeight / lineHeight / ...)";
            return loc;
        }
        if (!token.IsMap()) {
            loc.error = std::string("typography token `") + target.name +
                        "` is not a field map";
            return loc;
        }
        YAML::Node field = token[target.field];
        if (!field) {
            loc.error = std::string("typography field `") + target.name + "." +
                        target.field + "` not found in DESIGN.md";
            return loc;
        }
        if (!field.IsScalar()) {
            loc.error = std::string("typography field `") + target.name + "." +
                        target.field + "` is not a scalar value";
            return loc;
        }
        loc.value_line = field.Mark().line;
        loc.leaf_key = target.field;
        loc.previous_value = field.as<std::string>();
    } else {
        // Flat scalar groups: colors / spacing / rounded.
        if (!token.IsScalar()) {
            loc.error = std::string("token `") + group_key + "." +
                        target.name +
                        "` is not a scalar value (nested palettes are not "
                        "lockable in Phase 4c)";
            return loc;
        }
        loc.value_line = token.Mark().line;
        loc.leaf_key = target.name;
        loc.previous_value = token.as<std::string>();
    }

    if (loc.value_line < 0) {
        loc.error = "could not determine the source line of token `" +
                    std::string(group_key) + "." + target.name + "`";
        return loc;
    }
    loc.found = true;
    return loc;
}

}  // namespace

// ── Public API ──────────────────────────────────────────────────────────

const char* token_group_key(TokenGroup group) {
    switch (group) {
        case TokenGroup::colors:     return "colors";
        case TokenGroup::spacing:    return "spacing";
        case TokenGroup::rounded:    return "rounded";
        case TokenGroup::typography: return "typography";
    }
    return "";
}

std::optional<TokenTarget> classify_token_tweak(
    const std::string& anchor_id, const std::string& property_path) {
    // The dotted path we classify against: prefer the property path, but
    // fall back to the `designtoken:` anchor payload when the property
    // path is not itself a token path.
    std::string dotted = property_path;

    constexpr std::string_view kAnchorPrefix{"designtoken:"};
    const bool anchor_is_token =
        anchor_id.size() > kAnchorPrefix.size() &&
        anchor_id.compare(0, kAnchorPrefix.size(), kAnchorPrefix) == 0;

    // Helper: try to map a dotted path to a TokenTarget. Returns nullopt
    // if the first segment is not a canonical token group.
    auto classify_dotted =
        [](const std::string& path) -> std::optional<TokenTarget> {
        auto first_dot = path.find('.');
        if (first_dot == std::string::npos) return std::nullopt;
        std::string head = path.substr(0, first_dot);
        std::string rest = path.substr(first_dot + 1);
        if (rest.empty()) return std::nullopt;

        TokenTarget t;
        if (head == "colors") {
            t.group = TokenGroup::colors;
        } else if (head == "spacing") {
            t.group = TokenGroup::spacing;
        } else if (head == "rounded") {
            t.group = TokenGroup::rounded;
        } else if (head == "typography") {
            t.group = TokenGroup::typography;
        } else {
            return std::nullopt;
        }

        if (t.group == TokenGroup::typography) {
            // typography.<level>.<field> — both halves required.
            auto field_dot = rest.find('.');
            if (field_dot == std::string::npos) return std::nullopt;
            t.name = rest.substr(0, field_dot);
            t.field = rest.substr(field_dot + 1);
            if (t.name.empty() || t.field.empty()) return std::nullopt;
        } else {
            // A flat token name must not itself contain a dot — that
            // would be an element sub-path, not a token.
            if (rest.find('.') != std::string::npos) return std::nullopt;
            t.name = rest;
        }
        return t;
    };

    if (auto t = classify_dotted(dotted)) return t;

    // Property path is not a token path; consult the anchor convention.
    if (anchor_is_token) {
        std::string payload = anchor_id.substr(kAnchorPrefix.size());
        if (auto t = classify_dotted(payload)) return t;
    }
    return std::nullopt;
}

TokenLockResult lock_token_in_designmd(const std::string& markdown,
                                        const TokenTarget& target,
                                        const std::string& new_value) {
    TokenLockResult result;
    result.updated_markdown = markdown;  // unchanged-on-failure contract

    Frontmatter fm = split_frontmatter(markdown);
    if (!fm.present) {
        result.error =
            "DESIGN.md has no YAML frontmatter — nothing to lock into";
        return result;
    }

    YAML::Node root;
    try {
        root = YAML::Load(fm.yaml_text);
    } catch (const YAML::Exception& e) {
        result.error =
            std::string("DESIGN.md frontmatter is not valid YAML: ") +
            e.what();
        return result;
    }
    if (!root || !root.IsMap()) {
        result.error =
            "DESIGN.md frontmatter is not a YAML mapping at the top level";
        return result;
    }

    TokenLocation loc = locate_token(root, target);
    if (!loc.found) {
        result.error = loc.error;
        return result;
    }

    // Map the YAML-relative 0-based line to an absolute 1-based line in
    // the original markdown. yaml_text starts at fm.yaml_first_line.
    const int abs_line = fm.yaml_first_line + loc.value_line;

    LineIndex idx = index_lines(markdown);
    const std::size_t line_idx = static_cast<std::size_t>(abs_line - 1);
    if (abs_line < 1 || line_idx >= idx.starts.size()) {
        result.error = "could not locate the token's source line in "
                       "DESIGN.md (line index out of range)";
        return result;
    }

    const std::size_t line_start = idx.starts[line_idx];
    const std::size_t line_end = idx.ends[line_idx];
    std::string line = markdown.substr(line_start, line_end - line_start);

    std::size_t value_start = 0;
    std::size_t value_end = 0;
    if (!find_value_span(line, loc.leaf_key, value_start, value_end)) {
        // The yaml-cpp Mark pointed at a line that does not look like a
        // `key: value` line for this leaf key — refuse rather than guess.
        result.error =
            "could not unambiguously locate the value of token `" +
            std::string(token_group_key(target.group)) + "." + target.name +
            (target.field.empty() ? "" : "." + target.field) +
            "` in DESIGN.md source (the parsed line does not match the "
            "expected `" + loc.leaf_key + ": <value>` shape)";
        return result;
    }

    std::string old_value_raw = line.substr(value_start, value_end - value_start);
    const char quote = source_value_quote(old_value_raw);

    std::string rendered = render_scalar(new_value, quote);

    // Splice the new value into the line, then the line back into the
    // file. Every byte outside [value_start, value_end) is preserved,
    // including the line terminator (which lives outside line_end).
    std::string new_line = line.substr(0, value_start) + rendered +
                           line.substr(value_end);

    result.updated_markdown = markdown.substr(0, line_start) + new_line +
                              markdown.substr(line_end);
    result.ok = true;
    result.previous_value = loc.previous_value;
    result.line = abs_line;
    return result;
}

TokenLockResult lock_token_in_designmd(const std::string& markdown,
                                        const std::string& anchor_id,
                                        const std::string& property_path,
                                        const std::string& new_value) {
    auto target = classify_token_tweak(anchor_id, property_path);
    if (!target) {
        TokenLockResult result;
        result.updated_markdown = markdown;
        result.error =
            "tweak (anchor=\"" + anchor_id + "\", property=\"" +
            property_path +
            "\") is not a design token — element tweaks lock via Phase "
            "4a/4b, not DESIGN.md";
        return result;
    }
    return lock_token_in_designmd(markdown, *target, new_value);
}

}  // namespace pulp::view
