// version_diag.cpp — Implementation of `pulp doctor --versions`
//
// Pure-logic core + narrow I/O helpers so the analyzer can be
// exercised from unit tests without a live filesystem or a built
// binary. Only `render_report` writes to std::cout.

#include "version_diag.hpp"
#include "json_writer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <ostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  include <cstdlib>   // _dupenv_s
#  include <io.h>       // _isatty, _fileno
#  define pulp_isatty(fd)   _isatty(fd)
#  define pulp_fileno(stream) _fileno(stream)
#else
#  include <unistd.h>   // isatty, fileno
#  define pulp_isatty(fd)   ::isatty(fd)
#  define pulp_fileno(stream) ::fileno(stream)
#endif

namespace pulp::cli::version_diag {

namespace {

// Locally scoped home-directory lookup. cli_common.cpp also has a
// user_home_dir() but we deliberately don't take that dependency —
// version_diag is linked into the unit-test binary standalone.
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

// Minimal TOML scalar scanner — same shape as cli_common's
// read_pulp_toml_value helper. We keep a local copy so this
// translation unit has no link dependency on cli_common.cpp, which
// would pull in the entire CLI runtime.
//
// The earlier version matched any substring containing `key`, so a
// commented example like
// `# cli_min_version = "0.24.0"` triggered a false skew warning. Now
// we strip the line's `#` comment first, then insist the key appears
// as a full token (preceded by line-start/whitespace and followed by
// whitespace/'=') before reading the quoted value.
std::string read_toml_scalar(const fs::path& toml, const std::string& key) {
    if (!fs::exists(toml)) return {};
    std::ifstream f(toml);
    if (!f.is_open()) return {};
    std::string line;
    while (std::getline(f, line)) {
        // Strip in-line comments. TOML comments start with `#`; we don't
        // need full TOML-grammar support here (no `#` inside quoted
        // values in the surfaces we parse), so a straight first-`#` cut
        // matches the intent and avoids the commented-example false
        // positive.
        auto hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);

        // Locate the key with explicit boundary checks — preceded by
        // start-of-line or whitespace, followed by whitespace or '='.
        // Prevents `some_key_ending_in_cli_min_version = ...` and
        // similar substring aliasing.
        std::size_t pos = 0;
        while ((pos = line.find(key, pos)) != std::string::npos) {
            bool left_ok = (pos == 0) ||
                           line[pos - 1] == ' ' ||
                           line[pos - 1] == '\t';
            auto after = pos + key.size();
            bool right_ok = (after >= line.size()) ||
                            line[after] == '=' ||
                            line[after] == ' ' ||
                            line[after] == '\t';
            if (left_ok && right_ok) break;
            pos = after;
        }
        if (pos == std::string::npos) continue;

        auto q1 = line.find('"', pos);
        auto q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
        if (q1 != std::string::npos && q2 != std::string::npos) {
            return line.substr(q1 + 1, q2 - q1 - 1);
        }
    }
    return {};
}

// Tiny ANSI helpers, gated on `stdout is a TTY and NO_COLOR/--no-color
// are unset` so `pulp doctor --versions` stays script-friendly when
// piped into CI logs. Matches the rest of the CLI's
// cli_common::init_color() policy without pulling the whole runtime
// into this TU.
bool should_emit_color() {
    static const bool enabled = []() {
        // NO_COLOR (https://no-color.org/) — any non-empty value disables.
        if (const char* nc = std::getenv("NO_COLOR")) {
            if (nc[0] != '\0') return false;
        }
        // PULP_NO_COLOR — explicit opt-out for our own CI lanes.
        if (const char* pnc = std::getenv("PULP_NO_COLOR")) {
            if (pnc[0] != '\0') return false;
        }
        // TTY detection. Piped stdout means the consumer is machine-
        // parsing WARN/OK tokens — never inject escape codes there.
        return pulp_isatty(pulp_fileno(stdout)) != 0;
    }();
    return enabled;
}
const char* ansi_bold()   { return should_emit_color() ? "\x1b[1m"  : ""; }
const char* ansi_reset()  { return should_emit_color() ? "\x1b[0m"  : ""; }
const char* ansi_green()  { return should_emit_color() ? "\x1b[32m" : ""; }
const char* ansi_yellow() { return should_emit_color() ? "\x1b[33m" : ""; }
const char* ansi_dim()    { return should_emit_color() ? "\x1b[2m"  : ""; }

}  // namespace

