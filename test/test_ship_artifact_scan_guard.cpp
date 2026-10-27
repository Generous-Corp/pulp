// Unit tests for tools/cli/ship_artifact_scan_guard.hpp: the pure parts of the
// ship-time release-artifact scan, proven against fixture caches, provenance
// text and byte headers rather than a built plug-in.

#include "tools/cli/ship_artifact_scan_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::cli;

namespace {

fs::path temp_dir(const std::string& tag) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto dir = fs::temp_directory_path() / ("pulp-ship-scan-" + tag + "-" + std::to_string(stamp));
    fs::create_directories(dir);
    return dir;
}

void write(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    out << bytes;
}

bool has(const std::vector<std::string>& gates, std::string_view gate) {
    for (const auto& g : gates)
        if (g == gate)
            return true;
    return false;
}

const std::string kMachO64 = std::string("\xCF\xFA\xED\xFE", 4) + "rest of a thin image";
const std::string kFat = std::string("\xCA\xFE\xBA\xBE", 4) + "fat";
const std::string kElf = std::string("\x7F"
                                     "ELF",
                                     4) +
                         "elf";
const std::string kPe = "MZ\x90\x00 pe";

} // namespace

TEST_CASE("release scan gates come only from options the cache recorded", "[ship][artifact-scan]") {
    CHECK(release_scan_gates_from_cache("").empty());
    CHECK(release_scan_gates_from_cache("CMAKE_BUILD_TYPE:STRING=Release\n").empty());

    const auto gates = release_scan_gates_from_cache(
        "PULP_ENABLE_JS:BOOL=OFF\nPULP_TRACING:BOOL=OFF\nPULP_DEV_BUILD:BOOL=OFF\n"
        "PULP_DEV_BRIDGE_EXEC:BOOL=OFF\n");
    CHECK(has(gates, kGateJsOff));
    CHECK(has(gates, kGateTracingOff));
    CHECK(has(gates, kGateReloadShip));
    CHECK(has(gates, kGateDevEnvOff));
    CHECK(has(gates, kGateBridgeExecOff));
    CHECK(gates.size() == 5);

    // An option that is ON gates nothing, in every truthy spelling.
    for (const char* on : {"ON", "TRUE", "1", "yes", "Y"}) {
        INFO(on);
        CHECK(release_scan_gates_from_cache("PULP_TRACING:BOOL=" + std::string(on) + "\n").empty());
    }
}

TEST_CASE("an explicit watcher or bridge-exec entry wins over its profile fallback",
          "[ship][artifact-scan]") {
    // A dev build whose watcher was explicitly compiled out still scans for it.
    auto gates =
        release_scan_gates_from_cache("PULP_DEV_BUILD:BOOL=ON\nPULP_RELOAD_DEV_WATCHER:STRING=0\n");
    CHECK(has(gates, kGateReloadShip));
    CHECK_FALSE(has(gates, kGateDevEnvOff));
    // A release build that explicitly re-enabled the watcher must not forbid it.
    gates = release_scan_gates_from_cache(
        "PULP_DEV_BUILD:BOOL=OFF\nPULP_RELOAD_DEV_WATCHER:STRING=1\n");
    CHECK_FALSE(has(gates, kGateReloadShip));
    CHECK(has(gates, kGateDevEnvOff));
    // The same precedence for the exec bridge.
    gates = release_scan_gates_from_cache(
        "PULP_DEV_BRIDGE_EXEC:BOOL=OFF\nPULP_BRIDGE_EXEC_ENABLED:STRING=1\n");
    CHECK_FALSE(has(gates, kGateBridgeExecOff));
    gates = release_scan_gates_from_cache(
        "PULP_DEV_BRIDGE_EXEC:BOOL=ON\nPULP_BRIDGE_EXEC_ENABLED:STRING=0\n");
    CHECK(has(gates, kGateBridgeExecOff));
}

