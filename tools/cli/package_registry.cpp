// SPDX-License-Identifier: MIT
#include "package_registry.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <regex>
#include <sstream>

namespace pulp::cli::pkg {

// ── Helpers ──

static std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool write_file(const fs::path& path, const std::string& content) {
    std::ofstream f(path);
    if (!f) return false;
    f << content;
    return f.good();
}

static std::string to_lower(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return r;
}

static std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// ── Minimal JSON Parser ──
// Handles: objects, arrays, strings, numbers, booleans, null.
// Sufficient for registry.json and packages.lock.json.

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type type = Null;
    bool bool_val = false;
    double num_val = 0;
    std::string str_val;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    const JsonValue* get(const std::string& key) const {
        if (type != Object) return nullptr;
        for (auto& [k, v] : obj)
            if (k == key) return &v;
        return nullptr;
    }

    std::string as_string() const {
        return type == String ? str_val : std::string{};
    }

    bool as_bool() const { return type == Bool && bool_val; }

    int as_int() const { return type == Number ? static_cast<int>(num_val) : 0; }

    std::vector<std::string> as_string_array() const {
        std::vector<std::string> r;
        if (type == Array)
            for (auto& v : arr)
                if (v.type == String) r.push_back(v.str_val);
        return r;
    }
};

struct JsonParser {
    const std::string& src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size() && std::isspace(static_cast<unsigned char>(src[pos])))
            ++pos;
    }

    char peek() { skip_ws(); return pos < src.size() ? src[pos] : '\0'; }
    char next() { skip_ws(); return pos < src.size() ? src[pos++] : '\0'; }

    std::string parse_string() {
        if (next() != '"') return {};
        std::string r;
        while (pos < src.size()) {
            char c = src[pos++];
            if (c == '"') return r;
            if (c == '\\' && pos < src.size()) {
                c = src[pos++];
                switch (c) {
                    case '"': r += '"'; break;
                    case '\\': r += '\\'; break;
                    case '/': r += '/'; break;
                    case 'n': r += '\n'; break;
                    case 'r': r += '\r'; break;
                    case 't': r += '\t'; break;
                    case 'u': pos += 4; r += '?'; break;  // skip unicode escapes
                    default: r += c;
                }
            } else {
                r += c;
            }
        }
        return r;
    }

    JsonValue parse_value() {
        char c = peek();
        if (c == '"') {
            return {JsonValue::String, false, 0, parse_string(), {}, {}};
        }
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (c == 't' || c == 'f') {
            bool val = (c == 't');
            pos += val ? 4 : 5;  // "true" or "false"
            return {JsonValue::Bool, val, 0, {}, {}, {}};
        }
        if (c == 'n') { pos += 4; return {}; }  // null
        // number
        size_t start = pos;
        skip_ws();
        start = pos;
        while (pos < src.size() && (std::isdigit(static_cast<unsigned char>(src[pos])) ||
               src[pos] == '-' || src[pos] == '+' || src[pos] == '.' || src[pos] == 'e' || src[pos] == 'E'))
            ++pos;
        double val = 0;
        try { val = std::stod(src.substr(start, pos - start)); } catch (...) {}
        return {JsonValue::Number, false, val, {}, {}, {}};
    }

    JsonValue parse_object() {
        JsonValue r;
        r.type = JsonValue::Object;
        next();  // skip '{'
        if (peek() == '}') { next(); return r; }
        while (true) {
            auto key = parse_string();
            next();  // skip ':'
            auto val = parse_value();
            r.obj.push_back({key, val});
            if (peek() == '}') { next(); return r; }
            next();  // skip ','
        }
    }

    JsonValue parse_array() {
        JsonValue r;
        r.type = JsonValue::Array;
        next();  // skip '['
        if (peek() == ']') { next(); return r; }
        while (true) {
            r.arr.push_back(parse_value());
            if (peek() == ']') { next(); return r; }
            next();  // skip ','
        }
    }

    JsonValue parse() { return parse_value(); }
};

