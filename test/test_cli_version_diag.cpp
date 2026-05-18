// test_cli_version_diag.cpp — Unit tests for `pulp doctor --versions`
//
// Issue #499 Slice 1. Exercises the pure-logic core of version_diag
// (semver parse/compare + skew analyzer) and the plugin.json scraper,
// without shelling out to the built binary. Shell-out coverage lives
// in test_cli_shellout.cpp ([cli][shellout][issue-499]) and in the
// cli-doctor-versions CTest case registered in tools/cli/CMakeLists.txt.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/version_diag.hpp"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace pulp::cli::version_diag;

namespace {

// Scoped tempdir helper — keeps each TEST_CASE isolated without
// depending on GTest-style fixtures. Catch2 is used for every other
// unit test in this repo; matches the house style.
struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        // %p gives a unique per-process token; combine with a counter
        // for parallel test isolation.
        static std::atomic<int> seq{0};
        int n = seq.fetch_add(1);
        path = base / ("pulp-version-diag-test-" + std::to_string(
                          reinterpret_cast<std::uintptr_t>(this)) + "-" +
                       std::to_string(n));
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void write_file(const fs::path& p, const std::string& body) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << body;
}

}  // namespace

TEST_CASE("parse_semver accepts clean M.N.P triples", "[version-diag][issue-499]") {
    auto v = parse_semver("0.24.0");
    REQUIRE(v.comparable);
    REQUIRE(v.major == 0);
    REQUIRE(v.minor == 24);
    REQUIRE(v.patch == 0);
    REQUIRE(v.raw == "0.24.0");
}

TEST_CASE("parse_semver tolerates a leading 'v'", "[version-diag][issue-499]") {
    auto v = parse_semver("v1.2.3");
    REQUIRE(v.comparable);
    REQUIRE(v.major == 1);
    REQUIRE(v.minor == 2);
    REQUIRE(v.patch == 3);
}

TEST_CASE("parse_semver marks non-triple strings non-comparable",
          "[version-diag][issue-499]") {
    // Untagged / dev builds must be flagged non-comparable so skew
    // analysis silently skips them — this is the "untagged builds
    // are silently skipped" rule from the design doc.
    for (auto s : {"0.24.0-dev", "main", "", "abcd", "1.2", "1.2.3.4"}) {
        auto v = parse_semver(s);
        REQUIRE_FALSE(v.comparable);
    }
}

TEST_CASE("compare_semver orders by major/minor/patch",
          "[version-diag][issue-499]") {
    auto a = parse_semver("0.24.0");
    auto b = parse_semver("0.25.0");
    auto c = parse_semver("0.24.0");
    REQUIRE(compare_semver(a, b) < 0);
    REQUIRE(compare_semver(b, a) > 0);
    REQUIRE(compare_semver(a, c) == 0);

    auto d = parse_semver("1.0.0");
    REQUIRE(compare_semver(a, d) < 0);
    REQUIRE(compare_semver(d, b) > 0);
}

TEST_CASE("read_plugin_version scrapes the version field from plugin.json",
          "[version-diag][issue-499]") {
    TempDir tmp;
    auto plugin_json = tmp.path / ".claude-plugin" / "plugin.json";
    write_file(plugin_json, R"({
        "name": "pulp",
        "version": "0.6.0",
        "description": "test"
    })");

    auto v = read_plugin_version(plugin_json);
    REQUIRE(v.comparable);
    REQUIRE(v.major == 0);
    REQUIRE(v.minor == 6);
    REQUIRE(v.patch == 0);
}

TEST_CASE("read_plugin_version returns empty on missing file",
          "[version-diag][issue-499]") {
    auto v = read_plugin_version("/nonexistent/path/plugin.json");
    REQUIRE_FALSE(v.comparable);
    REQUIRE(v.raw.empty());
}

