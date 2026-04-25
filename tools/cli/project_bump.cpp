// project_bump.cpp — Pure-logic surface for `pulp project bump` / undo.
//
// Release-discovery Slice 7 (#564 / parent #499). See
// `project_bump.hpp` for the design contract and schema.

#include "project_bump.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>

namespace pulp::cli::project_bump {

namespace {

// ── Small JSON reader (same pattern as projects_registry.cpp) ───────────────

struct TinyJson {
    const std::string& src;
    std::size_t pos = 0;

    void skip_ws() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos;
            else break;
        }
    }

    bool match(char c) {
        skip_ws();
        if (pos < src.size() && src[pos] == c) { ++pos; return true; }
        return false;
    }

    std::string read_string() {
        skip_ws();
        if (pos >= src.size() || src[pos] != '"') return {};
        ++pos;
        std::string out;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return out;
            if (c == '\\' && pos < src.size()) {
                char esc = src[pos++];
                switch (esc) {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case '/':  out += '/';  break;
                    case 'n':  out += '\n'; break;
                    case 'r':  out += '\r'; break;
                    case 't':  out += '\t'; break;
                    case 'u':
                        if (pos + 4 <= src.size()) pos += 4;
                        out += '?';
                        break;
                    default: out += esc; break;
                }
            } else {
                out += c;
            }
        }
        return out;
    }

    // Read true/false/null/numeric literal as a raw substring.
    std::string read_primitive() {
        skip_ws();
        std::string out;
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ',' || c == '}' || c == ']' || c == ' ' ||
                c == '\t' || c == '\n' || c == '\r') break;
            out += c;
            ++pos;
        }
        return out;
    }

    void skip_value() {
        skip_ws();
        if (pos >= src.size()) return;
        char c = src[pos];
        if (c == '"') {
            (void)read_string();
        } else if (c == '{' || c == '[') {
            char open = c;
            char close = (c == '{') ? '}' : ']';
            ++pos;
            int depth = 1;
            while (pos < src.size() && depth > 0) {
                char d = src[pos];
                if (d == '"') { (void)read_string(); continue; }
                if (d == open) ++depth;
                else if (d == close) --depth;
                ++pos;
            }
        } else {
            while (pos < src.size() &&
                   src[pos] != ',' && src[pos] != '}' && src[pos] != ']') {
                ++pos;
            }
        }
    }
};

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(c) & 0xff);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Rewrite the substring of `src` [start, end) with `replacement`.
std::string splice(const std::string& src, std::size_t start,
                   std::size_t end, const std::string& replacement) {
    std::string out;
    out.reserve(src.size() - (end - start) + replacement.size());
    out.append(src, 0, start);
    out.append(replacement);
    out.append(src, end, std::string::npos);
    return out;
}

bool is_hex_only(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

}  // namespace

// ── Pin discovery / normalization ───────────────────────────────────────────

bool pin_has_v_prefix(const std::string& raw_pin) {
    return !raw_pin.empty() && (raw_pin.front() == 'v' || raw_pin.front() == 'V');
}

std::string normalize_pin(const std::string& raw_pin) {
    std::string s = raw_pin;
    if (pin_has_v_prefix(s)) s.erase(0, 1);
    auto t = parse_semver_strict(s);
    if (!t.ok) return {};
    return std::to_string(t.major) + "." +
           std::to_string(t.minor) + "." +
           std::to_string(t.patch);
}

SemverTriple parse_semver_strict(const std::string& s) {
    SemverTriple out;
    std::string v = s;
    if (!v.empty() && (v.front() == 'v' || v.front() == 'V')) v.erase(0, 1);
    std::regex re(R"(^(\d+)\.(\d+)\.(\d+)$)");
    std::smatch m;
    if (!std::regex_match(v, m, re)) return out;
    try {
        out.major = std::stoi(m[1].str());
        out.minor = std::stoi(m[2].str());
        out.patch = std::stoi(m[3].str());
        out.ok = true;
    } catch (...) {}
    return out;
}

