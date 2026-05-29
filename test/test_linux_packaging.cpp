#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <pulp/ship/codesign.hpp>
#include <pulp/ship/installer.hpp>
#include <pulp/platform/child_process.hpp>

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;

namespace {

struct TempDir {
    fs::path path;

    explicit TempDir(const char* stem) {
        path = fs::temp_directory_path()
             / (std::string("pulp-linux-package-") + stem + "-"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(path);
    }

    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << body;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

pulp::platform::ProcessResult run_sh(const std::string& command) {
    return pulp::platform::exec("/bin/sh", {"-c", command}, 60000);
}

bool command_available(const std::string& command) {
    return run_sh("command -v " + command + " >/dev/null 2>&1").exit_code == 0;
}

std::string quote(const fs::path& path) {
    return "'" + path.string() + "'";
}

} // namespace

TEST_CASE("Linux packaging exposes no-op signing surface honestly",
          "[ship][linux-package][coverage]") {
    auto signing = pulp::ship::check_codesign("/tmp/missing");
    REQUIRE_FALSE(signing.is_signed);
    REQUIRE_FALSE(signing.is_valid);
    REQUIRE_FALSE(signing.is_notarized);
    REQUIRE(signing.identity.empty());
    REQUIRE(signing.team_id.empty());
    REQUIRE(signing.error.empty());

    REQUIRE_FALSE(pulp::ship::check_notarization("/tmp/missing"));
    REQUIRE_FALSE(pulp::ship::codesign("/tmp/missing", "identity", ""));
    REQUIRE_FALSE(pulp::ship::codesign("/tmp/missing", "identity", "entitlements.plist"));
    REQUIRE_FALSE(pulp::ship::notarize_submit("/tmp/missing", "apple", "team", "password").has_value());
    REQUIRE_FALSE(pulp::ship::notarize_submit_asc("/tmp/missing",
                                                  "/tmp/AuthKey_TEST.p8",
                                                  "TESTKEY123",
                                                  "12345678-1234-1234-1234-123456789abc")
                  .has_value());

    auto status = pulp::ship::notarize_check("request-id");
    REQUIRE_FALSE(status.complete);
    REQUIRE_FALSE(status.success);
    REQUIRE(status.message.empty());

    REQUIRE_FALSE(pulp::ship::notarize_staple("/tmp/missing"));
    REQUIRE(pulp::ship::list_signing_identities().empty());
    REQUIRE(pulp::ship::default_audio_entitlements().empty());
}

TEST_CASE("Linux packaging rejects macOS installer entry points",
          "[ship][linux-package][coverage]") {
    REQUIRE_FALSE(pulp::ship::create_pkg("component", "out.pkg", "com.pulp.test", "1.0.0", ""));
    REQUIRE_FALSE(pulp::ship::create_dmg("source", "out.dmg", "Volume"));

    std::vector<pulp::ship::InstallComponent> components = {
        {.path = "Plugin.vst3", .install_location = "/Library/Audio/Plug-Ins/VST3"},
    };
    REQUIRE_FALSE(pulp::ship::create_combined_pkg(components, "out.pkg",
                                                  "com.pulp.test", "1.0.0", ""));
    REQUIRE_FALSE(pulp::ship::create_combined_pkg({}, "out.pkg",
                                                  "com.pulp.test", "1.0.0", "Developer ID"));
}

TEST_CASE("Linux packaging creates tarballs with only present plugin formats",
          "[ship][linux-package][coverage]") {
    TempDir temp("tarball");
    auto build = temp.path / "build";
    write_file(build / "VST3" / "Echo.vst3" / "Contents" / "module.txt", "vst3");
    write_file(build / "CLAP" / "Echo.clap", "clap");
    write_file(build / "ignored.txt", "ignore");

    auto output = temp.path / "plugins.tar.gz";
    REQUIRE(pulp::ship::create_tar_gz("Echo", build.string(), output.string()));
    REQUIRE(fs::is_regular_file(output));
    REQUIRE(fs::file_size(output) > 0);

    auto listing = run_sh("tar tzf " + quote(output));
    REQUIRE(listing.exit_code == 0);
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("VST3/Echo.vst3/Contents/module.txt"));
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("CLAP/Echo.clap"));
    REQUIRE(listing.stdout_output.find("LV2/") == std::string::npos);
    REQUIRE(listing.stdout_output.find("ignored.txt") == std::string::npos);
}