static std::string json_escape(const std::string& s) {
    std::string r;
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default: r += c;
        }
    }
    return r;
}

// ── Platform Targets ──

std::optional<PlatformTarget> PlatformTarget::parse(const std::string& s) {
    auto dash = s.find('-');
    if (dash == std::string::npos || dash == 0 || dash == s.size() - 1)
        return std::nullopt;

    PlatformTarget t;
    t.platform = s.substr(0, dash);
    t.arch = s.substr(dash + 1);

    if (!is_valid_target(t)) return std::nullopt;
    return t;
}

std::vector<PlatformTarget> default_targets() {
    return {{"macOS", "arm64"}, {"Windows", "x64"}, {"Linux", "x64"}};
}

bool is_valid_target(const PlatformTarget& t) {
    static const std::vector<std::string> platforms = {
        "macOS", "Windows", "Linux", "iOS", "WASM"
    };
    static const std::vector<std::string> archs = {
        "arm64", "x64", "x86", "wasm32"
    };
    bool plat_ok = std::find(platforms.begin(), platforms.end(), t.platform) != platforms.end();
    bool arch_ok = std::find(archs.begin(), archs.end(), t.arch) != archs.end();
    return plat_ok && arch_ok;
}

// ── License Policy ──

LicenseVerdict check_license(const std::string& spdx_id) {
    static const std::vector<std::string> allowed = {
        "MIT", "MIT-0", "BSD-2-Clause", "BSD-3-Clause", "Apache-2.0",
        "ISC", "zlib", "BSL-1.0", "Unlicense", "CC0-1.0",
    };
    static const std::vector<std::string> rejected_prefixes = {
        "GPL", "LGPL", "AGPL", "SSPL",
    };

    auto lower = to_lower(spdx_id);
    for (auto& a : allowed)
        if (to_lower(a) == lower) return LicenseVerdict::allowed;

    for (auto& r : rejected_prefixes)
        if (lower.find(to_lower(r)) == 0) return LicenseVerdict::rejected;

    if (to_lower(spdx_id) == "mpl-2.0") return LicenseVerdict::review_required;
    if (to_lower(spdx_id) == "proprietary") return LicenseVerdict::rejected;

    return LicenseVerdict::review_required;
}

const char* license_verdict_label(LicenseVerdict v) {
    switch (v) {
        case LicenseVerdict::allowed: return "allowed";
        case LicenseVerdict::review_required: return "review required";
        case LicenseVerdict::rejected: return "rejected";
    }
    return "unknown";
}

// ── Registry Loading ──