int compare_semver(const SemverTriple& a, const SemverTriple& b) {
    if (!a.ok || !b.ok) return 0;
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool is_downgrade(const std::string& from, const std::string& to) {
    auto a = parse_semver_strict(from);
    auto b = parse_semver_strict(to);
    if (!a.ok || !b.ok) return false;
    return compare_semver(b, a) < 0;
}

// Find a pin literal between start and end in `src` looking for the
// first token after `anchor` matching a pin-ish shape. Returns
// (start, end) substring range covering the literal, or (0,0) on
// failure.
static bool find_literal_after(const std::string& src,
                               std::size_t search_start,
                               std::size_t call_end,
                               std::string& out_literal,
                               std::size_t& out_start,
                               std::size_t& out_end) {
    // Skip whitespace / newlines
    std::size_t p = search_start;
    while (p < call_end) {
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++p;
        else break;
    }
    if (p >= call_end) return false;
    std::size_t lit_start = p;
    // Read until next whitespace or closing paren.
    while (p < call_end) {
        char c = src[p];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ')') break;
        ++p;
    }
    if (p == lit_start) return false;
    out_literal = src.substr(lit_start, p - lit_start);
    out_start = lit_start;
    out_end = p;
    return true;
}

// Locate the next `FetchContent_Declare(pulp ...)` call and within it
// the GIT_TAG argument.
static bool find_fetch_content(const std::string& src, PinSite& out) {
    // Regex is intentionally narrow: match "FetchContent_Declare(" then
    // whitespace then "pulp" as its own token, then locate the matching
    // close paren by forward scan.
    std::regex decl_re(R"(FetchContent_Declare\s*\(\s*pulp\b)",
                       std::regex_constants::ECMAScript);
    auto begin = std::sregex_iterator(src.begin(), src.end(), decl_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        // Scan forward for matching close paren (no nesting in CMake).
        std::size_t p = static_cast<std::size_t>(it->position()) + it->length();
        std::size_t call_end = std::string::npos;
        for (std::size_t q = p; q < src.size(); ++q) {
            if (src[q] == ')') { call_end = q; break; }
        }
        if (call_end == std::string::npos) continue;

        // Find GIT_TAG inside [p, call_end)
        std::regex tag_re(R"(\bGIT_TAG\b)", std::regex_constants::ECMAScript);
        std::string sub = src.substr(p, call_end - p);
        std::smatch m;
        if (!std::regex_search(sub, m, tag_re)) continue;
        std::size_t tag_pos = p + static_cast<std::size_t>(m.position()) + m.length();
        std::string literal;
        std::size_t ls = 0, le = 0;
        if (!find_literal_after(src, tag_pos, call_end, literal, ls, le)) continue;
        out.kind = PinKind::FetchContentGitTag;
        out.current_pin = literal;
        out.start = ls;
        out.end = le;
        return true;
    }
    return false;
}

// Locate first `<macro>(...)` where the first VERSION keyword's
// argument is the pin literal.
static bool find_version_in_call(const std::string& src,
                                 const std::regex& call_re,
                                 PinKind kind,
                                 PinSite& out) {
    auto begin = std::sregex_iterator(src.begin(), src.end(), call_re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::size_t p = static_cast<std::size_t>(it->position()) + it->length();
        std::size_t call_end = std::string::npos;
        // Handle balanced parens conservatively — CMake arg lists
        // don't nest, but a multi-line project() can span lines.
        int depth = 1;
        for (std::size_t q = p; q < src.size(); ++q) {
            if (src[q] == '(') ++depth;
            else if (src[q] == ')') {
                --depth;
                if (depth == 0) { call_end = q; break; }
            }
        }
        if (call_end == std::string::npos) continue;

        std::regex ver_re(R"(\bVERSION\b)", std::regex_constants::ECMAScript);
        std::string sub = src.substr(p, call_end - p);
        std::smatch m;
        if (!std::regex_search(sub, m, ver_re)) continue;
        std::size_t ver_pos = p + static_cast<std::size_t>(m.position()) + m.length();
        std::string literal;
        std::size_t ls = 0, le = 0;
        if (!find_literal_after(src, ver_pos, call_end, literal, ls, le)) continue;
        out.kind = kind;
        out.current_pin = literal;
        out.start = ls;
        out.end = le;
        return true;
    }
    return false;
}

static std::size_t find_balanced_call_end(const std::string& src, std::size_t p) {
    std::size_t call_end = std::string::npos;
    int depth = 1;
    for (std::size_t q = p; q < src.size(); ++q) {
        if (src[q] == '(') ++depth;
        else if (src[q] == ')') {
            --depth;
            if (depth == 0) { call_end = q; break; }
        }
    }
    return call_end;
}

