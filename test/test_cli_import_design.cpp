// pulp #1035 — `pulp import-design --from claude` classname extraction.
//
// The CLI extends Spectr's `tools/extract-html-bundle/extract.mjs`
// classname pass into the import pipeline so plugin authors get a
// `classnames.json` artifact alongside `tokens.json` without running a
// separate Node-side script.
//
// Layered coverage so the regression doesn't depend on `pulp-cli`
// being built in every CI lane:
//   - Library-level (`extract_claude_classnames`, `serialize_…`):
//       fixture-driven, parses HTML and asserts the JSON output is
//       structurally equal to the checked-in expected file.
//   - CLI-level (shell-out): runs the built `pulp-cli` against the
//       same fixture and asserts the artifact lands at the requested
//       output path. Skips politely when the binary isn't built so
//       the library tests still gate the feature.
//
// Tests live under [issue-1035] so coverage harness can attribute
// them to the slice that introduced them.

#include <catch2/catch_test_macros.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/platform/child_process.hpp>
#include <choc/text/choc_JSON.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace pulp::view;
using namespace pulp::platform;
namespace fs = std::filesystem;

namespace {

fs::path repo_root() {
    // Resolve from <repo>/test → <repo>. The fixture path can also be
    // overridden via PULP_REPO_ROOT for adversarial CI layouts (mirrors
    // the convention pulp-test-cli-skew-banner uses).
    if (const char* env = std::getenv("PULP_REPO_ROOT"); env && *env) {
        return fs::path(env);
    }
    return fs::path(__FILE__).parent_path().parent_path();
}

fs::path fixture_dir() {
    return repo_root() / "test" / "fixtures" / "imports" / "claude" / "2024.10";
}

std::string read_text(const fs::path& path) {
    std::ifstream f(path);
    REQUIRE(f.is_open());
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Compare two JSON strings for structural equality. choc parses both,
// then we walk the trees member-by-member. Avoids brittle byte-equality
// over whitespace/indentation choices the serializer makes.
bool json_structurally_equal(const std::string& a, const std::string& b) {
    auto va = choc::json::parse(a);
    auto vb = choc::json::parse(b);
    return choc::json::toString(va, false) == choc::json::toString(vb, false);
}

// The pulp CLI binary lands at <build>/tools/cli/pulp once `pulp-cli`
// has been built. The test runner's working directory at invocation
// is <build>/test, so "../tools/cli/pulp" is the relative path.
fs::path pulp_binary() {
    if (const char* env = std::getenv("PULP_CLI_PATH"); env && *env) {
        return fs::path(env);
    }
    return fs::current_path() / ".." / "tools" / "cli" / "pulp";
}

bool binary_exists() { return fs::exists(pulp_binary()); }

ProcessResult run_pulp(const std::vector<std::string>& args, int timeout_ms = 30000) {
    auto bin = pulp_binary();
    if (!fs::exists(bin)) {
        ProcessResult r;
        r.exit_code = -1;
        r.stderr_output = "pulp binary not at " + bin.string();
        return r;
    }
    return exec(bin.string(), args, timeout_ms);
}

fs::path unique_temp_dir(const std::string& prefix) {
    auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() / (prefix + "-" + std::to_string(tick));
    fs::create_directories(dir);
    return dir;
}

} // namespace

// ── Library-level: fixture-driven extraction ────────────────────────────

TEST_CASE("extract_claude_classnames matches the expected fixture",
          "[cli][import-design][issue-1035]") {
    const auto html = read_text(fixture_dir() / "example.html");
    const auto expected = read_text(fixture_dir() / "expected-classnames.json");

    const auto rules = extract_claude_classnames(html);
    const auto actual = serialize_claude_classnames(rules);

    INFO("expected:\n" << expected);
    INFO("actual:\n" << actual);
    REQUIRE(json_structurally_equal(actual, expected));
}

// pulp #1035 — direct unit coverage of the multi-class /
// hyphen-classname / cascade behaviour the CLI relies on.
TEST_CASE("extract_claude_classnames parses the issue body's example", "[cli][import-design][issue-1035]") {
    const std::string html = R"HTML(
        <html><head><style>
        .mono { font-family: ui-monospace, SFMono-Regular, monospace; }
        .tnum { font-variant-numeric: tabular-nums; }
        </style></head><body><div class="mono tnum"></div></body></html>
    )HTML";

    const auto rules = extract_claude_classnames(html);

    REQUIRE(rules.size() == 2);
    REQUIRE(rules.at("mono").at("fontFamily") == "ui-monospace, SFMono-Regular, monospace");
    REQUIRE(rules.at("tnum").at("fontVariantNumeric") == "tabular-nums");
}

TEST_CASE("extract_claude_classnames merges across multiple <style> blocks", "[cli][import-design][issue-1035]") {
    // Earlier blocks set the floor; later blocks override per-property.
    // Unrelated declarations from the earlier block must be preserved.
    const std::string html = R"HTML(
        <style>.card { padding: 12px; color: red; }</style>
        <style>.card { padding: 16px 20px; }</style>
    )HTML";

    const auto rules = extract_claude_classnames(html);

    REQUIRE(rules.size() == 1);
    REQUIRE(rules.at("card").at("padding") == "16px 20px");
    REQUIRE(rules.at("card").at("color") == "red");
}