static PackageDescriptor parse_package(const std::string& id, const JsonValue& j) {
    PackageDescriptor pkg;
    pkg.id = id;
    if (auto v = j.get("name")) pkg.name = v->as_string();
    if (auto v = j.get("version")) pkg.version = v->as_string();
    if (auto v = j.get("description")) pkg.description = v->as_string();
    if (auto v = j.get("license")) pkg.license = v->as_string();
    if (auto v = j.get("category")) pkg.category = v->as_string();
    if (auto v = j.get("url")) pkg.url = v->as_string();
    if (auto v = j.get("rt_safe")) pkg.rt_safe = v->as_bool();
    if (auto v = j.get("unique_value")) pkg.unique_value = v->as_string();
    if (auto v = j.get("tags")) pkg.tags = v->as_string_array();
    if (auto v = j.get("provides")) pkg.provides = v->as_string_array();
    if (auto v = j.get("alternatives")) pkg.alternatives = v->as_string_array();

    if (auto f = j.get("fetch")) {
        if (auto v = f->get("method")) pkg.fetch.method = v->as_string();
        if (auto v = f->get("git_repository")) pkg.fetch.git_repository = v->as_string();
        if (auto v = f->get("git_tag")) pkg.fetch.git_tag = v->as_string();
    }

    if (auto c = j.get("cmake")) {
        if (auto v = c->get("targets")) pkg.cmake.targets = v->as_string_array();
        if (auto v = c->get("header_only")) pkg.cmake.header_only = v->as_bool();
        if (auto v = c->get("include_dir")) pkg.cmake.include_dir = v->as_string();
    }

    if (auto p = j.get("platforms"); p && p->type == JsonValue::Object) {
        for (auto& [name, val] : p->obj) {
            PlatformSupport ps;
            if (auto a = val.get("architectures")) ps.architectures = a->as_string_array();
            if (auto n = val.get("notes")) ps.notes = n->as_string();
            pkg.platforms[name] = ps;
        }
    }

    if (auto o = j.get("overlaps_with_builtin"); o && o->type == JsonValue::Object) {
        for (auto& [k, v] : o->obj)
            pkg.overlaps_with_builtin[k] = v.as_string();
    }

    if (auto ver = j.get("verification")) {
        if (auto v = ver->get("last_verified")) pkg.verification.last_verified = v->as_string();
        if (auto v = ver->get("verified_version")) pkg.verification.verified_version = v->as_string();
        if (auto bs = ver->get("build_status"); bs && bs->type == JsonValue::Object) {
            for (auto& [k, v] : bs->obj)
                pkg.verification.build_status[k] = v.as_string();
        }
    }

    return pkg;
}

RegistryLoadResult load_registry(const fs::path& registry_path) {
    RegistryLoadResult result;

    auto content = read_file(registry_path);
    if (content.empty()) {
        result.error = "Cannot read registry file: " + registry_path.string();
        return result;
    }

    JsonParser parser{content};
    auto root = parser.parse();
    if (root.type != JsonValue::Object) {
        result.error = "Registry file is not a valid JSON object";
        return result;
    }

    if (auto v = root.get("registry_version"))
        result.registry.version = v->as_int();

    if (auto pkgs = root.get("packages"); pkgs && pkgs->type == JsonValue::Object) {
        for (auto& [id, val] : pkgs->obj)
            result.registry.packages[id] = parse_package(id, val);
    }

    return result;
}

// ── Lock File ──

LockFile load_lock_file(const fs::path& path) {
    LockFile lock;
    auto content = read_file(path);
    if (content.empty()) return lock;

    JsonParser parser{content};
    auto root = parser.parse();
    if (root.type != JsonValue::Object) return lock;

    if (auto v = root.get("lockfile_version")) lock.version = v->as_int();

    if (auto pkgs = root.get("packages"); pkgs && pkgs->type == JsonValue::Object) {
        for (auto& [id, val] : pkgs->obj) {
            LockedPackage lp;
            if (auto v = val.get("version")) lp.version = v->as_string();
            if (auto v = val.get("resolved")) lp.resolved = v->as_string();
            if (auto v = val.get("integrity")) lp.integrity = v->as_string();
            if (auto v = val.get("commit")) lp.commit = v->as_string();
            lock.packages[id] = lp;
        }
    }

    return lock;
}

bool save_lock_file(const fs::path& path, const LockFile& lock) {
    std::ostringstream os;
    os << "{\n";
    os << "  \"lockfile_version\": " << lock.version << ",\n";
    os << "  \"packages\": {";

    bool first = true;
    for (auto& [id, lp] : lock.packages) {
        if (!first) os << ",";
        first = false;
        os << "\n    \"" << json_escape(id) << "\": {\n";
        os << "      \"version\": \"" << json_escape(lp.version) << "\",\n";
        os << "      \"resolved\": \"" << json_escape(lp.resolved) << "\",\n";
        os << "      \"integrity\": \"" << json_escape(lp.integrity) << "\",\n";
        os << "      \"commit\": \"" << json_escape(lp.commit) << "\"\n";
        os << "    }";
    }

    os << "\n  }\n}\n";
    return write_file(path, os.str());
}

// ── Target Config (TOML) ──