TEST_CASE("read_plugin_version returns empty when manifest has no version field",
          "[version-diag][coverage][issue-643]") {
    TempDir tmp;
    auto plugin_json = tmp.path / ".claude-plugin" / "plugin.json";
    write_file(plugin_json, R"({
        "name": "pulp",
        "description": "test"
    })");

    auto v = read_plugin_version(plugin_json);
    REQUIRE_FALSE(v.comparable);
    REQUIRE(v.raw.empty());
}

TEST_CASE("read_plugin_version accepts tag-style version strings",
          "[version-diag][coverage][phase3]") {
    TempDir tmp;
    auto plugin_json = tmp.path / ".claude-plugin" / "plugin.json";
    write_file(plugin_json, R"({
        "name": "pulp",
        "version": "v1.2.3"
    })");

    auto v = read_plugin_version(plugin_json);
    REQUIRE(v.comparable);
    REQUIRE(v.raw == "v1.2.3");
    REQUIRE(v.major == 1);
    REQUIRE(v.minor == 2);
    REQUIRE(v.patch == 3);
}

TEST_CASE("locate_plugin_json prefers an explicit override",
          "[version-diag][issue-499]") {
    TempDir tmp;
    auto explicit_path = tmp.path / "custom-plugin.json";
    write_file(explicit_path, R"({"version": "9.9.9"})");

    auto found = locate_plugin_json(tmp.path, explicit_path);
    REQUIRE(found == explicit_path);
}

TEST_CASE("locate_plugin_json falls back to the repo .claude-plugin dir",
          "[version-diag][issue-499]") {
    TempDir tmp;
    auto repo_plugin = tmp.path / ".claude-plugin" / "plugin.json";
    write_file(repo_plugin, R"({"version": "0.6.0"})");

    auto found = locate_plugin_json(tmp.path, {});
    REQUIRE(found == repo_plugin);
}

TEST_CASE("analyze warns when cli_min_version exceeds installed CLI",
          "[version-diag][issue-499]") {
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    r.project_cli_min = parse_semver("0.24.0");

    auto findings = r.analyze();
    REQUIRE_FALSE(findings.empty());
    bool saw_warn = false;
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Warn &&
            f.message.find("pulp upgrade") != std::string::npos) {
            saw_warn = true;
        }
    }
    REQUIRE(saw_warn);
}

TEST_CASE("analyze is silent when cli_min_version is satisfied",
          "[version-diag][issue-499]") {
    VersionReport r;
    r.cli = parse_semver("0.24.0");
    r.project_cli_min = parse_semver("0.20.0");
    // No project_sdk — so no "compatible" Info line either. Findings
    // should therefore be empty (no warnings, no info).
    auto findings = r.analyze();
    REQUIRE(findings.empty());
}

TEST_CASE("analyze warns when project SDK is ahead of the installed CLI",
          "[version-diag][issue-499]") {
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    r.project_sdk = parse_semver("0.24.0");

    auto findings = r.analyze();
    bool saw_warn = false;
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Warn &&
            f.message.find("Project SDK is v0.24.0") != std::string::npos) {
            saw_warn = true;
        }
    }
    REQUIRE(saw_warn);
}

TEST_CASE("execution preflight blocks when project requirements exceed installed CLI",
          "[version-diag][issue-682]") {
    auto preflight = analyze_execution_preflight(parse_semver("0.33.0"),
                                                 parse_semver("0.38.0"),
                                                 parse_semver("0.36.0"));
    REQUIRE_FALSE(preflight.supported);
    REQUIRE(preflight.required_cli.comparable);
    REQUIRE(preflight.required_cli.raw == "0.38.0");
    REQUIRE(preflight.blockers.size() == 2);

    bool saw_sdk = false;
    bool saw_cli_min = false;
    for (const auto& blocker : preflight.blockers) {
        if (blocker.find("SDK v0.38.0") != std::string::npos) saw_sdk = true;
        if (blocker.find("cli_min_version v0.36.0") != std::string::npos) saw_cli_min = true;
    }
    REQUIRE(saw_sdk);
    REQUIRE(saw_cli_min);
}

