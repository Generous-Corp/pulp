#include <catch2/catch_test_macros.hpp>
#include <pulp/ship/codesign.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
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
std::string disk_image_failure_hint(const std::string& output);
bool create_dmg_image(const std::string& source_path, const std::string& output_path,
                      const std::string& volume_name, std::string& error);
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
          "[ship][codesign][issue-644]") {
    REQUIRE_FALSE(codesign("/nonexistent/binary", "Developer ID Application: Missing"));
}

TEST_CASE("codesign includes entitlements path on failure path",
          "[ship][codesign][issue-644]") {
    REQUIRE_FALSE(codesign("/nonexistent/binary",
                           "Developer ID Application: Missing",
                           "/nonexistent/entitlements.plist"));
}

#if defined(_WIN32)
// W7 signtool failure contract (#295 lesson): an empty identity or path must be
// rejected up front rather than invoking signtool with `/n ""` (which could
// latch onto an unintended cert or no-op "successfully"). This guard runs
// without signtool present, so it's exercised on the Windows CI lane.
TEST_CASE("codesign (Windows) rejects empty identity and empty path before signing",
          "[ship][codesign][windows]") {
    REQUIRE_FALSE(codesign("C:/some/app.exe", ""));        // empty identity
    REQUIRE_FALSE(codesign("", "Some Cert"));               // empty path
    REQUIRE_FALSE(codesign("", ""));                        // both empty
}
#endif

TEST_CASE("notarization staple on nonexistent path is false",
          "[ship][codesign][issue-644]") {
    REQUIRE_FALSE(notarize_staple("/nonexistent/binary"));
}

TEST_CASE("ASC notarization submit fails closed for missing inputs",
          "[ship][codesign]") {
    auto request = notarize_submit_asc("/nonexistent/archive.zip",
                                       "/nonexistent/AuthKey_TEST.p8",
                                       "TESTKEY123",
                                       "12345678-1234-1234-1234-123456789abc");
    REQUIRE_FALSE(request.has_value());
}

