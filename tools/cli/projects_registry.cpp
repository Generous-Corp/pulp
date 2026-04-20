// projects_registry.cpp — Implementation of the `~/.pulp/projects.json`
// registry. See projects_registry.hpp for the design contract.
//
// Issue #499 / #552 Slice 1b.

#include "projects_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <system_error>

#ifdef _WIN32
#  include <cstdlib>   // _dupenv_s
#endif

namespace pulp::cli::projects_registry {

namespace {

fs::path user_home_dir_local() {
#ifdef _WIN32
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "USERPROFILE") == 0 && buf) {
        fs::path p(buf);
        std::free(buf);
        return p;
    }
    return {};
#else
    if (const char* h = std::getenv("HOME")) return fs::path(h);
    return {};
#endif
}

// Try to canonicalise; fall back to the absolute form if the path
// doesn't exist yet (registry can hold entries for directories the
// user subsequently deleted — we still want a stable key).
fs::path canonicalish(const fs::path& p) {
    std::error_code ec;
    auto abs = fs::absolute(p, ec);
    if (ec) abs = p;
    auto canon = fs::weakly_canonical(abs, ec);
    return ec ? abs : canon;
}

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

// A deliberately small JSON reader. The registry schema is flat:
//   { "projects": [{"path":"...","name":"...","registered_at":"..."}, ...] }
// Using a tiny grammar keeps this module free of the pkg::JsonParser
// dependency (which lives in tool_registry's link lane) so the unit
// test can stay link-light. Unknown fields are skipped silently.
struct TinyJson {
    const std::string& src;
    size_t pos = 0;

    void skip_ws() {
        while (pos < src.size()) {
            char c = src[pos];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos;
            } else {
                break;
            }
        }
    }

    bool match(char c) {
        skip_ws();
        if (pos < src.size() && src[pos] == c) { ++pos; return true; }
        return false;
    }

    // Parses a JSON string literal, returning the decoded value.
    // Returns empty string on malformed input (registry is best-effort).
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
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                        // Skip 4 hex chars; we don't need full Unicode
                        // decoding for the fields we care about.
                        if (pos + 4 <= src.size()) pos += 4;
                        out += '?';
                        break;
                    default: out += esc; break;
                }
            } else {
                out += c;
            }
        }
        return out;  // unterminated, but tolerated
    }

    // Skip a JSON value of unknown shape — used to tolerate forward-
    // compatible additions to the schema.
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
            // Primitive — scan until a structural character.
            while (pos < src.size() &&
                   src[pos] != ',' && src[pos] != '}' && src[pos] != ']') {
                ++pos;
            }
        }
    }
};

}  // namespace

fs::path registry_path(const fs::path& override_home) {
    if (!override_home.empty()) {
        return override_home / "projects.json";
    }
    // $PULP_HOME override takes precedence over HOME — mirrors the
    // pulp_home() helper in cli_common.cpp.
    if (const char* env = std::getenv("PULP_HOME"); env && *env) {
        return fs::path(env) / "projects.json";
    }
    auto home = user_home_dir_local();
    if (home.empty()) return {};
    return home / ".pulp" / "projects.json";
}

std::vector<Project> read_registry(const fs::path& registry_json) {
    std::vector<Project> out;
    if (registry_json.empty() || !fs::exists(registry_json)) return out;

    std::ifstream f(registry_json);
    if (!f.is_open()) return out;
    std::stringstream buf;
    buf << f.rdbuf();
    auto body = buf.str();

    TinyJson j{body};
    if (!j.match('{')) return out;

    while (true) {
        j.skip_ws();
        if (j.match('}')) break;
        auto key = j.read_string();
        j.skip_ws();
        if (!j.match(':')) return out;

        if (key != "projects") {
            j.skip_value();
        } else {
            j.skip_ws();
            if (!j.match('[')) { j.skip_value(); } else {
                while (true) {
                    j.skip_ws();
                    if (j.match(']')) break;
                    if (!j.match('{')) { j.skip_value(); continue; }

                    Project p;
                    while (true) {
                        j.skip_ws();
                        if (j.match('}')) break;
                        auto field = j.read_string();
                        j.skip_ws();
                        if (!j.match(':')) break;
                        // Codex 2026-04-21 wave 2 P1 on #563: the schema
                        // documents unknown fields as forward-compatible,
                        // so a future writer is allowed to emit e.g.
                        // `"meta": {...}` or `"pinned": true`. If we
                        // assume every value is a string (`read_string`
                        // returns "" on non-quoted input) and do NOT
                        // advance past the non-string value, the parser
                        // can loop indefinitely on that object. Peek at
                        // the next char: strings go through the normal
                        // path so we can capture known fields; anything
                        // else is consumed by `skip_value()` which
                        // correctly walks arrays/objects/primitives.
                        j.skip_ws();
                        if (j.pos < j.src.size() && j.src[j.pos] == '"') {
                            auto val = j.read_string();
                            if (field == "path") p.path = fs::path(val);
                            else if (field == "name") p.name = val;
                            else if (field == "registered_at") p.registered_at = val;
                            // Other known-string fields would go here.
                        } else {
                            j.skip_value();
                        }
                        j.skip_ws();
                        if (j.match(',')) continue;
                    }
                    if (!p.path.empty()) out.push_back(std::move(p));
                    j.skip_ws();
                    if (j.match(',')) continue;
                }
            }
        }
        j.skip_ws();
        if (j.match(',')) continue;
    }
    return out;
}

