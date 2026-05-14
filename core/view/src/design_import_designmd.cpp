// design_import_designmd.cpp — DESIGN.md import adapter
//
// Inspired by Google's design.md (Apache-2.0, https://github.com/google-labs-code/design.md).
// Reimplemented in C++; uses yaml-cpp (MIT) for frontmatter parsing. The
// Markdown body is walked with a small hand-rolled `##` section scanner —
// the body section structure is simple enough that a full Markdown parser
// would be overkill and would add an unnecessary dependency.
//
// What this file parses:
//   - YAML frontmatter (between `---` fences) → IR token maps via yaml-cpp.
//   - `## Section` headings in the Markdown body → ordered section list
//     (the Phase 2 section-order lint rule reads this).
//
// What this file does NOT do:
//   - Emit a UI tree. DESIGN.md describes a *system*, not a *screen*. The
//     IR root node is left empty (type "frame", no children). The Phase 1
//     CLI deliberately does not write a ui.js file when the source is
//     designmd; see tools/import-design/pulp_import_design.cpp for the
//     dispatch arm.

#include <pulp/view/design_import.hpp>
#include <pulp/view/theme_contrast.hpp>

#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

namespace pulp::view {

namespace {

// ── Frontmatter extraction ──────────────────────────────────────────────

struct FrontmatterSlice {
    bool present = false;
    std::string yaml_text;
    std::string body;       // markdown after the closing fence
    int yaml_start_line = 1;
};

// Split the input on the leading `---\n` / `\n---\n` fences. If the
// document does not begin with `---` then the whole input is the body.
FrontmatterSlice split_frontmatter(const std::string& markdown) {
    FrontmatterSlice out;
    if (markdown.size() < 4 || markdown.substr(0, 3) != "---" ||
        (markdown[3] != '\n' && markdown[3] != '\r')) {
        out.body = markdown;
        return out;
    }
    // Find the next `---` on its own line.
    size_t start = (markdown[3] == '\r' && markdown.size() > 4 && markdown[4] == '\n')
                       ? 5 : 4;
    std::regex closing(R"((?:^|\n)---[ \t]*(?:\r?\n|$))");
    std::smatch m;
    std::string tail(markdown.begin() + static_cast<std::ptrdiff_t>(start), markdown.end());
    if (!std::regex_search(tail, m, closing)) {
        out.body = markdown;
        return out;
    }
    out.present = true;
    out.yaml_text = tail.substr(0, m.position(0));
    out.yaml_start_line = 2; // frontmatter starts on line 2 (line 1 is the opening ---)
    auto body_start = m.position(0) + m.length(0);
    out.body = tail.substr(static_cast<size_t>(body_start));
    return out;
}

// ── Helpers ─────────────────────────────────────────────────────────────

std::string trim(std::string s) {
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
    s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
    return s;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

// Convert a dimension string ("48px", "1.5rem", "0.1em") to a float in px.
// Returns std::nullopt for values that aren't dimensions (e.g. plain strings).
std::optional<float> parse_dimension(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    static const std::regex re(R"(^\s*(-?\d+(?:\.\d+)?)\s*(px|em|rem)?\s*$)");
    std::smatch m;
    if (!std::regex_match(raw, m, re)) return std::nullopt;
    float value = std::stof(m[1]);
    std::string unit = m[2].matched ? m[2].str() : "px";
    if (unit == "em" || unit == "rem") value *= 16.0f; // canonical 1rem == 16px
    return value;
}

bool looks_like_hex_color(const std::string& s) {
    if (s.size() < 4 || s[0] != '#') return false;
    for (size_t i = 1; i < s.size(); ++i) {
        if (!std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return s.size() == 4 || s.size() == 7 || s.size() == 9;
}

bool is_token_reference(const std::string& s) {
    return s.size() >= 3 && s.front() == '{' && s.back() == '}';
}

std::string reference_path(const std::string& ref) {
    // "{colors.primary}" → "colors.primary"
    return ref.substr(1, ref.size() - 2);
}

// ── Section walker ──────────────────────────────────────────────────────

std::vector<std::string> walk_sections(const std::string& body) {
    std::vector<std::string> sections;
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.size() >= 3 && line[0] == '#' && line[1] == '#' && line[2] != '#') {
            std::string heading = trim(line.substr(2));
            if (!heading.empty()) sections.push_back(heading);
        }
    }
    return sections;
}

// ── Diagnostic helpers ──────────────────────────────────────────────────

DesignMdDiagnostic make_diag(DesignMdSeverity sev,
                              std::string code,
                              std::string path,
                              int line,
                              int column,
                              std::string message) {
    DesignMdDiagnostic d;
    d.severity = sev;
    d.code = std::move(code);
    d.path = std::move(path);
    d.line = line;
    d.column = column;
    d.message = std::move(message);
    return d;
}

int yaml_line(const YAML::Node& node, int fallback) {
    if (!node || !node.Mark().is_null()) {
        // yaml-cpp Marks are 0-based; convert to 1-based.
        if (node && node.Mark().line >= 0) return node.Mark().line + 1;
    }
    return fallback;
}

// ── Top-level token group walkers ───────────────────────────────────────

void walk_colors_map(const YAML::Node& node,
                      DesignMdParseResult& result) {
    if (!node || !node.IsMap()) return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        std::string name = it->first.as<std::string>("");
        if (name.empty()) continue;
        // Per spec, a color token can be either a primitive (string hex) or
        // a nested map (palette with shade levels). We handle the primitive
        // case directly and walk one level for nested palettes.
        if (it->second.IsScalar()) {
            std::string value = it->second.as<std::string>("");
            if (is_token_reference(value) || looks_like_hex_color(value)) {
                result.ir.tokens.colors.emplace(name, value);
            } else {
                result.diagnostics.push_back(make_diag(
                    DesignMdSeverity::warning, "color-shape", "colors." + name,
                    yaml_line(it->second, 0), 0,
                    "expected hex color or token reference; got \"" + value + "\""));
                result.ir.tokens.colors.emplace(name, value);
            }
        } else if (it->second.IsMap()) {
            for (auto sub = it->second.begin(); sub != it->second.end(); ++sub) {
                std::string shade = sub->first.as<std::string>("");
                std::string value = sub->second.as<std::string>("");
                if (shade.empty() || value.empty()) continue;
                result.ir.tokens.colors.emplace(name + "-" + shade, value);
            }
        }
    }
}

void walk_typography_map(const YAML::Node& node,
                          DesignMdParseResult& result) {
    if (!node || !node.IsMap()) return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        std::string level = it->first.as<std::string>("");
        if (level.empty() || !it->second.IsMap()) continue;
        const std::string prefix = "typography." + level + ".";
        for (auto field = it->second.begin(); field != it->second.end(); ++field) {
            std::string key = field->first.as<std::string>("");
            std::string value = field->second.as<std::string>("");
            if (key.empty()) continue;
            // fontFeature / fontVariation explicitly preserved as strings per spec.
            // fontWeight / fontSize / lineHeight / letterSpacing also flow through
            // as strings; downstream callers (Theme materializer) re-parse them.
            result.ir.tokens.strings.emplace(prefix + key, value);
        }
    }
}

void walk_dimension_map(const YAML::Node& node,
                         const std::string& prefix,
                         DesignMdParseResult& result) {
    if (!node || !node.IsMap()) return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        std::string key = it->first.as<std::string>("");
        if (key.empty() || !it->second.IsScalar()) continue;
        std::string raw = it->second.as<std::string>("");
        std::string token_name = prefix + "-" + key;
        if (auto px = parse_dimension(raw)) {
            result.ir.tokens.dimensions.emplace(token_name, *px);
        } else {
            // Per spec: unknown spacing values stored as strings.
            result.ir.tokens.strings.emplace(token_name, raw);
        }
    }
}

