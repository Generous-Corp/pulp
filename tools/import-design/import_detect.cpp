// SPDX-License-Identifier: MIT
//
// pulp #1031 — implementation of the versioned import detector.

#include "import_detect.hpp"

#include "../cli/json_parser.hpp"

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace pulp::import_detect {

namespace {

using pulp::cli::pkg::JsonParser;
using pulp::cli::pkg::JsonValue;

std::string read_text_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

FingerprintClause parse_clause(const JsonValue& v) {
    FingerprintClause c;
    if (v.type != JsonValue::Object) return c;
    if (auto* k = v.get("kind"); k && k->type == JsonValue::String) {
        c.raw_kind = k->str_val;
        if (k->str_val == "directory-files")
            c.kind = FingerprintClause::Kind::directory_files;
        else if (k->str_val == "html-script-src")
            c.kind = FingerprintClause::Kind::html_script_src;
        else if (k->str_val == "html-script-type")
            c.kind = FingerprintClause::Kind::html_script_type;
        else if (k->str_val == "tailwind-config-token")
            c.kind = FingerprintClause::Kind::tailwind_config_token;
        else if (k->str_val == "filename")
            c.kind = FingerprintClause::Kind::filename;
        else if (k->str_val == "frontmatter-fence")
            c.kind = FingerprintClause::Kind::frontmatter_fence;
        else if (k->str_val == "frontmatter-key")
            c.kind = FingerprintClause::Kind::frontmatter_key;
        else if (k->str_val == "json-key-equals")
            c.kind = FingerprintClause::Kind::json_key_equals;
    }
    if (auto* files = v.get("files"); files && files->type == JsonValue::Array)
        c.files = files->as_string_array();
    if (auto* re = v.get("regex"); re && re->type == JsonValue::String)
        c.regex = re->str_val;
    if (auto* val = v.get("value"); val && val->type == JsonValue::String)
        c.value = val->str_val;
    if (auto* any = v.get("any-of"); any && any->type == JsonValue::Array)
        c.any_of = any->as_string_array();
    if (auto* req = v.get("required"); req && req->type == JsonValue::String)
        c.required = req->str_val;
    if (auto* pp = v.get("path"); pp && pp->type == JsonValue::String)
        c.path = pp->str_val;
    return c;
}

// Lowercase + trim helper for tag-attr scrapes.
std::string lower_trim(std::string s) {
    auto first = s.find_first_not_of(" \t\r\n");
    auto last = s.find_last_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    s = s.substr(first, last - first + 1);
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Extract all <script ...> attribute values for `attr` from `html`.
// Hand-rolled to avoid pulling a full HTML parser into the CLI build.
std::vector<std::string> scrape_script_attr(const std::string& html, const std::string& attr) {
    std::vector<std::string> out;
    std::string html_lower = html;
    for (auto& c : html_lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string needle = attr;
    for (auto& c : needle)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    size_t i = 0;
    while (i < html.size()) {
        // Find next "<script" (case-insensitive) opening.
        auto open = html_lower.find("<script", i);
        if (open == std::string::npos) break;
        const auto tag_name_end = open + 7;
        if (tag_name_end < html_lower.size()) {
            const char next = html_lower[tag_name_end];
            if (!(next == '>' || next == '/' || next == ' ' ||
                  next == '\t' || next == '\r' || next == '\n')) {
                i = tag_name_end;
                continue;
            }
        }
        // Find the closing '>' of this open tag.
        auto close = html.find('>', open);
        if (close == std::string::npos) break;
        std::string tag = html.substr(open, close - open);
        // Lowercase a copy for attr lookup; preserve original for value.
        std::string tag_lower = tag;
        for (auto& c : tag_lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        size_t a = 0;
        while ((a = tag_lower.find(needle + "=", a)) != std::string::npos) {
            if (a > 0) {
                const char before = tag_lower[a - 1];
                if (!(before == ' ' || before == '\t' ||
                      before == '\r' || before == '\n')) {
                    a += needle.size() + 1;
                    continue;
                }
            }
            // Skip past `attr=`.
            auto v = a + needle.size() + 1;
            if (v < tag.size() && (tag[v] == '"' || tag[v] == '\'')) {
                char q = tag[v++];
                auto end = tag.find(q, v);
                if (end != std::string::npos)
                    out.push_back(tag.substr(v, end - v));
            } else {
                auto end = tag.find_first_of(" \t>", v);
                if (end == std::string::npos) end = tag.size();
                out.push_back(tag.substr(v, end - v));
            }
            break;
        }
        i = close + 1;
    }
    return out;
}

// Greedy scrape of identifiers from a tailwind.config inline script.
// We grep for sequences inside theme.extend.colors / .spacing / similar
// keys. The detector matches against any-of clauses, so this just needs
// to surface representative tokens — we don't reconstruct a real tree.
std::vector<std::string> scrape_tailwind_tokens(const std::string& html) {
    std::set<std::string> seen;
    // Match identifier-like keys "<ident>": before a colon or comma in
    // a JS/JSON-ish context. Strict enough to skip noise but loose
    // enough to handle Material 3 hyphenated tokens like
    // "surface-container".
    static const std::regex token_re(
        R"REX("([a-zA-Z][a-zA-Z0-9_-]{1,40})"\s*:)REX",
        std::regex::ECMAScript);
    auto begin = std::sregex_iterator(html.begin(), html.end(), token_re);
    auto end = std::sregex_iterator{};
    for (auto it = begin; it != end; ++it)
        seen.insert((*it)[1].str());
    return {seen.begin(), seen.end()};
}

}  // namespace

// ── Manifest parsing ───────────────────────────────────────────────────

std::optional<ImportsManifest> parse_compat_json(const std::string& text) {
    if (text.empty()) return std::nullopt;
    JsonParser p{text, 0};
    JsonValue root;
    try { root = p.parse(); }
    catch (...) { return std::nullopt; }
    if (root.type != JsonValue::Object) return std::nullopt;

    ImportsManifest m;
    if (auto* sv = root.get("compat-schema-version");
        sv && sv->type == JsonValue::String)
        m.compat_schema_version = sv->str_val;

    auto* imports = root.get("imports");
    if (!imports || imports->type != JsonValue::Object) return std::nullopt;

    for (const auto& [src_name, src_val] : imports->obj()) {
        if (src_val.type != JsonValue::Object) continue;
        SourceEntry s;
        s.source = src_name;
        if (auto* pv = src_val.get("parser-version");
            pv && pv->type == JsonValue::String)
            s.parser_version = pv->str_val;

        if (auto* dfs = src_val.get("detected-formats");
            dfs && dfs->type == JsonValue::Array) {
            for (const auto& f : dfs->arr()) {
                if (f.type != JsonValue::Object) continue;
                FormatEntry e;
                e.parser_version = s.parser_version;
                if (auto* fv = f.get("format-version");
                    fv && fv->type == JsonValue::String)
                    e.format_version = fv->str_val;
                if (auto* iv = f.get("introduced");
                    iv && iv->type == JsonValue::String)
                    e.introduced = iv->str_val;
                if (auto* dv = f.get("deprecated");
                    dv && dv->type == JsonValue::String)
                    e.deprecated = dv->str_val;
                if (auto* nv = f.get("notes");
                    nv && nv->type == JsonValue::String)
                    e.notes = nv->str_val;
                if (auto* mv = f.get("match");
                    mv && mv->type == JsonValue::String)
                    e.match = mv->str_val;
                if (auto* cv = f.get("min-confidence-pct");
                    cv && cv->type == JsonValue::Number)
                    e.min_confidence_pct = static_cast<int>(cv->num_val);
                if (auto* fp = f.get("fingerprint");
                    fp && fp->type == JsonValue::Array) {
                    for (const auto& cl : fp->arr())
                        e.fingerprint.push_back(parse_clause(cl));
                }
                s.formats.push_back(std::move(e));
            }
        }
        m.sources.push_back(std::move(s));
    }
    return m;
}

fs::path find_compat_json(const fs::path& start_dir) {
    fs::path cur = fs::absolute(start_dir);
    for (int i = 0; i < 32; ++i) {
        auto candidate = cur / "compat.json";
        if (fs::exists(candidate)) return candidate;
        if (!cur.has_parent_path() || cur.parent_path() == cur) break;
        cur = cur.parent_path();
    }
    return {};
}

// ── Fingerprinting ─────────────────────────────────────────────────────

InputSnapshot snapshot_input(const fs::path& input) {
    InputSnapshot snap;
    snap.root = input;
    if (!fs::exists(input)) return snap;

    if (fs::is_directory(input)) {
        snap.is_directory = true;
        std::error_code ec;
        std::vector<fs::path> html_candidates;
        for (auto& entry : fs::directory_iterator(input, ec)) {
            if (ec) break;
            auto path = entry.path();
            snap.directory_basenames.push_back(path.filename().string());
            std::error_code file_ec;
            if (entry.is_regular_file(file_ec)) {
                auto ext = lower_trim(path.extension().string());
                if (ext == ".html" || ext == ".htm")
                    html_candidates.push_back(path);
            }
        }
        std::sort(snap.directory_basenames.begin(), snap.directory_basenames.end());
        std::sort(html_candidates.begin(), html_candidates.end());

        // Prefer code.html (Stitch-style), then index.html.
        for (auto candidate : {"code.html", "index.html"}) {
            auto p = input / candidate;
            if (fs::exists(p)) {
                snap.html_text = read_text_file(p);
                break;
            }
        }
        if (snap.html_text.empty() && !html_candidates.empty())
            snap.html_text = read_text_file(html_candidates.front());
    } else {
        snap.filename = input.filename().string();
        snap.directory_basenames.push_back(snap.filename);
        snap.html_text = read_text_file(input);
    }

    if (!snap.html_text.empty()) {
        snap.script_srcs = scrape_script_attr(snap.html_text, "src");
        snap.script_types = scrape_script_attr(snap.html_text, "type");
        snap.tailwind_tokens = scrape_tailwind_tokens(snap.html_text);
    }

    // ── DESIGN.md frontmatter probe ────────────────────────────────────
    // Cheap leading-bytes check: only walk the file as frontmatter when it
    // starts with `---` followed by a newline. Then look for the closing
    // fence and collect top-level YAML keys. Hand-rolled rather than
    // pulling yaml-cpp into the detector library — keeps the detector
    // unit tests fast and the detect-only path zero-cost on non-MD inputs.
    auto probe_frontmatter = [&snap](const std::string& text) {
        if (text.size() < 4 || text.substr(0, 3) != "---") return;
        if (text[3] != '\n' && text[3] != '\r') return;
        size_t i = (text[3] == '\r' && text.size() > 4 && text[4] == '\n') ? 5 : 4;
        // Find a line that is exactly "---" (optionally with trailing CR).
        size_t close = std::string::npos;
        for (size_t pos = i; pos < text.size(); ) {
            size_t eol = text.find('\n', pos);
            std::string line = text.substr(pos, (eol == std::string::npos ? text.size() : eol) - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            // Trim trailing spaces.
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
            if (line == "---") { close = pos; break; }
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        if (close == std::string::npos) return;
        snap.has_frontmatter_fence = true;
        // Walk every top-level YAML key — defined as `^[A-Za-z_][A-Za-z0-9_-]*:`
        // with zero leading whitespace, between the opening and closing fences.
        for (size_t pos = i; pos < close; ) {
            size_t eol = text.find('\n', pos);
            size_t end = (eol == std::string::npos || eol > close) ? close : eol;
            std::string line = text.substr(pos, end - pos);
            if (eol == std::string::npos) pos = close;
            else pos = eol + 1;
            if (line.empty() || line[0] == ' ' || line[0] == '\t' || line[0] == '#') continue;
            // Strip CR.
            if (!line.empty() && line.back() == '\r') line.pop_back();
            auto colon = line.find(':');
            if (colon == std::string::npos) continue;
            std::string key = line.substr(0, colon);
            // Validate key shape.
            bool ok = !key.empty() && (std::isalpha(static_cast<unsigned char>(key[0])) || key[0] == '_');
            for (char c : key) {
                if (!ok) break;
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) ok = false;
            }
            if (ok) snap.frontmatter_keys.push_back(key);
        }
    };
    if (!snap.html_text.empty()) {
        // The `html_text` field is also the "primary text" for any input.
        // Probe only when the filename ends in `.md` to avoid wasting
        // cycles on non-Markdown inputs.
        std::string lower_name = snap.filename;
        std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_name.size() >= 3 && lower_name.substr(lower_name.size() - 3) == ".md")
            probe_frontmatter(snap.html_text);
    }
    return snap;
}

bool match_clause(const FingerprintClause& clause, const InputSnapshot& snap) {
    switch (clause.kind) {
        case FingerprintClause::Kind::directory_files: {
            if (clause.files.empty()) return false;
            for (const auto& want : clause.files) {
                bool found = false;
                for (const auto& have : snap.directory_basenames) {
                    if (have == want) { found = true; break; }
                }
                if (!found) return false;
            }
            return true;
        }
        case FingerprintClause::Kind::html_script_src: {
            if (clause.regex.empty() || snap.script_srcs.empty()) return false;
            std::regex re;
            try { re = std::regex(clause.regex, std::regex::ECMAScript); }
            catch (...) { return false; }
            for (const auto& src : snap.script_srcs)
                if (std::regex_search(src, re)) return true;
            return false;
        }
        case FingerprintClause::Kind::html_script_type: {
            if (clause.value.empty() || snap.script_types.empty()) return false;
            auto want = lower_trim(clause.value);
            for (const auto& t : snap.script_types)
                if (lower_trim(t) == want) return true;
            return false;
        }
        case FingerprintClause::Kind::tailwind_config_token: {
            if (clause.any_of.empty() || snap.tailwind_tokens.empty()) return false;
            for (const auto& want : clause.any_of) {
                for (const auto& have : snap.tailwind_tokens)
                    if (have == want) return true;
            }
            return false;
        }
        case FingerprintClause::Kind::filename: {
            if (clause.regex.empty() || snap.filename.empty()) return false;
            // Honour leading `(?i)` as a case-insensitive flag rather than
            // an inline group — libc++'s ECMAScript engine handles
            // `(?i)` unevenly across versions, and this is the only
            // flag DESIGN.md's fingerprint needs.
            std::string pattern = clause.regex;
            auto flags = std::regex::ECMAScript;
            if (pattern.compare(0, 4, "(?i)") == 0) {
                pattern = pattern.substr(4);
                flags = flags | std::regex::icase;
            }
            std::regex re;
            try { re = std::regex(pattern, flags); }
            catch (...) { return false; }
            return std::regex_search(snap.filename, re);
        }
        case FingerprintClause::Kind::frontmatter_fence: {
            // Either value="---" (literal) or empty (presence-only check).
            return snap.has_frontmatter_fence;
        }
        case FingerprintClause::Kind::frontmatter_key: {
            if (snap.frontmatter_keys.empty()) return false;
            if (!clause.required.empty()) {
                for (const auto& have : snap.frontmatter_keys)
                    if (have == clause.required) return true;
                return false;
            }
            for (const auto& want : clause.any_of) {
                for (const auto& have : snap.frontmatter_keys)
                    if (have == want) return true;
            }
            return false;
        }
        case FingerprintClause::Kind::json_key_equals: {
            // Parse snap.html_text as JSON (the field name is historical —
            // snapshot_input reads any text file into it). Walk the dotted
            // path and compare the resulting string value to clause.value.
            // Returns false on any parse failure / missing key / type
            // mismatch.
            if (clause.path.empty() || clause.value.empty() || snap.html_text.empty())
                return false;
            try {
                JsonParser p(snap.html_text);
                auto root = p.parse();
                const JsonValue* cur = &root;
                std::string remaining = clause.path;
                while (!remaining.empty()) {
                    auto dot = remaining.find('.');
                    std::string segment = (dot == std::string::npos)
                                              ? remaining
                                              : remaining.substr(0, dot);
                    if (cur->type != JsonValue::Object) return false;
                    cur = cur->get(segment);
                    if (cur == nullptr) return false;
                    if (dot == std::string::npos) break;
                    remaining = remaining.substr(dot + 1);
                }
                if (cur == nullptr || cur->type != JsonValue::String) return false;
                return cur->str_val == clause.value;
            } catch (...) {
                return false;
            }
        }
        case FingerprintClause::Kind::unknown:
        default:
            return false;
    }
}

DetectionResult detect(const ImportsManifest& manifest, const InputSnapshot& snap) {
    DetectionResult best;
    best.ok = true;

    int best_matched = -1;
    for (const auto& src : manifest.sources) {
        for (const auto& fmt : src.formats) {
            int matched = 0;
            std::vector<std::string> matched_kinds;
            std::vector<std::string> unmatched_kinds;
            for (const auto& cl : fmt.fingerprint) {
                if (match_clause(cl, snap)) {
                    ++matched;
                    matched_kinds.push_back(cl.raw_kind);
                } else {
                    unmatched_kinds.push_back(cl.raw_kind);
                }
            }
            const int total = static_cast<int>(fmt.fingerprint.size());
            const int confidence = total > 0 ? (matched * 100 / total) : 0;
            // Strict modes (DESIGN.md): reject this format unless every clause
            // matches (match="all-of") or the confidence clears the floor
            // (min_confidence_pct). This prevents generic Markdown files
            // with `---fence---` from masquerading as DESIGN.md.
            if (fmt.match == "all-of" && matched < total) continue;
            if (fmt.min_confidence_pct > 0 && confidence < fmt.min_confidence_pct) continue;
            // Prefer (a) more matches, then (b) higher confidence ratio.
            const bool better =
                matched > best_matched ||
                (matched == best_matched && matched > 0 &&
                 total > 0 && best.total_clauses > 0 &&
                 matched * best.total_clauses > best.matched_clauses * total);
            if (matched > 0 && better) {
                best.source = src.source;
                best.format_version = fmt.format_version;
                best.parser_version = fmt.parser_version;
                best.matched_clauses = matched;
                best.total_clauses = total;
                best.confidence_pct = total > 0 ? (matched * 100 / total) : 0;
                best.matched_kinds = std::move(matched_kinds);
                best.unmatched_kinds = std::move(unmatched_kinds);
                best_matched = matched;
            }
        }
    }
    return best;
}

// ── Reporting ──────────────────────────────────────────────────────────

NewFormatReport build_new_format_report(const ImportsManifest& manifest,
                                        const InputSnapshot& snap,
                                        const DetectionResult& closest) {
    NewFormatReport r;
    r.candidate_source = closest.source;
    r.based_on_source = closest.source;
    r.based_on_format_version = closest.format_version;
    // Placeholder candidate version. Caller hand-edits before
    // committing to compat.json.
    if (!closest.format_version.empty())
        r.candidate_format_version = closest.format_version + "+next";
    else
        r.candidate_format_version = "TODO-set-version";

    // Diff tailwind tokens against the closest format's any-of list.
    std::set<std::string> known;
    for (const auto& src : manifest.sources) {
        if (src.source != closest.source) continue;
        for (const auto& fmt : src.formats) {
            if (fmt.format_version != closest.format_version) continue;
            for (const auto& cl : fmt.fingerprint) {
                if (cl.kind == FingerprintClause::Kind::tailwind_config_token)
                    for (const auto& tok : cl.any_of) known.insert(tok);
            }
        }
    }
    for (const auto& tok : snap.tailwind_tokens) {
        if (!known.empty() && known.count(tok) == 0)
            r.additions.push_back(tok);
    }
    // Cap suggestions so the output stays human-readable.
    if (r.additions.size() > 20) r.additions.resize(20);
    return r;
}

std::string render_new_format_json(const NewFormatReport& r) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"candidate-source\": \"" << r.candidate_source << "\",\n";
    o << "  \"candidate-format-version\": \"" << r.candidate_format_version << "\",\n";
    o << "  \"fingerprint-additions\": [";
    if (!r.additions.empty()) {
        o << "\n    {\"kind\": \"tailwind-config-token\", \"any-of\": [";
        for (size_t i = 0; i < r.additions.size(); ++i) {
            if (i) o << ", ";
            o << "\"" << r.additions[i] << "\"";
        }
        o << "]}\n  ";
    }
    o << "],\n";
    o << "  \"fingerprint-removals\": [";
    if (!r.removals.empty()) {
        o << "\n    ";
        for (size_t i = 0; i < r.removals.size(); ++i) {
            if (i) o << ", ";
            o << "\"" << r.removals[i] << "\"";
        }
        o << "\n  ";
    }
    o << "],\n";
    o << "  \"based-on\": {\"source\": \"" << r.based_on_source
      << "\", \"format-version\": \"" << r.based_on_format_version << "\"}\n";
    o << "}\n";
    return o.str();
}

}  // namespace pulp::import_detect