TEST_CASE("create_pkg on nonexistent component is false",
          "[ship][codesign][issue-644]") {
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

#ifdef __APPLE__
TEST_CASE("a busy hdiutil failure names the unmount dissenter; other failures do not",
          "[ship][codesign]") {
    using detail::disk_image_failure_hint;

    // hdiutil collapses "someone vetoed the unmount of my temporary volume" into
    // this one line, so the hint is the only place a user learns what to do.
    const auto busy = disk_image_failure_hint("hdiutil: create failed - Resource busy");
    CHECK_FALSE(busy.empty());
    CHECK(busy.find("simdiskimaged") != std::string::npos);
    CHECK(busy.find("diskutil unmount") != std::string::npos);
    CHECK_FALSE(disk_image_failure_hint("hdiutil: attach failed - Device busy").empty());

    // A failure the hint cannot explain must not carry it: pointing at the
    // simulator when the source folder is missing is worse than saying nothing.
    CHECK(disk_image_failure_hint("hdiutil: create failed - No such file or directory").empty());
    CHECK(disk_image_failure_hint("hdiutil: create failed - No space left on device").empty());
    CHECK(disk_image_failure_hint("").empty());
}

TEST_CASE("a failed dmg build hands back what hdiutil said", "[ship][codesign]") {
    // hdiutil's output used to be redirected to /dev/null, so every DMG failure
    // reached the user as a bare `false`. Whatever hdiutil prints, the caller gets.
    ScopedTempDir temp("pulp-dmg-missing-src");
    std::string error;
    CHECK_FALSE(detail::create_dmg_image((temp.path / "not-here").string(),
                                         (temp.path / "out.dmg").string(),
                                         "PulpTest", error));
    CHECK_FALSE(error.empty());
    CHECK(error.find("hdiutil") != std::string::npos);
}

TEST_CASE("create_dmg fails when the source is missing", "[ship][codesign]") {
    ScopedTempDir temp("pulp-dmg-missing-src");
    CHECK_FALSE(create_dmg((temp.path / "not-here").string(),
                           (temp.path / "out.dmg").string(), "PulpTest"));
    CHECK_FALSE(fs::exists(temp.path / "out.dmg"));
}
#endif

TEST_CASE("codesign reports unsigned regular files without identity metadata",
          "[ship][codesign]") {
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
          "[ship][codesign]") {
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
          "[ship][codesign]") {
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

#ifdef __APPLE__
// The combined installer is component-SELECTABLE by default: it ships a
// distribution document with one user-toggleable choice per format (a
// "Customize" pane), so a user can install only the formats they want.
TEST_CASE("combined pkg is component-selectable with a choice per format",
          "[ship][codesign]") {
    ScopedTempDir temp("pulp-combined-pkg-choices");
    // pkgbuild --component accepts any directory as a "bundle".
    const auto au = temp.path / "Dummy.component";
    const auto vst3 = temp.path / "Dummy.vst3";
    fs::create_directories(au / "Contents/MacOS");
    fs::create_directories(vst3 / "Contents/MacOS");
    std::ofstream(au / "Contents/MacOS/Dummy") << "x";
    std::ofstream(vst3 / "Contents/MacOS/Dummy") << "x";
    // pkgbuild --component requires a bundle Info.plist.
    const char* kPlist = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>CFBundleIdentifier</key><string>dev.pulp.tests.dummy</string>"
        "<key>CFBundleVersion</key><string>1</string></dict></plist>";
    std::ofstream(au / "Contents/Info.plist") << kPlist;
    std::ofstream(vst3 / "Contents/Info.plist") << kPlist;

    const auto out = temp.path / "Combined.pkg";
    std::vector<InstallComponent> components{
        {au.string(), "/Library/Audio/Plug-Ins/Components", "Audio Unit (AU)"},
        {vst3.string(), "/Library/Audio/Plug-Ins/VST3", "VST3"},
    };
    // Unsigned build (no signing identity) — pkgbuild/productbuild still run.
    REQUIRE(create_combined_pkg(components, out.string(),
                                "dev.pulp.tests.choices", "1.2.3"));
    REQUIRE(fs::exists(out));

    // Extract the Distribution and confirm both selectable choices are present.
    const auto extract = temp.path / "x";
    fs::create_directories(extract);
    const std::string cmd = "xar -xf '" + out.string() + "' Distribution -C '" +
                            extract.string() + "' 2>/dev/null";
    REQUIRE(std::system(cmd.c_str()) == 0);
    std::ifstream dist(extract / "Distribution");
    std::stringstream ss;
    ss << dist.rdbuf();
    const std::string xml = ss.str();
    REQUIRE(xml.find("choices-outline") != std::string::npos);
    REQUIRE(xml.find("customize=\"allow\"") != std::string::npos);
    REQUIRE(xml.find("Audio Unit (AU)") != std::string::npos);
    REQUIRE(xml.find("VST3") != std::string::npos);
    int choices = 0;
    for (size_t p = 0; (p = xml.find("<choice ", p)) != std::string::npos; p += 8)
        ++choices;
    REQUIRE(choices == 2);
}
#endif

#ifdef __APPLE__
TEST_CASE("codesign parsers tolerate noisy command output",
          "[ship][codesign]") {
    auto info = detail::parse_codesign_details(
        "Executable=/tmp/Pulp.app\n"
        "Authority=Developer ID Application: Pulp Labs (TEAM123456)\n"
        "Authority=Apple Root CA\n"
        "TeamIdentifier=TEAM123456\n");
    REQUIRE(info.identity == "Developer ID Application: Pulp Labs (TEAM123456)");
    REQUIRE(info.team_id == "TEAM123456");

    auto submit_id = detail::parse_notarytool_submit_id(
        "Conducting pre-submission checks\n"
        "  id: 12345678-90ab-CDEF-1234-567890abcdef\n"
        "status: In Progress\n");
    REQUIRE(submit_id.has_value());
    REQUIRE(*submit_id == "12345678-90ab-CDEF-1234-567890abcdef");

    REQUIRE_FALSE(detail::parse_notarytool_submit_id("id: not-a-uuid").has_value());

    auto accepted = detail::parse_notarytool_status("status: Accepted\n");
    REQUIRE(accepted.complete);
    REQUIRE(accepted.success);

    auto rejected = detail::parse_notarytool_status("status: Rejected\n");
    REQUIRE(rejected.complete);
    REQUIRE_FALSE(rejected.success);

    auto identities = detail::parse_signing_identities(
        "  1) ABCDEF \"Developer ID Application: Pulp Labs (TEAM123456)\"\n"
        "  2) 012345 \"Apple Development: CI Bot (TEAM123456)\"\n"
        "     2 valid identities found\n");
    REQUIRE(identities.size() == 2);
    REQUIRE(identities[0] == "Developer ID Application: Pulp Labs (TEAM123456)");
    REQUIRE(identities[1] == "Apple Development: CI Bot (TEAM123456)");
}
#endif

TEST_CASE("default audio entitlements keep hardened runtime permissions narrow",
          "[ship][codesign]") {
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
          "[ship][codesign]") {
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
          "[ship][codesign]") {
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

    auto upper = pulp::ship::detail::parse_notarytool_submit_id(
        "id: ABCDEFAB-1234-5678-9ABC-DEF012345678\nstatus: Accepted");
    REQUIRE(upper.has_value());
    REQUIRE(*upper == "ABCDEFAB-1234-5678-9ABC-DEF012345678");

    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id("id: abc").has_value());
    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id(
        "id: abcdefab-1234-5678-9abc-def012345678-extra").has_value());
    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id(
        "id: abcdefab-1234-5678-9abc-def01234567g").has_value());
    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id("id: NOT-A-UUID").has_value());
    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id("status: Invalid").has_value());