TEST_CASE("execution preflight ignores satisfied and non-comparable requirements",
          "[version-diag][issue-682]") {
    auto satisfied = analyze_execution_preflight(parse_semver("0.38.0"),
                                                 parse_semver("0.38.0"),
                                                 parse_semver("0.37.0"));
    REQUIRE(satisfied.supported);
    REQUIRE(satisfied.blockers.empty());

    auto noncomparable = analyze_execution_preflight(parse_semver("0.38.0-dev"),
                                                     parse_semver("0.39.0"),
                                                     parse_semver("0.39.0"));
    REQUIRE(noncomparable.supported);
    REQUIRE(noncomparable.blockers.empty());
}

TEST_CASE("analyze emits a compatible Info line when CLI >= project SDK",
          "[version-diag][issue-499]") {
    VersionReport r;
    r.cli = parse_semver("0.24.0");
    r.project_sdk = parse_semver("0.24.0");

    auto findings = r.analyze();
    bool saw_ok = false;
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Info) saw_ok = true;
    }
    REQUIRE(saw_ok);
}

TEST_CASE("analyze skips skew checks silently when CLI is untagged",
          "[version-diag][issue-499]") {
    VersionReport r;
    r.cli = parse_semver("0.24.0-dev");    // non-comparable
    r.project_cli_min = parse_semver("0.25.0");
    r.project_sdk = parse_semver("0.25.0");

    auto findings = r.analyze();
    // Neither rule fires when one side is non-comparable.
    REQUIRE(findings.empty());
}

TEST_CASE("read_project_cli_min_version reads pulp.toml field",
          "[version-diag][issue-499]") {
    TempDir tmp;
    auto toml = tmp.path / "pulp.toml";
    write_file(toml, "[pulp]\nsdk_version = \"0.24.0\"\ncli_min_version = \"0.22.0\"\n");
    auto v = read_project_cli_min_version(tmp.path);
    REQUIRE(v.comparable);
    REQUIRE(v.major == 0);
    REQUIRE(v.minor == 22);
    REQUIRE(v.patch == 0);
}

TEST_CASE("read_project_cli_min_version returns empty when absent",
          "[version-diag][issue-499]") {
    TempDir tmp;
    auto toml = tmp.path / "pulp.toml";
    write_file(toml, "[pulp]\nsdk_version = \"0.24.0\"\n");
    auto v = read_project_cli_min_version(tmp.path);
    REQUIRE_FALSE(v.comparable);
}

TEST_CASE("read_project_cli_min_version ignores an empty project root",
          "[version-diag][coverage][issue-643]") {
    auto v = read_project_cli_min_version({});
    REQUIRE_FALSE(v.comparable);
    REQUIRE(v.raw.empty());
}

// Codex 2026-04-21 review on #546: the earlier scanner treated any line
// containing the `cli_min_version` substring as a real config entry and
// grabbed the next quoted value. A commented example therefore produced
// a false skew warning in `pulp doctor --versions`. The fix strips
// in-line `#` comments before matching, so the commented line is
// silently ignored and `read_project_cli_min_version` returns empty.
TEST_CASE("read_project_cli_min_version ignores commented-out examples",
          "[version-diag][issue-499][codex-546]") {
    TempDir tmp;
    auto toml = tmp.path / "pulp.toml";
    write_file(toml,
               "[pulp]\n"
               "sdk_version = \"0.24.0\"\n"
               "# cli_min_version = \"0.22.0\"\n"
               "# Example: cli_min_version = \"0.23.0\"\n");
    auto v = read_project_cli_min_version(tmp.path);
    REQUIRE_FALSE(v.comparable);
}

