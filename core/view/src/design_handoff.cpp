// design_handoff.cpp — parse a Claude Design project handoff into a contract.

#include <pulp/design/design_handoff.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>

namespace pulp::design {

namespace {

std::string to_lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view s) {
    auto issp = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (!s.empty() && issp(s.front())) s.remove_prefix(1);
    while (!s.empty() && issp(s.back())) s.remove_suffix(1);
    return s;
}

// Strip surrounding Markdown emphasis markers (**bold**, *italic*, `code`) and
// list-bullet/hash prefixes so a value compares cleanly.
std::string_view strip_emphasis(std::string_view s) {
    s = trim(s);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::string_view mark : {"**", "*", "`", "_"}) {
            if (s.size() >= 2 * mark.size() && s.substr(0, mark.size()) == mark &&
                s.substr(s.size() - mark.size()) == mark) {
                s.remove_prefix(mark.size());
                s.remove_suffix(mark.size());
                s = trim(s);
                changed = true;
            }
        }
    }
    return s;
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

// Strip any leading/trailing Markdown emphasis characters and whitespace. Unlike
// strip_emphasis this does not require matched pairs, so it cleans a key like
// `**Fidelity` from `**Fidelity:**` where the colon splits the bold span.
std::string_view strip_marker_chars(std::string_view s) {
    auto is_mark = [](char c) {
        return c == '*' || c == '_' || c == '`' ||
               std::isspace(static_cast<unsigned char>(c)) != 0;
    };
    while (!s.empty() && is_mark(s.front())) s.remove_prefix(1);
    while (!s.empty() && is_mark(s.back())) s.remove_suffix(1);
    return s;
}

// Heading level (number of leading '#'), or 0 if the line is not an ATX heading.
int heading_level(std::string_view line) {
    line = trim(line);
    int n = 0;
    while (n < static_cast<int>(line.size()) && line[n] == '#') ++n;
    if (n == 0 || n >= static_cast<int>(line.size()) || line[n] != ' ') return 0;
    return n;
}

std::string_view heading_text(std::string_view line) {
    line = trim(line);
    size_t n = 0;
    while (n < line.size() && line[n] == '#') ++n;
    return trim(line.substr(n));
}

// A `- key: value` (or `* key: value`) bullet. Returns false if the line is not
// a key/value bullet.
bool parse_kv_bullet(std::string_view line, std::string& key, std::string& value) {
    std::string_view t = trim(line);
    if (t.empty() || (t.front() != '-' && t.front() != '*')) return false;
    t.remove_prefix(1);
    t = trim(t);
    auto colon = t.find(':');
    if (colon == std::string_view::npos) return false;
    key = std::string(strip_emphasis(t.substr(0, colon)));
    value = std::string(strip_emphasis(t.substr(colon + 1)));
    return !key.empty();
}

// The bullet text of a `-`/`*` list item, or "" if the line is not a bullet.
std::string_view bullet_text(std::string_view line) {
    std::string_view t = trim(line);
    if (t.empty() || (t.front() != '-' && t.front() != '*')) return {};
    t.remove_prefix(1);
    return trim(t);
}

// Split a heading like "Home (screens/home.html)" into name + optional path.
// A trailing parenthetical is only treated as a source path when it looks like
// one (contains a '/' or a file extension); otherwise `### Login (mobile)` keeps
// "Login (mobile)" as the name rather than inventing a path "mobile".
void split_name_path(std::string_view text, std::string& name, std::string& path) {
    text = strip_emphasis(text);
    auto open = text.rfind('(');
    auto close = text.rfind(')');
    if (open != std::string_view::npos && close == text.size() - 1 && close > open) {
        std::string_view inner = trim(text.substr(open + 1, close - open - 1));
        bool path_like = contains(inner, "/") || contains(inner, ".htm") ||
                         contains(inner, ".js") || contains(inner, ".html");
        // The contract promises a *relative* source path that a consumer joins onto
        // the project dir. An absolute path or a ".." segment would escape that root,
        // so reject those: keep the whole heading as the name and emit no path.
        bool traversal = !inner.empty() && (inner.front() == '/' || inner == ".." ||
                                            inner.starts_with("../") || contains(inner, "/../") ||
                                            inner.ends_with("/.."));
        if (path_like && !traversal) {
            path = std::string(inner);
            name = std::string(trim(text.substr(0, open)));
            return;
        }
    }
    name = std::string(text);
    path.clear();
}

// Split a comma/`/`-separated list of system slugs into trimmed non-empty items.
std::vector<std::string> split_systems(std::string_view v) {
    std::vector<std::string> out;
    v = strip_emphasis(v);
    size_t start = 0;
    for (size_t i = 0; i <= v.size(); ++i) {
        if (i == v.size() || v[i] == ',' || v[i] == '/') {
            auto item = trim(v.substr(start, i - start));
            if (!item.empty()) out.emplace_back(item);
            start = i + 1;
        }
    }
    return out;
}