std::vector<PlatformTarget> read_project_targets(const fs::path& project_root) {
    auto toml_path = project_root / "pulp.toml";
    auto content = read_file(toml_path);
    if (content.empty()) return default_targets();

    // Find [project] section and targets/platforms array
    std::istringstream stream(content);
    std::string line;
    bool in_project = false;
    std::string targets_line;

    while (std::getline(stream, line)) {
        auto trimmed = trim(line);
        if (trimmed.starts_with("[") && trimmed.ends_with("]")) {
            in_project = (trimmed == "[project]");
            continue;
        }
        if (in_project) {
            if (trimmed.starts_with("targets") && trimmed.find('=') != std::string::npos) {
                targets_line = trimmed;
                // Collect multi-line array
                while (targets_line.find(']') == std::string::npos && std::getline(stream, line))
                    targets_line += " " + trim(line);
                break;
            }
            if (trimmed.starts_with("platforms") && trimmed.find('=') != std::string::npos) {
                targets_line = trimmed;
                while (targets_line.find(']') == std::string::npos && std::getline(stream, line))
                    targets_line += " " + trim(line);
                break;
            }
        }
    }

    if (targets_line.empty()) return default_targets();

    // Extract array items from TOML-style [...] array
    auto bracket_start = targets_line.find('[');
    auto bracket_end = targets_line.rfind(']');
    if (bracket_start == std::string::npos || bracket_end == std::string::npos)
        return default_targets();

    auto array_content = targets_line.substr(bracket_start + 1, bracket_end - bracket_start - 1);
    std::vector<PlatformTarget> result;
    std::regex item_re("\"([^\"]+)\"");
    std::sregex_iterator it(array_content.begin(), array_content.end(), item_re);
    std::sregex_iterator end;

    bool is_platforms = targets_line.starts_with("platforms");
    for (; it != end; ++it) {
        std::string val = (*it)[1].str();
        if (is_platforms) {
            // Expand platform to default arch
            std::string arch = "x64";
            if (val == "macOS" || val == "iOS") arch = "arm64";
            if (val == "WASM") arch = "wasm32";
            auto t = PlatformTarget::parse(val + "-" + arch);
            if (t) result.push_back(*t);
        } else {
            auto t = PlatformTarget::parse(val);
            if (t) result.push_back(*t);
        }
    }

    return result.empty() ? default_targets() : result;
}

bool write_project_targets(const fs::path& project_root,
                           const std::vector<PlatformTarget>& targets) {
    auto toml_path = project_root / "pulp.toml";
    auto content = read_file(toml_path);

    // Build the targets array string
    std::ostringstream arr;
    arr << "targets = [\n";
    for (size_t i = 0; i < targets.size(); ++i) {
        arr << "  \"" << targets[i].to_string() << "\"";
        if (i + 1 < targets.size()) arr << ",";
        arr << "\n";
    }
    arr << "]";

    if (content.empty()) {
        // Create new file
        std::ostringstream os;
        os << "[project]\n" << arr.str() << "\n";
        return write_file(toml_path, os.str());
    }

    // Find and replace/insert in [project] section
    std::istringstream stream(content);
    std::ostringstream out;
    std::string line;
    bool in_project = false;
    bool replaced = false;
    bool section_found = false;

    while (std::getline(stream, line)) {
        auto trimmed = trim(line);

        if (trimmed.starts_with("[") && trimmed.ends_with("]")) {
            if (in_project && !replaced) {
                out << arr.str() << "\n";
                replaced = true;
            }
            in_project = (trimmed == "[project]");
            if (in_project) section_found = true;
            out << line << "\n";
            continue;
        }

        if (in_project && !replaced &&
            (trimmed.starts_with("targets") || trimmed.starts_with("platforms")) &&
            trimmed.find('=') != std::string::npos) {
            // Skip old targets/platforms lines (including multi-line arrays)
            if (trimmed.find(']') == std::string::npos) {
                while (std::getline(stream, line)) {
                    if (trim(line).find(']') != std::string::npos) break;
                }
            }
            out << arr.str() << "\n";
            replaced = true;
            continue;
        }

        out << line << "\n";
    }

    if (!section_found) {
        out << "\n[project]\n" << arr.str() << "\n";
    } else if (in_project && !replaced) {
        out << arr.str() << "\n";
    }

    return write_file(toml_path, out.str());
}

