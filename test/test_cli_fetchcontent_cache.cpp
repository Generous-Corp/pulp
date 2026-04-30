// test_cli_fetchcontent_cache.cpp — Unit tests for the
// `pulp doctor --caches` discovery + healing core. Issue #744.
//
// All four acceptance scenarios from the issue are covered (healthy /
// dangling / stale-commit / root-owned). Tests inject a deterministic
// `DiscoveryEnv` so they don't touch the developer's real
// `~/Library/Caches/Pulp/` state — that's the same isolation pattern
// used by test_cli_version_diag and test_cli_projects_registry.
//
// One additional set of tests covers `parse_declared_refs_from_text`
// (the CMakeLists.txt scrape) and `apply_fixes` (the dry-run path).

#include "tools/cli/fetchcontent_cache.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace fcc = pulp::cli::fetchcontent_cache;
namespace fs = std::filesystem;

namespace {

// Convenience helper: build a `DiscoveryEnv` whose lstat / stat /
// list_dir return canned data. The MockState struct is owned by the
// test and the lambdas capture it by reference, mirroring how a real
// fixture would back the env.
struct MockState {
    std::vector<fs::path> children;
    std::map<fs::path, fcc::StatInfo> lstat_results;       // empty Optional → "not present"
    std::map<fs::path, fcc::StatInfo> stat_follow_results; // for symlink targets
};

fcc::DiscoveryEnv make_mock_env(const fs::path& root,
                                fcc::DeclaredRefs refs,
                                MockState& state) {
    fcc::DiscoveryEnv env;
    env.cache_root = root;
    env.declared_refs = std::move(refs);
    env.lstat = [&state](const fs::path& p) -> std::optional<fcc::StatInfo> {
        auto it = state.lstat_results.find(p);
        if (it == state.lstat_results.end()) return std::nullopt;
        return it->second;
    };
    env.stat_follow = [&state](const fs::path& p) -> std::optional<fcc::StatInfo> {
        auto it = state.stat_follow_results.find(p);
        if (it == state.stat_follow_results.end()) return std::nullopt;
        return it->second;
    };
    env.list_dir = [&state](const fs::path&) {
        return state.children;
    };
    return env;
}

std::string json_escape_for_test(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

std::string json_field_fragment(const std::string& key,
                                const std::string& value) {
    return "\"" + key + "\": \"" + json_escape_for_test(value) + "\"";
}

}  // namespace

// ── Acceptance scenario 1: healthy ──────────────────────────────────────────

TEST_CASE("discover: healthy entries report Healthy and exit 0",
          "[fetchcontent_cache][issue-744]") {
    fs::path root = "/fake/cache";
    fs::path entry = root / "choc-f0f5cdf5a938b8b779fea6c083571cce5ccab925";
    MockState state;
    state.children = {entry};

    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = false;
    info.is_directory = true;
    info.is_user_writable = true;
    state.lstat_results[entry] = info;

    fcc::DeclaredRefs refs{
        {"choc", "f0f5cdf5a938b8b779fea6c083571cce5ccab925"}};
    auto env = make_mock_env(root, refs, state);

    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].status == fcc::CacheStatus::Healthy);
    CHECK_FALSE(entries[0].fixable);
    CHECK_FALSE(fcc::any_unhealthy(entries));

    std::stringstream out;
    int rc = fcc::render_report(entries, root, out);
    CHECK(rc == 0);
    auto s = out.str();
    CHECK(s.find("[ok]") != std::string::npos);
    CHECK(s.find(entry.filename().string()) != std::string::npos);
}

// ── Acceptance scenario 2: dangling symlink ─────────────────────────────────

TEST_CASE("discover: dangling symlink reports Dangling, fixable, exit 1",
          "[fetchcontent_cache][issue-744]") {
    fs::path root = "/fake/cache";
    fs::path entry = root / "threejs-077dd13c0e869d9f3dbe55875686f920367de457";
    MockState state;
    state.children = {entry};

    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = true;
    info.symlink_target = "/Users/me/Code/three.js";  // deleted
    info.is_user_writable = true;
    state.lstat_results[entry] = info;
    // Note: stat_follow has NO entry for `entry`, simulating the
    // dangling target case (target doesn't exist when followed).

    fcc::DeclaredRefs refs{
        {"threejs", "077dd13c0e869d9f3dbe55875686f920367de457"}};
    auto env = make_mock_env(root, refs, state);

    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].status == fcc::CacheStatus::Dangling);
    CHECK(entries[0].is_symlink);
    CHECK(entries[0].fixable);
    CHECK(entries[0].resolved_target == fs::path("/Users/me/Code/three.js"));
    CHECK(entries[0].reason.find("symlink target missing") != std::string::npos);
    CHECK(entries[0].reason.find("/Users/me/Code/three.js") != std::string::npos);
    CHECK(fcc::any_unhealthy(entries));

    std::stringstream out;
    int rc = fcc::render_report(entries, root, out);
    CHECK(rc == 1);
    CHECK(out.str().find("dangling-symlink") != std::string::npos);
    CHECK(out.str().find("/Users/me/Code/three.js") != std::string::npos);
}