#endif
}

TEST_CASE("notarytool submit parser keeps UUID boundaries strict",
          "[ship][codesign]") {
#ifdef __APPLE__
    auto tab_boundary = pulp::ship::detail::parse_notarytool_submit_id(
        "id: 12345678-abcd-4abc-9def-123456789abc\tstatus: Accepted");
    REQUIRE(tab_boundary.has_value());
    REQUIRE(*tab_boundary == "12345678-abcd-4abc-9def-123456789abc");

    auto trailing_newline = pulp::ship::detail::parse_notarytool_submit_id(
        "createdDate: today\nid: 87654321-abcd-4abc-9def-123456789abc\n");
    REQUIRE(trailing_newline.has_value());
    REQUIRE(*trailing_newline == "87654321-abcd-4abc-9def-123456789abc");

    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id(
        "request id: 12345678-abcd-4abc-9def-123456789abc").has_value());
    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id(
        "id:12345678-abcd-4abc-9def-123456789abc").has_value());
    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id(
        "id: 12345678-abcd-4abc-9def-123456789abc-extra").has_value());
    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id(
        "id: 12345678-abcd-4abc-9def-123456789ab").has_value());
#endif
}

TEST_CASE("notarytool status parser classifies terminal states",
          "[ship][codesign]") {
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

TEST_CASE("notarytool status parser preserves raw diagnostic output",
          "[ship][codesign]") {
#ifdef __APPLE__
    const std::string invalid_log = R"(status: Invalid
issues:
  - path: Pulp.vst3
    message: unsigned nested helper
)";
    auto invalid = pulp::ship::detail::parse_notarytool_status(invalid_log);
    REQUIRE(invalid.complete);
    REQUIRE_FALSE(invalid.success);
    REQUIRE(invalid.message == invalid_log);
    REQUIRE(invalid.message.find("unsigned nested helper") != std::string::npos);

    const std::string uploaded_log = "status: Uploaded\nid: pending";
    auto uploaded = pulp::ship::detail::parse_notarytool_status(uploaded_log);
    REQUIRE_FALSE(uploaded.complete);
    REQUIRE_FALSE(uploaded.success);
    REQUIRE(uploaded.message == uploaded_log);
#endif
}

TEST_CASE("security identity parser preserves quoted display names",
          "[ship][codesign]") {
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

TEST_CASE("security identity parser ignores unquoted validity summaries",
          "[ship][codesign]") {
#ifdef __APPLE__
    auto identities = pulp::ship::detail::parse_signing_identities(R"IDENTITIES(
Policy: Code Signing
  1) ABCDEFABCDEFABCDEFABCDEFABCDEFABCDEFABCD "Developer ID Application: Pulp, Inc. (ABCDE12345)"
  2) FEDCBAFEDCBAFEDCBAFEDCBAFEDCBAFEDCBAFEDC "Apple Development: Local Tester (VWXYZ98765)"
     2 valid identities found
     0 revoked identities found
)IDENTITIES");
    REQUIRE(identities.size() == 2);
    REQUIRE(identities[0] == "Developer ID Application: Pulp, Inc. (ABCDE12345)");
    REQUIRE(identities[1] == "Apple Development: Local Tester (VWXYZ98765)");

    auto quoted_summary = pulp::ship::detail::parse_signing_identities(
        "     \"summary in quotes\" should be preserved by the low-level parser\n");
    REQUIRE(quoted_summary.size() == 1);
    REQUIRE(quoted_summary[0] == "summary in quotes");
#endif
}

TEST_CASE("mac codesign parsers ignore unrelated command noise",
          "[ship][codesign]") {
#ifdef __APPLE__
    auto details = pulp::ship::detail::parse_codesign_details(R"DETAILS(
warning: unable to build chain to self-signed root
Executable=/tmp/Pulp.app/Contents/MacOS/Pulp
Authority=Developer ID Application: Pulp Audio LLC (ABCDE12345)
note: some unrelated diagnostic
TeamIdentifier=ABCDE12345
)DETAILS");
    REQUIRE(details.identity == "Developer ID Application: Pulp Audio LLC (ABCDE12345)");
    REQUIRE(details.team_id == "ABCDE12345");

    REQUIRE_FALSE(pulp::ship::detail::parse_notarytool_submit_id(R"NOTARY(
id = 12345678-abcd-4abc-9def-123456789abc
status: Accepted
)NOTARY").has_value());

    auto pending = pulp::ship::detail::parse_notarytool_status("status: Uploaded\nstatusSummary: In progress");
    REQUIRE_FALSE(pending.complete);
    REQUIRE_FALSE(pending.success);
    REQUIRE(pending.message.find("Uploaded") != std::string::npos);
#endif
}