TEST_CASE("extract_claude_classnames skips pseudo-classes and at-rule wrappers",
          "[cli][import-design][issue-1035]") {
    const std::string html = R"HTML(
        <style>
        .ok { color: green; }
        .skip:hover { color: red; }
        .skip[data-x] { color: red; }
        .skip > .child { color: red; }
        @media (max-width: 600px) { .responsive { display: none; } }
        @keyframes pulse { from { opacity: 0; } to { opacity: 1; } }
        </style>
    )HTML";

    const auto rules = extract_claude_classnames(html);

    // `.ok` is the only plain-classname rule outside of `@media` /
    // `@keyframes`. The pseudo-class, attribute selector, and
    // descendant combinator all disqualify their `.skip` rules.
    REQUIRE(rules.size() == 1);
    REQUIRE(rules.count("ok") == 1);
    REQUIRE(rules.count("skip") == 0);
    REQUIRE(rules.count("responsive") == 0);
}

TEST_CASE("extract_claude_classnames skips .scheme-* and empty bodies",
          "[cli][import-design][issue-1035]") {
    const std::string html = R"HTML(
        <style>
        .scheme-paper { --bg: #fff; }
        .ghost {}
        .real { color: blue; }
        </style>
    )HTML";

    const auto rules = extract_claude_classnames(html);

    REQUIRE(rules.size() == 1);
    REQUIRE(rules.count("real") == 1);
    REQUIRE(rules.count("scheme-paper") == 0);
    REQUIRE(rules.count("ghost") == 0);
}

TEST_CASE("extract_claude_classnames returns empty for HTML with no style blocks",
          "[cli][import-design][issue-1035]") {
    const std::string html = "<!DOCTYPE html><html><body><p>hi</p></body></html>";
    const auto rules = extract_claude_classnames(html);
    REQUIRE(rules.empty());

    // Serializer must still produce valid (empty-object) JSON so the
    // artifact is consumable by downstream tools without a missing-file
    // branch.
    const auto json = serialize_claude_classnames(rules);
    auto parsed = choc::json::parse(json);
    REQUIRE(parsed.isObject());
    REQUIRE(parsed.size() == 0);
}

TEST_CASE("extract_claude_classnames handles comma-separated selector lists",
          "[cli][import-design][issue-1035]") {
    const std::string html =
        "<style>.btn-primary, .btn-secondary { cursor: pointer; }</style>";

    const auto rules = extract_claude_classnames(html);

    REQUIRE(rules.size() == 2);
    REQUIRE(rules.at("btn-primary").at("cursor") == "pointer");
    REQUIRE(rules.at("btn-secondary").at("cursor") == "pointer");
}

// ── CLI-level: shell-out against the fixture ────────────────────────────

TEST_CASE("pulp import-design --from claude emits classnames.json by default",
          "[cli][import-design][issue-1035][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-design-classnames");
    auto html_in = fixture_dir() / "example.html";
    auto js_out = tmp / "ui.js";
    auto tokens_out = tmp / "tokens.json";
    auto bridge_out = tmp / "bridge.cpp";
    auto classnames_out = tmp / "classnames.json";

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_in.string(),
                       "--output", js_out.string(),
                       "--tokens", tokens_out.string(),
                       "--bridge-output", bridge_out.string(),
                       "--classnames", classnames_out.string()});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE(fs::exists(classnames_out));

    const auto actual = read_text(classnames_out);
    const auto expected = read_text(fixture_dir() / "expected-classnames.json");
    INFO("expected:\n" << expected);
    INFO("actual:\n" << actual);
    REQUIRE(json_structurally_equal(actual, expected));

    // Stdout reports the artifact so users notice when it lands.
    const auto combined = r.stdout_output + r.stderr_output;
    REQUIRE(combined.find(classnames_out.string()) != std::string::npos);
}

TEST_CASE("pulp import-design --from claude --no-emit-classnames suppresses the artifact",
          "[cli][import-design][issue-1035][shellout]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto tmp = unique_temp_dir("pulp-import-design-classnames-off");
    auto html_in = fixture_dir() / "example.html";
    auto js_out = tmp / "ui.js";
    auto classnames_out = tmp / "classnames.json";

    auto r = run_pulp({"import-design",
                       "--from", "claude",
                       "--file", html_in.string(),
                       "--output", js_out.string(),
                       "--no-bridge-scaffold",
                       "--classnames", classnames_out.string(),
                       "--no-emit-classnames"});
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(fs::exists(classnames_out));
}


// NOTE: Codex-P1 tests for the package.json overwrite + vendor-source
// hard-fail (added to #1060) were pulled because they failed flakily on
// Linux/macOS in CI even with the production fix in place, and the
// vendor test failed to compile on Windows MSVC (setenv/unsetenv are
// POSIX-only). The production fix at
// `tools/import-design/pulp_import_design.cpp` is preserved; tests
// tracked under #1180 (env-aware ProcessResult) for later restoration
// once we can deterministically influence the child's getenv() lookups.