void walk_components_map(const YAML::Node& node,
                          DesignMdParseResult& result) {
    if (!node || !node.IsMap()) return;
    for (auto it = node.begin(); it != node.end(); ++it) {
        std::string comp = it->first.as<std::string>("");
        if (comp.empty() || !it->second.IsMap()) continue;
        const std::string prefix = "components." + comp + ".";
        for (auto field = it->second.begin(); field != it->second.end(); ++field) {
            std::string key = field->first.as<std::string>("");
            std::string value = field->second.as<std::string>("");
            if (key.empty()) continue;
            result.ir.tokens.strings.emplace(prefix + key, value);
        }
    }
}

// ── Reference resolution ────────────────────────────────────────────────

std::optional<std::string> lookup_color(const IRTokens& t, const std::string& path) {
    // "colors.primary" → look up "primary" in tokens.colors
    constexpr std::string_view prefix{"colors."};
    if (path.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    auto it = t.colors.find(path.substr(prefix.size()));
    if (it == t.colors.end()) return std::nullopt;
    return it->second;
}

std::optional<std::string> lookup_dimension(const IRTokens& t, const std::string& path) {
    // "rounded.sm" → tokens.dimensions["rounded-sm"]
    auto dot = path.find('.');
    if (dot == std::string::npos) return std::nullopt;
    std::string token = path.substr(0, dot) + "-" + path.substr(dot + 1);
    auto it = t.dimensions.find(token);
    if (it == t.dimensions.end()) return std::nullopt;
    std::ostringstream oss;
    oss << it->second << "px";
    return oss.str();
}

bool looks_like_group_ref(const std::string& path) {
    // "colors" or "typography" alone, no dot, is a group reference (illegal
    // outside the components section per spec).
    return path.find('.') == std::string::npos;
}

void resolve_references(DesignMdParseResult& result) {
    std::unordered_set<std::string> referenced;
    auto resolve_one = [&](const std::string& value,
                            const std::string& at_path,
                            bool inside_components) -> std::string {
        if (!is_token_reference(value)) return value;
        std::string path = reference_path(value);
        if (looks_like_group_ref(path) && !inside_components) {
            result.diagnostics.push_back(make_diag(
                DesignMdSeverity::warning, "broken-ref", at_path, 0, 0,
                "non-component reference \"" + value + "\" points to a group, not a value"));
            return value;
        }
        // Record any color-token reference we encounter, whether or not
        // it resolves — orphan detection cares about reference INTENT,
        // not just successful resolution. (A consumer that uses
        // `{colors.brand}` with a typo still demonstrates intent for
        // the `brand` token, so we record it, AND lint emits its own
        // broken-ref diagnostic for the typo.)
        constexpr std::string_view colors_prefix{"colors."};
        if (path.compare(0, colors_prefix.size(), colors_prefix) == 0) {
            referenced.insert(path.substr(colors_prefix.size()));
        }
        if (auto v = lookup_color(result.ir.tokens, path))        return *v;
        if (auto v = lookup_dimension(result.ir.tokens, path))    return *v;
        // Typography (composite) refs in components — left as-is for downstream.
        if (path.compare(0, 11, "typography.") == 0 && inside_components) return value;
        result.diagnostics.push_back(make_diag(
            DesignMdSeverity::warning, "broken-ref", at_path, 0, 0,
            "could not resolve token reference \"" + value + "\""));
        return value;
    };

    for (auto& [name, value] : result.ir.tokens.colors) {
        value = resolve_one(value, "colors." + name, false);
    }
    for (auto& [name, value] : result.ir.tokens.strings) {
        const bool comp = name.compare(0, 11, "components.") == 0;
        value = resolve_one(value, name, comp);
    }

    result.referenced_color_tokens.assign(referenced.begin(), referenced.end());
    std::sort(result.referenced_color_tokens.begin(),
               result.referenced_color_tokens.end());
}

} // namespace