// ── Acceptance scenario 3: stale commit ─────────────────────────────────────

TEST_CASE("discover: cached ref differs from declared REF reports StaleCommit",
          "[fetchcontent_cache][issue-744]") {
    fs::path root = "/fake/cache";
    // Old cached entry (predates a CMakeLists pin bump).
    fs::path entry = root / "choc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    MockState state;
    state.children = {entry};

    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = false;
    info.is_directory = true;
    info.is_user_writable = true;
    state.lstat_results[entry] = info;

    // Declared in CMakeLists has advanced.
    fcc::DeclaredRefs refs{
        {"choc", "f0f5cdf5a938b8b779fea6c083571cce5ccab925"}};
    auto env = make_mock_env(root, refs, state);

    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].status == fcc::CacheStatus::StaleCommit);
    CHECK(entries[0].fixable);
    CHECK(entries[0].declared_ref == "f0f5cdf5a938b8b779fea6c083571cce5ccab925");
    CHECK(entries[0].cached_ref == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    CHECK(fcc::any_unhealthy(entries));
}

// ── Acceptance scenario 4: root-owned ───────────────────────────────────────

TEST_CASE("discover: not-user-writable directory reports RootOwned, not fixable",
          "[fetchcontent_cache][issue-744]") {
    fs::path root = "/fake/cache";
    fs::path entry = root / "yoga-v3.2.1";
    MockState state;
    state.children = {entry};

    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = false;
    info.is_directory = true;
    info.is_user_writable = false;  // owned by root
    state.lstat_results[entry] = info;

    fcc::DeclaredRefs refs{{"yoga", "v3.2.1"}};
    auto env = make_mock_env(root, refs, state);

    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].status == fcc::CacheStatus::RootOwned);
    CHECK_FALSE(entries[0].fixable);
    CHECK(entries[0].remediation.find("sudo") != std::string::npos);
    CHECK(fcc::any_unhealthy(entries));

    // apply_fixes must NOT touch a root-owned entry.
    auto results = fcc::apply_fixes(entries, /*dry_run=*/false);
    REQUIRE(results.size() == 1);
    CHECK(results[0].outcome == fcc::FixOutcome::Skipped);
}

// ── Dangling + root-owned combo: classification escalates to RootOwned ─────

TEST_CASE("discover: dangling symlink owned by root reports RootOwned",
          "[fetchcontent_cache][issue-744]") {
    fs::path root = "/fake/cache";
    fs::path entry = root / "threejs-077dd13c0e869d9f3dbe55875686f920367de457";
    MockState state;
    state.children = {entry};

    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = true;
    info.symlink_target = "/Users/me/Code/three.js";
    info.is_user_writable = false;  // root-owned dangling symlink
    state.lstat_results[entry] = info;

    fcc::DeclaredRefs refs{{"threejs", "077dd13c0e869d9f3dbe55875686f920367de457"}};
    auto env = make_mock_env(root, refs, state);
    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    // Even though it's dangling, the safety gate escalates to
    // RootOwned because the agent must not silently sudo-rm.
    CHECK(entries[0].status == fcc::CacheStatus::RootOwned);
    CHECK_FALSE(entries[0].fixable);
}

// ── Empty cache root → silent OK, exit 0 ───────────────────────────────────

TEST_CASE("discover: missing cache root returns empty and renders OK",
          "[fetchcontent_cache][issue-744]") {
    fs::path root = "/nonexistent";
    MockState state;  // no children
    fcc::DeclaredRefs refs;
    auto env = make_mock_env(root, refs, state);
    auto entries = fcc::discover_fetchcontent_cache(env);
    CHECK(entries.empty());
    CHECK_FALSE(fcc::any_unhealthy(entries));

    std::stringstream out;
    int rc = fcc::render_report(entries, root, out);
    CHECK(rc == 0);
    CHECK(out.str().find("no entries") != std::string::npos);
}

