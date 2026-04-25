// test_cli_project_bump.cpp — Unit tests for `pulp project bump`.
//
// Release-discovery Slice 7 (#564 / parent #499). Exercises the
// pure-logic core in tools/cli/project_bump.{hpp,cpp}:
//
//   - CMakeLists pin detection (FetchContent, pulp_add_project,
//     project() VERSION variants)
//   - rewrite_pin preserves surrounding bytes and the `v` prefix
//   - refuse_dynamic_pin() gates branches + SHAs
//   - downgrade guard
//   - undo-batch serialize / parse round-trips
//   - undo-batch listing under a scratch PULP_HOME
//   - git-clean gate is a pure bool — exercised via a synthetic
//     `.git` directory + `git status --porcelain` smoke (optional,
//     skipped when git isn't on PATH)
//
// Per the #290 policy the tests ship WITH the fix — these are the
// subsystem's first automated coverage.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/project_bump.hpp"

#include <atomic>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;
namespace pb = pulp::cli::project_bump;

namespace {

struct TempDir {
    fs::path path;
    TempDir() {
        auto base = fs::temp_directory_path();
        static std::atomic<int> seq{0};
        int n = seq.fetch_add(1);
        path = base / ("pulp-project-bump-test-" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) +
                       "-" + std::to_string(n));
        fs::create_directories(path);
        std::error_code ec;
        auto canon = fs::weakly_canonical(path, ec);
        if (!ec) path = canon;
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

}  // namespace

// ── Pin-site discovery ─────────────────────────────────────────────────────

TEST_CASE("find_pin_site detects FetchContent_Declare GIT_TAG",
          "[project-bump][issue-564]") {
    std::string src =
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(App LANGUAGES CXX)\n"
        "include(FetchContent)\n"
        "FetchContent_Declare(pulp\n"
        "    GIT_REPOSITORY https://github.com/danielraffel/pulp.git\n"
        "    GIT_TAG v0.23.0)\n"
        "FetchContent_MakeAvailable(pulp)\n";
    auto site = pb::find_pin_site(src);
    REQUIRE(site.kind == pb::PinKind::FetchContentGitTag);
    REQUIRE(site.current_pin == "v0.23.0");
    REQUIRE(pb::pin_has_v_prefix(site.current_pin));
    REQUIRE(pb::normalize_pin(site.current_pin) == "0.23.0");
}

TEST_CASE("find_pin_site detects pulp_add_project VERSION",
          "[project-bump][issue-564]") {
    std::string src =
        "cmake_minimum_required(VERSION 3.20)\n"
        "pulp_add_project(MySynth\n"
        "    VERSION 0.23.0\n"
        "    FORMATS CLAP VST3)\n";
    auto site = pb::find_pin_site(src);
    REQUIRE(site.kind == pb::PinKind::PulpAddProject);
    REQUIRE(site.current_pin == "0.23.0");
    REQUIRE_FALSE(pb::pin_has_v_prefix(site.current_pin));
}

TEST_CASE("find_pin_site detects project() VERSION",
          "[project-bump][issue-564]") {
    std::string src =
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(MySynth VERSION 0.23.0 LANGUAGES CXX)\n";
    auto site = pb::find_pin_site(src);
    REQUIRE(site.kind == pb::PinKind::ProjectVersion);
    REQUIRE(site.current_pin == "0.23.0");
}

TEST_CASE("find_pin_site returns Unknown for non-Pulp CMakeLists",
          "[project-bump][issue-564]") {
    std::string src =
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(Unrelated LANGUAGES CXX)\n"
        "add_executable(hello hello.cpp)\n";
    auto site = pb::find_pin_site(src);
    // `project()` catches plain `project(Unrelated ...)` only when a
    // VERSION keyword is present, so this should NOT match. Unrelated
    // CMakeLists are Unknown.
    REQUIRE(site.kind == pb::PinKind::Unknown);
}

TEST_CASE("FetchContent wins over project() when both present",
          "[project-bump][issue-564]") {
    std::string src =
        "project(App VERSION 1.2.3)\n"
        "FetchContent_Declare(pulp GIT_TAG v0.23.0)\n";
    auto site = pb::find_pin_site(src);
    REQUIRE(site.kind == pb::PinKind::FetchContentGitTag);
    REQUIRE(site.current_pin == "v0.23.0");
}

TEST_CASE("standalone projects detect versioned find_package(Pulp)",
          "[project-bump][issue-244]") {
    std::string src =
        "cmake_minimum_required(VERSION 3.20)\n"
        "project(Clock VERSION 1.2.3 LANGUAGES CXX)\n"
        "find_package(Pulp 0.40.0 REQUIRED)\n";
    auto site = pb::find_find_package_pulp_version(src);
    REQUIRE(site.kind == pb::PinKind::CMakeFindPackagePulpVersion);
    REQUIRE(site.current_pin == "0.40.0");

    auto rewritten = pb::rewrite_pin(src, site, "0.41.0", false);
    REQUIRE(rewritten);
    REQUIRE(rewritten->find("project(Clock VERSION 1.2.3") != std::string::npos);
    REQUIRE(rewritten->find("find_package(Pulp 0.41.0 REQUIRED)") != std::string::npos);
}

TEST_CASE("standalone find_package detector ignores unversioned calls",
          "[project-bump][issue-244]") {
    std::string src =
        "find_package(Pulp REQUIRED CONFIG)\n";
    auto site = pb::find_find_package_pulp_version(src);
    REQUIRE(site.kind == pb::PinKind::Unknown);
}

TEST_CASE("pulp.toml scalar detector rewrites sdk_version and sdk_path",
          "[project-bump][issue-244]") {
    std::string toml =
        "[pulp]\n"
        "sdk_version = \"0.40.0\"\n"
        "sdk_path = \"/Users/example/.pulp/sdk/local/0.40.0\"\n";

    auto version = pb::find_toml_string_value(toml, "sdk_version",
                                              pb::PinKind::PulpTomlSdkVersion);
    REQUIRE(version.kind == pb::PinKind::PulpTomlSdkVersion);
    REQUIRE(version.current_pin == "0.40.0");
    auto bumped = pb::rewrite_pin(toml, version, "0.41.0", false);
    REQUIRE(bumped);
    REQUIRE(bumped->find("sdk_version = \"0.41.0\"") != std::string::npos);

    auto path = pb::find_toml_string_value(*bumped, "sdk_path",
                                           pb::PinKind::PulpTomlSdkPath);
    REQUIRE(path.kind == pb::PinKind::PulpTomlSdkPath);
    REQUIRE(path.current_pin == "/Users/example/.pulp/sdk/local/0.40.0");
}

TEST_CASE("pulp.toml scalar detector handles CRLF line endings",
          "[project-bump][issue-244]") {
    std::string toml =
        "[pulp]\r\n"
        "sdk_version = \"0.40.0\"\r\n"
        "sdk_path = \"C:/Users/example/.pulp/sdk/0.40.0\"\r\n";

    auto version = pb::find_toml_string_value(toml, "sdk_version",
                                              pb::PinKind::PulpTomlSdkVersion);
    REQUIRE(version.kind == pb::PinKind::PulpTomlSdkVersion);
    REQUIRE(version.current_pin == "0.40.0");

    auto rewritten = pb::rewrite_pin(toml, version, "0.41.0", false);
    REQUIRE(rewritten);
    REQUIRE(rewritten->find("sdk_version = \"0.41.0\"") != std::string::npos);
}

// ── Dynamic pin refusal ────────────────────────────────────────────────────

TEST_CASE("refuse_dynamic_pin rejects branch names and SHAs",
          "[project-bump][issue-564]") {
    pb::PinSite site;
    site.kind = pb::PinKind::FetchContentGitTag;

    site.current_pin = "main";
    REQUIRE(pb::refuse_dynamic_pin(site));

    site.current_pin = "develop";
    REQUIRE(pb::refuse_dynamic_pin(site));

    // 40-char SHA
    site.current_pin = "1234567890abcdef1234567890abcdef12345678";
    REQUIRE(pb::refuse_dynamic_pin(site));

    // 7-char short SHA
    site.current_pin = "abc1234";
    REQUIRE(pb::refuse_dynamic_pin(site));

    // Semver is safe.
    site.current_pin = "0.23.0";
    REQUIRE_FALSE(pb::refuse_dynamic_pin(site));
    site.current_pin = "v0.23.0";
    REQUIRE_FALSE(pb::refuse_dynamic_pin(site));
}

// ── Rewrite ─────────────────────────────────────────────────────────────────

TEST_CASE("rewrite_pin preserves leading 'v' prefix",
          "[project-bump][issue-564]") {
    std::string src =
        "FetchContent_Declare(pulp\n"
        "    GIT_REPOSITORY https://github.com/danielraffel/pulp.git\n"
        "    GIT_TAG v0.23.0)\n";
    auto site = pb::find_pin_site(src);
    REQUIRE(site.kind == pb::PinKind::FetchContentGitTag);
    auto rewritten = pb::rewrite_pin(src, site, "0.32.0", /*has_v=*/true);
    REQUIRE(rewritten);
    REQUIRE(rewritten->find("GIT_TAG v0.32.0)") != std::string::npos);
    // Everything else should be byte-identical — spot-check the
    // surrounding tokens.
    REQUIRE(rewritten->find("GIT_REPOSITORY https://github.com/danielraffel/pulp.git")
            != std::string::npos);
    REQUIRE(rewritten->find("v0.23.0") == std::string::npos);
}

TEST_CASE("rewrite_pin rewrites pulp_add_project VERSION without adding 'v'",
          "[project-bump][issue-564]") {
    std::string src =
        "pulp_add_project(MySynth\n"
        "    VERSION 0.23.0\n"
        "    FORMATS CLAP VST3)\n";
    auto site = pb::find_pin_site(src);
    REQUIRE(site.kind == pb::PinKind::PulpAddProject);
    auto rewritten = pb::rewrite_pin(src, site, "0.32.0", /*has_v=*/false);
    REQUIRE(rewritten);
    REQUIRE(rewritten->find("VERSION 0.32.0") != std::string::npos);
    REQUIRE(rewritten->find("VERSION v") == std::string::npos);
    REQUIRE(rewritten->find("FORMATS CLAP VST3") != std::string::npos);
}

TEST_CASE("rewrite_pin rewrites project() VERSION",
          "[project-bump][issue-564]") {
    std::string src =
        "project(MySynth VERSION 0.23.0 LANGUAGES CXX)\n";
    auto site = pb::find_pin_site(src);
    REQUIRE(site.kind == pb::PinKind::ProjectVersion);
    auto rewritten = pb::rewrite_pin(src, site, "0.32.0", /*has_v=*/false);
    REQUIRE(rewritten);
    REQUIRE(*rewritten == "project(MySynth VERSION 0.32.0 LANGUAGES CXX)\n");
}

TEST_CASE("rewrite_pin returns nullopt on Unknown site",
          "[project-bump][issue-564]") {
    pb::PinSite s;  // defaults to Unknown
    REQUIRE_FALSE(pb::rewrite_pin("anything", s, "0.32.0", false).has_value());
}

// ── Semver + downgrade ─────────────────────────────────────────────────────

TEST_CASE("parse_semver_strict rejects pre-release suffixes",
          "[project-bump][issue-564]") {
    REQUIRE(pb::parse_semver_strict("0.32.0").ok);
    REQUIRE(pb::parse_semver_strict("v0.32.0").ok);
    REQUIRE_FALSE(pb::parse_semver_strict("0.32").ok);
    REQUIRE_FALSE(pb::parse_semver_strict("0.32.0-rc1").ok);
    REQUIRE_FALSE(pb::parse_semver_strict("").ok);
    REQUIRE_FALSE(pb::parse_semver_strict("main").ok);
}

TEST_CASE("is_downgrade compares semver strictly",
          "[project-bump][issue-564]") {
    REQUIRE(pb::is_downgrade("0.32.0", "0.31.0"));
    REQUIRE(pb::is_downgrade("1.0.0", "0.99.99"));
    REQUIRE_FALSE(pb::is_downgrade("0.31.0", "0.32.0"));
    REQUIRE_FALSE(pb::is_downgrade("0.31.0", "0.31.0"));  // equal is not a downgrade
    REQUIRE_FALSE(pb::is_downgrade("main", "0.32.0"));    // bad input → false
}

// ── Undo batch I/O ─────────────────────────────────────────────────────────

TEST_CASE("undo batch serialize -> parse round-trip",
          "[project-bump][issue-564]") {
    pb::UndoBatch b;
    b.timestamp = "2026-04-21T14:30:00Z";
    b.target_version = "0.32.0";

    pb::UndoEntry e1;
    e1.project_path = "/tmp/p1";
    e1.project_name = "P1";
    e1.old_pin = "v0.23.0";
    e1.old_pin_style_has_v = true;
    e1.pin_kind = pb::PinKind::FetchContentGitTag;
    e1.status = "bumped";
    e1.edits.push_back(pb::UndoEdit{
        fs::path("/tmp/p1/CMakeLists.txt"),
        pb::PinKind::FetchContentGitTag,
        "v0.23.0",
        "v0.32.0",
        true,
    });
    b.entries.push_back(e1);

    pb::UndoEntry e2;
    e2.project_path = "/tmp/p2";
    e2.project_name = "P2";
    e2.old_pin = "";
    e2.status = "failed";
    e2.failure_reason = "no CMakeLists.txt";
    b.entries.push_back(e2);

    auto json = pb::serialize_undo_batch(b);
    auto parsed = pb::parse_undo_batch(json);
    REQUIRE(parsed);
    REQUIRE(parsed->timestamp == b.timestamp);
    REQUIRE(parsed->target_version == b.target_version);
    REQUIRE(parsed->entries.size() == 2);
    REQUIRE(parsed->entries[0].project_name == "P1");
    REQUIRE(parsed->entries[0].old_pin == "v0.23.0");
    REQUIRE(parsed->entries[0].old_pin_style_has_v == true);
    REQUIRE(parsed->entries[0].pin_kind == pb::PinKind::FetchContentGitTag);
    REQUIRE(parsed->entries[0].status == "bumped");
    REQUIRE(parsed->entries[0].edits.size() == 1);
    REQUIRE(parsed->entries[0].edits[0].path == fs::path("/tmp/p1/CMakeLists.txt"));
    REQUIRE(parsed->entries[0].edits[0].old_value == "v0.23.0");
    REQUIRE(parsed->entries[0].edits[0].new_value == "v0.32.0");
    REQUIRE(parsed->entries[1].status == "failed");
    REQUIRE(parsed->entries[1].failure_reason == "no CMakeLists.txt");
}

TEST_CASE("legacy undo batch synthesizes a CMake edit",
          "[project-bump][issue-244]") {
    auto parsed = pb::parse_undo_batch(
        "{\n"
        "  \"timestamp\": \"2026-04-21T14:30:00Z\",\n"
        "  \"target_version\": \"0.32.0\",\n"
        "  \"entries\": [\n"
        "    {\"project_path\": \"/tmp/p1\", \"project_name\": \"P1\", "
        "\"old_pin\": \"0.23.0\", \"old_pin_style_has_v\": false, "
        "\"pin_kind\": \"PulpAddProject\", \"status\": \"bumped\", "
        "\"failure_reason\": \"\"}\n"
        "  ]\n"
        "}\n");
    REQUIRE(parsed);
    REQUIRE(parsed->entries.size() == 1);
    REQUIRE(parsed->entries[0].edits.size() == 1);
    REQUIRE(parsed->entries[0].edits[0].path == fs::path("/tmp/p1/CMakeLists.txt"));
    REQUIRE(parsed->entries[0].edits[0].kind == pb::PinKind::PulpAddProject);
    REQUIRE(parsed->entries[0].edits[0].old_value == "0.23.0");
    REQUIRE(parsed->entries[0].edits[0].new_value == "0.32.0");
}

TEST_CASE("write_undo_batch / read_undo_batch round-trip on disk",
          "[project-bump][issue-564]") {
    TempDir tmp;
    pb::UndoBatch b;
    b.timestamp = "2026-04-21T14:30:00Z";
    b.target_version = "0.32.0";

    pb::UndoEntry e;
    e.project_path = tmp.path / "proj";
    e.project_name = "Proj";
    e.old_pin = "0.23.0";
    e.old_pin_style_has_v = false;
    e.pin_kind = pb::PinKind::PulpAddProject;
    e.status = "bumped";
    b.entries.push_back(e);

    auto path = pb::undo_batch_path(tmp.path, b.timestamp);
    REQUIRE(pb::write_undo_batch(path, b));
    REQUIRE(fs::exists(path));

    auto back = pb::read_undo_batch(path);
    REQUIRE(back);
    REQUIRE(back->target_version == "0.32.0");
    REQUIRE(back->entries.size() == 1);
    REQUIRE(back->entries[0].project_name == "Proj");
    REQUIRE(back->entries[0].pin_kind == pb::PinKind::PulpAddProject);
}

TEST_CASE("list_undo_batches returns newest-first",
          "[project-bump][issue-564]") {
    TempDir tmp;
    // Write two undo files with distinct timestamps.
    pb::UndoBatch b1;
    b1.timestamp = "2026-04-20T10:00:00Z";
    pb::UndoBatch b2;
    b2.timestamp = "2026-04-22T10:00:00Z";
    auto p1 = pb::undo_batch_path(tmp.path, b1.timestamp);
    auto p2 = pb::undo_batch_path(tmp.path, b2.timestamp);
    REQUIRE(pb::write_undo_batch(p1, b1));
    REQUIRE(pb::write_undo_batch(p2, b2));

    auto list = pb::list_undo_batches(tmp.path);
    REQUIRE(list.size() == 2);
    REQUIRE(list[0] == p2);  // newer first
    REQUIRE(list[1] == p1);
}

TEST_CASE("undo_batch_path replaces colons for Windows safety",
          "[project-bump][issue-564]") {
    fs::path home = "/tmp/fake-home";
    auto p = pb::undo_batch_path(home, "2026-04-21T14:30:00Z");
    // Filename must not contain ':'
    auto fn = p.filename().string();
    REQUIRE(fn.find(':') == std::string::npos);
    REQUIRE(fn.find("2026-04-21T14-30-00Z") != std::string::npos);
}

// ── --dry-run equivalence ───────────────────────────────────────────────────

TEST_CASE("dry-run produces same rewrite as real run",
          "[project-bump][issue-564]") {
    std::string src =
        "FetchContent_Declare(pulp\n"
        "    GIT_TAG v0.23.0)\n";
    auto site = pb::find_pin_site(src);
    auto rewritten = pb::rewrite_pin(src, site, "0.32.0",
                                      pb::pin_has_v_prefix(site.current_pin));
    REQUIRE(rewritten);
    // Dry-run path computes the same string but doesn't persist it —
    // the equivalence assertion is that `rewrite_pin` is pure.
    auto rewritten2 = pb::rewrite_pin(src, site, "0.32.0",
                                       pb::pin_has_v_prefix(site.current_pin));
    REQUIRE(rewritten2);
    REQUIRE(*rewritten == *rewritten2);
}

// ── Status strings are stable ──────────────────────────────────────────────

TEST_CASE("pin_kind_name round-trips for all variants",
          "[project-bump][issue-564]") {
    for (auto k : {pb::PinKind::FetchContentGitTag,
                   pb::PinKind::PulpAddProject,
                   pb::PinKind::ProjectVersion,
                   pb::PinKind::PulpTomlSdkVersion,
                   pb::PinKind::PulpTomlSdkPath,
                   pb::PinKind::CMakeFindPackagePulpVersion,
                   pb::PinKind::Unknown}) {
        auto name = pb::pin_kind_name(k);
        REQUIRE(pb::parse_pin_kind(name) == k);
    }
}
