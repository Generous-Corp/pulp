// Coverage for the release-archive checksum verifier that gates
// `pulp upgrade` and `pulp sdk install` extraction on the release's
// SHA256SUMS manifest.

#include <catch2/catch_test_macros.hpp>

#include "support/unique_temp_dir.hpp"
#include "tools/cli/release_checksum.hpp"
#include "tools/cli/upgrade_url.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

namespace fs = std::filesystem;
namespace rc = pulp::cli::release_checksum;

namespace {

constexpr const char* kAbcSha256 =
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

fs::path write_archive(const fs::path& dir, const std::string& name, const std::string& bytes) {
    const auto path = dir / name;
    std::ofstream out(path, std::ios::binary);
    out << bytes;
    return path;
}

} // namespace

TEST_CASE("manifest lookup matches the exact basename only", "[cli][release-checksum]") {
    const std::string upper(kAbcSha256);
    std::string manifest;
    manifest += std::string(kAbcSha256) + "  pulp-darwin-arm64.tar.gz\n";
    manifest += std::string(kAbcSha256) + " *pulp-linux-x64.tar.gz\r\n";
    manifest +=
        "BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD  xpulp-windows-x64.zip\n";

    auto hit = rc::manifest_digest_for(manifest, "pulp-darwin-arm64.tar.gz");
    REQUIRE(hit.found());
    CHECK(hit.digest == kAbcSha256);

    auto binary_marker = rc::manifest_digest_for(manifest, "pulp-linux-x64.tar.gz");
    REQUIRE(binary_marker.found());
    CHECK(binary_marker.digest == kAbcSha256);

    auto uppercase = rc::manifest_digest_for(manifest, "xpulp-windows-x64.zip");
    REQUIRE(uppercase.found());
    CHECK(uppercase.digest == kAbcSha256);

    auto missing = rc::manifest_digest_for(manifest, "pulp-windows-x64.zip");
    CHECK_FALSE(missing.found());
    CHECK(missing.error.find("no entry for pulp-windows-x64.zip") != std::string::npos);
}

TEST_CASE("manifest lookup refuses duplicate and malformed entries", "[cli][release-checksum]") {
    const std::string duplicate =
        std::string(kAbcSha256) + "  a.tar.gz\n" + std::string(kAbcSha256) + "  a.tar.gz\n";
    auto dup = rc::manifest_digest_for(duplicate, "a.tar.gz");
    CHECK_FALSE(dup.found());
    CHECK(dup.error.find("more than once") != std::string::npos);

    const std::string malformed = std::string(kAbcSha256) + "  a.tar.gz\nnot-a-digest  b.tar.gz\n";
    auto bad = rc::manifest_digest_for(malformed, "a.tar.gz");
    CHECK_FALSE(bad.found());
    CHECK(bad.error.find("malformed SHA256SUMS line 2") != std::string::npos);
}

TEST_CASE("version floor comparison fails closed", "[cli][release-checksum]") {
    CHECK(rc::version_at_or_above("0.663.1", "0.663.1"));
    CHECK(rc::version_at_or_above("v0.700.0", "0.663.1"));
    CHECK(rc::version_at_or_above("1.0.0-rc1", "0.663.1"));
    CHECK(rc::version_at_or_above("0.663.1-pre", "0.663.1"));
    CHECK(rc::version_at_or_above("0.663.1-rc.1+build.7", "0.663.1"));
    CHECK_FALSE(rc::version_at_or_above("0.663.0", "0.663.1"));
    CHECK_FALSE(rc::version_at_or_above("0.663.0-pre", "0.663.1"));
    CHECK_FALSE(rc::version_at_or_above("0.99.9", "0.663.1"));
    CHECK(rc::version_at_or_above("garbage", "0.663.1"));
}