TEST_CASE("discover: lstat miss reports Unknown and blocks preflight",
          "[fetchcontent_cache][issue-643]") {
    fs::path root = "/fake/cache";
    fs::path entry = root / "choc-f0f5cdf5a938b8b779fea6c083571cce5ccab925";
    MockState state;
    state.children = {entry};

    fcc::DeclaredRefs refs{
        {"choc", "f0f5cdf5a938b8b779fea6c083571cce5ccab925"}};
    auto env = make_mock_env(root, refs, state);

    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].status == fcc::CacheStatus::Unknown);
    CHECK_FALSE(entries[0].fixable);
    CHECK(entries[0].reason == "lstat failed");
    CHECK(entries[0].remediation.find("ls -la") != std::string::npos);
    CHECK(fcc::any_unhealthy(entries));
    CHECK(fcc::blocks_preflight(entries));

    std::stringstream out;
    int rc = fcc::render_preflight(entries, root, out);
    CHECK(rc == 1);
    CHECK(out.str().find("unknown") != std::string::npos);
    CHECK(out.str().find("lstat failed") != std::string::npos);
}

// ── apply_fixes: dry-run skips real I/O ─────────────────────────────────────

TEST_CASE("apply_fixes: dry-run reports DryRun for fixable entries",
          "[fetchcontent_cache][issue-744]") {
    fcc::CacheEntry e;
    e.name = "x";
    e.path = "/fake/cache/x";
    e.status = fcc::CacheStatus::StaleCommit;
    e.fixable = true;
    auto results = fcc::apply_fixes({e}, /*dry_run=*/true);
    REQUIRE(results.size() == 1);
    CHECK(results[0].outcome == fcc::FixOutcome::DryRun);
}

TEST_CASE("apply_fixes: healthy entries always Skipped",
          "[fetchcontent_cache][issue-744]") {
    fcc::CacheEntry e;
    e.name = "x";
    e.path = "/fake/cache/x";
    e.status = fcc::CacheStatus::Healthy;
    e.fixable = false;
    auto results = fcc::apply_fixes({e}, /*dry_run=*/false);
    REQUIRE(results.size() == 1);
    CHECK(results[0].outcome == fcc::FixOutcome::Skipped);
}

// ── apply_fixes: real removal on a tempdir-backed entry ────────────────────

TEST_CASE("apply_fixes: removes a real fixable entry from a tempdir",
          "[fetchcontent_cache][issue-744]") {
    auto tmp = fs::temp_directory_path() / "pulp-fcc-test-744";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "stale-deadbeef");
    {
        std::ofstream(tmp / "stale-deadbeef" / "marker.txt")
            << "stale\n";
    }
    REQUIRE(fs::exists(tmp / "stale-deadbeef" / "marker.txt"));

    fcc::CacheEntry e;
    e.name = "stale-deadbeef";
    e.path = tmp / "stale-deadbeef";
    e.status = fcc::CacheStatus::StaleCommit;
    e.fixable = true;

    auto results = fcc::apply_fixes({e}, /*dry_run=*/false);
    REQUIRE(results.size() == 1);
    CHECK(results[0].outcome == fcc::FixOutcome::Removed);
    CHECK_FALSE(fs::exists(e.path));
    fs::remove_all(tmp, ec);
}

// ── parse_declared_refs_from_text: handles the real CMakeLists shape ───────

TEST_CASE("parse_declared_refs_from_text: extracts name → REF map",
          "[fetchcontent_cache][issue-744]") {
    const char* cmake = R"(
        # comments allowed
        pulp_register_fetchcontent_source(choc REF f0f5cdf5a938b8b779fea6c083571cce5ccab925)
        pulp_register_fetchcontent_source(SDL3 DIR sdl3 REF release-3.2.12)
        pulp_register_fetchcontent_source(threejs
            REF 077dd13c0e869d9f3dbe55875686f920367de457)
        # not real:
        # pulp_register_fetchcontent_source(commented REF abc)
    )";
    auto refs = fcc::parse_declared_refs_from_text(cmake);
    CHECK(refs.size() == 3);
    CHECK(refs.at("choc") == "f0f5cdf5a938b8b779fea6c083571cce5ccab925");
    CHECK(refs.at("sdl3") == "release-3.2.12");
    CHECK(refs.at("threejs") == "077dd13c0e869d9f3dbe55875686f920367de457");
    // Comments are scrubbed line-by-line before the regex pass, so
    // commented-out calls do NOT pollute the declared-refs map. This
    // is what protects stale-commit detection from false positives on
    // example/documentation snippets in CMakeLists.
    CHECK(refs.count("commented") == 0);
}