TEST_CASE("Linux packaging tarball path fails closed without plugin formats",
          "[ship][linux-package][coverage]") {
    TempDir temp("empty-tarball");
    auto build = temp.path / "build";
    fs::create_directories(build);
    write_file(build / "notes.txt", "not a plugin bundle");

    auto output = temp.path / "empty.tar.gz";
    REQUIRE_FALSE(pulp::ship::create_tar_gz("Empty", build.string(), output.string()));
    REQUIRE_FALSE(fs::exists(output));
}

TEST_CASE("Linux packaging tarballs support LV2-only bundles",
          "[ship][linux-package][coverage]") {
    TempDir temp("lv2-tarball");
    auto build = temp.path / "build";
    write_file(build / "LV2" / "Pad.lv2" / "manifest.ttl", "manifest");
    write_file(build / "LV2" / "Pad.lv2" / "dsp.so", "binary");
    write_file(build / "Docs" / "readme.txt", "not packaged");

    auto output = temp.path / "lv2.tar.gz";
    REQUIRE(pulp::ship::create_tar_gz("Pad", build.string(), output.string()));
    REQUIRE(fs::is_regular_file(output));
    REQUIRE(fs::file_size(output) > 0);

    auto listing = run_sh("tar tzf " + quote(output));
    REQUIRE(listing.exit_code == 0);
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("LV2/Pad.lv2/manifest.ttl"));
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("LV2/Pad.lv2/dsp.so"));
    REQUIRE(listing.stdout_output.find("VST3/") == std::string::npos);
    REQUIRE(listing.stdout_output.find("CLAP/") == std::string::npos);
    REQUIRE(listing.stdout_output.find("Docs/readme.txt") == std::string::npos);

    auto extract = temp.path / "extract";
    fs::create_directories(extract);
    auto unpack = run_sh("tar xzf " + quote(output) + " -C " + quote(extract));
    REQUIRE(unpack.exit_code == 0);
    REQUIRE(read_file(extract / "LV2" / "Pad.lv2" / "manifest.ttl") == "manifest");
    REQUIRE(read_file(extract / "LV2" / "Pad.lv2" / "dsp.so") == "binary");
    REQUIRE_FALSE(fs::exists(extract / "Docs" / "readme.txt"));
}

TEST_CASE("Linux packaging tarballs preserve nested plugin payloads only",
          "[ship][linux-package][coverage][requested]") {
    TempDir temp("nested-plugin-payloads");
    auto build = temp.path / "build";
    write_file(build / "VST3" / "Nested.vst3" / "Contents" / "Resources" / "preset.json", "preset");
    write_file(build / "CLAP" / "Nested.clap" / "resources" / "skin.png", "png");
    write_file(build / "LV2" / "Nested.lv2" / "resources" / "manifest.ttl", "ttl");
    write_file(build / "vst3" / "lowercase.vst3", "ignored");
    write_file(build / "Other" / "Nested.vst3", "ignored");

    auto output = temp.path / "nested.tar.gz";
    REQUIRE(pulp::ship::create_tar_gz("Nested", build.string(), output.string()));

    auto extract = temp.path / "extract";
    fs::create_directories(extract);
    auto unpack = run_sh("tar xzf " + quote(output) + " -C " + quote(extract));
    REQUIRE(unpack.exit_code == 0);

    REQUIRE(read_file(extract / "VST3" / "Nested.vst3" / "Contents" / "Resources" / "preset.json") == "preset");
    REQUIRE(read_file(extract / "CLAP" / "Nested.clap" / "resources" / "skin.png") == "png");
    REQUIRE(read_file(extract / "LV2" / "Nested.lv2" / "resources" / "manifest.ttl") == "ttl");
    REQUIRE_FALSE(fs::exists(extract / "vst3" / "lowercase.vst3"));
    REQUIRE_FALSE(fs::exists(extract / "Other" / "Nested.vst3"));
}

TEST_CASE("Linux packaging tarballs preserve format directory boundaries",
          "[ship][linux-package][coverage][requested]") {
    TempDir temp("format-boundaries");
    auto build = temp.path / "build";
    write_file(build / "VST3" / "Format.vst3" / "Contents" / "module.txt", "vst3");
    write_file(build / "CLAP" / "Format.clap", "clap");
    write_file(build / "LV2" / "Format.lv2" / "manifest.ttl", "lv2");
    write_file(build / "VST3.backup" / "Old.vst3" / "Contents" / "module.txt", "old");
    write_file(build / "CLAP.tmp" / "Old.clap", "old");
    write_file(build / "LV2-disabled" / "Old.lv2" / "manifest.ttl", "old");

    auto output = temp.path / "format-boundaries.tar.gz";
    REQUIRE(pulp::ship::create_tar_gz("Format", build.string(), output.string()));

    auto listing = run_sh("tar tzf " + quote(output));
    REQUIRE(listing.exit_code == 0);
    REQUIRE_THAT(listing.stdout_output,
                 ContainsSubstring("VST3/Format.vst3/Contents/module.txt"));
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("CLAP/Format.clap"));
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("LV2/Format.lv2/manifest.ttl"));
    REQUIRE(listing.stdout_output.find("VST3.backup") == std::string::npos);
    REQUIRE(listing.stdout_output.find("CLAP.tmp") == std::string::npos);
    REQUIRE(listing.stdout_output.find("LV2-disabled") == std::string::npos);
}

