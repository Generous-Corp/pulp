// Resolver-only tests for the real-plugin fixture lane logic.
//
// Exercises `pulp::test::real_plugins::resolve_fixture()` under three
// developer scenarios:
//   1. Cache populated — bundle exists in the override cache
//   2. Cache empty     — override cache is set but contains nothing
//   3. Cache partial   — one plugin's bundle exists, another is missing
//
// Each scenario is run for both lanes (pinned + developer-supplied) so
// the resolver's "TBD sha256 still skips when override is absent" guard
// is asserted, and the "TBD sha256 accepts the bundle when override is
// set + bundle present" lane is asserted.
//
// This test is intentionally NOT gated behind PULP_REAL_PLUGIN_TESTS:
// it never loads a real plugin binary. The fixture files written under
// the temp cache are zero-byte placeholders — the resolver only cares
// about existence at the right path, not the contents.

#include <catch2/catch_test_macros.hpp>

#include "real_plugin_fixture.hpp"

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using namespace pulp::test::real_plugins;

namespace {

// Build a synthetic manifest entry in memory so the test doesn't depend on
// the on-disk real_plugins.toml. We populate all three OS slots so the test
// runs unchanged across the platforms Pulp builds on.
PluginEntry make_entry(const std::string& id, const std::string& bundle_relpath,
                       const std::string& sha) {
    PluginEntry e;
    e.id = id;
    e.display_name = id;
    e.format = "vst3";
    e.bundle_relpath = bundle_relpath;

    PluginPlatform p;
    p.url = "https://example.invalid/" + id + ".zip";
    p.sha256 = sha;
    p.archive_kind = "zip";
    e.macos   = p;
    e.linux_  = p;
    e.windows = p;
    return e;
}

// Write a non-empty placeholder for the resolver's shape check
// (Codex PR #3015 P2 added the "empty bundle is not a fixture" guard).
void touch(const fs::path& bundle) {
    fs::create_directories(bundle.parent_path());
    std::ofstream(bundle) << "PULP-FIXTURE-PLACEHOLDER";
}

// Zero-byte file at the bundle path — used by the regression test to
// confirm the resolver SKIPs instead of accepting a malformed fixture.
void touch_empty(const fs::path& bundle) {
    fs::create_directories(bundle.parent_path());
    std::ofstream(bundle) << "";
}

// Empty directory at the bundle path — for the directory-shape side of
// the same shape check.
void touch_empty_dir(const fs::path& bundle) {
    fs::create_directories(bundle);
}

struct TempCache {
    fs::path root;
    explicit TempCache(const std::string& tag) {
        root = fs::temp_directory_path()
             / ("pulp-real-plugin-cache-test-" + tag);
        fs::remove_all(root);
        fs::create_directories(root);
    }
    ~TempCache() { fs::remove_all(root); }
};

} // namespace

TEST_CASE("resolver: pinned lane requires bundle on disk",
          "[host][integration][real-plugins][resolver]") {
    TempCache cache("pinned-missing");
    const auto e = make_entry("pinned", "Pinned.vst3", "deadbeef");

    const auto r = resolve_fixture(e, cache.root);
    REQUIRE(r.source == FixtureSource::Missing);
    REQUIRE_FALSE(r.skip_reason.empty());
}

TEST_CASE("resolver: pinned lane accepts bundle when present",
          "[host][integration][real-plugins][resolver]") {
    TempCache cache("pinned-present");
    const auto e = make_entry("pinned", "Pinned.vst3", "deadbeef");
    touch(cache.root / e.id / e.bundle_relpath);

    const auto r = resolve_fixture(e, cache.root);
    REQUIRE(r.source == FixtureSource::PinnedDownload);
    REQUIRE(r.bundle_path == cache.root / e.id / e.bundle_relpath);
    REQUIRE(r.skip_reason.empty());
}

TEST_CASE("resolver: TBD entry skips when cache override is empty",
          "[host][integration][real-plugins][resolver]") {
    TempCache cache("tbd-empty");
    const auto e = make_entry("vital-like", "Vital.vst3", "TBD");

    const auto r = resolve_fixture(e, cache.root);
    REQUIRE(r.source == FixtureSource::Missing);
    REQUIRE(r.skip_reason.find("developer-supplied") != std::string::npos);
}

TEST_CASE("resolver: TBD entry uses developer-supplied lane when bundle present",
          "[host][integration][real-plugins][resolver]") {
    TempCache cache("tbd-present");
    const auto e = make_entry("vital-like", "Vital.vst3", "TBD");
    touch(cache.root / e.id / e.bundle_relpath);

    const auto r = resolve_fixture(e, cache.root);
    REQUIRE(r.source == FixtureSource::DeveloperSupplied);
    REQUIRE(r.bundle_path == cache.root / e.id / e.bundle_relpath);
    REQUIRE(r.skip_reason.empty());
}