// ── Multi-hyphen dep names split correctly ─────────────────────────────────

TEST_CASE("split_entry_name: longest-match prefers declared dep names",
          "[fetchcontent_cache][issue-744]") {
    fs::path root = "/fake/cache";
    fs::path entry = root / "wgpu-macos-aarch64-release-v24.0.3.1";
    MockState state;
    state.children = {entry};
    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = false;
    info.is_directory = true;
    info.is_user_writable = true;
    state.lstat_results[entry] = info;

    fcc::DeclaredRefs refs{
        {"wgpu-macos-aarch64-release", "v24.0.3.1"}};
    auto env = make_mock_env(root, refs, state);
    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].dep_name == "wgpu-macos-aarch64-release");
    CHECK(entries[0].cached_ref == "v24.0.3.1");
    CHECK(entries[0].status == fcc::CacheStatus::Healthy);
}

// ── FetchContent scratch dirs (`<dep>-src`/`-build`/`-subbuild`) skipped ──

TEST_CASE("discover: FetchContent scratch dirs do not register as stale-commit",
          "[fetchcontent_cache][issue-744]") {
    // CMake's FetchContent always creates `<dep>-src`, `<dep>-build`,
    // and `<dep>-subbuild` next to the populated source dir we ourselves
    // place via pulp_register_fetchcontent_source. They are CMake's
    // working state, not entries we own — they must not trigger the
    // stale-commit warning even though their final segment ("src",
    // "build", "subbuild") obviously won't match the declared REF.
    fs::path root = "/fake/cache";
    fs::path scratch_src     = root / "catch2-src";
    fs::path scratch_build   = root / "catch2-build";
    fs::path scratch_subdir  = root / "catch2-subbuild";
    fs::path real_entry      = root / "catch2-v3.7.1";

    MockState state;
    state.children = {scratch_build, scratch_src, scratch_subdir, real_entry};
    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = false;
    info.is_directory = true;
    info.is_user_writable = true;
    state.lstat_results[scratch_src] = info;
    state.lstat_results[scratch_build] = info;
    state.lstat_results[scratch_subdir] = info;
    state.lstat_results[real_entry] = info;

    fcc::DeclaredRefs refs{{"catch2", "v3.7.1"}};
    auto env = make_mock_env(root, refs, state);
    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 4);
    for (const auto& e : entries) {
        INFO("entry: " << e.name << " status: " << fcc::status_label(e.status));
        CHECK(e.status == fcc::CacheStatus::Healthy);
    }
    CHECK_FALSE(fcc::any_unhealthy(entries));
}

// ── Codex P1 (PR #753): stale-ref entries must NOT block preflight ─────────
//
// CMake's pulp_register_fetchcontent_source override path keys on the
// CURRENT sanitized ref (`<dep>-<ref>`), so leftover `<dep>-<oldref>`
// directories are normally harmless — configure either ignores them or
// refetches. Hard-failing `pulp build`/`pulp test` on every stale entry
// would block every developer with an older cache after any pin bump.
//
// The contract is:
//   - any_unhealthy()       → still true (StaleCommit warrants reporting)
//   - blocks_preflight()    → false (StaleCommit alone never gates build)
//   - render_preflight()    → returns 0, emits nothing for stale-only state
//   - apply_fixes()         → still removes stale entries on `--fix`

TEST_CASE("preflight: stale-ref-only state does NOT block build/test",
          "[fetchcontent_cache][issue-744][pr-753]") {
    fs::path root = "/fake/cache";
    fs::path entry = root / "choc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    MockState state;
    state.children = {entry};

    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = false;
    info.is_directory = true;
    info.is_user_writable = true;
    state.lstat_results[entry] = info;

    fcc::DeclaredRefs refs{
        {"choc", "f0f5cdf5a938b8b779fea6c083571cce5ccab925"}};
    auto env = make_mock_env(root, refs, state);

    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].status == fcc::CacheStatus::StaleCommit);

    // The doctor report still flags it (any_unhealthy stays true so
    // `pulp doctor --caches` exits non-zero and `--fix` cleans it).
    CHECK(fcc::any_unhealthy(entries));

    // But preflight must NOT block: this is the bug Codex flagged.
    CHECK_FALSE(fcc::blocks_preflight(entries));

    // render_preflight: silent + exit 0 when only stale entries exist.
    std::stringstream out;
    int rc = fcc::render_preflight(entries, root, out);
    CHECK(rc == 0);
    CHECK(out.str().empty());
}

