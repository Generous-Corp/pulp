// SPDX-License-Identifier: MIT
//
// pulp #1814 — unit tests for the version-pinned SDK tarball filename
// helper. Pins the back-compat contract on the legacy unversioned
// filename so `pulp cache fetch skia` can clean it up best-effort
// without ambiguity, and asserts the new versioned shape so a future
// edit can't accidentally drop the version pin and reintroduce the
// stale-cache bug.

#include "../tools/cli/sdk_cache_paths.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

using pulp::cli::sdk_tarball_filename;
using pulp::cli::legacy_unversioned_sdk_tarball_filename;

TEST_CASE("sdk_tarball_filename: encodes the SDK version + platform",
          "[cli][cache][issue-1814]") {
    REQUIRE(sdk_tarball_filename("0.92.0", "darwin-arm64") ==
            "pulp-sdk-v0.92.0-darwin-arm64.tar.gz");
    REQUIRE(sdk_tarball_filename("1.0.0", "linux-x64") ==
            "pulp-sdk-v1.0.0-linux-x64.tar.gz");
    REQUIRE(sdk_tarball_filename("0.78.3", "windows-arm64") ==
            "pulp-sdk-v0.78.3-windows-arm64.tar.gz");
}

TEST_CASE("sdk_tarball_filename: different versions produce different filenames "
          "so cache lookup misses on stale tarballs",
          "[cli][cache][issue-1814]") {
    // The pulp #1814 bug was that v0.85 and v0.92 shared the same
    // cache key. The structural fix here is to make the cache key
    // include the version so a v0.92 install cannot find a v0.85
    // tarball even by accident.
    const std::string v_old = sdk_tarball_filename("0.85.0", "darwin-arm64");
    const std::string v_new = sdk_tarball_filename("0.92.0", "darwin-arm64");
    REQUIRE_FALSE(v_old == v_new);
}

TEST_CASE("legacy_unversioned_sdk_tarball_filename: matches the pre-#1814 shape",
          "[cli][cache][issue-1814]") {
    // The legacy name is the public contract between the pre-#1814 CLI
    // and the post-#1814 cleanup path; locking it in unit tests keeps
    // a future rename from breaking the cleanup of stale tarballs.
    REQUIRE(legacy_unversioned_sdk_tarball_filename("darwin-arm64") ==
            "pulp-sdk-darwin-arm64.tar.gz");
    REQUIRE(legacy_unversioned_sdk_tarball_filename("linux-x64") ==
            "pulp-sdk-linux-x64.tar.gz");
    REQUIRE(legacy_unversioned_sdk_tarball_filename("windows-x64") ==
            "pulp-sdk-windows-x64.tar.gz");
}

TEST_CASE("sdk_tarball_filename and legacy form disagree on every supported "
          "platform — guarantees the cleanup pass deletes legacy files only",
          "[cli][cache][issue-1814]") {
    // If the legacy file ever happens to equal a versioned file, the
    // cleanup pass would delete a fresh cache hit. Guard against
    // accidental shape convergence.
    for (const auto& platform : {"darwin-arm64", "darwin-x64",
                                  "linux-arm64", "linux-x64",
                                  "windows-arm64", "windows-x64"}) {
        const std::string legacy = legacy_unversioned_sdk_tarball_filename(platform);
        const std::string versioned = sdk_tarball_filename("0.92.0", platform);
        REQUIRE_FALSE(legacy == versioned);
    }
}