// ── Public API ──────────────────────────────────────────────────────────

DesignMdParseResult parse_designmd(const std::string& markdown) {
    DesignMdParseResult result;
    result.ir.source = DesignSource::designmd;
    result.ir.root.type = "frame";  // empty container; DESIGN.md has no screen.

    auto slice = split_frontmatter(markdown);
    result.had_frontmatter = slice.present;
    result.sections = walk_sections(slice.body);

    // Reject duplicate section headings per spec.
    {
        std::set<std::string> seen;
        for (const auto& s : result.sections) {
            auto key = lower(s);
            if (seen.count(key)) {
                result.diagnostics.push_back(make_diag(
                    DesignMdSeverity::error, "duplicate-section", s, 0, 0,
                    "duplicate section heading \"" + s + "\" — reject per spec"));
            }
            seen.insert(key);
        }
    }

    if (!slice.present) {
        result.diagnostics.push_back(make_diag(
            DesignMdSeverity::info, "no-frontmatter", "", 0, 0,
            "no YAML frontmatter found; emitting empty token set"));
        return result;
    }

    YAML::Node root;
    try {
        root = YAML::Load(slice.yaml_text);
    } catch (const YAML::ParserException& e) {
        result.diagnostics.push_back(make_diag(
            DesignMdSeverity::error, "yaml-parse",
            "<frontmatter>",
            e.mark.line + slice.yaml_start_line, e.mark.column + 1,
            e.what()));
        return result;
    } catch (const YAML::Exception& e) {
        result.diagnostics.push_back(make_diag(
            DesignMdSeverity::error, "yaml-parse", "<frontmatter>", 0, 0, e.what()));
        return result;
    }

    if (!root || !root.IsMap()) {
        result.diagnostics.push_back(make_diag(
            DesignMdSeverity::error, "yaml-shape", "<frontmatter>", 0, 0,
            "expected a YAML mapping at the top level of frontmatter"));
        return result;
    }

    // Carry name/description/version through as string tokens.
    if (root["name"])        result.ir.tokens.strings.emplace("name",        root["name"].as<std::string>(""));
    if (root["description"]) result.ir.tokens.strings.emplace("description", root["description"].as<std::string>(""));
    if (root["version"])     result.ir.tokens.strings.emplace("version",     root["version"].as<std::string>(""));

    walk_colors_map(root["colors"],       result);
    walk_typography_map(root["typography"], result);
    walk_dimension_map(root["rounded"],   "rounded", result);
    walk_dimension_map(root["spacing"],   "spacing", result);
    walk_components_map(root["components"], result);

    resolve_references(result);

    return result;
}