TEST_CASE("preflight: dangling symlinks DO block build/test",
          "[fetchcontent_cache][issue-744][pr-753]") {
    // Counter-test: confirm we still gate on truly broken states.
    fs::path root = "/fake/cache";
    fs::path entry = root / "threejs-077dd13c0e869d9f3dbe55875686f920367de457";
    MockState state;
    state.children = {entry};

    fcc::StatInfo info;
    info.exists = true;
    info.is_symlink = true;
    info.symlink_target = "/Users/me/Code/three.js";
    info.is_user_writable = true;
    state.lstat_results[entry] = info;

    fcc::DeclaredRefs refs{
        {"threejs", "077dd13c0e869d9f3dbe55875686f920367de457"}};
    auto env = make_mock_env(root, refs, state);

    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 1);
    REQUIRE(entries[0].status == fcc::CacheStatus::Dangling);
    CHECK(fcc::blocks_preflight(entries));

    std::stringstream out;
    int rc = fcc::render_preflight(entries, root, out);
    CHECK(rc == 1);
    CHECK_FALSE(out.str().empty());
}

TEST_CASE("preflight: mixed stale + dangling reports only the dangling entry",
          "[fetchcontent_cache][issue-744][pr-753]") {
    // When both stale and blocking states are present, preflight blocks
    // (because of the dangling), but the rendered report should only
    // mention the truly blocking entries — listing stale entries here
    // would mislead the user into thinking the build was blocked by them.
    fs::path root = "/fake/cache";
    fs::path stale    = root / "choc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    fs::path dangling = root / "threejs-077dd13c0e869d9f3dbe55875686f920367de457";
    MockState state;
    state.children = {stale, dangling};

    fcc::StatInfo stale_info;
    stale_info.exists = true;
    stale_info.is_symlink = false;
    stale_info.is_directory = true;
    stale_info.is_user_writable = true;
    state.lstat_results[stale] = stale_info;

    fcc::StatInfo dangling_info;
    dangling_info.exists = true;
    dangling_info.is_symlink = true;
    dangling_info.symlink_target = "/Users/me/Code/three.js";
    dangling_info.is_user_writable = true;
    state.lstat_results[dangling] = dangling_info;

    fcc::DeclaredRefs refs{
        {"choc",    "f0f5cdf5a938b8b779fea6c083571cce5ccab925"},
        {"threejs", "077dd13c0e869d9f3dbe55875686f920367de457"}};
    auto env = make_mock_env(root, refs, state);

    auto entries = fcc::discover_fetchcontent_cache(env);
    REQUIRE(entries.size() == 2);
    CHECK(fcc::blocks_preflight(entries));

    std::stringstream out;
    int rc = fcc::render_preflight(entries, root, out);
    CHECK(rc == 1);
    auto s = out.str();
    CHECK(s.find("threejs") != std::string::npos);
    CHECK(s.find("choc-aaaaa") == std::string::npos);
}

// ── Codex P2 (PR #753): --caches --json exit code reflects health ──────────
//
// `render_report_json` itself always returns 0, but the cmd_doctor
// caller is responsible for setting the exit code via any_unhealthy().
// The tests below exercise the underlying contract render_report_json
// satisfies (data surface always succeeds) and confirm that
// any_unhealthy() — which the JSON path now calls — returns the right
// value for the unhealthy fixture.

TEST_CASE("render_report_json: data surface always returns 0",
          "[fetchcontent_cache][issue-744][pr-753]") {
    fs::path root = "/fake/cache";
    fs::path stale = root / "choc-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    MockState state;
    state.children = {stale};
    fcc::StatInfo info;
    info.exists = true;
    info.is_directory = true;
    info.is_user_writable = true;
    state.lstat_results[stale] = info;

    fcc::DeclaredRefs refs{
        {"choc", "f0f5cdf5a938b8b779fea6c083571cce5ccab925"}};
    auto env = make_mock_env(root, refs, state);
    auto entries = fcc::discover_fetchcontent_cache(env);

    // The function itself is a pure data surface — exit 0 always.
    std::stringstream out;
    CHECK(fcc::render_report_json(entries, root, out) == 0);

    // But any_unhealthy() — which cmd_doctor now uses for the JSON
    // exit code — correctly reports the unhealthy fixture as non-zero.
    CHECK(fcc::any_unhealthy(entries));

    // And the JSON body itself reflects the same health verdict.
    CHECK(out.str().find("\"healthy\": false") != std::string::npos);
}