TEST_CASE("resolver: partial cache — only the present plugin resolves",
          "[host][integration][real-plugins][resolver]") {
    TempCache cache("partial");
    const auto present = make_entry("present", "Present.vst3", "TBD");
    const auto missing = make_entry("missing", "Missing.vst3", "TBD");
    touch(cache.root / present.id / present.bundle_relpath);

    const auto r_present = resolve_fixture(present, cache.root);
    REQUIRE(r_present.source == FixtureSource::DeveloperSupplied);

    const auto r_missing = resolve_fixture(missing, cache.root);
    REQUIRE(r_missing.source == FixtureSource::Missing);
    REQUIRE(r_missing.skip_reason.find("developer-supplied bundle expected") != std::string::npos);
}

TEST_CASE("resolver: TBD entry without override never silently passes",
          "[host][integration][real-plugins][resolver]") {
    // No cache_root_override is passed, and we explicitly do not set
    // PULP_REAL_PLUGIN_CACHE in this scenario. The resolver MUST refuse
    // the entry — never silently rely on a stale ~/.cache file. If the
    // env var happens to be set in the developer's shell, we accept that
    // and skip the assertion (we can't safely mutate the environment in
    // a test without risking flakes on parallel runs).
    if (developer_cache_override().has_value()) {
        WARN("PULP_REAL_PLUGIN_CACHE is set in the test environment — "
             "skipping the negative-path assertion.");
        return;
    }
    const auto e = make_entry("vital-like", "Vital.vst3", "TBD");
    const auto r = resolve_fixture(e); // no override
    REQUIRE(r.source == FixtureSource::Missing);
    REQUIRE(r.skip_reason.find("PULP_REAL_PLUGIN_CACHE") != std::string::npos);
}

// Regression: Codex PR #3015 P2. The developer-supplied lane previously
// accepted ANY existing path, including zero-byte placeholders left over
// from a `touch $PULP_REAL_PLUGIN_CACHE/vital/Vital.vst3` mistake.
// `PluginSlot::load` would then hard-fail on the bogus bundle instead of
// the resolver returning a clean SKIP with actionable guidance.
TEST_CASE("resolver: developer-supplied lane skips an empty-file bundle "
          "(regression: PR #3015 review)",
          "[host][integration][real-plugins][resolver][issue-3015]") {
    TempCache cache("tbd-empty-bundle-file");
    const auto e = make_entry("vital-like", "Vital.vst3", "TBD");
    touch_empty(cache.root / e.id / e.bundle_relpath);

    const auto r = resolve_fixture(e, cache.root);
    REQUIRE(r.source == FixtureSource::Missing);
    REQUIRE(r.skip_reason.find("empty") != std::string::npos);
}

TEST_CASE("resolver: developer-supplied lane skips an empty-directory bundle "
          "(regression: PR #3015 review)",
          "[host][integration][real-plugins][resolver][issue-3015]") {
    TempCache cache("tbd-empty-bundle-dir");
    const auto e = make_entry("vital-like", "Vital.vst3", "TBD");
    touch_empty_dir(cache.root / e.id / e.bundle_relpath);

    const auto r = resolve_fixture(e, cache.root);
    REQUIRE(r.source == FixtureSource::Missing);
    REQUIRE(r.skip_reason.find("empty") != std::string::npos);
}

TEST_CASE("resolver: source labels are stable",
          "[host][integration][real-plugins][resolver]") {
    REQUIRE(source_label(FixtureSource::Missing)           == "missing");
    REQUIRE(source_label(FixtureSource::PinnedDownload)    == "pinned-download");
    REQUIRE(source_label(FixtureSource::DeveloperSupplied) == "developer-supplied");
}

TEST_CASE("manifest: file on disk parses without errors",
          "[host][integration][real-plugins][resolver]") {
    // The CMake target injects PULP_REAL_PLUGINS_TOML; if absent (e.g. an
    // out-of-tree build that didn't propagate it), this test is skipped
    // because it would otherwise look for the manifest at the cwd.
#ifdef PULP_REAL_PLUGINS_TOML
    const fs::path cfg = fs::path(PULP_REAL_PLUGINS_TOML);
    REQUIRE(fs::exists(cfg));
    const auto entries = parse_real_plugins_toml(cfg);
    REQUIRE(entries.size() >= 4);

    // Spot-check that the auth-gated plugins still have TBD sha entries on
    // at least one platform — the developer-supplied lane exists precisely
    // because those rows can't be hash-pinned today.
    bool found_tbd_vital = false;
    bool found_tbd_obxd = false;
    for (const auto& e : entries) {
        auto has_tbd = [&](const std::optional<PluginPlatform>& p) {
            return p.has_value() && (p->sha256 == "TBD" || p->sha256.empty());
        };
        if (e.id == "vital" && (has_tbd(e.macos) || has_tbd(e.linux_) || has_tbd(e.windows)))
            found_tbd_vital = true;
        if (e.id == "obxd" && (has_tbd(e.macos) || has_tbd(e.linux_) || has_tbd(e.windows)))
            found_tbd_obxd = true;
    }
    REQUIRE(found_tbd_vital);
    REQUIRE(found_tbd_obxd);
#else
    WARN("PULP_REAL_PLUGINS_TOML not injected; skipping on-disk manifest check.");
#endif
}
