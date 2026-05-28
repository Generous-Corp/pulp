#include <catch2/catch_test_macros.hpp>
#include <pulp/ship/codesign.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>
#ifdef __APPLE__
#include <unistd.h>
#endif

using namespace pulp::ship;
namespace fs = std::filesystem;

#ifdef __APPLE__
namespace pulp::ship::detail {
SigningInfo parse_codesign_details(const std::string& output);
std::optional<std::string> parse_notarytool_submit_id(const std::string& output);
NotarizationStatus parse_notarytool_status(const std::string& output);
std::vector<std::string> parse_signing_identities(const std::string& output);
}
#endif

namespace {

struct ScopedTempDir {
    explicit ScopedTempDir(std::string prefix) {
        auto templ = (fs::temp_directory_path() / (std::move(prefix) + "-XXXXXX")).string();
#ifdef __APPLE__
        auto* made = mkdtemp(templ.data());
        REQUIRE(made != nullptr);
        path = made;
#else
        path = fs::temp_directory_path() / templ;
        fs::create_directories(path);
#endif
    }

    ~ScopedTempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    fs::path path;
};

}  // namespace

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

TEST_CASE("codesign reports unsigned regular files without identity metadata",
          "[ship][codesign][coverage][phase3]") {
    ScopedTempDir temp("pulp-codesign-unsigned");
    const auto file = temp.path / "plain-tool";
    std::ofstream(file) << "not a Mach-O";
    fs::permissions(file,
                    fs::perms::owner_read | fs::perms::owner_write | fs::perms::owner_exec,
                    fs::perm_options::replace);
    REQUIRE(fs::is_regular_file(file));
    REQUIRE(fs::file_size(file) > 0);

    auto info = check_codesign(file.string());
    REQUIRE_FALSE(info.is_signed);
    REQUIRE_FALSE(info.is_valid);
#ifdef __APPLE__
    REQUIRE_FALSE(info.error.empty());
#else
    REQUIRE(info.error.empty());
#endif
    REQUIRE(info.identity.empty());
    REQUIRE(info.team_id.empty());
    REQUIRE_FALSE(info.is_notarized);
}

TEST_CASE("codesign failure paths leave requested outputs absent",
          "[ship][codesign][coverage][phase3]") {
    ScopedTempDir temp("pulp-codesign-failures");
    const auto pkg = temp.path / "missing.pkg";
    const auto signed_pkg = temp.path / "missing-signed.pkg";
    const auto dmg = temp.path / "missing.dmg";
    const auto component = temp.path / "missing.vst3";

    REQUIRE_FALSE(fs::exists(component));
    REQUIRE_FALSE(create_pkg(component.string(),
                             pkg.string(),
                             "dev.pulp.tests.missing",
                             "1.2.3"));
    REQUIRE_FALSE(fs::exists(pkg));
    REQUIRE_FALSE(create_pkg(component.string(),
                             signed_pkg.string(),
                             "dev.pulp.tests.missing",
                             "1.2.3",
                             "Developer ID Installer: Missing"));
    REQUIRE_FALSE(fs::exists(signed_pkg));
    REQUIRE_FALSE(create_dmg(component.string(), dmg.string(), "Missing Component"));
    REQUIRE_FALSE(fs::exists(dmg));
    REQUIRE_FALSE(check_notarization(component.string()));
    REQUIRE_FALSE(notarize_staple(component.string()));
}