// Key-boundary check: a key whose name CONTAINS `cli_min_version` as a
// substring must not alias onto the real key. Guards against the
// substring-aliasing half of the #546 finding.
TEST_CASE("read_project_cli_min_version ignores substring-matching keys",
          "[version-diag][issue-499][codex-546]") {
    TempDir tmp;
    auto toml = tmp.path / "pulp.toml";
    write_file(toml,
               "[pulp]\n"
               "legacy_cli_min_version_history = \"0.22.0\"\n"
               "sdk_version = \"0.24.0\"\n");
    auto v = read_project_cli_min_version(tmp.path);
    REQUIRE_FALSE(v.comparable);
}

// Real entry still reads correctly when it follows comments and
// unrelated keys — proves the fix didn't break the happy path.
TEST_CASE("read_project_cli_min_version still reads through comments",
          "[version-diag][issue-499][codex-546]") {
    TempDir tmp;
    auto toml = tmp.path / "pulp.toml";
    write_file(toml,
               "[pulp]\n"
               "# cli_min_version = \"0.22.0\"  # not this one\n"
               "cli_min_version = \"0.23.0\"\n"
               "# trailing comment\n");
    auto v = read_project_cli_min_version(tmp.path);
    REQUIRE(v.comparable);
    REQUIRE(v.major == 0);
    REQUIRE(v.minor == 23);
    REQUIRE(v.patch == 0);
}

// ── Slice 1b (#552) — per-project skew via VersionReport.projects ──

TEST_CASE("analyze warns per-project when project[].cli_min exceeds CLI",
          "[version-diag][issue-552]") {
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    ProjectEntry p;
    p.path = "/tmp/a";
    p.name = "A";
    p.cli_min = parse_semver("0.24.0");
    r.projects.push_back(p);

    auto findings = r.analyze();
    bool saw_a_warn = false;
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Warn &&
            f.message.find("Project 'A' requires CLI") != std::string::npos) {
            saw_a_warn = true;
        }
    }
    REQUIRE(saw_a_warn);
}

TEST_CASE("analyze warns per-project when project[].sdk is ahead of CLI",
          "[version-diag][issue-552]") {
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    ProjectEntry p;
    p.path = "/tmp/other";
    p.name = "Other";
    p.sdk = parse_semver("0.24.0");
    r.projects.push_back(p);

    auto findings = r.analyze();
    bool saw_warn = false;
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Warn &&
            f.message.find("Project 'Other' SDK is v0.24.0") != std::string::npos) {
            saw_warn = true;
        }
    }
    REQUIRE(saw_warn);
}

TEST_CASE("analyze warns when a registered project has gone missing on disk",
          "[version-diag][issue-552]") {
    // Stale-entry policy: keep the entry (never auto-prune) but surface
    // a prominent warning with a copy-paste remove hint. Documented in
    // projects_registry.hpp.
    VersionReport r;
    r.cli = parse_semver("0.24.0");
    ProjectEntry p;
    p.path = "/tmp/gone-away";
    p.name = "GoneAway";
    p.missing_on_disk = true;
    r.projects.push_back(p);

    auto findings = r.analyze();
    bool saw_missing = false;
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Warn &&
            f.message.find("no longer exists") != std::string::npos &&
            f.message.find("pulp projects remove") != std::string::npos) {
            saw_missing = true;
        }
    }
    REQUIRE(saw_missing);
}

TEST_CASE("analyze is silent when a per-project SDK matches the CLI",
          "[version-diag][issue-552]") {
    VersionReport r;
    r.cli = parse_semver("0.24.0");
    ProjectEntry p;
    p.path = "/tmp/ok";
    p.name = "OK";
    p.sdk = parse_semver("0.24.0");
    r.projects.push_back(p);

    auto findings = r.analyze();
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Warn) {
            REQUIRE(f.message.find("'OK'") == std::string::npos);
        }
    }
}

// ── Slice 6 (#551) — plugin ↔ CLI skew detection ─────────────────────