TEST_CASE("Linux packaging tarballs preserve empty declared format directories",
          "[ship][linux-package][coverage][requested]") {
    TempDir temp("empty-format-directories");
    auto build = temp.path / "build";
    fs::create_directories(build / "VST3");
    fs::create_directories(build / "CLAP");
    write_file(build / "Docs" / "ignored.txt", "ignored");

    auto output = temp.path / "empty-formats.tar.gz";
    REQUIRE(pulp::ship::create_tar_gz("EmptyFormats", build.string(), output.string()));
    REQUIRE(fs::is_regular_file(output));
    REQUIRE(fs::file_size(output) > 0);

    auto listing = run_sh("tar tzf " + quote(output));
    REQUIRE(listing.exit_code == 0);
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("VST3/"));
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("CLAP/"));
    REQUIRE(listing.stdout_output.find("LV2/") == std::string::npos);
    REQUIRE(listing.stdout_output.find("Docs/ignored.txt") == std::string::npos);
}

TEST_CASE("Linux packaging tarball command quotes paths with spaces",
          "[ship][linux-package][coverage]") {
    TempDir temp("space-tarball");
    auto build = temp.path / "build with spaces";
    write_file(build / "VST3" / "Space Echo.vst3" / "Contents" / "module.txt", "vst3");
    write_file(build / "CLAP" / "Space Echo.clap", "clap");
    write_file(build / "LV2" / "Space Echo.lv2" / "manifest.ttl", "lv2");
    write_file(build / "Docs" / "readme.txt", "not packaged");

    auto output = temp.path / "plugins with spaces.tar.gz";
    REQUIRE(pulp::ship::create_tar_gz("Space Echo", build.string(), output.string()));
    REQUIRE(fs::is_regular_file(output));
    REQUIRE(fs::file_size(output) > 0);

    auto listing = run_sh("tar tzf " + quote(output));
    REQUIRE(listing.exit_code == 0);
    REQUIRE_THAT(listing.stdout_output,
                 ContainsSubstring("VST3/Space Echo.vst3/Contents/module.txt"));
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("CLAP/Space Echo.clap"));
    REQUIRE_THAT(listing.stdout_output, ContainsSubstring("LV2/Space Echo.lv2/manifest.ttl"));
    REQUIRE(listing.stdout_output.find("Docs/readme.txt") == std::string::npos);

    auto extract = temp.path / "extract with spaces";
    fs::create_directories(extract);
    auto unpack = run_sh("tar xzf " + quote(output) + " -C " + quote(extract));
    REQUIRE(unpack.exit_code == 0);
    REQUIRE(read_file(extract / "VST3" / "Space Echo.vst3" / "Contents" / "module.txt") == "vst3");
    REQUIRE(read_file(extract / "CLAP" / "Space Echo.clap") == "clap");
    REQUIRE(read_file(extract / "LV2" / "Space Echo.lv2" / "manifest.ttl") == "lv2");
}

TEST_CASE("Linux packaging tarball failures do not leave partial archives",
          "[ship][linux-package][coverage]") {
    TempDir temp("tarball-failures");

    auto missing_build_output = temp.path / "missing-build.tar.gz";
    REQUIRE_FALSE(pulp::ship::create_tar_gz("Missing",
                                            (temp.path / "missing-build").string(),
                                            missing_build_output.string()));
    REQUIRE_FALSE(fs::exists(missing_build_output));

    auto build = temp.path / "build";
    write_file(build / "VST3" / "Broken.vst3" / "Contents" / "module.txt", "vst3");

    auto missing_parent_output = temp.path / "missing-parent" / "plugins.tar.gz";
    REQUIRE_FALSE(pulp::ship::create_tar_gz("Broken", build.string(),
                                            missing_parent_output.string()));
    REQUIRE_FALSE(fs::exists(missing_parent_output));
    REQUIRE(fs::is_regular_file(build / "VST3" / "Broken.vst3" / "Contents" / "module.txt"));
}

