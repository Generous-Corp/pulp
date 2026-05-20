// test_cli_tweaks_shellout.cpp — shell-out tests for `pulp tweaks diff`.
//
// Per CLAUDE.md: "CLI behavior changes — shell out to the built binary,
// assert exit code + stderr content." `pulp tweaks diff` (Inspector
// Phase 2) compares the pulp-tweaks.json sidecar against a design
// snapshot and reports clean / drifted / orphaned tweaks. These tests
// drive the real binary with fixture files in a scratch directory and
// pin the exit-code contract (0 = no drift, 1 = drift, 2 = usage/file
// error) plus the human + JSON output shapes.
//
// Spec: planning/2026-05-18-inspector-direct-manipulation-roadmap.md

#include "test_cli_shellout_helpers.hpp"

using namespace pulp_test_cli;
namespace fs = std::filesystem;

namespace {

// A pulp-tweaks.json with two anchors: `anchor-live` (2 tweaks) and
// `anchor-gone` (1 tweak). Tests pair this with design fixtures that
// keep or drop those anchors.
const char* kTweaksFixture = R"({
  "$schema": "pulp-tweaks://v1",
  "version": 1,
  "tweaks": {
    "anchor-live": { "paint.backgroundColor": "#222222", "layout.padding": 16 },
    "anchor-gone": { "layout.margin": 8 }
  },
  "bypassed": {}
})";

}  // namespace

TEST_CASE("pulp tweaks --help prints usage and exits 0",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"tweaks", "--help"});
    REQUIRE(r.exit_code == 0);
    REQUIRE_FALSE(r.timed_out);
    REQUIRE(r.stdout_output.find("pulp tweaks") != std::string::npos);
    REQUIRE(r.stdout_output.find("diff") != std::string::npos);
}

TEST_CASE("pulp tweaks with no subcommand prints usage and exits 0",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"tweaks"});
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("tweaks") != std::string::npos);
}

TEST_CASE("pulp tweaks <unknown-subcommand> exits 2 with a diagnostic",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"tweaks", "frobnicate"});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("unknown") != std::string::npos);
}