TEST_CASE("SDK provenance supplies the gates a consumer's cache never records",
          "[ship][artifact-scan]") {
    const std::string release =
        "{\n  \"schema\": \"pulp.sdk-provenance.v1\",\n  \"bridge_exec\": false,\n"
        "  \"build_profile\": \"release\"\n}\n";
    auto gates = release_scan_gates_from_provenance(release);
    CHECK(has(gates, kGateBridgeExecOff));
    CHECK(has(gates, kGateDevEnvOff));
    CHECK(has(gates, kGateReloadShip));
    CHECK(gates.size() == 3);

    const std::string dev = "{\"bridge_exec\": true, \"build_profile\": \"dev\"}";
    CHECK(release_scan_gates_from_provenance(dev).empty());

    // A stamp that predates build_profile still proves the exec bridge.
    CHECK(release_scan_gates_from_provenance("{\"bridge_exec\": false}") ==
          std::vector<std::string>{std::string(kGateBridgeExecOff)});
    CHECK(release_scan_gates_from_provenance("not json").empty());

    // The union keeps each gate once, cache order first.
    const auto merged =
        release_scan_gates("PULP_TRACING:BOOL=OFF\nPULP_DEV_BRIDGE_EXEC:BOOL=OFF\n", release);
    CHECK(merged ==
          std::vector<std::string>{std::string(kGateTracingOff), std::string(kGateBridgeExecOff),
                                   std::string(kGateDevEnvOff), std::string(kGateReloadShip)});
    CHECK(release_scan_gates("PULP_TRACING:BOOL=OFF\n", std::nullopt) ==
          std::vector<std::string>{std::string(kGateTracingOff)});
}

TEST_CASE("the scanner is found beside the source tree, then in the SDK, else nowhere",
          "[ship][artifact-scan]") {
    const auto root = temp_dir("script");
    const auto source = root / "src";
    const auto sdk = root / "sdk";
    const std::string cache = "CMAKE_HOME_DIRECTORY:INTERNAL=" + source.string() + "\n" +
                              "Pulp_DIR:PATH=" + (sdk / "lib" / "cmake" / "Pulp").string() + "\n";
    CHECK_FALSE(release_scan_script(cache).has_value());

    // The script alone is not enough: the list must sit beside it.
    write(sdk / "lib" / "cmake" / "Pulp" / std::string(kReleaseScanScriptName), "# scanner\n");
    CHECK_FALSE(release_scan_script(cache).has_value());
    write(sdk / "lib" / "cmake" / "Pulp" / std::string(kReleaseScanListName), "{}\n");
    REQUIRE(release_scan_script(cache).has_value());
    CHECK(*release_scan_script(cache) ==
          sdk / "lib" / "cmake" / "Pulp" / std::string(kReleaseScanScriptName));

    write(source / "tools" / "cmake" / std::string(kReleaseScanScriptName), "# scanner\n");
    write(source / "tools" / "cmake" / std::string(kReleaseScanListName), "{}\n");
    REQUIRE(release_scan_script(cache).has_value());
    CHECK(*release_scan_script(cache) ==
          source / "tools" / "cmake" / std::string(kReleaseScanScriptName));

    CHECK(sdk_prefix_from_cache(cache) == sdk);
    CHECK_FALSE(sdk_prefix_from_cache("Pulp_DIR:PATH=Pulp_DIR-NOTFOUND\n").has_value());
    CHECK_FALSE(sdk_prefix_from_cache("").has_value());

    write(sdk / std::string(kSdkProvenanceName), "{\"bridge_exec\": false}\n");
    REQUIRE(read_sdk_provenance(cache).has_value());
    CHECK(release_scan_gates_from_provenance(*read_sdk_provenance(cache)) ==
          std::vector<std::string>{std::string(kGateBridgeExecOff)});

    // The recorded cmake is used when it exists; otherwise PATH's.
    CHECK(release_scan_cmake("CMAKE_COMMAND:INTERNAL=" + (root / "missing-cmake").string() +
                             "\n") == "cmake");
    write(root / "cmake", "#!/bin/sh\n");
    CHECK(release_scan_cmake("CMAKE_COMMAND:INTERNAL=" + (root / "cmake").string() + "\n") ==
          (root / "cmake").string());
    fs::remove_all(root);
}