// ── Remote Registry ──

fs::path default_cache_dir() {
#ifdef __APPLE__
    if (auto home = std::getenv("HOME"))
        return fs::path(home) / ".pulp";
#elif defined(_WIN32)
    if (auto appdata = std::getenv("LOCALAPPDATA"))
        return fs::path(appdata) / "Pulp";
#else
    if (auto home = std::getenv("HOME"))
        return fs::path(home) / ".pulp";
#endif
    return fs::temp_directory_path() / "pulp-cache";
}

static bool download_file(const std::string& url, const fs::path& dest) {
    fs::create_directories(dest.parent_path());
    // Use curl on macOS/Linux, PowerShell on Windows
#ifdef _WIN32
    std::string cmd = "powershell -Command \"Invoke-WebRequest -Uri '" + url +
                      "' -OutFile '" + dest.string() + "'\" 2>nul";
#else
    std::string cmd = "curl -sSfL -o '" + dest.string() + "' '" + url + "' 2>/dev/null";
#endif
    return std::system(cmd.c_str()) == 0;
}

static bool is_cache_fresh(const fs::path& cache_file, int ttl_hours) {
    if (!fs::exists(cache_file)) return false;
    auto mod_time = fs::last_write_time(cache_file);
    auto now = fs::file_time_type::clock::now();
    auto age = std::chrono::duration_cast<std::chrono::hours>(now - mod_time);
    return age.count() < ttl_hours;
}

RegistryLoadResult load_remote_registry(const std::string& url,
                                         const fs::path& cache_dir,
                                         int ttl_hours) {
    auto cache_file = cache_dir / "registry-cache.json";

    if (is_cache_fresh(cache_file, ttl_hours))
        return load_registry(cache_file);

    if (download_file(url, cache_file))
        return load_registry(cache_file);

    // Fall back to stale cache if download fails
    if (fs::exists(cache_file))
        return load_registry(cache_file);

    return {{}, "Failed to fetch remote registry from " + url};
}

RegistryLoadResult refresh_remote_registry(const std::string& url,
                                            const fs::path& cache_dir) {
    auto cache_file = cache_dir / "registry-cache.json";
    if (download_file(url, cache_file))
        return load_registry(cache_file);
    return {{}, "Failed to fetch remote registry from " + url};
}

// ── Semver ──

std::optional<SemVer> SemVer::parse(const std::string& s) {
    SemVer v;
    std::string input = s;
    // Strip leading 'v' or 'V'
    if (!input.empty() && (input[0] == 'v' || input[0] == 'V'))
        input = input.substr(1);

    // Split on '-' for pre-release
    auto dash = input.find('-');
    if (dash != std::string::npos) {
        v.pre = input.substr(dash + 1);
        input = input.substr(0, dash);
    }

    // Parse major.minor.patch
    int parts[3] = {0, 0, 0};
    int idx = 0;
    std::istringstream ss(input);
    std::string token;
    while (std::getline(ss, token, '.') && idx < 3) {
        try { parts[idx] = std::stoi(token); } catch (...) { return std::nullopt; }
        ++idx;
    }
    if (idx == 0) return std::nullopt;

    v.major = parts[0]; v.minor = parts[1]; v.patch = parts[2];
    return v;
}

bool SemVer::operator<(const SemVer& o) const {
    if (major != o.major) return major < o.major;
    if (minor != o.minor) return minor < o.minor;
    if (patch != o.patch) return patch < o.patch;
    // Pre-release sorts before release
    if (pre.empty() && !o.pre.empty()) return false;
    if (!pre.empty() && o.pre.empty()) return true;
    return pre < o.pre;
}

