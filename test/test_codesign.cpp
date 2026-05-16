#include <catch2/catch_test_macros.hpp>
#include <pulp/ship/codesign.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#ifdef __APPLE__
#include <unistd.h>
#endif

using namespace pulp::ship;
namespace fs = std::filesystem;

TEST_CASE("check_codesign on system binary", "[ship][codesign]") {
#ifdef __APPLE__
    // /usr/bin/true is always signed on macOS
    auto info = check_codesign("/usr/bin/true");
    REQUIRE(info.is_signed);
    REQUIRE(info.is_valid);
    REQUIRE_FALSE(info.identity.empty());
#else
    auto info = check_codesign("/usr/bin/true");
    REQUIRE_FALSE(info.is_signed);
#endif
}

TEST_CASE("check_codesign on nonexistent path", "[ship][codesign]") {
    auto info = check_codesign("/nonexistent/binary");
    REQUIRE_FALSE(info.is_signed);
    REQUIRE_FALSE(info.is_valid);
}

TEST_CASE("check_notarization on nonexistent path is false", "[ship][codesign]") {
    REQUIRE_FALSE(check_notarization("/nonexistent/binary"));
}

TEST_CASE("codesign rejects nonexistent path and identity",
          "[ship][codesign][coverage][issue-644]") {
    REQUIRE_FALSE(codesign("/nonexistent/binary", "Developer ID Application: Missing"));
}

TEST_CASE("codesign includes entitlements path on failure path",
          "[ship][codesign][coverage][issue-644]") {
    REQUIRE_FALSE(codesign("/nonexistent/binary",
                           "Developer ID Application: Missing",
                           "/nonexistent/entitlements.plist"));
}

TEST_CASE("notarization staple on nonexistent path is false",
          "[ship][codesign][coverage][issue-644]") {
    REQUIRE_FALSE(notarize_staple("/nonexistent/binary"));
}

TEST_CASE("create_pkg on nonexistent component is false",
          "[ship][codesign][coverage][issue-644]") {
    auto output = (fs::temp_directory_path() / "pulp-missing-component.pkg").string();
    REQUIRE_FALSE(create_pkg("/nonexistent/component.vst3",
                             output,
                             "dev.pulp.missing",
                             "1.0.0"));
}

TEST_CASE("list_signing_identities does not crash", "[ship][codesign]") {
    auto ids = list_signing_identities();
    // May be empty on CI, but should not crash
    REQUIRE(true);
}

TEST_CASE("default_audio_entitlements returns valid plist", "[ship][codesign]") {
    auto plist = default_audio_entitlements();
#ifdef __APPLE__
    REQUIRE_FALSE(plist.empty());
    REQUIRE(plist.find("audio-input") != std::string::npos);
    REQUIRE(plist.find("network.client") != std::string::npos);
    REQUIRE(plist.find("</plist>") != std::string::npos);
#else
    // Non-Apple returns empty
    REQUIRE(plist.empty());
#endif
}

TEST_CASE("create_dmg produces a file from valid source", "[ship][codesign]") {
#ifdef __APPLE__
    auto root_template = (fs::temp_directory_path() / "pulp-dmg-test-XXXXXX").string();
    REQUIRE(mkdtemp(root_template.data()) != nullptr);

    fs::path root(root_template);
    fs::path source = root / "source";
    fs::create_directories(source);

    std::ofstream(source / "test.txt") << "pulp";

    fs::path dmg_path = root / "output.dmg";
    bool ok = create_dmg(source.string(), dmg_path.string(), "PulpTest");
    REQUIRE(ok);

    REQUIRE(fs::is_regular_file(dmg_path));
    REQUIRE(fs::file_size(dmg_path) > 0);

    fs::remove_all(root);
#endif
}
