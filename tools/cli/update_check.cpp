// update_check.cpp — Implementation for release-discovery Slice 2 (#547).
//
// All non-trivial string/time logic lives here so the unit tests can
// compile just update_check.cpp + the test TU without pulling in
// pulp-cli's runtime link surface.

#include "update_check.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <cstdlib>
#endif

namespace pulp::cli::update_check {

namespace {

std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return s.substr(1, s.size() - 2);
    return s;
}

// Extract a scalar JSON field ("key": <value>) from an arbitrary blob.
// Handles string and integer values. Deliberately tolerant — GitHub's
// releases/latest payload is stable enough that a regex scan is fine,
// and this keeps us from pulling a JSON dep for one endpoint.
std::string extract_json_string(const std::string& body, const std::string& key) {
    std::string pattern = "\"" + key + "\"\\s*:\\s*\"([^\"\\\\]*(?:\\\\.[^\"\\\\]*)*)\"";
    try {
        std::regex re(pattern);
        std::smatch m;
        if (std::regex_search(body, m, re)) {
            return m[1].str();
        }
    } catch (const std::regex_error&) {
        // Swallow — regex build failures shouldn't crash a background fetch.
    }
    return {};
}

std::int64_t extract_json_int(const std::string& body, const std::string& key) {
    std::string pattern = "\"" + key + "\"\\s*:\\s*(-?\\d+)";
    try {
        std::regex re(pattern);
        std::smatch m;
        if (std::regex_search(body, m, re)) {
            try { return std::stoll(m[1].str()); } catch (...) { return 0; }
        }
    } catch (const std::regex_error&) {
    }
    return 0;
}

std::string normalize_tag(std::string tag) {
    if (!tag.empty() && (tag.front() == 'v' || tag.front() == 'V')) {
        tag.erase(tag.begin());
    }
    return tag;
}

// Minimal JSON escape for strings we write.
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
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// Shell out and capture stdout. POSIX only uses popen; Windows invokes
// the same shape because `curl` is bundled with modern Windows (10+).
// If the caller needs a PowerShell fallback, they can override via
// GitHubReleasesFetcher's command composer.
std::string run_capture(const std::string& cmd) {
#ifdef _WIN32
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) return {};
    std::string out;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    return out;
}

}  // namespace

// ── JSON I/O ────────────────────────────────────────────────────────────────

CacheEntry parse_cache_json(const std::string& json) {
    CacheEntry e;
    // Missing-field tolerance: extract_* return {} / 0 when absent.
    auto schema = extract_json_int(json, "schema");
    if (schema > 0) e.schema = static_cast<int>(schema);
    e.last_check_epoch_sec = extract_json_int(json, "last_check_epoch_sec");
    e.latest_version = extract_json_string(json, "latest_version");
    e.release_notes_url = extract_json_string(json, "release_notes_url");
    e.banner_shown_for_version = extract_json_string(json, "banner_shown_for_version");
    return e;
}

std::string serialize_cache_json(const CacheEntry& entry) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"schema\": " << entry.schema << ",\n";
    os << "  \"last_check_epoch_sec\": " << entry.last_check_epoch_sec << ",\n";
    os << "  \"latest_version\": \"" << json_escape(entry.latest_version) << "\",\n";
    os << "  \"release_notes_url\": \"" << json_escape(entry.release_notes_url) << "\",\n";
    os << "  \"banner_shown_for_version\": \""
       << json_escape(entry.banner_shown_for_version) << "\"\n";
    os << "}\n";
    return os.str();
}

std::optional<CacheEntry> read_cache_file(const fs::path& cache_path) {
    std::error_code ec;
    if (!fs::exists(cache_path, ec)) return std::nullopt;
    std::ifstream f(cache_path);
    if (!f.is_open()) return std::nullopt;
    std::ostringstream buf;
    buf << f.rdbuf();
    return parse_cache_json(buf.str());
}

bool write_cache_file(const fs::path& cache_path, const CacheEntry& entry) {
    std::error_code ec;
    auto parent = cache_path.parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent, ec);
        // Non-fatal — the open below will report the real failure.
    }
    auto tmp = cache_path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << serialize_cache_json(entry);
        if (!f.good()) return false;
    }
    fs::rename(tmp, cache_path, ec);
    if (ec) {
        // Fallback for cross-device rename (rare in ~/.pulp but possible
        // if HOME is on a bind mount).
        fs::copy_file(tmp, cache_path, fs::copy_options::overwrite_existing, ec);
        fs::remove(tmp, ec);
        return !ec;
    }
    return true;
}