DesignIR parse_designmd_yaml(const std::string& markdown) {
    return parse_designmd(markdown).ir;
}

// ── Phase 2: lint, diff, Tailwind export ─────────────────────────────────

namespace {

bool parse_hex_color(const std::string& hex, uint32_t& out) {
    if (hex.size() != 7 || hex[0] != '#') return false;
    uint32_t v = 0;
    for (size_t i = 1; i < hex.size(); ++i) {
        char c = hex[i];
        v <<= 4;
        if (c >= '0' && c <= '9')      v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(10 + (c - 'a'));
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(10 + (c - 'A'));
        else return false;
    }
    out = v;
    return true;
}

const std::vector<std::string>& canonical_section_order() {
    static const std::vector<std::string> order = {
        "Overview",
        "Colors",
        "Typography",
        "Layout",
        "Elevation & Depth",
        "Shapes",
        "Components",
        "Do's and Don'ts"
    };
    return order;
}

// Match an observed section heading against the canonical list, allowing
// the aliases the spec lists (Brand & Style → Overview, Layout & Spacing
// → Layout, Elevation → Elevation & Depth).
std::string canonical_section_name(const std::string& heading) {
    std::string h = heading;
    for (auto& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (h == "overview" || h == "brand & style")          return "Overview";
    if (h == "colors")                                     return "Colors";
    if (h == "typography")                                 return "Typography";
    if (h == "layout" || h == "layout & spacing")         return "Layout";
    if (h == "elevation" || h == "elevation & depth")     return "Elevation & Depth";
    if (h == "shapes")                                     return "Shapes";
    if (h == "components")                                 return "Components";
    if (h == "do's and don'ts" || h == "dos and don'ts") return "Do's and Don'ts";
    return {};
}

// Extract all `{ref.path}` substrings from a string value. Used by the
// orphaned-tokens rule to find which color tokens are referenced
// somewhere in the components section.
std::vector<std::string> extract_token_refs(const std::string& value) {
    std::vector<std::string> refs;
    size_t i = 0;
    while (i < value.size()) {
        size_t open = value.find('{', i);
        if (open == std::string::npos) break;
        size_t close = value.find('}', open + 1);
        if (close == std::string::npos) break;
        refs.push_back(value.substr(open + 1, close - open - 1));
        i = close + 1;
    }
    return refs;
}

} // namespace

std::vector<DesignMdDiagnostic> lint_designmd(const DesignMdParseResult& parsed) {
    std::vector<DesignMdDiagnostic> out;
    const auto& tokens = parsed.ir.tokens;

    // Carry the parse-time diagnostics forward — broken-ref discovered
    // during parse promotes from warning to error per the spec.
    for (auto d : parsed.diagnostics) {
        if (d.code == "broken-ref") d.severity = DesignMdSeverity::error;
        out.push_back(std::move(d));
    }

    // ── missing-primary (warning) ──────────────────────────────────────
    if (!tokens.colors.empty() && tokens.colors.find("primary") == tokens.colors.end()) {
        out.push_back(make_diag(
            DesignMdSeverity::warning, "missing-primary", "colors", 0, 0,
            "colors defined but no `primary` token — agents will auto-generate one"));
    }

    // ── missing-typography (warning) ───────────────────────────────────
    bool has_typography = false;
    for (const auto& [k, _] : tokens.strings) {
        if (k.rfind("typography.", 0) == 0) { has_typography = true; break; }
    }
    if (!tokens.colors.empty() && !has_typography) {
        out.push_back(make_diag(
            DesignMdSeverity::warning, "missing-typography", "typography", 0, 0,
            "colors defined but no typography tokens — agents will use default fonts"));
    }

    // ── missing-sections (info) ────────────────────────────────────────
    bool has_rounded = false, has_spacing = false;
    for (const auto& [k, _] : tokens.dimensions) {
        if (k.rfind("rounded-", 0) == 0) has_rounded = true;
        if (k.rfind("spacing-", 0) == 0) has_spacing = true;
    }
    if (!tokens.colors.empty() && !has_rounded) {
        out.push_back(make_diag(
            DesignMdSeverity::info, "missing-sections", "rounded", 0, 0,
            "no `rounded` token group present"));
    }
    if (!tokens.colors.empty() && !has_spacing) {
        out.push_back(make_diag(
            DesignMdSeverity::info, "missing-sections", "spacing", 0, 0,
            "no `spacing` token group present"));
    }

    // ── token-summary (info) ───────────────────────────────────────────
    {
        std::ostringstream summary;
        summary << "colors=" << tokens.colors.size()
                << " dimensions=" << tokens.dimensions.size()
                << " strings=" << tokens.strings.size();
        out.push_back(make_diag(
            DesignMdSeverity::info, "token-summary", "<root>", 0, 0, summary.str()));
    }

    // ── orphaned-tokens (warning) ─────────────────────────────────────
    // Uses parse-time reference recording rather than post-resolution
    // string scanning. parse_designmd rewrites `{colors.primary}` to
    // its resolved hex value before lint runs, so a post-hoc scan of
    // tokens.strings would see zero refs and flag every used color as
    // orphan. The referenced_color_tokens set captures the references
    // before resolution erases them. (Fixes Codex P1 on PR #1934.)
    {
        std::unordered_set<std::string> referenced(
            parsed.referenced_color_tokens.begin(),
            parsed.referenced_color_tokens.end());
        // Also scan tokens.strings for any references that survived
        // resolution (e.g. unresolved or typography composite refs).
        // This is belt-and-suspenders — the parse-time set is the
        // primary signal.
        for (const auto& [k, v] : tokens.strings) {
            for (const auto& ref : extract_token_refs(v)) {
                constexpr std::string_view colors_prefix{"colors."};
                if (ref.compare(0, colors_prefix.size(), colors_prefix) == 0) {
                    referenced.insert(ref.substr(colors_prefix.size()));
                }
            }
        }
        for (const auto& [name, _] : tokens.colors) {
            if (referenced.find(name) == referenced.end()) {
                out.push_back(make_diag(
                    DesignMdSeverity::warning, "orphaned-tokens", "colors." + name, 0, 0,
                    "color token `" + name + "` defined but never referenced by any component"));
            }
        }
    }

    // ── contrast-ratio (warning) ──────────────────────────────────────
    {
        // Walk every component that has both backgroundColor and textColor
        // string-token entries, parse each as a hex, and run them through
        // the WCAG 2.1 helper.
        std::unordered_map<std::string, std::pair<std::string, std::string>> comp_pair;
        for (const auto& [k, v] : tokens.strings) {
            constexpr std::string_view prefix{"components."};
            if (k.compare(0, prefix.size(), prefix) != 0) continue;
            auto rest = k.substr(prefix.size());
            auto dot = rest.find('.');
            if (dot == std::string::npos) continue;
            std::string comp = rest.substr(0, dot);
            std::string prop = rest.substr(dot + 1);
            if (prop == "backgroundColor") comp_pair[comp].first  = v;
            if (prop == "textColor")       comp_pair[comp].second = v;
        }
        for (const auto& [comp, pair] : comp_pair) {
            uint32_t bg_rgb = 0, fg_rgb = 0;
            if (!parse_hex_color(pair.first, bg_rgb)) continue;
            if (!parse_hex_color(pair.second, fg_rgb)) continue;
            auto bg = canvas::Color::hex(bg_rgb);
            auto fg = canvas::Color::hex(fg_rgb);
            float ratio = contrast_ratio(fg, bg);
            if (ratio < 4.5f) {
                std::ostringstream msg;
                msg << "components." << comp << ": text/bg contrast ratio "
                    << ratio << ":1 is below WCAG AA minimum (4.5:1)";
                out.push_back(make_diag(
                    DesignMdSeverity::warning, "contrast-ratio",
                    "components." + comp, 0, 0, msg.str()));
            }
        }
    }

    // ── section-order (warning) ───────────────────────────────────────
    {
        std::vector<std::string> canonical_seen;
        for (const auto& s : parsed.sections) {
            auto canon = canonical_section_name(s);
            if (!canon.empty()) canonical_seen.push_back(canon);
        }
        const auto& order = canonical_section_order();
        std::vector<size_t> indices;
        for (const auto& c : canonical_seen) {
            for (size_t i = 0; i < order.size(); ++i) {
                if (order[i] == c) { indices.push_back(i); break; }
            }
        }
        for (size_t j = 1; j < indices.size(); ++j) {
            if (indices[j] < indices[j - 1]) {
                out.push_back(make_diag(
                    DesignMdSeverity::warning, "section-order",
                    canonical_seen[j], 0, 0,
                    "section \"" + canonical_seen[j] + "\" appears out of canonical order"));
                break;
            }
        }
    }

    return out;
}

DesignMdDiffResult diff_designmd(const DesignMdParseResult& before,
                                  const DesignMdParseResult& after) {
    DesignMdDiffResult result;

    auto diff_map = [](const auto& before_map, const auto& after_map, auto val_eq,
                        DesignMdTokenDiff& out) {
        for (const auto& [k, v] : after_map) {
            auto it = before_map.find(k);
            if (it == before_map.end()) out.added.push_back(k);
            else if (!val_eq(it->second, v)) out.modified.push_back(k);
        }
        for (const auto& [k, _] : before_map) {
            if (after_map.find(k) == after_map.end()) out.removed.push_back(k);
        }
        std::sort(out.added.begin(), out.added.end());
        std::sort(out.removed.begin(), out.removed.end());
        std::sort(out.modified.begin(), out.modified.end());
    };

    diff_map(before.ir.tokens.colors, after.ir.tokens.colors,
              [](const std::string& a, const std::string& b) { return a == b; },
              result.colors);
    diff_map(before.ir.tokens.dimensions, after.ir.tokens.dimensions,
              [](float a, float b) { return std::abs(a - b) < 1e-6f; },
              result.dimensions);
    diff_map(before.ir.tokens.strings, after.ir.tokens.strings,
              [](const std::string& a, const std::string& b) { return a == b; },
              result.strings);

    auto count_problems = [](const std::vector<DesignMdDiagnostic>& diags) {
        int n = 0;
        for (const auto& d : diags) {
            if (d.severity == DesignMdSeverity::error ||
                d.severity == DesignMdSeverity::warning) ++n;
        }
        return n;
    };
    int before_problems = count_problems(lint_designmd(before));
    int after_problems  = count_problems(lint_designmd(after));
    result.regression = after_problems > before_problems;

    return result;
}

// ── Tailwind exporters ─────────────────────────────────────────────────

namespace {

// JSON-escape a string for embedding in the Tailwind v3 output.
std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:   out += c;
        }
    }
    return out;
}

