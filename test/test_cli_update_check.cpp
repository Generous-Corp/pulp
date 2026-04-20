// Release-discovery Slice 2 (#547) — unit tests for the update-check core.
//
// Covers:
//   - Cache JSON round-trip
//   - Cache read/write against a tmp dir
//   - is_cache_stale across intervals
//   - Semver parsing / is_newer
//   - Banner composition (exact string)
//   - TOML key writer: replace-in-place, append-to-section,
//     create-missing-section, comment tolerance
//   - Fetcher injection: refresh_cache with a FakeFetcher never
//     touches the network, carries forward previous values on failure

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/update_check.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#  include <process.h>
#  define pulp_getpid() _getpid()
#else
#  include <unistd.h>
#  define pulp_getpid() ::getpid()
#endif

namespace uc = pulp::cli::update_check;
namespace fs = std::filesystem;

namespace {

struct FakeFetcher : uc::Fetcher {
    uc::FetchResult canned;
    int call_count = 0;
    std::string last_owner_repo;

    uc::FetchResult fetch_latest_release(const std::string& owner_repo) override {
        ++call_count;
        last_owner_repo = owner_repo;
        return canned;
    }
};

fs::path make_tmpdir(const std::string& tag) {
    auto base = fs::temp_directory_path() /
                ("pulp-test-update-check-" + tag + "-" +
                 std::to_string(pulp_getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::error_code ec;
    fs::create_directories(base, ec);
    return base;
}

}  // namespace

// ── Cache JSON round-trip ───────────────────────────────────────────────────

TEST_CASE("update-cache JSON round-trips through parse/serialize",
          "[cli][update-check][issue-547]") {
    uc::CacheEntry e;
    e.last_check_epoch_sec = 1'713'638'400;
    e.latest_version = "0.28.0";
    e.release_notes_url = "https://github.com/danielraffel/pulp/releases/tag/v0.28.0";
    e.banner_shown_for_version = "0.27.0";

    auto s = uc::serialize_cache_json(e);
    auto e2 = uc::parse_cache_json(s);

    REQUIRE(e2.schema == uc::kCacheSchemaVersion);
    REQUIRE(e2.last_check_epoch_sec == e.last_check_epoch_sec);
    REQUIRE(e2.latest_version == e.latest_version);
    REQUIRE(e2.release_notes_url == e.release_notes_url);
    REQUIRE(e2.banner_shown_for_version == e.banner_shown_for_version);
}

TEST_CASE("parse_cache_json tolerates missing/unknown fields",
          "[cli][update-check][issue-547]") {
    auto a = uc::parse_cache_json("{}");
    REQUIRE(a.last_check_epoch_sec == 0);
    REQUIRE(a.latest_version.empty());

    // Forward-compat: future schema with extra fields must not reject.
    auto b = uc::parse_cache_json(R"({
        "schema": 2,
        "last_check_epoch_sec": 42,
        "latest_version": "1.2.3",
        "future_field_we_dont_know": "ok"
    })");
    REQUIRE(b.schema == 2);
    REQUIRE(b.last_check_epoch_sec == 42);
    REQUIRE(b.latest_version == "1.2.3");

    // Malformed JSON yields defaults, not an exception.
    auto c = uc::parse_cache_json("not json at all");
    REQUIRE(c.latest_version.empty());
}

TEST_CASE("write_cache_file + read_cache_file atomically round-trip",
          "[cli][update-check][issue-547]") {
    auto dir = make_tmpdir("roundtrip");
    auto path = dir / "update-cache.json";

    uc::CacheEntry e;
    e.last_check_epoch_sec = 99'999;
    e.latest_version = "9.9.9";
    REQUIRE(uc::write_cache_file(path, e));
    REQUIRE(fs::exists(path));

    auto back = uc::read_cache_file(path);
    REQUIRE(back.has_value());
    REQUIRE(back->latest_version == "9.9.9");

    fs::remove_all(dir);
}

TEST_CASE("read_cache_file returns nullopt only for missing files",
          "[cli][update-check][issue-547]") {
    auto dir = make_tmpdir("missing");
    auto path = dir / "nope.json";
    REQUIRE_FALSE(uc::read_cache_file(path).has_value());

    // Malformed → empty entry, not nullopt.
    auto bad = dir / "bad.json";
    std::ofstream(bad) << "}{not valid";
    auto result = uc::read_cache_file(bad);
    REQUIRE(result.has_value());
    REQUIRE(result->latest_version.empty());
    fs::remove_all(dir);
}

// ── Age check ───────────────────────────────────────────────────────────────

TEST_CASE("is_cache_stale: never-checked + interval rules",
          "[cli][update-check][issue-547]") {
    uc::CacheEntry never{};   // last_check = 0
    REQUIRE(uc::is_cache_stale(never, 1'000, 24));

    // Fresh: 1h old under a 24h interval -> not stale
    uc::CacheEntry fresh;
    fresh.last_check_epoch_sec = 1'000'000;
    REQUIRE_FALSE(uc::is_cache_stale(fresh, 1'000'000 + 3600, 24));

    // Exactly at interval boundary -> stale.
    REQUIRE(uc::is_cache_stale(fresh, 1'000'000 + 24 * 3600, 24));

    // Clock skew backwards -> defensively refresh.
    REQUIRE(uc::is_cache_stale(fresh, 500'000, 24));

    // interval_hours == 0 -> disabled (never stale).
    REQUIRE_FALSE(uc::is_cache_stale(never, 1'000, 0));
}

// ── Semver ──────────────────────────────────────────────────────────────────

TEST_CASE("parse_semver handles common shapes", "[cli][update-check][issue-547]") {
    auto a = uc::parse_semver("0.27.0");
    REQUIRE(a.ok);
    REQUIRE(a.major == 0);
    REQUIRE(a.minor == 27);
    REQUIRE(a.patch == 0);

    auto b = uc::parse_semver("v1.2.3");
    REQUIRE(b.ok);
    REQUIRE(b.major == 1);

    auto c = uc::parse_semver("1.2.3-rc.1");
    REQUIRE(c.ok);
    REQUIRE(c.patch == 3);

    auto d = uc::parse_semver("not-a-version");
    REQUIRE_FALSE(d.ok);
}

TEST_CASE("is_newer compares correctly", "[cli][update-check][issue-547]") {
    REQUIRE(uc::is_newer("0.27.0", "0.28.0"));
    REQUIRE(uc::is_newer("0.27.0", "1.0.0"));
    REQUIRE_FALSE(uc::is_newer("0.28.0", "0.28.0"));
    REQUIRE_FALSE(uc::is_newer("0.28.0", "0.27.9"));
    // Unparseable input is never reported newer.
    REQUIRE_FALSE(uc::is_newer("dev-build", "1.0.0"));
    REQUIRE_FALSE(uc::is_newer("1.0.0", "abc"));
}

// ── Banner ──────────────────────────────────────────────────────────────────

TEST_CASE("compose_banner emits the exact Section A single-line shape",
          "[cli][update-check][issue-547]") {
    auto b = uc::compose_banner("0.27.0", "0.28.0");
    // Locked verbatim — any reviewer can eyeball this against the
    // design doc. If you need to change this line, update the test
    // in the same PR.
    REQUIRE(b ==
            "Pulp v0.28.0 available (you have v0.27.0). "
            "Run `pulp upgrade` or `pulp config set update.mode manual` to silence.");
}

// ── TOML key writer ─────────────────────────────────────────────────────────

TEST_CASE("write_toml_key_in_section creates missing section",
          "[cli][update-check][issue-547]") {
    auto out = uc::write_toml_key_in_section("", "update", "mode", "manual");
    REQUIRE(out.find("[update]") != std::string::npos);
    REQUIRE(out.find("mode = \"manual\"") != std::string::npos);
    REQUIRE(uc::read_toml_key_in_section(out, "update", "mode") == "manual");
}

TEST_CASE("write_toml_key_in_section replaces existing key in-place",
          "[cli][update-check][issue-547]") {
    std::string src =
        "[update]\n"
        "mode = \"prompt\"\n"
        "check_interval_hours = \"24\"\n";
    auto out = uc::write_toml_key_in_section(src, "update", "mode", "off");
    REQUIRE(uc::read_toml_key_in_section(out, "update", "mode") == "off");
    REQUIRE(uc::read_toml_key_in_section(out, "update", "check_interval_hours") == "24");
    // No duplicate key line.
    std::size_t count = 0;
    std::string::size_type p = 0;
    while ((p = out.find("mode =", p)) != std::string::npos) { ++count; ++p; }
    REQUIRE(count == 1);
}

TEST_CASE("write_toml_key_in_section appends key to existing section",
          "[cli][update-check][issue-547]") {
    std::string src =
        "[update]\n"
        "mode = \"prompt\"\n"
        "\n"
        "[create]\n"
        "projects_dir = \"~/dev\"\n";
    auto out = uc::write_toml_key_in_section(src, "update", "check_interval_hours", "12");
    REQUIRE(uc::read_toml_key_in_section(out, "update", "check_interval_hours") == "12");
    REQUIRE(uc::read_toml_key_in_section(out, "update", "mode") == "prompt");
    REQUIRE(uc::read_toml_key_in_section(out, "create", "projects_dir") == "~/dev");
}

TEST_CASE("read_toml_key_in_section ignores commented examples",
          "[cli][update-check][issue-547]") {
    std::string src =
        "[update]\n"
        "# mode = \"auto\"   # example\n"
        "mode = \"prompt\"\n";
    REQUIRE(uc::read_toml_key_in_section(src, "update", "mode") == "prompt");
}

// ── Fetcher injection ───────────────────────────────────────────────────────

TEST_CASE("refresh_cache consumes fake fetcher, no network",
          "[cli][update-check][issue-547]") {
    FakeFetcher fetcher;
    fetcher.canned.ok = true;
    fetcher.canned.latest_version = "1.2.3";
    fetcher.canned.release_notes_url = "https://example/tag/v1.2.3";

    uc::CacheEntry prev;
    auto next = uc::refresh_cache(fetcher, prev, "owner/repo", 1'000);

    REQUIRE(fetcher.call_count == 1);
    REQUIRE(fetcher.last_owner_repo == "owner/repo");
    REQUIRE(next.latest_version == "1.2.3");
    REQUIRE(next.release_notes_url == "https://example/tag/v1.2.3");
    REQUIRE(next.last_check_epoch_sec == 1'000);
}

TEST_CASE("refresh_cache carries forward previous on fetch failure",
          "[cli][update-check][issue-547]") {
    FakeFetcher fetcher;
    fetcher.canned.ok = false;
    fetcher.canned.error = "network down";

    uc::CacheEntry prev;
    prev.latest_version = "0.99.0";
    prev.release_notes_url = "https://example/tag/v0.99.0";
    prev.last_check_epoch_sec = 500;

    auto next = uc::refresh_cache(fetcher, prev, "owner/repo", 2'000);
    // Previous latest preserved — don't clobber a known-good value on
    // a transient network failure. But the timestamp advances so we
    // don't hammer the API on every invocation.
    REQUIRE(next.latest_version == "0.99.0");
    REQUIRE(next.release_notes_url == "https://example/tag/v0.99.0");
    REQUIRE(next.last_check_epoch_sec == 2'000);
}

// ── Banner-suppression bookkeeping ──────────────────────────────────────────

TEST_CASE("banner_shown_for_version gates banner reprint",
          "[cli][update-check][issue-547]") {
    // This is a pure-state-machine test. The CLI main loop logic is:
    //   if (latest > installed && banner_shown_for_version != latest) banner();
    // We re-derive that here to pin the semantics: freshly installed
    // user sees the banner once; after cmd_upgrade bumps
    // banner_shown_for_version, the next invocation stays silent.
    uc::CacheEntry cache;
    cache.latest_version = "0.28.0";
    cache.banner_shown_for_version = "";
    auto should_show = [&](const std::string& installed) {
        return uc::is_newer(installed, cache.latest_version) &&
               cache.banner_shown_for_version != cache.latest_version;
    };
    REQUIRE(should_show("0.27.0"));

    // Simulate marking the banner as shown.
    cache.banner_shown_for_version = cache.latest_version;
    REQUIRE_FALSE(should_show("0.27.0"));

    // Simulate a newer release arriving post-refresh.
    cache.latest_version = "0.29.0";
    REQUIRE(should_show("0.27.0"));
}