// ── Age Check ───────────────────────────────────────────────────────────────

bool is_cache_stale(const CacheEntry& cache,
                    std::int64_t now_epoch_sec,
                    int interval_hours) {
    if (interval_hours <= 0) return false;   // disabled
    if (cache.last_check_epoch_sec <= 0) return true;
    std::int64_t delta = now_epoch_sec - cache.last_check_epoch_sec;
    if (delta < 0) return true;   // clock went backwards; refresh defensively
    return delta >= static_cast<std::int64_t>(interval_hours) * 3600;
}

std::int64_t now_epoch_sec() {
    using namespace std::chrono;
    return duration_cast<seconds>(system_clock::now().time_since_epoch()).count();
}

// ── Semver ──────────────────────────────────────────────────────────────────

SemverTriple parse_semver(const std::string& s_in) {
    SemverTriple out;
    std::string s = s_in;
    if (!s.empty() && (s.front() == 'v' || s.front() == 'V')) s.erase(s.begin());
    // Strip pre-release/build suffix — we only rank on M.N.P today.
    auto cut = s.find_first_of("-+");
    if (cut != std::string::npos) s = s.substr(0, cut);

    int parts[3] = {0, 0, 0};
    int idx = 0;
    std::string buf;
    auto flush = [&]() -> bool {
        if (buf.empty()) return false;
        for (char c : buf) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        try { parts[idx] = std::stoi(buf); } catch (...) { return false; }
        buf.clear();
        return true;
    };
    for (char c : s) {
        if (c == '.') {
            if (!flush()) return out;
            if (++idx >= 3) break;
        } else {
            buf += c;
        }
    }
    if (idx <= 2) {
        if (!flush()) return out;
    }
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    out.ok = true;
    return out;
}

int compare_semver(const SemverTriple& a, const SemverTriple& b) {
    if (!a.ok || !b.ok) return 0;
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

bool is_newer(const std::string& installed, const std::string& latest) {
    auto a = parse_semver(installed);
    auto b = parse_semver(latest);
    if (!a.ok || !b.ok) return false;
    return compare_semver(b, a) > 0;
}

// ── Banner ──────────────────────────────────────────────────────────────────

std::string compose_banner(const std::string& installed_version,
                           const std::string& latest_version) {
    // Exact single-line shape locked by the design doc Section A.
    // Keep under ~120 chars so it doesn't wrap on typical terminals.
    std::ostringstream os;
    os << "Pulp v" << latest_version
       << " available (you have v" << installed_version
       << "). Run `pulp upgrade` or `pulp config set update.mode manual` to silence.";
    return os.str();
}

// ── TOML writer ─────────────────────────────────────────────────────────────

namespace {

// Tokenise the TOML source into lines, keeping trailing newlines so we
// can faithfully round-trip. Uses LF only — mixed CRLF inputs get
// normalized on write (matching the version_diag pattern).
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c == '\r') {
            // skipped — re-emitted as LF
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool line_is_section_header(const std::string& line, std::string* section_out) {
    auto t = trim(line);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
    auto name = trim(t.substr(1, t.size() - 2));
    if (section_out) *section_out = name;
    return true;
}

// Detects "key = ..." matching a given key on a line (whitespace-tolerant,
// ignores inline comments). Does NOT match commented-out examples.
bool line_sets_key(const std::string& line, const std::string& key) {
    auto hash = line.find('#');
    auto effective = (hash == std::string::npos) ? line : line.substr(0, hash);
    auto t = trim(effective);
    if (t.size() < key.size() + 1) return false;
    if (t.compare(0, key.size(), key) != 0) return false;
    auto after = t.substr(key.size());
    auto ta = trim(after);
    if (ta.empty() || ta.front() != '=') return false;
    return true;
}

}  // namespace

std::string write_toml_key_in_section(const std::string& source,
                                      const std::string& section,
                                      const std::string& key,
                                      const std::string& value) {
    auto lines = split_lines(source);

    // Locate the target section's span.
    int section_begin = -1;
    int section_end = static_cast<int>(lines.size());   // exclusive
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        std::string name;
        if (line_is_section_header(lines[i], &name)) {
            if (section_begin >= 0) {
                section_end = i;
                break;
            }
            if (name == section) {
                section_begin = i;
            }
        }
    }

    std::string formatted = key + " = \"" + value + "\"";

    if (section_begin < 0) {
        // Section not present — append. Preserve existing trailing
        // newline if any; separate from prior content with a blank.
        std::ostringstream out;
        out << source;
        if (!source.empty() && source.back() != '\n') out << "\n";
        if (!source.empty()) out << "\n";
        out << "[" << section << "]\n";
        out << formatted << "\n";
        return out.str();
    }

    // Check for an existing key inside the section.
    int key_line = -1;
    int last_nonblank_in_section = section_begin;
    for (int i = section_begin + 1; i < section_end; ++i) {
        if (line_sets_key(lines[i], key)) {
            key_line = i;
            break;
        }
        if (!trim(lines[i]).empty()) last_nonblank_in_section = i;
    }

    std::ostringstream out;
    for (int i = 0; i < static_cast<int>(lines.size()); ++i) {
        if (i == key_line) {
            out << formatted << "\n";
            continue;
        }
        out << lines[i] << "\n";
        if (key_line < 0 && i == last_nonblank_in_section) {
            // Insert new key after the last non-blank line of the target
            // section (or immediately after the header if the section is
            // empty).
            out << formatted << "\n";
        }
    }
    return out.str();
}