TEST_CASE("read_plugin_min_cli_version scrapes min_cli_version field",
          "[version-diag][issue-551]") {
    TempDir tmp;
    auto plugin_json = tmp.path / ".claude-plugin" / "plugin.json";
    write_file(plugin_json, R"({
        "name": "pulp",
        "version": "0.8.0",
        "min_cli_version": "0.31.0",
        "description": "test"
    })");

    auto v = read_plugin_min_cli_version(plugin_json);
    REQUIRE(v.comparable);
    REQUIRE(v.major == 0);
    REQUIRE(v.minor == 31);
    REQUIRE(v.patch == 0);
}

TEST_CASE("read_plugin_min_cli_version is empty when the field is absent",
          "[version-diag][issue-551]") {
    // Forward-compatible: older plugin builds (pre-Slice 6) shipped no
    // min_cli_version field. The helper must return empty, not choke,
    // so the skew analyzer silently skips the check.
    TempDir tmp;
    auto plugin_json = tmp.path / ".claude-plugin" / "plugin.json";
    write_file(plugin_json, R"({
        "name": "pulp",
        "version": "0.7.0"
    })");
    auto v = read_plugin_min_cli_version(plugin_json);
    REQUIRE_FALSE(v.comparable);
    REQUIRE(v.raw.empty());
}

TEST_CASE("analyze warns when plugin_min_cli exceeds installed CLI",
          "[version-diag][issue-551]") {
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    r.plugin = parse_semver("0.8.0");
    r.plugin_min_cli = parse_semver("0.31.0");

    auto findings = r.analyze();
    bool saw_warn = false;
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Warn &&
            f.message.find("Claude plugin requires CLI") != std::string::npos &&
            f.message.find("pulp upgrade") != std::string::npos) {
            saw_warn = true;
        }
    }
    REQUIRE(saw_warn);
}

TEST_CASE("analyze is silent when plugin_min_cli is satisfied by CLI",
          "[version-diag][issue-551]") {
    VersionReport r;
    r.cli = parse_semver("0.31.0");
    r.plugin = parse_semver("0.8.0");
    r.plugin_min_cli = parse_semver("0.31.0");  // equal — satisfied

    auto findings = r.analyze();
    for (auto& f : findings) {
        if (f.severity == SkewSeverity::Warn) {
            REQUIRE(f.message.find("Claude plugin requires") == std::string::npos);
        }
    }
}

TEST_CASE("analyze skips plugin_min_cli check when the field is absent",
          "[version-diag][issue-551]") {
    // Backward-compat: a plugin manifest without min_cli_version must
    // never surface a skew warning. This is the "older plugin build"
    // path the design doc calls out explicitly.
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    r.plugin = parse_semver("0.7.0");
    // plugin_min_cli deliberately empty.
    auto findings = r.analyze();
    for (auto& f : findings) {
        REQUIRE(f.message.find("Claude plugin requires") == std::string::npos);
    }
}

TEST_CASE("render_report_json exposes plugin_min_cli as a top-level field",
          "[version-diag][issue-551]") {
    // The /upgrade slash command + plugin skills rely on this key to
    // make a skew decision. Renaming or dropping it breaks the helper
    // at `.agents/skills/_common/cli_version_check.sh`.
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    r.plugin = parse_semver("0.8.0");
    r.plugin_min_cli = parse_semver("0.31.0");

    std::stringstream capture;
    auto* prev = std::cout.rdbuf(capture.rdbuf());
    int rc = render_report_json(r);
    std::cout.rdbuf(prev);

    REQUIRE(rc == 0);
    auto out = capture.str();
    REQUIRE(out.find("\"plugin_min_cli\":") != std::string::npos);
    // Skew finding should also land in findings[] so JSON consumers
    // don't need to re-run the comparison themselves.
    REQUIRE(out.find("Claude plugin requires") != std::string::npos);
}