TEST_CASE("Linux packaging builds deb archives and removes staging",
          "[ship][linux-package][coverage]") {
    if (!command_available("dpkg-deb"))
        SKIP("dpkg-deb is required to inspect generated deb archives");

    TempDir temp("deb");
    auto build = temp.path / "build";
    write_file(build / "VST3" / "Meter.vst3" / "Contents" / "module.txt", "vst3");
    write_file(build / "CLAP" / "Meter.clap", "clap");
    write_file(build / "LV2" / "Meter.lv2" / "manifest.ttl", "lv2");

    auto output = temp.path / "meter.deb";
    REQUIRE(pulp::ship::create_deb("pulp-meter", "1.2.3", build.string(),
                                   output.string(), "Pulp Audio"));
    REQUIRE(fs::is_regular_file(output));
    REQUIRE(fs::file_size(output) > 0);
    REQUIRE_FALSE(fs::exists(build / "deb-staging"));

    auto control = run_sh("dpkg-deb --field " + quote(output));
    REQUIRE(control.exit_code == 0);
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Package: pulp-meter"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Version: 1.2.3"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Architecture: amd64"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Maintainer: Pulp Audio"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Section: sound"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Priority: optional"));

    auto contents = run_sh("dpkg-deb --contents " + quote(output));
    REQUIRE(contents.exit_code == 0);
    REQUIRE_THAT(contents.stdout_output, ContainsSubstring("./usr/lib/vst3/Meter.vst3/Contents/module.txt"));
    REQUIRE_THAT(contents.stdout_output, ContainsSubstring("./usr/lib/clap/Meter.clap"));
    REQUIRE_THAT(contents.stdout_output, ContainsSubstring("./usr/lib/lv2/Meter.lv2/manifest.ttl"));
}

TEST_CASE("Linux packaging deb archives can carry metadata without plugin dirs",
          "[ship][linux-package][coverage]") {
    if (!command_available("dpkg-deb"))
        SKIP("dpkg-deb is required to inspect generated deb archives");

    TempDir temp("metadata-only-deb");
    auto build = temp.path / "build";
    fs::create_directories(build);
    write_file(build / "notes.txt", "not installed");

    auto output = temp.path / "metadata.deb";
    REQUIRE(pulp::ship::create_deb("pulp-empty", "0.0.1", build.string(),
                                   output.string(), "Pulp Tests"));
    REQUIRE(fs::is_regular_file(output));
    REQUIRE(fs::file_size(output) > 0);
    REQUIRE_FALSE(fs::exists(build / "deb-staging"));

    auto control = run_sh("dpkg-deb --field " + quote(output));
    REQUIRE(control.exit_code == 0);
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Package: pulp-empty"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Version: 0.0.1"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Maintainer: Pulp Tests"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Description: pulp-empty audio plugin"));

    auto contents = run_sh("dpkg-deb --contents " + quote(output));
    REQUIRE(contents.exit_code == 0);
    REQUIRE(contents.stdout_output.find("./usr/lib/vst3/") == std::string::npos);
    REQUIRE(contents.stdout_output.find("./usr/lib/clap/") == std::string::npos);
    REQUIRE(contents.stdout_output.find("./usr/lib/lv2/") == std::string::npos);
    REQUIRE(contents.stdout_output.find("notes.txt") == std::string::npos);
}

TEST_CASE("Linux packaging deb failures remove staging and preserve inputs",
          "[ship][linux-package][coverage]") {
    if (!command_available("dpkg-deb"))
        SKIP("dpkg-deb is required to inspect generated deb archives");

    TempDir temp("deb-failure-cleanup");
    auto build = temp.path / "build";
    write_file(build / "CLAP" / "Failure.clap", "clap");

    auto output = temp.path / "missing-parent" / "failure.deb";
    REQUIRE_FALSE(pulp::ship::create_deb("pulp-failure", "1.0.0", build.string(),
                                         output.string(), "Pulp Tests"));
    REQUIRE_FALSE(fs::exists(output));
    REQUIRE_FALSE(fs::exists(build / "deb-staging"));
    REQUIRE(fs::is_regular_file(build / "CLAP" / "Failure.clap"));
    REQUIRE(read_file(build / "CLAP" / "Failure.clap") == "clap");
}