std::string read_toml_key_in_section(const std::string& source,
                                     const std::string& section,
                                     const std::string& key) {
    auto lines = split_lines(source);
    std::string current_section;
    for (const auto& raw_line : lines) {
        auto hash = raw_line.find('#');
        auto effective = (hash == std::string::npos) ? raw_line
                                                     : raw_line.substr(0, hash);
        auto t = trim(effective);
        if (t.empty()) continue;
        std::string name;
        if (line_is_section_header(effective, &name)) {
            current_section = name;
            continue;
        }
        if (current_section != section) continue;
        auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        auto k = trim(t.substr(0, eq));
        if (k != key) continue;
        return strip_quotes(trim(t.substr(eq + 1)));
    }
    return {};
}

// ── Real fetcher ────────────────────────────────────────────────────────────

FetchResult GitHubReleasesFetcher::fetch_latest_release(const std::string& owner_repo) {
    FetchResult r;
    // Anonymous — rate-limited to 60/hr per IP. That's plenty given the
    // 24h cache. A User-Agent is required by GitHub; we use a static
    // "pulp-cli" string rather than the installed version so the header
    // stays constant across releases (helps their abuse-detection
    // heuristics treat us as one client, not N).
    std::string url = "https://api.github.com/repos/" + owner_repo + "/releases/latest";
#ifdef _WIN32
    // PowerShell path — curl is present on Win10+, but Invoke-WebRequest
    // is the native fallback. Keep the stderr redirect so background
    // output stays quiet.
    std::string cmd = "curl -fsSL -A \"pulp-cli\" -H \"Accept: application/vnd.github+json\" \"" +
                      url + "\" 2>NUL";
#else
    std::string cmd = "curl -fsSL -A 'pulp-cli' -H 'Accept: application/vnd.github+json' '" +
                      url + "' 2>/dev/null";
#endif
    auto body = run_capture(cmd);
    if (body.empty()) {
        r.error = "empty response or curl failed";
        return r;
    }
    auto tag = extract_json_string(body, "tag_name");
    auto html_url = extract_json_string(body, "html_url");
    if (tag.empty()) {
        r.error = "could not parse tag_name from response";
        return r;
    }
    r.ok = true;
    r.latest_version = normalize_tag(tag);
    r.release_notes_url = html_url;
    return r;
}

// ── Orchestrator ────────────────────────────────────────────────────────────

CacheEntry refresh_cache(Fetcher& fetcher,
                         const CacheEntry& previous,
                         const std::string& owner_repo,
                         std::int64_t now_epoch_sec_val) {
    CacheEntry next = previous;
    next.schema = kCacheSchemaVersion;
    next.last_check_epoch_sec = now_epoch_sec_val;

    auto r = fetcher.fetch_latest_release(owner_repo);
    if (r.ok) {
        next.latest_version = r.latest_version;
        next.release_notes_url = r.release_notes_url;
    }
    // On failure we keep the previous latest_version so the banner
    // survives a transient network blip — but we still advance
    // last_check_epoch_sec so we don't retry every invocation.
    return next;
}

}  // namespace pulp::cli::update_check