TEST_CASE("render_report returns non-zero and prints plugin/project warnings",
          "[version-diag][issue-643]") {
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    r.plugin = parse_semver("0.8.0");
    r.plugin_min_cli = parse_semver("0.31.0");
    r.project_sdk = parse_semver("0.24.0");
    r.project_cli_min = parse_semver("0.25.0");
    r.project_root = "/tmp/pulp-render-warn";
    r.plugin_json_path = "/tmp/pulp-render-warn/plugin.json";

    std::stringstream capture;
    auto* prev = std::cout.rdbuf(capture.rdbuf());
    int rc = render_report(r);
    std::cout.rdbuf(prev);

    REQUIRE(rc == 1);
    auto out = capture.str();
    REQUIRE(out.find("Pulp Version Diagnostics") != std::string::npos);
    REQUIRE(out.find("Plugin:") != std::string::npos);
    REQUIRE(out.find("needs CLI >= v0.31.0") != std::string::npos);
    REQUIRE(out.find("WARN") != std::string::npos);
    REQUIRE(out.find("Claude plugin requires CLI >= v0.31.0") != std::string::npos);
    REQUIRE(out.find("Project requires CLI >= v0.25.0") != std::string::npos);
    REQUIRE(out.find("Project SDK is v0.24.0") != std::string::npos);
    REQUIRE(out.find("3 warnings") != std::string::npos);
}

TEST_CASE("render_report returns zero for an empty comparable report",
          "[version-diag][issue-643]") {
    VersionReport r;
    r.cli = parse_semver("0.31.0");

    std::stringstream capture;
    auto* prev = std::cout.rdbuf(capture.rdbuf());
    int rc = render_report(r);
    std::cout.rdbuf(prev);

    REQUIRE(rc == 0);
    auto out = capture.str();
    REQUIRE(out.find("CLI:") != std::string::npos);
    REQUIRE(out.find("v0.31.0") != std::string::npos);
    REQUIRE(out.find("No version information to compare.") != std::string::npos);
    REQUIRE(out.find("All checks passed.") != std::string::npos);
    REQUIRE(out.find("WARN") == std::string::npos);
}

TEST_CASE("render_report_json emits JSON with projects[] and findings[]",
          "[version-diag][issue-552]") {
    // Capture std::cout via a redirected rdbuf so we can test without
    // shelling out. The JSON shape is a documented surface — see
    // docs/reference/cli.md#doctor.
    VersionReport r;
    r.cli = parse_semver("0.20.0");
    ProjectEntry p;
    p.path = "/tmp/a";
    p.name = "A";
    p.cli_min = parse_semver("0.24.0");
    r.projects.push_back(p);

    std::stringstream capture;
    auto* prev = std::cout.rdbuf(capture.rdbuf());
    int rc = render_report_json(r);
    std::cout.rdbuf(prev);

    REQUIRE(rc == 0);
    auto out = capture.str();
    REQUIRE(out.find("\"projects\":") != std::string::npos);
    REQUIRE(out.find("\"findings\":") != std::string::npos);
    REQUIRE(out.find("\"name\": \"A\"") != std::string::npos);
    REQUIRE(out.find("\"severity\": \"warn\"") != std::string::npos);
}

TEST_CASE("render_report_json escapes project fields",
          "[version-diag][coverage][phase3]") {
    VersionReport r;
    r.cli = parse_semver("0.31.0");

    ProjectEntry p;
    p.path = "/tmp/project\"with\\chars";
    p.name = "Quoted \"Project\"\nName";
    p.sdk = parse_semver("0.31.0");
    r.projects.push_back(p);

    std::stringstream capture;
    auto* prev = std::cout.rdbuf(capture.rdbuf());
    int rc = render_report_json(r);
    std::cout.rdbuf(prev);

    REQUIRE(rc == 0);
    auto out = capture.str();
    REQUIRE(out.find(R"("path": "/tmp/project\"with\\chars")") != std::string::npos);
    REQUIRE(out.find(R"("name": "Quoted \"Project\"\nName")") != std::string::npos);
}