PinSite find_find_package_pulp_version(const std::string& cmake_source) {
    PinSite site;
    std::regex re(R"(\bfind_package\s*\(\s*Pulp\b)",
                  std::regex_constants::ECMAScript);
    auto begin = std::sregex_iterator(cmake_source.begin(), cmake_source.end(), re);
    auto end = std::sregex_iterator();
    for (auto it = begin; it != end; ++it) {
        std::size_t p = static_cast<std::size_t>(it->position()) + it->length();
        auto call_end = find_balanced_call_end(cmake_source, p);
        if (call_end == std::string::npos) continue;

        std::string literal;
        std::size_t ls = 0, le = 0;
        if (!find_literal_after(cmake_source, p, call_end, literal, ls, le)) continue;
        if (normalize_pin(literal).empty()) continue;
        site.kind = PinKind::CMakeFindPackagePulpVersion;
        site.current_pin = literal;
        site.start = ls;
        site.end = le;
        return site;
    }
    return site;
}

PinSite find_toml_string_value(const std::string& toml_source,
                               const std::string& key,
                               PinKind kind) {
    PinSite site;
    std::regex re("(^|[\\r\\n])[ \\t]*" + key + "[ \\t]*=[ \\t]*\"([^\"]*)(\")",
                  std::regex_constants::ECMAScript);
    std::smatch m;
    if (!std::regex_search(toml_source, m, re)) return site;
    site.kind = kind;
    site.current_pin = m[2].str();
    site.start = static_cast<std::size_t>(m.position(2));
    site.end = site.start + site.current_pin.size();
    return site;
}

PinSite find_pin_site(const std::string& cmake_source) {
    PinSite site;
    // 1. FetchContent_Declare(pulp ... GIT_TAG vX.Y.Z)
    if (find_fetch_content(cmake_source, site)) return site;

    // 2. pulp_add_project(NAME VERSION X.Y.Z ...)
    {
        std::regex re(R"(\bpulp_add_project\s*\()",
                      std::regex_constants::ECMAScript);
        if (find_version_in_call(cmake_source, re,
                                 PinKind::PulpAddProject, site)) return site;
    }

    // 3. project(NAME VERSION X.Y.Z ...)
    {
        std::regex re(R"(\bproject\s*\()", std::regex_constants::ECMAScript);
        if (find_version_in_call(cmake_source, re,
                                 PinKind::ProjectVersion, site)) return site;
    }

    return site;  // kind = Unknown
}

bool refuse_dynamic_pin(const PinSite& site) {
    if (site.kind == PinKind::Unknown) return true;
    const std::string& raw = site.current_pin;
    if (raw.empty()) return true;

    // Semver shape? Safe.
    auto normalized = normalize_pin(raw);
    if (!normalized.empty()) return false;

    // Hex-only, 7-40 chars → SHA pin. Refuse.
    if (raw.size() >= 7 && raw.size() <= 40 && is_hex_only(raw)) return true;

    // Anything else (branch names, tags without semver, empty) →
    // refuse.
    return true;
}

std::optional<std::string> rewrite_pin(const std::string& cmake_source,
                                       const PinSite& site,
                                       const std::string& new_pin,
                                       bool new_pin_style_has_v) {
    if (site.kind == PinKind::Unknown) return std::nullopt;
    if (site.end <= site.start || site.end > cmake_source.size()) return std::nullopt;
    // Verify the byte span still matches what we captured. Defends
    // against a caller who mutated the source between find and rewrite.
    if (cmake_source.substr(site.start, site.end - site.start) != site.current_pin) {
        return std::nullopt;
    }
    std::string replacement = new_pin_style_has_v ? ("v" + new_pin) : new_pin;
    return splice(cmake_source, site.start, site.end, replacement);
}

// ── Undo batch JSON I/O ─────────────────────────────────────────────────────

const char* pin_kind_name(PinKind k) {
    switch (k) {
        case PinKind::PulpTomlSdkVersion: return "PulpTomlSdkVersion";
        case PinKind::PulpTomlSdkPath:    return "PulpTomlSdkPath";
        case PinKind::CMakeFindPackagePulpVersion: return "CMakeFindPackagePulpVersion";
        case PinKind::FetchContentGitTag: return "FetchContentGitTag";
        case PinKind::PulpAddProject:     return "PulpAddProject";
        case PinKind::ProjectVersion:     return "ProjectVersion";
        case PinKind::Unknown:            return "Unknown";
    }
    return "Unknown";
}