TEST_CASE("pulp tweaks diff reports orphaned tweaks and exits 1",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto dir = unique_temp_dir("pulp-tweaks-diff-orphan");
    fs::create_directories(dir);
    auto tweaks = dir / "pulp-tweaks.json";
    auto design = dir / "design.json";
    write_text(tweaks, kTweaksFixture);
    // Re-import dropped `anchor-gone`.
    write_text(design, R"(["anchor-live"])");

    auto r = run_pulp({"tweaks", "diff",
                       "--tweaks", tweaks.string(),
                       "--design", design.string()});
    REQUIRE_FALSE(r.timed_out);
    // Drift present → exit 1.
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.stdout_output.find("orphaned 1") != std::string::npos);
    REQUIRE(r.stdout_output.find("anchor-gone") != std::string::npos);
    REQUIRE(r.stdout_output.find("anchor-not-found") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pulp tweaks diff with no drift exits 0",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto dir = unique_temp_dir("pulp-tweaks-diff-clean");
    fs::create_directories(dir);
    auto tweaks = dir / "pulp-tweaks.json";
    auto design = dir / "design.json";
    write_text(tweaks, kTweaksFixture);
    // Design still exposes both anchors.
    write_text(design, R"({"anchors":["anchor-live","anchor-gone"]})");

    auto r = run_pulp({"tweaks", "diff",
                       "--tweaks", tweaks.string(),
                       "--design", design.string()});
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.stdout_output.find("No drift") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pulp tweaks diff --json emits a structured report",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto dir = unique_temp_dir("pulp-tweaks-diff-json");
    fs::create_directories(dir);
    auto tweaks = dir / "pulp-tweaks.json";
    auto design = dir / "design.json";
    write_text(tweaks, kTweaksFixture);
    write_text(design, R"(["anchor-live"])");

    auto r = run_pulp({"tweaks", "diff",
                       "--tweaks", tweaks.string(),
                       "--design", design.string(),
                       "--json"});
    REQUIRE(r.exit_code == 1);
    // JSON keys present; no human-readable banner.
    REQUIRE(r.stdout_output.find("\"summary\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"orphaned\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("\"reason\"") != std::string::npos);
    REQUIRE(r.stdout_output.find("anchor-not-found") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pulp tweaks diff detects property-level drift via the anchors map",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }

    auto dir = unique_temp_dir("pulp-tweaks-diff-prop");
    fs::create_directories(dir);
    auto tweaks = dir / "pulp-tweaks.json";
    auto design = dir / "design.json";
    write_text(tweaks, kTweaksFixture);
    // Anchors survive, but `anchor-live` no longer exposes
    // layout.padding — that tweak should be reported as drifted.
    write_text(design, R"({"anchors":{
        "anchor-live": ["paint.backgroundColor"],
        "anchor-gone": ["layout.margin"]
    }})");

    auto r = run_pulp({"tweaks", "diff",
                       "--tweaks", tweaks.string(),
                       "--design", design.string()});
    REQUIRE(r.exit_code == 1);
    REQUIRE(r.stdout_output.find("drifted  1") != std::string::npos);
    REQUIRE(r.stdout_output.find("property-not-found") != std::string::npos);
    REQUIRE(r.stdout_output.find("layout.padding") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pulp tweaks diff with a missing tweaks file exits 2",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto dir = unique_temp_dir("pulp-tweaks-diff-missing");
    fs::create_directories(dir);
    auto missing = dir / "does-not-exist.json";

    auto r = run_pulp({"tweaks", "diff", "--tweaks", missing.string()});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("Error") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pulp tweaks diff with a malformed design file exits 2",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto dir = unique_temp_dir("pulp-tweaks-diff-baddesign");
    fs::create_directories(dir);
    auto tweaks = dir / "pulp-tweaks.json";
    auto design = dir / "design.json";
    write_text(tweaks, kTweaksFixture);
    write_text(design, "{ this is not json");

    auto r = run_pulp({"tweaks", "diff",
                       "--tweaks", tweaks.string(),
                       "--design", design.string()});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("Error") != std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pulp tweaks diff rejects a non-array property manifest entry",
          "[cli][shellout][tweaks][phase2][regression]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto dir = unique_temp_dir("pulp-tweaks-diff-bad-props");
    fs::create_directories(dir);
    auto tweaks = dir / "pulp-tweaks.json";
    auto design = dir / "design.json";
    write_text(tweaks, kTweaksFixture);
    write_text(design, R"({"anchors":{"anchor-live":{"layout.padding":true}}})");

    auto r = run_pulp({"tweaks", "diff",
                       "--tweaks", tweaks.string(),
                       "--design", design.string()});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("property-path arrays") != std::string::npos);
    REQUIRE(r.stdout_output.find("property-not-found") == std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

// Codex P2 (#2437): a typo'd manifest where an anchor maps to a bare
// string (instead of a property-path array) must surface a manifest
// validation error (exit 2), not silently create an empty property
// set that misreports every tweak under it as `property-not-found`
// drift (exit 1).
TEST_CASE("pulp tweaks diff rejects a string-valued anchor manifest entry",
          "[cli][shellout][tweaks][phase2][regression][issue-2437]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto dir = unique_temp_dir("pulp-tweaks-diff-string-anchor");
    fs::create_directories(dir);
    auto tweaks = dir / "pulp-tweaks.json";
    auto design = dir / "design.json";
    write_text(tweaks, kTweaksFixture);
    // `anchor-live` maps to a bare string — a manifest typo. This must
    // be rejected as a file error, never treated as "no properties".
    write_text(design, R"({"anchors":{"anchor-live":"layout.padding"}})");

    auto r = run_pulp({"tweaks", "diff",
                       "--tweaks", tweaks.string(),
                       "--design", design.string()});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("property-path arrays") != std::string::npos);
    // No false drift output leaked to stdout.
    REQUIRE(r.stdout_output.find("property-not-found") == std::string::npos);
    REQUIRE(r.stdout_output.find("drifted") == std::string::npos);

    std::error_code ec;
    fs::remove_all(dir, ec);
}

TEST_CASE("pulp tweaks diff rejects an unknown flag with exit 2",
          "[cli][shellout][tweaks][phase2]") {
    if (!binary_exists()) { SUCCEED("skipped: pulp not built"); return; }
    auto r = run_pulp({"tweaks", "diff", "--bogus-flag"});
    REQUIRE(r.exit_code == 2);
    REQUIRE(r.stderr_output.find("unknown") != std::string::npos);
}