void sort_unique(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
}

// A Markdown table separator row (cells are only dashes/colons/spaces).
bool is_table_separator(std::string_view line) {
    line = trim(line);
    if (line.empty() || line.front() != '|') return false;
    for (char c : line)
        if (c != '|' && c != '-' && c != ':' && c != ' ') return false;
    return contains(line, "-");
}

std::vector<std::string> table_cells(std::string_view line) {
    std::vector<std::string> cells;
    std::string_view t = trim(line);
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|') t.remove_suffix(1);
    size_t start = 0;
    for (size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == '|') {
            cells.emplace_back(strip_emphasis(t.substr(start, i - start)));
            start = i + 1;
        }
    }
    return cells;
}

// Which top-level section a `##`(+) heading names, for routing content.
enum class Section { none, fidelity, systems, screens, tokens, interactions };

Section classify_section(std::string_view htext) {
    std::string h = to_lower(strip_emphasis(htext));
    if (contains(h, "fidelity")) return Section::fidelity;
    if (contains(h, "design system") || contains(h, "bound")) return Section::systems;
    if (contains(h, "screen")) return Section::screens;
    if (contains(h, "token")) return Section::tokens;
    if (contains(h, "interaction") || contains(h, "state") || contains(h, "behavior"))
        return Section::interactions;
    return Section::none;
}

}  // namespace

std::string_view fidelity_intent_name(FidelityIntent intent) {
    switch (intent) {
        case FidelityIntent::hifi: return "hifi";
        case FidelityIntent::lofi: return "lofi";
        case FidelityIntent::unspecified: break;
    }
    return "unspecified";
}

FidelityIntent fidelity_intent_from_text(std::string_view text) {
    std::string t = to_lower(strip_emphasis(text));
    // An explicit hi-fi/lo-fi token is a deliberate declaration and wins
    // outright over incidental prose — "lo-fi (don't trace pixels)" is lo-fi
    // even though it mentions "pixels". A line carrying both is contradictory.
    bool hi = contains(t, "hi-fi") || contains(t, "hifi") || contains(t, "hi fi");
    bool lo = contains(t, "lo-fi") || contains(t, "lofi") || contains(t, "lo fi");
    if (hi != lo) return hi ? FidelityIntent::hifi : FidelityIntent::lofi;
    if (hi && lo) return FidelityIntent::unspecified;  // both declared — ambiguous
    // No explicit token: fall back to weaker intent keywords. Conflicting weak
    // signals (e.g. "no pixel matching; adapt to our system" has both "pixel"
    // and "adapt") resolve to unspecified rather than whichever appears first —
    // keyword matching cannot read the negation, so it must not guess.
    bool hi_kw = false, lo_kw = false;
    for (std::string_view w : {"high", "pixel", "exact", "faithful", "recreate",
                               "1:1", "one-to-one"})
        if (contains(t, w)) { hi_kw = true; break; }
    for (std::string_view w : {"low", "restyle", "re-style", "reskin", "re-skin",
                               "adapt", "reinterpret"})
        if (contains(t, w)) { lo_kw = true; break; }
    if (hi_kw != lo_kw) return hi_kw ? FidelityIntent::hifi : FidelityIntent::lofi;
    return FidelityIntent::unspecified;
}