PinKind parse_pin_kind(const std::string& name) {
    if (name == "PulpTomlSdkVersion") return PinKind::PulpTomlSdkVersion;
    if (name == "PulpTomlSdkPath")    return PinKind::PulpTomlSdkPath;
    if (name == "CMakeFindPackagePulpVersion") return PinKind::CMakeFindPackagePulpVersion;
    if (name == "FetchContentGitTag") return PinKind::FetchContentGitTag;
    if (name == "PulpAddProject")     return PinKind::PulpAddProject;
    if (name == "ProjectVersion")     return PinKind::ProjectVersion;
    return PinKind::Unknown;
}

std::string serialize_undo_batch(const UndoBatch& batch) {
    std::ostringstream os;
    os << "{\n"
       << "  \"timestamp\": \""      << json_escape(batch.timestamp)      << "\",\n"
       << "  \"target_version\": \"" << json_escape(batch.target_version) << "\",\n"
       << "  \"entries\": [";
    bool first = true;
    for (const auto& e : batch.entries) {
        if (!first) os << ",";
        first = false;
        os << "\n    {"
           << "\"project_path\": \"" << json_escape(e.project_path.generic_string()) << "\", "
           << "\"project_name\": \"" << json_escape(e.project_name)                   << "\", "
           << "\"old_pin\": \""      << json_escape(e.old_pin)                        << "\", "
           << "\"old_pin_style_has_v\": " << (e.old_pin_style_has_v ? "true" : "false") << ", "
           << "\"pin_kind\": \""     << pin_kind_name(e.pin_kind)                     << "\", "
           << "\"status\": \""       << json_escape(e.status)                         << "\", "
           << "\"failure_reason\": \"" << json_escape(e.failure_reason)               << "\", "
           << "\"edits\": [";
        bool first_edit = true;
        for (const auto& edit : e.edits) {
            if (!first_edit) os << ", ";
            first_edit = false;
            os << "{"
               << "\"path\": \"" << json_escape(edit.path.generic_string()) << "\", "
               << "\"kind\": \"" << pin_kind_name(edit.kind) << "\", "
               << "\"old_value\": \"" << json_escape(edit.old_value) << "\", "
               << "\"new_value\": \"" << json_escape(edit.new_value) << "\", "
               << "\"old_value_style_has_v\": "
               << (edit.old_value_style_has_v ? "true" : "false")
               << "}";
        }
        os << "]"
           << "}";
    }
    os << "\n  ]\n}\n";
    return os.str();
}