TEST_CASE("render_report_json: healthy fixture reports healthy=true",
          "[fetchcontent_cache][issue-744][pr-753]") {
    fs::path root = "/fake/cache";
    fs::path entry = root / "choc-f0f5cdf5a938b8b779fea6c083571cce5ccab925";
    MockState state;
    state.children = {entry};
    fcc::StatInfo info;
    info.exists = true;
    info.is_directory = true;
    info.is_user_writable = true;
    state.lstat_results[entry] = info;

    fcc::DeclaredRefs refs{
        {"choc", "f0f5cdf5a938b8b779fea6c083571cce5ccab925"}};
    auto env = make_mock_env(root, refs, state);
    auto entries = fcc::discover_fetchcontent_cache(env);

    std::stringstream out;
    CHECK(fcc::render_report_json(entries, root, out) == 0);
    CHECK_FALSE(fcc::any_unhealthy(entries));
    CHECK(out.str().find("\"healthy\": true") != std::string::npos);
}

TEST_CASE("render_report_json: escapes control characters in entry fields",
          "[fetchcontent_cache][issue-643]") {
    std::string weird = std::string("dep\n\t") + char(0x01) + "\"\\suffix";

    fcc::CacheEntry e;
    e.name = weird;
    e.path = fs::path("/fake/cache") / weird;
    e.status = fcc::CacheStatus::Dangling;
    e.is_symlink = true;
    e.resolved_target = fs::path("/tmp") / weird;
    e.declared_ref = weird;
    e.cached_ref = weird;
    e.dep_name = weird;
    e.reason = weird;
    e.remediation = weird;
    e.fixable = true;

    std::stringstream out;
    CHECK(fcc::render_report_json({e}, fs::path("/fake/cache") / weird, out) == 0);
    auto s = out.str();

    CHECK(s.find(json_field_fragment("cache_root",
                                     (fs::path("/fake/cache") / weird).string()))
          != std::string::npos);
    CHECK(s.find("\"name\": \"dep\\n\\t\\u0001\\\"\\\\suffix\"")
          != std::string::npos);
    CHECK(s.find(json_field_fragment("path", e.path.string()))
          != std::string::npos);
    CHECK(s.find(json_field_fragment("resolved_target",
                                     e.resolved_target.string()))
          != std::string::npos);
    CHECK(s.find("\"declared_ref\": \"dep\\n\\t\\u0001\\\"\\\\suffix\"")
          != std::string::npos);
    CHECK(s.find("\"cached_ref\": \"dep\\n\\t\\u0001\\\"\\\\suffix\"")
          != std::string::npos);
    CHECK(s.find("\"dep_name\": \"dep\\n\\t\\u0001\\\"\\\\suffix\"")
          != std::string::npos);
    CHECK(s.find("\"reason\": \"dep\\n\\t\\u0001\\\"\\\\suffix\"")
          != std::string::npos);
    CHECK(s.find("\"remediation\": \"dep\\n\\t\\u0001\\\"\\\\suffix\"")
          != std::string::npos);
}

// ── default_cache_root respects PULP_SHARED_FETCHCONTENT_SOURCE_DIR ────────

TEST_CASE("default_cache_root: PULP_SHARED_FETCHCONTENT_SOURCE_DIR override",
          "[fetchcontent_cache][issue-744]") {
#ifdef _WIN32
    _putenv_s("PULP_SHARED_FETCHCONTENT_SOURCE_DIR", "/tmp/pulp-fcc-744-override");
#else
    setenv("PULP_SHARED_FETCHCONTENT_SOURCE_DIR",
           "/tmp/pulp-fcc-744-override", 1);
#endif
    auto root = fcc::default_cache_root();
    CHECK(root == fs::path("/tmp/pulp-fcc-744-override"));
#ifdef _WIN32
    _putenv_s("PULP_SHARED_FETCHCONTENT_SOURCE_DIR", "");
#else
    unsetenv("PULP_SHARED_FETCHCONTENT_SOURCE_DIR");
#endif
}