HandoffContract parse_handoff_readme(std::string_view markdown, std::string_view source) {
    HandoffContract c;
    c.format_version = std::string(kHandoffFormatVersion);
    c.source = std::string(source);

    Section section = Section::none;
    HandoffScreen* current_screen = nullptr;

    // Merge fidelity readings across the whole document: a single explicit
    // declaration wins, but two contradictory explicit declarations
    // (`Fidelity: hi-fi` on one line, `lo-fi` on another) lock the result to
    // unspecified rather than letting the first line win by accident.
    bool fidelity_locked = false;
    auto set_fidelity = [&](FidelityIntent add) {
        if (fidelity_locked || add == FidelityIntent::unspecified) return;
        if (c.fidelity == FidelityIntent::unspecified) {
            c.fidelity = add;
        } else if (c.fidelity != add) {
            c.fidelity = FidelityIntent::unspecified;
            fidelity_locked = true;
        }
    };

    size_t pos = 0;
    while (pos <= markdown.size()) {
        auto nl = markdown.find('\n', pos);
        std::string_view line =
            markdown.substr(pos, (nl == std::string_view::npos ? markdown.size() : nl) - pos);
        pos = (nl == std::string_view::npos) ? markdown.size() + 1 : nl + 1;

        int level = heading_level(line);
        if (level == 1) {  // document title — reset section context
            section = Section::none;
            current_screen = nullptr;
            continue;
        }
        if (level == 2) {
            section = classify_section(heading_text(line));
            current_screen = nullptr;
            // A `## Fidelity: hi-fi` heading carries the value inline.
            if (section == Section::fidelity) {
                auto htext = heading_text(line);
                if (auto colon = htext.find(':'); colon != std::string_view::npos)
                    set_fidelity(fidelity_intent_from_text(htext.substr(colon + 1)));
            }
            continue;
        }
        if (level == 3 && section == Section::screens) {
            c.screens.emplace_back();
            current_screen = &c.screens.back();
            split_name_path(heading_text(line), current_screen->name, current_screen->path);
            continue;
        }

        std::string_view t = trim(line);

        // A list bullet (`- `/`* ` + space) belongs to its section (a screen
        // spec or an interaction note), never the global key parser — otherwise
        // `* Design system: x` under a screen would be stolen into the global
        // contract and dropped as a spec. `**Bold:**` is not a bullet (the char
        // after `*` is another `*`, not whitespace), so it still parses.
        bool is_bullet = t.size() >= 2 && (t[0] == '-' || t[0] == '*') &&
                         (t[1] == ' ' || t[1] == '\t');

        // `Fidelity:` / `Bound to:` / `Design system:` key lines work anywhere,
        // not just under a matching heading — they are commonly in the preamble.
        if (!is_bullet) {
            std::string_view stripped = strip_emphasis(t);
            auto colon = stripped.find(':');
            if (colon != std::string_view::npos) {
                std::string key = to_lower(strip_marker_chars(stripped.substr(0, colon)));
                std::string_view val = stripped.substr(colon + 1);
                if (key == "fidelity") {
                    set_fidelity(fidelity_intent_from_text(val));
                    continue;
                }
                if (key == "bound to" || key == "design system" || key == "design systems") {
                    for (auto& s : split_systems(val)) c.design_systems.push_back(std::move(s));
                    continue;
                }
            }
        }

        switch (section) {
            case Section::fidelity:
                if (!t.empty()) set_fidelity(fidelity_intent_from_text(t));
                break;
            case Section::systems:
                if (auto b = bullet_text(line); !b.empty())
                    for (auto& s : split_systems(b)) c.design_systems.push_back(std::move(s));
                break;
            case Section::screens:
                if (current_screen) {
                    std::string k, v;
                    if (parse_kv_bullet(line, k, v)) current_screen->specs.emplace_back(k, v);
                }
                break;
            case Section::tokens:
                if (is_table_separator(t)) break;
                if (!t.empty() && t.front() == '|') {
                    auto cells = table_cells(t);
                    if (cells.size() >= 2 && !cells[0].empty()) {
                        std::string k0 = to_lower(cells[0]);
                        if (k0 != "token" && k0 != "name" && k0 != "key")  // skip header row
                            c.tokens.emplace_back(cells[0], cells[1]);
                    }
                }
                break;
            case Section::interactions:
                if (auto b = bullet_text(line); !b.empty())
                    c.interactions.emplace_back(strip_emphasis(b));
                break;
            case Section::none:
                break;
        }
    }

    sort_unique(c.design_systems);
    return c;
}

void merge_design_systems(HandoffContract& contract, const std::vector<std::string>& slugs) {
    for (const auto& s : slugs)
        if (!s.empty()) contract.design_systems.push_back(s);
    sort_unique(contract.design_systems);
}

std::string handoff_contract_json(const HandoffContract& contract) {
    auto root = choc::value::createObject("");
    root.addMember("format_version", contract.format_version);
    root.addMember("source", contract.source);
    root.addMember("fidelity", std::string(fidelity_intent_name(contract.fidelity)));

    auto systems = choc::value::createEmptyArray();
    for (const auto& s : contract.design_systems) systems.addArrayElement(s);
    root.addMember("design_systems", systems);

    auto screens = choc::value::createEmptyArray();
    for (const auto& s : contract.screens) {
        auto so = choc::value::createObject("");
        so.addMember("name", s.name);
        so.addMember("path", s.path);
        auto specs = choc::value::createObject("");
        // A README may repeat a spec key ("- Padding: 8" twice). choc's addMember
        // throws on a duplicate object key, so use setMember (last value wins) to
        // keep serialization total over untrusted input rather than aborting.
        for (const auto& [k, v] : s.specs) specs.setMember(k, v);
        so.addMember("specs", specs);
        screens.addArrayElement(so);
    }
    root.addMember("screens", screens);

    auto tokens = choc::value::createEmptyArray();
    for (const auto& [k, v] : contract.tokens) {
        auto to = choc::value::createObject("");
        to.addMember("name", k);
        to.addMember("value", v);
        tokens.addArrayElement(to);
    }
    root.addMember("tokens", tokens);

    auto inter = choc::value::createEmptyArray();
    for (const auto& i : contract.interactions) inter.addArrayElement(i);
    root.addMember("interactions", inter);

    return choc::json::toString(root, /*pretty=*/true);
}

}  // namespace pulp::design