TEST_CASE("native binaries are recognized by their image header", "[ship][artifact-scan]") {
    CHECK(looks_like_native_binary(kMachO64));
    CHECK(looks_like_native_binary(std::string("\xFE\xED\xFA\xCE", 4)));
    CHECK(looks_like_native_binary(std::string("\xFE\xED\xFA\xCF", 4)));
    CHECK(looks_like_native_binary(std::string("\xCE\xFA\xED\xFE", 4)));
    CHECK(looks_like_native_binary(kFat));
    CHECK(looks_like_native_binary(std::string("\xCA\xFE\xBA\xBF", 4)));
    CHECK(looks_like_native_binary(kElf));
    CHECK(looks_like_native_binary(kPe));
    CHECK_FALSE(looks_like_native_binary("<?xml version=\"1.0\"?>"));
    CHECK_FALSE(looks_like_native_binary("PK\x03\x04"));
    CHECK_FALSE(looks_like_native_binary(""));
    CHECK_FALSE(looks_like_native_binary("MZ"));
}

TEST_CASE("a bundle reduces to its native binaries and a loose file to itself",
          "[ship][artifact-scan]") {
    const auto root = temp_dir("targets");
    const auto bundle = root / "Gain.vst3";
    write(bundle / "Contents" / "Info.plist", "<?xml version=\"1.0\"?><plist/>");
    write(bundle / "Contents" / "MacOS" / "Gain", kMachO64);
    write(bundle / "Contents" / "Frameworks" / "Helper.dylib", kFat);
    write(bundle / "Contents" / "Resources" / "ui.js", "export default 1;");
    write(bundle / "Contents" / "Resources" / "notes.txt",
          "MZ is not a header when it is text? It is: refuse by magic.");

    auto targets = release_scan_targets(bundle);
    REQUIRE(targets.size() == 3); // the two images plus the "MZ" text, which the magic admits
    CHECK(targets[0] == bundle / "Contents" / "Frameworks" / "Helper.dylib");
    CHECK(targets[1] == bundle / "Contents" / "MacOS" / "Gain");
    CHECK(targets[2] == bundle / "Contents" / "Resources" / "notes.txt");

    // A loose native file is its own target; a loose text file is nothing.
    const auto loose = root / "libplugin.so";
    write(loose, kElf);
    CHECK(release_scan_targets(loose) == std::vector<fs::path>{loose});
    CHECK(release_scan_targets(bundle / "Contents" / "Info.plist").empty());
    CHECK(release_scan_targets(root / "absent").empty());

    // A symlink is never followed, so an alias cannot point the scan outside.
    std::error_code ec;
    fs::create_directory_symlink(root, bundle / "Contents" / "escape", ec);
    if (!ec) {
        CHECK(release_scan_targets(bundle).size() == 3);
        fs::create_symlink(loose, bundle / "Contents" / "MacOS" / "alias", ec);
        if (!ec)
            CHECK(release_scan_targets(bundle).size() == 3);
    }
    fs::remove_all(root);
}

TEST_CASE("the scan argv names every binary, the gates and the forbid mode",
          "[ship][artifact-scan]") {
    const auto argv =
        release_scan_argv("/opt/cmake", "/sdk/lib/cmake/Pulp/PulpReleaseArtifactScan.cmake",
                          {"tracing-off", "dev-env-off"},
                          {"/b/Gain.vst3/Contents/MacOS/Gain", "/b/x.so"}, "pulp ship sign");
    CHECK(argv == std::vector<std::string>{
                      "/opt/cmake", "-DBINS=/b/Gain.vst3/Contents/MacOS/Gain;/b/x.so",
                      "-DMODE=forbid", "-DGATES=tracing-off;dev-env-off", "-DLABEL=pulp ship sign",
                      "-P", "/sdk/lib/cmake/Pulp/PulpReleaseArtifactScan.cmake"});
}
