// Regression guard for the `pulp upgrade` release-asset URL template.
//
// Before #352 closed this, the CLI computed asset names as
// "pulp-<version>-<platform>-<arch>.<ext>" and every `pulp upgrade`
// 404'd on the download step because the release workflow uploads
// assets under "pulp-<platform>-<arch>.<ext>" (no version in the name).
// This test pins the expected URL shape against real v0.14.0 assets
// so the same drift cannot recur silently.

#include <catch2/catch_test_macros.hpp>

#include "tools/cli/upgrade_url.hpp"

using pulp::cli::pulp_upgrade_url_for;

TEST_CASE("pulp upgrade URL: darwin arm64 asset name matches release workflow",
          "[cli][upgrade][url][issue-352]") {
    auto r = pulp_upgrade_url_for("0.14.0", "darwin", "arm64");
    REQUIRE(r.asset == "pulp-darwin-arm64.tar.gz");
    REQUIRE(r.url ==
            "https://github.com/danielraffel/pulp/releases/download/"
            "v0.14.0/pulp-darwin-arm64.tar.gz");
}

TEST_CASE("pulp upgrade URL: linux x64 uses 'x64' not 'x86_64'",
          "[cli][upgrade][url][issue-352]") {
    auto r = pulp_upgrade_url_for("0.14.0", "linux", "x64");
    REQUIRE(r.asset == "pulp-linux-x64.tar.gz");
    REQUIRE(r.url.find("/v0.14.0/pulp-linux-x64.tar.gz") != std::string::npos);
}

TEST_CASE("pulp upgrade URL: linux arm64 asset", "[cli][upgrade][url]") {
    auto r = pulp_upgrade_url_for("0.14.0", "linux", "arm64");
    REQUIRE(r.asset == "pulp-linux-arm64.tar.gz");
}

TEST_CASE("pulp upgrade URL: windows uses .zip extension",
          "[cli][upgrade][url][windows]") {
    auto r = pulp_upgrade_url_for("0.14.0", "windows", "x64");
    REQUIRE(r.asset == "pulp-windows-x64.zip");
    auto arm = pulp_upgrade_url_for("0.14.0", "windows", "arm64");
    REQUIRE(arm.asset == "pulp-windows-arm64.zip");
}

TEST_CASE("pulp upgrade URL: version appears only in the release path, not the filename",
          "[cli][upgrade][url][issue-352]") {
    // The precise bug #352 captured: the version was baked into the
    // filename. Any future refactor that re-introduces the version in
    // the asset name fails here.
    auto r = pulp_upgrade_url_for("9.99.0", "darwin", "arm64");
    REQUIRE(r.asset.find("9.99.0") == std::string::npos);
    REQUIRE(r.url.find("v9.99.0/") != std::string::npos);
}

TEST_CASE("pulp upgrade URL: version field flows into the release tag verbatim",
          "[cli][upgrade][url]") {
    auto r = pulp_upgrade_url_for("1.2.3-rc.1", "darwin", "arm64");
    REQUIRE(r.url.find("v1.2.3-rc.1/") != std::string::npos);
}