TEST_CASE("Linux packaging deb generation removes stale staging before writing",
          "[ship][linux-package][coverage][requested]") {
    if (!command_available("dpkg-deb"))
        SKIP("dpkg-deb is required to inspect generated deb archives");

    TempDir temp("deb-stale-staging");
    auto build = temp.path / "build";
    write_file(build / "deb-staging" / "stale.txt", "must be removed");
    write_file(build / "VST3" / "Fresh.vst3" / "Contents" / "module.txt", "fresh");

    auto output = temp.path / "fresh.deb";
    REQUIRE(pulp::ship::create_deb("pulp-fresh", "9.8.7", build.string(),
                                   output.string(), "Pulp Tests"));
    REQUIRE_FALSE(fs::exists(build / "deb-staging"));

    auto contents = run_sh("dpkg-deb --contents " + quote(output));
    REQUIRE(contents.exit_code == 0);
    REQUIRE_THAT(contents.stdout_output,
                 ContainsSubstring("./usr/lib/vst3/Fresh.vst3/Contents/module.txt"));
    REQUIRE(contents.stdout_output.find("stale.txt") == std::string::npos);
}

TEST_CASE("Linux packaging deb preserves recursive plugin directories only",
          "[ship][linux-package][coverage][requested]") {
    if (!command_available("dpkg-deb"))
        SKIP("dpkg-deb is required to inspect generated deb archives");

    TempDir temp("deb-recursive-plugin-dirs");
    auto build = temp.path / "build";
    write_file(build / "VST3" / "Nested.vst3" / "Contents" / "Resources" / "preset.json", "preset");
    write_file(build / "CLAP" / "Nested.clap" / "resources" / "skin.png", "png");
    write_file(build / "LV2" / "Nested.lv2" / "resources" / "manifest.ttl", "ttl");
    write_file(build / "vst3" / "lowercase.vst3", "ignored");
    write_file(build / "Docs" / "manual.txt", "ignored");

    auto output = temp.path / "nested.deb";
    REQUIRE(pulp::ship::create_deb("pulp-nested", "2.0.0", build.string(),
                                   output.string(), "Pulp Tests"));
    REQUIRE_FALSE(fs::exists(build / "deb-staging"));

    auto contents = run_sh("dpkg-deb --contents " + quote(output));
    REQUIRE(contents.exit_code == 0);
    REQUIRE_THAT(contents.stdout_output,
                 ContainsSubstring("./usr/lib/vst3/Nested.vst3/Contents/Resources/preset.json"));
    REQUIRE_THAT(contents.stdout_output,
                 ContainsSubstring("./usr/lib/clap/Nested.clap/resources/skin.png"));
    REQUIRE_THAT(contents.stdout_output,
                 ContainsSubstring("./usr/lib/lv2/Nested.lv2/resources/manifest.ttl"));
    REQUIRE(contents.stdout_output.find("lowercase.vst3") == std::string::npos);
    REQUIRE(contents.stdout_output.find("manual.txt") == std::string::npos);
}

TEST_CASE("Linux packaging deb control keeps package metadata literal",
          "[ship][linux-package][coverage][requested]") {
    if (!command_available("dpkg-deb"))
        SKIP("dpkg-deb is required to inspect generated deb archives");

    TempDir temp("deb-metadata-literal");
    auto build = temp.path / "build";
    write_file(build / "CLAP" / "Literal.clap", "clap");

    auto output = temp.path / "literal.deb";
    REQUIRE(pulp::ship::create_deb("pulp-literal", "2026.5.28+coverage",
                                   build.string(), output.string(),
                                   "Pulp Tests"));
    REQUIRE_FALSE(fs::exists(build / "deb-staging"));

    auto control = run_sh("dpkg-deb --field " + quote(output));
    REQUIRE(control.exit_code == 0);
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Package: pulp-literal"));
    REQUIRE_THAT(control.stdout_output,
                 ContainsSubstring("Version: 2026.5.28+coverage"));
    REQUIRE_THAT(control.stdout_output, ContainsSubstring("Maintainer: Pulp Tests"));
    REQUIRE_THAT(control.stdout_output,
                 ContainsSubstring("Description: pulp-literal audio plugin"));

    auto contents = run_sh("dpkg-deb --contents " + quote(output));
    REQUIRE(contents.exit_code == 0);
    REQUIRE_THAT(contents.stdout_output, ContainsSubstring("./usr/lib/clap/Literal.clap"));
    REQUIRE(contents.stdout_output.find("./usr/lib/vst3/") == std::string::npos);
    REQUIRE(contents.stdout_output.find("./usr/lib/lv2/") == std::string::npos);
}