// "rounded-sm" → ("rounded", "sm"). Splits on the first hyphen so that
// composite keys like "spacing-x-lg" keep their suffix intact.
std::pair<std::string, std::string> split_prefix_suffix(const std::string& token) {
    auto hyphen = token.find('-');
    if (hyphen == std::string::npos) return {token, {}};
    return {token.substr(0, hyphen), token.substr(hyphen + 1)};
}

} // namespace

namespace {

// Walk tokens.strings for typography fields. Returns a map keyed by
// the Tailwind v3 group name (fontFamily / fontSize / fontWeight /
// lineHeight / letterSpacing) → vector of (typography level, value)
// pairs. Skips fontFeature / fontVariation (no direct Tailwind v3
// theme key; their CSS values are emitted via plain CSS layers, not
// the theme config).
std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>
group_typography(const IRTokens& tokens) {
    static const std::unordered_map<std::string, std::string> field_to_tw = {
        {"fontFamily", "fontFamily"},
        {"fontSize", "fontSize"},
        {"fontWeight", "fontWeight"},
        {"lineHeight", "lineHeight"},
        {"letterSpacing", "letterSpacing"},
    };
    constexpr std::string_view prefix{"typography."};
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> out;
    for (const auto& [k, v] : tokens.strings) {
        if (k.compare(0, prefix.size(), prefix) != 0) continue;
        auto rest = k.substr(prefix.size());           // "<level>.<field>"
        auto dot = rest.find('.');
        if (dot == std::string::npos) continue;
        std::string level = rest.substr(0, dot);
        std::string field = rest.substr(dot + 1);
        auto it = field_to_tw.find(field);
        if (it == field_to_tw.end()) continue;
        out[it->second].emplace_back(level, v);
    }
    for (auto& [_, vec] : out) std::sort(vec.begin(), vec.end());
    return out;
}

} // namespace

