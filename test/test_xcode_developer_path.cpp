// Unit tests for `pulp::cli::looks_like_full_xcode_developer_dir`.
//
// Pins the regression for #2969 (Codex comment 3305628892): the
// `pulp ship auv3-xcodeproj` developer-path check used to hard-match
// the literal substring `Xcode.app`, blocking beta-channel users with
// `Xcode-beta.app` (and any other custom-named full Xcode install).

#include <catch2/catch_test_macros.hpp>

#include "xcode_developer_path.hpp"

using pulp::cli::looks_like_full_xcode_developer_dir;

TEST_CASE("looks_like_full_xcode_developer_dir — canonical Xcode.app path",
          "[cli][ship][xcode-select]") {
    REQUIRE(looks_like_full_xcode_developer_dir(
        "/Applications/Xcode.app/Contents/Developer"));
}

// Regression: #2969 / Codex comment 3305628892. These are real paths
// produced by `xcode-select -p` on common beta / pinned installs that
// the previous literal `Xcode.app` substring check rejected.
TEST_CASE("looks_like_full_xcode_developer_dir — accepts beta / versioned bundles",
          "[cli][ship][xcode-select][issue-2969]") {
    REQUIRE(looks_like_full_xcode_developer_dir(
        "/Applications/Xcode-beta.app/Contents/Developer"));
    REQUIRE(looks_like_full_xcode_developer_dir(
        "/Applications/Xcode_15.app/Contents/Developer"));
    REQUIRE(looks_like_full_xcode_developer_dir(
        "/Applications/Xcode_16.2.app/Contents/Developer"));
    // Custom paths (non-/Applications) must work too.
    REQUIRE(looks_like_full_xcode_developer_dir(
        "/Users/dev/tools/Xcode-15.4.app/Contents/Developer"));
    // Even non-Xcode-named app bundles — the helper is name-agnostic by
    // design, so it accepts any `.app/Contents/Developer` path. The
    // outer caller can additionally check `xcodebuild` is usable; this
    // helper just rejects the command-line-tools shim and similar.
    REQUIRE(looks_like_full_xcode_developer_dir(
        "/Applications/MyForkOfXcode.app/Contents/Developer"));
}

TEST_CASE("looks_like_full_xcode_developer_dir — rejects command-line-tools shim",
          "[cli][ship][xcode-select]") {
    REQUIRE_FALSE(looks_like_full_xcode_developer_dir(
        "/Library/Developer/CommandLineTools"));
    REQUIRE_FALSE(looks_like_full_xcode_developer_dir(""));
    // No `.app/` anywhere.
    REQUIRE_FALSE(looks_like_full_xcode_developer_dir(
        "/Library/Developer/CommandLineTools/Contents/Developer"));
    // Empty bundle name (`/.app/...`) is malformed — reject.
    REQUIRE_FALSE(looks_like_full_xcode_developer_dir(
        "/.app/Contents/Developer"));
    // `.app` substring without `/Contents/Developer` suffix — incomplete.
    REQUIRE_FALSE(looks_like_full_xcode_developer_dir(
        "/Applications/Xcode.app"));
    REQUIRE_FALSE(looks_like_full_xcode_developer_dir(
        "/Applications/Xcode.app/Contents/"));
}
