#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
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