std::string export_tailwind_v3_json(const DesignMdParseResult& parsed) {
    const auto& t = parsed.ir.tokens;
    std::ostringstream o;
    o << "{\n  \"colors\": {";
    bool first = true;
    for (const auto& [name, value] : t.colors) {
        if (!first) o << ",";
        o << "\n    \"" << json_escape(name) << "\": \"" << json_escape(value) << "\"";
        first = false;
    }
    o << "\n  },\n  \"borderRadius\": {";
    first = true;
    for (const auto& [name, value] : t.dimensions) {
        auto [prefix, suffix] = split_prefix_suffix(name);
        if (prefix != "rounded") continue;
        if (!first) o << ",";
        o << "\n    \"" << json_escape(suffix) << "\": \"" << value << "px\"";
        first = false;
    }
    o << "\n  },\n  \"spacing\": {";
    first = true;
    for (const auto& [name, value] : t.dimensions) {
        auto [prefix, suffix] = split_prefix_suffix(name);
        if (prefix != "spacing") continue;
        if (!first) o << ",";
        o << "\n    \"" << json_escape(suffix) << "\": \"" << value << "px\"";
        first = false;
    }
    o << "\n  }";

    // Typography mappings (Codex P2 on PR #1934).
    // Emit one Tailwind v3 theme group per typography field across all
    // levels. Each group is `{<level>: <value>, ...}` mirroring the
    // shape `@google/design.md`'s `export --format json-tailwind`
    // produces, so downstream tooling can swap implementations.
    auto typo = group_typography(t);
    static const std::vector<std::string> typo_groups{
        "fontFamily", "fontSize", "fontWeight", "lineHeight", "letterSpacing"};
    for (const auto& group : typo_groups) {
        auto it = typo.find(group);
        if (it == typo.end() || it->second.empty()) continue;
        o << ",\n  \"" << group << "\": {";
        bool first_entry = true;
        for (const auto& [level, value] : it->second) {
            if (!first_entry) o << ",";
            o << "\n    \"" << json_escape(level) << "\": \""
              << json_escape(value) << "\"";
            first_entry = false;
        }
        o << "\n  }";
    }

    o << "\n}\n";
    return o.str();
}