std::optional<UndoBatch> parse_undo_batch(const std::string& json) {
    UndoBatch batch;
    TinyJson j{json};
    if (!j.match('{')) return std::nullopt;

    while (true) {
        j.skip_ws();
        if (j.match('}')) break;
        auto key = j.read_string();
        j.skip_ws();
        if (!j.match(':')) return std::nullopt;

        if (key == "timestamp") {
            batch.timestamp = j.read_string();
        } else if (key == "target_version") {
            batch.target_version = j.read_string();
        } else if (key == "entries") {
            j.skip_ws();
            if (!j.match('[')) { j.skip_value(); continue; }
            while (true) {
                j.skip_ws();
                if (j.match(']')) break;
                if (!j.match('{')) { j.skip_value(); continue; }
                UndoEntry e;
                while (true) {
                    j.skip_ws();
                    if (j.match('}')) break;
                    auto field = j.read_string();
                    j.skip_ws();
	                    if (!j.match(':')) break;
	                    j.skip_ws();
	                    if (field == "edits" && j.pos < j.src.size() && j.src[j.pos] == '[') {
	                        if (!j.match('[')) { j.skip_value(); }
	                        while (true) {
	                            j.skip_ws();
	                            if (j.match(']')) break;
	                            if (!j.match('{')) { j.skip_value(); continue; }
	                            UndoEdit edit;
	                            while (true) {
	                                j.skip_ws();
	                                if (j.match('}')) break;
	                                auto edit_field = j.read_string();
	                                j.skip_ws();
	                                if (!j.match(':')) break;
	                                j.skip_ws();
	                                if (j.pos < j.src.size() && j.src[j.pos] == '"') {
	                                    auto val = j.read_string();
                                    if      (edit_field == "path")      edit.path = fs::path(val);
                                    else if (edit_field == "kind")      edit.kind = parse_pin_kind(val);
                                    else if (edit_field == "old_value") edit.old_value = val;
                                    else if (edit_field == "new_value") edit.new_value = val;
	                                } else {
	                                    auto prim = j.read_primitive();
	                                    if (edit_field == "old_value_style_has_v") {
	                                        edit.old_value_style_has_v = (prim == "true");
	                                    }
	                                }
	                                j.skip_ws();
	                                if (j.match(',')) continue;
	                            }
	                            e.edits.push_back(std::move(edit));
	                            j.skip_ws();
	                            if (j.match(',')) continue;
	                        }
	                        j.skip_ws();
	                        if (j.match(',')) continue;
	                        continue;
	                    }
	                    if (j.pos < j.src.size() && j.src[j.pos] == '"') {
	                        auto val = j.read_string();
	                        if      (field == "project_path")    e.project_path = fs::path(val);
                        else if (field == "project_name")    e.project_name = val;
                        else if (field == "old_pin")         e.old_pin = val;
                        else if (field == "pin_kind")        e.pin_kind = parse_pin_kind(val);
                        else if (field == "status")          e.status = val;
                        else if (field == "failure_reason")  e.failure_reason = val;
                    } else {
                        // boolean / number / null
                        auto prim = j.read_primitive();
                        if (field == "old_pin_style_has_v") {
                            e.old_pin_style_has_v = (prim == "true");
                        }
                    }
	                    j.skip_ws();
	                    if (j.match(',')) continue;
	                }
	                if (e.edits.empty() && !e.old_pin.empty()) {
	                    e.edits.push_back(UndoEdit{
	                        e.project_path / "CMakeLists.txt",
	                        e.pin_kind,
	                        e.old_pin,
	                        batch.target_version,
	                        e.old_pin_style_has_v,
	                    });
	                }
	                batch.entries.push_back(std::move(e));
                j.skip_ws();
                if (j.match(',')) continue;
            }
        } else {
            j.skip_value();
        }
        j.skip_ws();
        if (j.match(',')) continue;
    }
    return batch;
}

bool write_undo_batch(const fs::path& path, const UndoBatch& batch) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << serialize_undo_batch(batch);
        if (!f.good()) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) {
            std::error_code ec2;
            fs::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

std::optional<UndoBatch> read_undo_batch(const fs::path& path) {
    if (path.empty()) return std::nullopt;
    if (!fs::exists(path)) return std::nullopt;
    std::ifstream f(path);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream buf;
    buf << f.rdbuf();
    return parse_undo_batch(buf.str());
}

std::vector<fs::path> list_undo_batches(const fs::path& pulp_home) {
    std::vector<fs::path> out;
    if (pulp_home.empty()) return out;
    std::error_code ec;
    if (!fs::is_directory(pulp_home, ec)) return out;
    for (auto& e : fs::directory_iterator(pulp_home, ec)) {
        if (ec) break;
        if (!e.is_regular_file()) continue;
        auto fn = e.path().filename().string();
        if (fn.rfind("bump-undo-", 0) == 0 &&
            fn.size() > 5 &&
            fn.substr(fn.size() - 5) == ".json") {
            out.push_back(e.path());
        }
    }
    // Newest first — ISO-8601 Z stamps sort lexicographically.
    std::sort(out.begin(), out.end(),
              [](const fs::path& a, const fs::path& b) {
                  return a.filename().string() > b.filename().string();
              });
    return out;
}

fs::path undo_batch_path(const fs::path& pulp_home, const std::string& timestamp) {
    if (pulp_home.empty()) return {};
    // ISO-8601 contains ':' which is unsafe on Windows filenames.
    // Replace with '-' to stay portable. Tests cover both shapes.
    std::string safe = timestamp;
    for (auto& c : safe) if (c == ':') c = '-';
    return pulp_home / ("bump-undo-" + safe + ".json");
}

std::string now_iso8601_utc() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

}  // namespace pulp::cli::project_bump