bool SemVer::operator==(const SemVer& o) const {
    return major == o.major && minor == o.minor && patch == o.patch && pre == o.pre;
}

bool SemVer::compatible_with(const SemVer& constraint) const {
    // ^constraint: same major, >= minor.patch
    if (major != constraint.major) return false;
    if (minor < constraint.minor) return false;
    if (minor == constraint.minor && patch < constraint.patch) return false;
    return true;
}

// ── Quality Score ──

QualityScore compute_quality(const PackageDescriptor& pkg) {
    QualityScore q;

    // License (0-25)
    auto verdict = check_license(pkg.license);
    if (verdict == LicenseVerdict::allowed) q.license = 25;
    else if (verdict == LicenseVerdict::review_required) q.license = 10;

    // Platforms (0-25): 5 points per supported primary platform
    int plat_count = 0;
    for (auto& [name, ps] : pkg.platforms) {
        if (name == "macOS" || name == "Windows" || name == "Linux")
            ++plat_count;
    }
    q.platforms = std::min(25, plat_count * 8);

    // Verification (0-25)
    int pass_count = 0, total_count = 0;
    for (auto& [key, status] : pkg.verification.build_status) {
        ++total_count;
        if (status == "pass") ++pass_count;
    }
    if (total_count > 0)
        q.verification = 25 * pass_count / total_count;

    // Maintenance (0-25): based on verification date freshness
    if (!pkg.verification.last_verified.empty()) {
        // Simple heuristic: verified at all = 15 points, recent = 25
        q.maintenance = 15;
        // Could parse date and compare, but for now a static score
    }

    q.total = q.license + q.platforms + q.verification + q.maintenance;

    if (q.total >= 75) q.tier = "official";
    else if (q.total >= 40) q.tier = "community";
    else q.tier = "experimental";

    return q;
}

// ── Queries ──

std::vector<PlatformTarget> unsupported_targets(
    const PackageDescriptor& pkg,
    const std::vector<PlatformTarget>& targets) {

    std::vector<PlatformTarget> unsupported;
    for (auto& t : targets) {
        auto it = pkg.platforms.find(t.platform);
        if (it == pkg.platforms.end()) {
            unsupported.push_back(t);
            continue;
        }
        auto& archs = it->second.architectures;
        if (std::find(archs.begin(), archs.end(), t.arch) == archs.end())
            unsupported.push_back(t);
    }
    return unsupported;
}

std::vector<const PackageDescriptor*> search(const Registry& reg,
                                              const std::string& query) {
    auto q = to_lower(query);

    struct ScoredPkg {
        const PackageDescriptor* pkg;
        int score;
    };
    std::vector<ScoredPkg> results;

    for (auto& [id, pkg] : reg.packages) {
        int score = 0;

        // Exact ID match
        if (to_lower(id) == q) score += 100;
        // ID contains query
        else if (to_lower(id).find(q) != std::string::npos) score += 50;

        // Name match
        if (to_lower(pkg.name).find(q) != std::string::npos) score += 40;

        // Category match
        if (to_lower(pkg.category) == q) score += 30;

        // Tags match
        for (auto& tag : pkg.tags)
            if (to_lower(tag).find(q) != std::string::npos) { score += 20; break; }

        // Provides match
        for (auto& p : pkg.provides)
            if (to_lower(p).find(q) != std::string::npos) { score += 20; break; }

        // Description match
        if (to_lower(pkg.description).find(q) != std::string::npos) score += 10;

        if (score > 0) results.push_back({&pkg, score});
    }

    std::sort(results.begin(), results.end(),
              [](auto& a, auto& b) { return a.score > b.score; });

    std::vector<const PackageDescriptor*> out;
    for (auto& r : results) out.push_back(r.pkg);
    return out;
}

}  // namespace pulp::cli::pkg