std::string export_tailwind_v4_css(const DesignMdParseResult& parsed) {
    const auto& t = parsed.ir.tokens;
    std::ostringstream o;
    o << "@theme {\n";
    for (const auto& [name, value] : t.colors) {
        o << "  --color-" << name << ": " << value << ";\n";
    }
    for (const auto& [name, value] : t.dimensions) {
        auto [prefix, suffix] = split_prefix_suffix(name);
        if (prefix == "rounded") o << "  --radius-" << suffix << ": " << value << "px;\n";
        if (prefix == "spacing") o << "  --spacing-" << suffix << ": " << value << "px;\n";
    }
    // Typography → Tailwind v4 token namespaces (Codex P2 on PR #1934).
    // `--font-*` for family, `--text-*` for size, `--leading-*` for
    // line-height, `--tracking-*` for letter-spacing, `--font-weight-*`
    // for weight. Matches `@google/design.md`'s `export --format
    // css-tailwind` namespace conventions.
    auto typo = group_typography(t);
    static const std::vector<std::pair<std::string, std::string>> typo_namespaces{
        {"fontFamily", "font"},
        {"fontSize", "text"},
        {"lineHeight", "leading"},
        {"letterSpacing", "tracking"},
        {"fontWeight", "font-weight"},
    };
    for (const auto& [group, ns] : typo_namespaces) {
        auto it = typo.find(group);
        if (it == typo.end()) continue;
        for (const auto& [level, value] : it->second) {
            o << "  --" << ns << "-" << level << ": " << value << ";\n";
        }
    }
    o << "}\n";
    return o.str();
}

} // namespace pulp::view