TEST_CASE("release archive verifies against its manifest entry", "[cli][release-checksum]") {
    const auto dir = pulp::test::make_unique_temp_dir("pulp-test-release-checksum");
    const auto archive = write_archive(dir, "pulp-darwin-arm64.tar.gz", "abc");
    const std::string manifest = std::string(kAbcSha256) + "  pulp-darwin-arm64.tar.gz\n";

    auto verdict = rc::verify_release_archive(archive, "pulp-darwin-arm64.tar.gz", "0.700.0",
                                              "0.663.1", manifest);
    CHECK(verdict.ok());
    CHECK(verdict.status == rc::Status::verified);
    CHECK(verdict.digest == kAbcSha256);
    CHECK(verdict.message.find("Verified SHA-256") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("release archive with a wrong or missing digest is refused", "[cli][release-checksum]") {
    const auto dir = pulp::test::make_unique_temp_dir("pulp-test-release-checksum");
    const auto archive = write_archive(dir, "pulp-darwin-arm64.tar.gz", "abd");
    const std::string manifest = std::string(kAbcSha256) + "  pulp-darwin-arm64.tar.gz\n";

    auto mismatch = rc::verify_release_archive(archive, "pulp-darwin-arm64.tar.gz", "0.700.0",
                                               "0.663.1", manifest);
    CHECK_FALSE(mismatch.ok());
    CHECK(mismatch.status == rc::Status::refused);
    CHECK(mismatch.message.find("SHA-256 mismatch") != std::string::npos);
    CHECK(mismatch.digest.empty());

    const std::string other = std::string(kAbcSha256) + "  other.tar.gz\n";
    auto missing = rc::verify_release_archive(archive, "pulp-darwin-arm64.tar.gz", "0.700.0",
                                              "0.663.1", other);
    CHECK_FALSE(missing.ok());
    CHECK(missing.message.find("no entry") != std::string::npos);

    auto unreadable = rc::verify_release_archive(dir / "absent.tar.gz", "pulp-darwin-arm64.tar.gz",
                                                 "0.700.0", "0.663.1", manifest);
    CHECK_FALSE(unreadable.ok());
    CHECK(unreadable.message.find("could not hash") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("missing manifest is refused at the floor and warned below it",
          "[cli][release-checksum]") {
    const auto dir = pulp::test::make_unique_temp_dir("pulp-test-release-checksum");
    const auto archive = write_archive(dir, "pulp-darwin-arm64.tar.gz", "abc");

    auto refused = rc::verify_release_archive(archive, "pulp-darwin-arm64.tar.gz", "0.663.1",
                                              "0.663.1", std::nullopt);
    CHECK_FALSE(refused.ok());
    CHECK(refused.message.find("could not fetch SHA256SUMS") != std::string::npos);

    // An exact-floor pre-release is at the floor, so its missing manifest is
    // refused too (the Rust verifier agrees).
    auto prerelease = rc::verify_release_archive(archive, "pulp-darwin-arm64.tar.gz", "0.663.1-pre",
                                                 "0.663.1", std::nullopt);
    CHECK_FALSE(prerelease.ok());
    CHECK(prerelease.message.find("could not fetch SHA256SUMS") != std::string::npos);

    auto legacy = rc::verify_release_archive(archive, "pulp-darwin-arm64.tar.gz", "0.600.0",
                                             "0.663.1", std::nullopt);
    CHECK(legacy.ok());
    CHECK(legacy.status == rc::Status::unverified_legacy);
    CHECK(legacy.message.find("NOT verified") != std::string::npos);
    fs::remove_all(dir);
}

TEST_CASE("only a plain M.m.p release version may reach a URL, path, or shell",
          "[cli][release-checksum][upgrade]") {
    CHECK(pulp::cli::pulp_release_version_is_well_formed("0.663.1"));
    CHECK(pulp::cli::pulp_release_version_is_well_formed("1.0.0"));
    CHECK(pulp::cli::pulp_release_version_is_well_formed("0.1009.0"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed(""));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("0.1"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("0.1.0.0"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("v0.1.0"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("0.1.0-pre"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("0.1.0 "));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("0..1"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("0.1.0; rm -rf ~"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("$(touch pwned)"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("0.1.0`id`"));
    CHECK_FALSE(pulp::cli::pulp_release_version_is_well_formed("../0.1.0"));
}

TEST_CASE("manifest URL sits beside the archive on the same release tag",
          "[cli][release-checksum]") {
    const auto [asset, url] = pulp::cli::pulp_upgrade_url_for("0.700.0", "darwin", "arm64");
    CHECK(url == "https://github.com/Generous-Corp/pulp/releases/download/v0.700.0/" + asset);
    CHECK(pulp::cli::pulp_release_asset_url("0.700.0", std::string(rc::kManifestAssetName)) ==
          "https://github.com/Generous-Corp/pulp/releases/download/v0.700.0/SHA256SUMS");
}