bool write_registry(const fs::path& registry_json,
                    const std::vector<Project>& projects) {
    if (registry_json.empty()) return false;

    std::error_code ec;
    fs::create_directories(registry_json.parent_path(), ec);
    if (ec) return false;

    auto tmp = registry_json;
    tmp += ".tmp";

    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << "{\n  \"projects\": [";
        bool first = true;
        for (auto& p : projects) {
            if (!first) f << ",";
            first = false;
            f << "\n    {"
              << "\"path\": \""           << json_escape(p.path.generic_string()) << "\", "
              << "\"name\": \""           << json_escape(p.name)                  << "\", "
              << "\"registered_at\": \""  << json_escape(p.registered_at)         << "\""
              << "}";
        }
        f << "\n  ]\n}\n";
        if (!f.good()) return false;
    }

    fs::rename(tmp, registry_json, ec);
    if (ec) {
        // rename may fail on some filesystems when the destination
        // already exists; fall back to remove+copy.
        std::error_code ec2;
        fs::remove(registry_json, ec2);
        fs::rename(tmp, registry_json, ec2);
        if (ec2) {
            fs::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

std::vector<Project> add_project(const fs::path& registry_json,
                                 const fs::path& project_path,
                                 const std::string& project_name,
                                 bool* out_wrote_ok) {
    auto projects = read_registry(registry_json);
    auto canon = canonicalish(project_path);

    auto match = [&](const Project& p) {
        return canonicalish(p.path) == canon;
    };

    auto it = std::find_if(projects.begin(), projects.end(), match);
    if (it != projects.end()) {
        it->path = canon;
        if (!project_name.empty()) it->name = project_name;
        it->registered_at = now_iso8601_utc();
    } else {
        Project p;
        p.path = canon;
        p.name = project_name.empty() ? canon.filename().string() : project_name;
        p.registered_at = now_iso8601_utc();
        projects.push_back(std::move(p));
    }

    // Codex 2026-04-21 wave 2 P2 on #563: previous version dropped the
    // `write_registry()` return value, so callers saw a successful
    // in-memory upsert even when the backing store failed to persist
    // (unwritable $PULP_HOME, missing parent directory, etc.). Surface
    // the write result via `out_wrote_ok` so callers can distinguish
    // "registered and saved" from "intended but not durable".
    const bool wrote = write_registry(registry_json, projects);
    if (out_wrote_ok) *out_wrote_ok = wrote;
    return projects;
}

bool remove_project(const fs::path& registry_json,
                    const fs::path& project_path) {
    auto projects = read_registry(registry_json);
    auto canon = canonicalish(project_path);

    auto before = projects.size();
    projects.erase(std::remove_if(projects.begin(), projects.end(),
                                  [&](const Project& p) {
                                      return canonicalish(p.path) == canon;
                                  }),
                   projects.end());

    if (projects.size() == before) return false;
    return write_registry(registry_json, projects);
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

std::vector<fs::path> scan_parent_pulp_projects(const fs::path& start) {
    std::vector<fs::path> out;
    std::error_code ec;

    fs::path dir = start;
    if (dir.empty()) dir = fs::current_path(ec);
    if (ec || dir.empty()) return out;
    dir = canonicalish(dir);

    // Regex tuned to `pulp_add_plugin(...)`, `pulp_add_ios_auv3(...)`,
    // and any future `pulp_add_*` macro the SDK introduces. Intentionally
    // forgiving: whitespace before the paren, optional leading CMake
    // comment is fine because we only need one match anywhere in the
    // file to decide the directory looks like a Pulp project.
    static const std::regex macro_re(R"(\bpulp_add_[A-Za-z0-9_]+\s*\()");

    while (!dir.empty()) {
        auto cmake_txt = dir / "CMakeLists.txt";
        if (fs::exists(cmake_txt, ec) && !ec) {
            std::ifstream f(cmake_txt);
            if (f.is_open()) {
                std::stringstream buf;
                buf << f.rdbuf();
                auto body = buf.str();
                if (std::regex_search(body, macro_re)) {
                    out.push_back(dir);
                }
            }
        }

        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }

    return out;
}

}  // namespace pulp::cli::projects_registry