TEST_CASE("combined pkg rejects empty and missing component inputs",
          "[ship][codesign][coverage][phase3]") {
    ScopedTempDir temp("pulp-combined-pkg-failures");
    const auto empty_output = temp.path / "empty.pkg";
    const auto missing_output = temp.path / "missing.pkg";

#ifdef __APPLE__
    const auto staging_dir = fs::temp_directory_path()
        / ("pulp-pkg-staging-" + std::to_string(getpid()));
    std::error_code cleanup_ec;
    fs::remove_all(staging_dir, cleanup_ec);
#endif

    REQUIRE_FALSE(fs::exists(empty_output));
    REQUIRE_FALSE(create_combined_pkg({},
                                      empty_output.string(),
                                      "dev.pulp.tests.empty",
                                      "2.0.0"));
    REQUIRE_FALSE(fs::exists(empty_output));
#ifdef __APPLE__
    REQUIRE_FALSE(fs::exists(staging_dir));
#endif

    std::vector<InstallComponent> components{
        {(temp.path / "missing-a.component").string(), "/Library/Audio/Plug-Ins/Components"},
        {(temp.path / "missing-b.vst3").string(), "/Library/Audio/Plug-Ins/VST3"},
    };
    REQUIRE_FALSE(fs::exists(components[0].path));
    REQUIRE_FALSE(fs::exists(components[1].path));
    REQUIRE_FALSE(create_combined_pkg(components,
                                      missing_output.string(),
                                      "dev.pulp.tests.missing",
                                      "2.0.0",
                                      "Developer ID Installer: Missing"));
    REQUIRE_FALSE(fs::exists(missing_output));
#ifdef __APPLE__
    REQUIRE_FALSE(fs::exists(staging_dir));
#endif
    REQUIRE(components[0].install_location.find("Components") != std::string::npos);
    REQUIRE(components[1].install_location.find("VST3") != std::string::npos);
}

TEST_CASE("default audio entitlements keep hardened runtime permissions narrow",
          "[ship][codesign][coverage][phase3]") {
    const auto plist = default_audio_entitlements();
#ifdef __APPLE__
    REQUIRE(plist.find(R"(<?xml version="1.0" encoding="UTF-8"?>)") != std::string::npos);
    REQUIRE(plist.find("<plist version=\"1.0\">") != std::string::npos);
    REQUIRE(plist.find("<dict>") != std::string::npos);
    REQUIRE(plist.find("<key>com.apple.security.device.audio-input</key>") != std::string::npos);
    REQUIRE(plist.find("<key>com.apple.security.network.client</key>") != std::string::npos);
    REQUIRE(plist.find("com.apple.security.network.server") == std::string::npos);
    REQUIRE(plist.find("com.apple.security.cs.disable-library-validation") == std::string::npos);
    REQUIRE(plist.find("com.apple.security.files.user-selected.read-write") == std::string::npos);
    REQUIRE(plist.find("</dict>") != std::string::npos);
    REQUIRE(plist.find("</plist>") != std::string::npos);
#else
    REQUIRE(plist.empty());
#endif
}

TEST_CASE("codesign detail parser extracts identity and team fields",
          "[ship][codesign][coverage][phase3]") {
#ifdef __APPLE__
    auto info = pulp::ship::detail::parse_codesign_details(R"(
Executable=/Applications/Pulp.app/Contents/MacOS/Pulp
Identifier=dev.pulp.app
Authority=Developer ID Application: Pulp Audio LLC (ABCDE12345)
Authority=Developer ID Certification Authority
TeamIdentifier=ABCDE12345
Runtime Version=15.0.0
)");

    REQUIRE(info.identity == "Developer ID Application: Pulp Audio LLC (ABCDE12345)");
    REQUIRE(info.team_id == "ABCDE12345");
    REQUIRE_FALSE(info.is_signed);
    REQUIRE_FALSE(info.is_valid);
    REQUIRE_FALSE(info.is_notarized);
    REQUIRE(info.error.empty());

    auto missing_team = pulp::ship::detail::parse_codesign_details(
        "Authority=Apple Development: Local Tester (VWXYZ98765)\n");
    REQUIRE(missing_team.identity == "Apple Development: Local Tester (VWXYZ98765)");
    REQUIRE(missing_team.team_id.empty());

    auto missing_identity = pulp::ship::detail::parse_codesign_details(
        "TeamIdentifier=VWXYZ98765\n");
    REQUIRE(missing_identity.identity.empty());
    REQUIRE(missing_identity.team_id == "VWXYZ98765");

    auto first_authority = pulp::ship::detail::parse_codesign_details(R"(
Authority=Developer ID Application: First Team (FIRST12345)
Authority=Developer ID Application: Second Team (SECOND12345)
TeamIdentifier=FIRST12345
)");
    REQUIRE(first_authority.identity == "Developer ID Application: First Team (FIRST12345)");
    REQUIRE(first_authority.team_id == "FIRST12345");
#endif
}