Semver parse_semver(const std::string& s) {
    Semver out;
    out.raw = s;
    if (s.empty()) return out;

    // Strip a leading "v" for tolerance with tag-style strings.
    std::string body = (s.front() == 'v' || s.front() == 'V') ? s.substr(1) : s;

    static const std::regex triple(R"(^(\d+)\.(\d+)\.(\d+)$)");
    std::smatch m;
    if (std::regex_match(body, m, triple)) {
        out.major = std::stoi(m[1].str());
        out.minor = std::stoi(m[2].str());
        out.patch = std::stoi(m[3].str());
        out.comparable = true;
    }
    return out;
}

int compare_semver(const Semver& a, const Semver& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

Semver read_plugin_version(const fs::path& plugin_json_path) {
    Semver out;
    if (plugin_json_path.empty() || !fs::exists(plugin_json_path)) return out;

    std::ifstream f(plugin_json_path);
    if (!f.is_open()) return out;

    std::stringstream buf;
    buf << f.rdbuf();
    auto body = buf.str();

    // Cheap, dependency-free: scrape the first `"version"` field.
    // The plugin manifest is maintained in-repo and has a stable shape.
    // Use a custom delimiter on the raw string so inner `"` characters
    // don't end the literal.
    static const std::regex ver_re(R"RE("version"\s*:\s*"([^"]+)")RE");
    std::smatch m;
    if (std::regex_search(body, m, ver_re)) {
        return parse_semver(m[1].str());
    }
    return out;
}

Semver read_plugin_min_cli_version(const fs::path& plugin_json_path) {
    Semver out;
    if (plugin_json_path.empty() || !fs::exists(plugin_json_path)) return out;

    std::ifstream f(plugin_json_path);
    if (!f.is_open()) return out;

    std::stringstream buf;
    buf << f.rdbuf();
    auto body = buf.str();

    // Same cheap scan as read_plugin_version — the key is a stable
    // top-level JSON string. Absent field → empty Semver (no skew
    // check). Older plugin manifests won't have this field and must
    // silently no-op.
    static const std::regex mcv_re(
        R"RE("min_cli_version"\s*:\s*"([^"]+)")RE");
    std::smatch m;
    if (std::regex_search(body, m, mcv_re)) {
        return parse_semver(m[1].str());
    }
    return out;
}

fs::path locate_plugin_json(const fs::path& active_repo_root,
                            const fs::path& override_path) {
    if (!override_path.empty() && fs::exists(override_path)) {
        return override_path;
    }
    if (!active_repo_root.empty()) {
        auto in_repo = active_repo_root / ".claude-plugin" / "plugin.json";
        if (fs::exists(in_repo)) return in_repo;
    }
    auto home = user_home_dir_local();
    if (!home.empty()) {
        for (auto suffix : {
                fs::path{".claude"} / "plugins" / "pulp" / "plugin.json",
                fs::path{".claude-plugin"} / "pulp" / "plugin.json",
             }) {
            auto candidate = home / suffix;
            if (fs::exists(candidate)) return candidate;
        }
    }
    return {};
}

Semver read_project_cli_min_version(const fs::path& project_root) {
    if (project_root.empty()) return {};
    auto raw = read_toml_scalar(project_root / "pulp.toml", "cli_min_version");
    return parse_semver(raw);
}

ExecutionPreflight analyze_execution_preflight(const Semver& cli,
                                               const Semver& project_sdk,
                                               const Semver& project_cli_min) {
    ExecutionPreflight out;

    auto note_required = [&](const Semver& required) {
        if (!required.comparable) return;
        if (!out.required_cli.comparable ||
            compare_semver(required, out.required_cli) > 0) {
            out.required_cli = required;
        }
    };

    if (cli.comparable && project_cli_min.comparable &&
        compare_semver(project_cli_min, cli) > 0) {
        out.supported = false;
        out.blockers.push_back(
            "Project declares cli_min_version v" + project_cli_min.raw +
            " but installed CLI is v" + cli.raw);
        note_required(project_cli_min);
    }

    if (cli.comparable && project_sdk.comparable &&
        compare_semver(project_sdk, cli) > 0) {
        out.supported = false;
        out.blockers.push_back(
            "Project pins SDK v" + project_sdk.raw +
            " but installed CLI is v" + cli.raw);
        note_required(project_sdk);
    }

    return out;
}

std::vector<SkewFinding> VersionReport::analyze() const {
    std::vector<SkewFinding> findings;

    // Rule 1: hard skew against cli_min_version — the primary reason
    // the design doc surfaces this at all. Warn, never block.
    if (cli.comparable && project_cli_min.comparable) {
        if (compare_semver(project_cli_min, cli) > 0) {
            findings.push_back({
                SkewSeverity::Warn,
                "Project requires CLI >= v" + project_cli_min.raw +
                    " but installed CLI is v" + cli.raw +
                    " — run `pulp upgrade`",
            });
        }
    }

    // Claude plugin's `min_cli_version` is ahead of the installed CLI.
    // This is the exact mirror of the project-side cli_min_version
    // rule, but from the plugin's perspective — the same skew banner
    // the plugin-side skill prints on first invocation, surfaced via
    // `pulp doctor --versions` too (#551).
    if (cli.comparable && plugin_min_cli.comparable) {
        if (compare_semver(plugin_min_cli, cli) > 0) {
            findings.push_back({
                SkewSeverity::Warn,
                "Claude plugin requires CLI >= v" + plugin_min_cli.raw +
                    " but installed CLI is v" + cli.raw +
                    " — run `pulp upgrade`",
            });
        }
    }

    // Rule 2: project SDK newer than the CLI binary. This is the
    // "fleet of projects pulled ahead of the user's machine" case.
    // Advisory only.
    if (cli.comparable && project_sdk.comparable) {
        if (compare_semver(project_sdk, cli) > 0) {
            findings.push_back({
                SkewSeverity::Warn,
                "Project SDK is v" + project_sdk.raw +
                    " but installed CLI is v" + cli.raw +
                    " — consider `pulp upgrade`",
            });
        } else {
            findings.push_back({
                SkewSeverity::Info,
                "CLI v" + cli.raw + " is compatible with project SDK v" +
                    project_sdk.raw,
            });
        }
    }

    // Per-project registry entries (#552). Same semantics as rules 1/2
    // but each finding names the project so the user can tell which
    // entry in a multi-project fleet is behind.
    // Missing-on-disk entries produce a tidy "run `pulp projects
    // remove`" hint — we never auto-prune (see design doc).
    for (const auto& p : projects) {
        const std::string label = p.name.empty()
            ? p.path.filename().string()
            : p.name;

        if (p.missing_on_disk) {
            findings.push_back({
                SkewSeverity::Warn,
                "Registered project '" + label + "' at " + p.path.string() +
                    " no longer exists — run `pulp projects remove " +
                    p.path.string() + "` to forget it",
            });
            continue;
        }

        if (cli.comparable && p.cli_min.comparable &&
            compare_semver(p.cli_min, cli) > 0) {
            findings.push_back({
                SkewSeverity::Warn,
                "Project '" + label + "' requires CLI >= v" + p.cli_min.raw +
                    " but installed CLI is v" + cli.raw +
                    " — run `pulp upgrade`",
            });
        }

        if (cli.comparable && p.sdk.comparable &&
            compare_semver(p.sdk, cli) > 0) {
            findings.push_back({
                SkewSeverity::Warn,
                "Project '" + label + "' SDK is v" + p.sdk.raw +
                    " but installed CLI is v" + cli.raw +
                    " — consider `pulp upgrade`",
            });
        }
    }

    return findings;
}

int render_report(const VersionReport& report) {
    auto line = [](const std::string& label, const std::string& value) {
        std::cout << "  " << label;
        // Pad label column to 10 chars for alignment.
        if (label.size() < 10) {
            std::cout << std::string(10 - label.size(), ' ');
        }
        std::cout << value << "\n";
    };

    std::cout << ansi_bold() << "Pulp Version Diagnostics" << ansi_reset()
              << "\n========================\n\n";

    line("CLI:",
         report.cli.raw.empty() ? std::string("(unknown)")
                                : "v" + report.cli.raw);
    if (!report.plugin.raw.empty()) {
        line("Plugin:", "v" + report.plugin.raw +
                           (report.plugin_json_path.empty()
                                ? std::string{}
                                : "   (" + report.plugin_json_path.string() + ")"));
        if (report.plugin_min_cli.comparable) {
            // Render plugin min_cli_version right under the Plugin line
            // so the requirement sits next to the plugin version it's
            // attached to. Four extra spaces of indent (instead of the
            // usual two) visually subordinate it to the Plugin line above.
            std::cout << "    needs CLI >= v"
                      << report.plugin_min_cli.raw << "\n";
        }
    } else {
        line("Plugin:", "(not found — install the Claude Code plugin to enable checks)");
    }

    if (!report.project_sdk.raw.empty()) {
        line("SDK:", "v" + report.project_sdk.raw +
                        (report.project_root.empty()
                             ? std::string{}
                             : "   (" + report.project_root.string() + ")"));
    } else {
        line("SDK:", "(no active project — run this command from a Pulp project to compare)");
    }

    if (report.project_cli_min.comparable) {
        line("CLI min:", "v" + report.project_cli_min.raw +
                            "   (from pulp.toml)");
    }

    // Per-project lines (#552). Sourced from the registry at
    // ~/.pulp/projects.json plus any ancestor projects surfaced via
    // --scan-parents. Displayed as a separate block so the "active
    // project" line above keeps its primacy.
    if (!report.projects.empty()) {
        std::cout << "\n  Projects:\n";
        for (const auto& p : report.projects) {
            std::string label = p.name.empty()
                ? p.path.filename().string()
                : p.name;
            std::string tag = p.scanned ? " (scanned)" : "";
            if (p.missing_on_disk) tag = " (missing)";

            std::string sdk_display = p.sdk.raw.empty()
                ? std::string("(sdk ?)")
                : std::string("sdk v") + p.sdk.raw;
            std::string cli_min_display = p.cli_min.comparable
                ? std::string(", cli_min v") + p.cli_min.raw
                : std::string{};

            std::cout << "    - " << label << tag
                      << " [" << sdk_display << cli_min_display << "]\n"
                      << "      " << ansi_dim() << p.path.string()
                      << ansi_reset() << "\n";
        }
    }

    std::cout << "\n";

    auto findings = report.analyze();
    int warn_count = 0;
    if (findings.empty()) {
        std::cout << ansi_dim()
                  << "  No version information to compare."
                  << ansi_reset() << "\n";
    } else {
        std::cout << "  Skew checks:\n";
        for (auto& f : findings) {
            if (f.severity == SkewSeverity::Warn) {
                ++warn_count;
                std::cout << "    " << ansi_yellow() << "WARN" << ansi_reset()
                          << " — " << f.message << "\n";
            } else {
                std::cout << "    " << ansi_green() << "OK" << ansi_reset()
                          << "   — " << f.message << "\n";
            }
        }
    }

    std::cout << "\n";
    if (warn_count == 0) {
        std::cout << "  " << ansi_green() << "All checks passed."
                  << ansi_reset() << "\n";
    } else {
        std::cout << "  " << ansi_yellow() << warn_count
                  << " warning" << (warn_count == 1 ? "" : "s")
                  << ansi_reset()
                  << " — advisory only, commands continue to run.\n";
    }
    return warn_count > 0 ? 1 : 0;
}

namespace {

void write_semver_json(std::ostream& os, const Semver& v) {
    os << "{\"raw\": \"" << json_escape(v.raw) << "\""
       << ", \"comparable\": " << (v.comparable ? "true" : "false");
    if (v.comparable) {
        os << ", \"major\": " << v.major
           << ", \"minor\": " << v.minor
           << ", \"patch\": " << v.patch;
    }
    os << "}";
}

}  // namespace

int render_report_json(const VersionReport& report) {
    auto findings = report.analyze();

    std::cout << "{\n";
    std::cout << "  \"cli\": ";
    write_semver_json(std::cout, report.cli);
    std::cout << ",\n  \"plugin\": ";
    write_semver_json(std::cout, report.plugin);
    std::cout << ",\n  \"plugin_min_cli\": ";
    write_semver_json(std::cout, report.plugin_min_cli);
    std::cout << ",\n  \"plugin_json_path\": \""
              << json_escape(report.plugin_json_path.generic_string())
              << "\"";
    std::cout << ",\n  \"project_root\": \""
              << json_escape(report.project_root.generic_string()) << "\"";
    std::cout << ",\n  \"project_sdk\": ";
    write_semver_json(std::cout, report.project_sdk);
    std::cout << ",\n  \"project_cli_min\": ";
    write_semver_json(std::cout, report.project_cli_min);

    std::cout << ",\n  \"projects\": [";
    for (size_t i = 0; i < report.projects.size(); ++i) {
        const auto& p = report.projects[i];
        std::cout << (i == 0 ? "\n    " : ",\n    ") << "{";
        std::cout << "\"path\": \""           << json_escape(p.path.generic_string()) << "\", "
                  << "\"name\": \""           << json_escape(p.name) << "\", "
                  << "\"sdk\": ";
        write_semver_json(std::cout, p.sdk);
        std::cout << ", \"cli_min\": ";
        write_semver_json(std::cout, p.cli_min);
        std::cout << ", \"missing_on_disk\": "
                  << (p.missing_on_disk ? "true" : "false");
        std::cout << ", \"scanned\": "
                  << (p.scanned ? "true" : "false");
        std::cout << "}";
    }
    if (!report.projects.empty()) std::cout << "\n  ";
    std::cout << "]";

    std::cout << ",\n  \"findings\": [";
    for (size_t i = 0; i < findings.size(); ++i) {
        const auto& f = findings[i];
        std::cout << (i == 0 ? "\n    " : ",\n    ") << "{"
                  << "\"severity\": \""
                  << (f.severity == SkewSeverity::Warn ? "warn" : "info")
                  << "\", "
                  << "\"message\": \"" << json_escape(f.message) << "\""
                  << "}";
    }
    if (!findings.empty()) std::cout << "\n  ";
    std::cout << "]\n";

    std::cout << "}\n";
    return 0;
}

}  // namespace pulp::cli::version_diag
