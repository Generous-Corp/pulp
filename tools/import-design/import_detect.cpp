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
    }
    if (auto* files = v.get("files"); files && files->type == JsonValue::Array)
        c.files = files->as_string_array();
    if (auto* re = v.get("regex"); re && re->type == JsonValue::String)
        c.regex = re->str_val;
    if (auto* val = v.get("value"); val && val->type == JsonValue::String)
        c.value = val->str_val;
    if (auto* any = v.get("any-of"); any && any->type == JsonValue::Array)
        c.any_of = any->as_string_array();
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
    size_t i = 0;
    while (i < html.size()) {
        // Find next "<script" (case-insensitive) opening.
        auto open = html.find("<script", i);
        if (open == std::string::npos) break;
        // Find the closing '>' of this open tag.
        auto close = html.find('>', open);
        if (close == std::string::npos) break;
        std::string tag = html.substr(open, close - open);
        // Lowercase a copy for attr lookup; preserve original for value.
        std::string tag_lower = tag;
        for (auto& c : tag_lower)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        std::string needle = attr;
        for (auto& c : needle)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        auto a = tag_lower.find(needle + "=");
        if (a != std::string::npos) {
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
        for (auto& entry : fs::directory_iterator(input, ec)) {
            if (!ec)
                snap.directory_basenames.push_back(entry.path().filename().string());
        }
        // Prefer code.html (Stitch-style), then index.html.
        for (auto candidate : {"code.html", "index.html"}) {
            auto p = input / candidate;
            if (fs::exists(p)) {
                snap.html_text = read_text_file(p);
                break;
            }
        }
    } else {
        snap.directory_basenames.push_back(input.filename().string());
        snap.html_text = read_text_file(input);
    }

    if (!snap.html_text.empty()) {
        snap.script_srcs = scrape_script_attr(snap.html_text, "src");
        snap.script_types = scrape_script_attr(snap.html_text, "type");
        snap.tailwind_tokens = scrape_tailwind_tokens(snap.html_text);
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