TEST_CASE("notarytool submit parser accepts UUID ids only",
          "[ship][codesign][coverage][phase3]") {
#ifdef __APPLE__
    auto accepted = pulp::ship::detail::parse_notarytool_submit_id(R"(
Submission ID received
  id: 12345678-abcd-4abc-9def-123456789abc
  status: Accepted
)");
    REQUIRE(accepted.has_value());
    REQUIRE(*accepted == "12345678-abcd-4abc-9def-123456789abc");

    auto lower = pulp::ship::detail::parse_notarytool_submit_id(
        "id: abcdefab-1234-5678-9abc-def012345678");
    REQUIRE(lower.has_value());
    REQUIRE(*lower == "abcdefab-1234-5678-9abc-def012345678");

    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id("id: NOT-A-UUID").has_value());
    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id("status: Invalid").has_value());
#endif
}

TEST_CASE("notarytool status parser classifies terminal states",
          "[ship][codesign][coverage][phase3]") {
#ifdef __APPLE__
    auto accepted = pulp::ship::detail::parse_notarytool_status("status: Accepted\nmessage: Ready");
    REQUIRE(accepted.complete);
    REQUIRE(accepted.success);
    REQUIRE(accepted.message.find("Accepted") != std::string::npos);

    auto invalid = pulp::ship::detail::parse_notarytool_status("status: Invalid\nmessage: bad signature");
    REQUIRE(invalid.complete);
    REQUIRE_FALSE(invalid.success);
    REQUIRE(invalid.message.find("bad signature") != std::string::npos);

    auto rejected = pulp::ship::detail::parse_notarytool_status("status: Rejected");
    REQUIRE(rejected.complete);
    REQUIRE_FALSE(rejected.success);
    REQUIRE(rejected.message == "status: Rejected");

    auto progress = pulp::ship::detail::parse_notarytool_status("status: In Progress");
    REQUIRE_FALSE(progress.complete);
    REQUIRE_FALSE(progress.success);
    REQUIRE(progress.message == "status: In Progress");
#endif
}

TEST_CASE("security identity parser preserves quoted display names",
          "[ship][codesign][coverage][phase3]") {
#ifdef __APPLE__
    auto identities = pulp::ship::detail::parse_signing_identities(R"IDENTITIES(
  1) AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA "Developer ID Application: Pulp Audio LLC (ABCDE12345)"
  2) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB "Apple Development: Local Tester (VWXYZ98765)"
     2 valid identities found
)IDENTITIES");
    REQUIRE(identities.size() == 2);
    REQUIRE(identities[0] == "Developer ID Application: Pulp Audio LLC (ABCDE12345)");
    REQUIRE(identities[1] == "Apple Development: Local Tester (VWXYZ98765)");

    auto mixed = pulp::ship::detail::parse_signing_identities(R"IDENTITIES(
  malformed no quote
  3) CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC "Mac Developer: First" trailing text
  "bare quoted identity"
  4) DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD ""
)IDENTITIES");
    REQUIRE(mixed.size() == 3);
    REQUIRE(mixed[0] == "Mac Developer: First");
    REQUIRE(mixed[1] == "bare quoted identity");
    REQUIRE(mixed[2].empty());

    auto none = pulp::ship::detail::parse_signing_identities("0 valid identities found\n");
    REQUIRE(none.empty());

    auto unbalanced = pulp::ship::detail::parse_signing_identities(R"IDENTITIES(
  1) AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA "Missing close
  2) BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB No opening quote"
)IDENTITIES");
    REQUIRE(unbalanced.empty());
#endif
}
