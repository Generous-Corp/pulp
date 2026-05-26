// test_cli_mac_runtime_validators.cpp — Phase 5 chainer plan
//
// Hermetic unit tests for `pulp validate --target {standalone|auv3|macho}`
// (see planning/2026-05-24-linux-macos-chainer-gap-closure-plan.md).
// Uses the MacValidatorEnv injection seam to stub every shell-out and
// filesystem probe, so these tests run on any host — including Linux
// CI legs — without needing real .app/.appex/auval.

#include <catch2/catch_test_macros.hpp>

#include "../tools/cli/mac_runtime_validators.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;
namespace mr = pulp::cli::mac_runtime;

namespace {

struct StubEnv {
    std::unordered_set<std::string> existing_paths;
    std::unordered_map<std::string, std::string> path_to_executable;
    std::unordered_map<std::string, std::pair<int, std::string>> cmd_results;
    std::vector<std::string> commands_run;
    bool is_apple{true};

    mr::MacValidatorEnv build() {
        mr::MacValidatorEnv e;
        e.is_apple_host = is_apple;
        auto exec_table = path_to_executable;
        e.find_executable = [exec_table](const std::string& name) {
            auto it = exec_table.find(name);
            if (it == exec_table.end()) return std::string{};
            return it->second;
        };
        auto& cmds = commands_run;
        auto results = cmd_results;
        e.run_capture = [&cmds, results](const std::string& cmd)
            -> std::pair<int, std::string> {
            cmds.push_back(cmd);
            for (const auto& [key, val] : results) {
                if (cmd.find(key) != std::string::npos) return val;
            }
            return {0, ""};
        };
        auto exists = existing_paths;
        e.path_exists = [exists](const fs::path& p) {
            return exists.count(p.string()) > 0;
        };
        return e;
    }
};

}  // namespace

TEST_CASE("expand_target_name", "[cli][mac-validators][phase5]") {
    REQUIRE(mr::expand_target_name("standalone")
            == std::vector<std::string>{"standalone"});
    REQUIRE(mr::expand_target_name("auv3")
            == std::vector<std::string>{"auv3"});
    REQUIRE(mr::expand_target_name("macho")
            == std::vector<std::string>{"macho"});
    REQUIRE(mr::expand_target_name("all")
            == std::vector<std::string>{"standalone", "auv3", "macho"});
    REQUIRE(mr::expand_target_name("bogus").empty());
    REQUIRE(mr::expand_target_name("").empty());
}

TEST_CASE("resolve_standalone_executable", "[cli][mac-validators][phase5]") {
    StubEnv s;
    fs::path bundle = "/tmp/Foo.app";
    s.existing_paths.insert((bundle / "Contents" / "MacOS").string());
    s.existing_paths.insert((bundle / "Contents" / "MacOS" / "Foo").string());

    auto env = s.build();
    auto exe = mr::resolve_standalone_executable(bundle, env);
    REQUIRE(exe == bundle / "Contents" / "MacOS" / "Foo");
}

TEST_CASE("resolve_standalone_executable rejects non-app",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    auto env = s.build();
    REQUIRE(mr::resolve_standalone_executable("/tmp/Foo.appex", env).empty());
    REQUIRE(mr::resolve_standalone_executable("/tmp/Foo.component", env).empty());
    REQUIRE(mr::resolve_standalone_executable("", env).empty());
}

TEST_CASE("standalone validator: missing bundle fails",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    auto env = s.build();
    auto r = mr::run_standalone_validator("/tmp/missing.app", env);
    REQUIRE(r.status == "fail");
    REQUIRE(r.summary == "bundle not found");
}

TEST_CASE("standalone validator: skips non-Apple hosts",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    s.is_apple = false;
    auto env = s.build();
    auto r = mr::run_standalone_validator("/tmp/Foo.app", env);
    REQUIRE(r.status == "skip");
    REQUIRE(r.summary == "macOS host required");
}

TEST_CASE("standalone validator: passes when smoke exec returns 0",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    fs::path bundle = "/tmp/Synth.app";
    s.existing_paths.insert(bundle.string());
    s.existing_paths.insert((bundle / "Contents" / "MacOS").string());
    s.existing_paths.insert((bundle / "Contents" / "MacOS" / "Synth").string());
    // Any command that contains the binary path returns success.
    s.cmd_results["Synth"] = {0, ""};
    auto env = s.build();
    auto r = mr::run_standalone_validator(bundle, env);
    REQUIRE(r.status == "pass");
    REQUIRE(r.exit_code == 0);
    REQUIRE(r.target == "standalone");
}

TEST_CASE("standalone validator: surfaces non-zero exit",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    fs::path bundle = "/tmp/Crashy.app";
    s.existing_paths.insert(bundle.string());
    s.existing_paths.insert((bundle / "Contents" / "MacOS").string());
    s.existing_paths.insert((bundle / "Contents" / "MacOS" / "Crashy").string());
    s.cmd_results["Crashy"] = {127, "Library not loaded"};
    auto env = s.build();
    auto r = mr::run_standalone_validator(bundle, env);
    REQUIRE(r.status == "fail");
    REQUIRE(r.exit_code == 127);
    REQUIRE(r.summary.find("Library not loaded") != std::string::npos);
}

TEST_CASE("parse_au_component_tuple", "[cli][mac-validators][phase5]") {
    std::string plist_dump = R"(
        "AudioComponents" => [
            0 => {
                "type" => "aufx"
                "subtype" => "Plpe"
                "manufacturer" => "Plpa"
                "name" => "Pulp Echo"
            }
        ]
    )";
    auto tuple = mr::parse_au_component_tuple(plist_dump);
    REQUIRE(tuple.type == "aufx");
    REQUIRE(tuple.subtype == "Plpe");
    REQUIRE(tuple.manufacturer == "Plpa");
}

TEST_CASE("parse_au_component_tuple handles missing fields",
          "[cli][mac-validators][phase5]") {
    auto tuple = mr::parse_au_component_tuple("nothing here");
    REQUIRE(tuple.type.empty());
    REQUIRE(tuple.subtype.empty());
    REQUIRE(tuple.manufacturer.empty());
}

TEST_CASE("auv3 validator: skips on non-Apple",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    s.is_apple = false;
    auto env = s.build();
    auto r = mr::run_auv3_validator("/tmp/Foo.appex", env);
    REQUIRE(r.status == "skip");
}

TEST_CASE("auv3 validator: pass path through auval",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    // Use the appex directly — the container-walk path uses real
    // fs::directory_iterator, which can't be stubbed via MacValidatorEnv.
    // The container-walk codepath gets a happy-path real-fs assertion
    // below; here we exercise the auval-dispatch path explicitly.
    fs::path appex = "/tmp/Synth.appex";
    s.existing_paths.insert(appex.string());
    s.existing_paths.insert((appex / "Contents" / "Info.plist").string());
    s.path_to_executable["plutil"] = "/usr/bin/plutil";
    s.path_to_executable["pluginkit"] = "/usr/bin/pluginkit";
    s.path_to_executable["auval"] = "/usr/bin/auval";
    s.cmd_results["plutil"] = {0,
        R"("type" => "aufx" "subtype" => "Plpe" "manufacturer" => "Plpa")"};
    s.cmd_results["auval"] = {0, "AU VALIDATION SUCCEEDED"};
    auto env = s.build();
    auto r = mr::run_auv3_validator(appex, env);
    REQUIRE(r.status == "pass");
    REQUIRE(r.tool == "auval");
}

TEST_CASE("auv3 validator: container walk finds embedded appex",
          "[cli][mac-validators][phase5]") {
    // Real-FS variant: build a tiny .app bundle on disk so the
    // directory_iterator inside run_auv3_validator can find the
    // embedded .appex. We make the auval call deterministic by
    // not registering an auval binary in PATH — the test then
    // confirms we got far enough to *attempt* auval (skip with
    // "auval not on PATH"), proving the container walk succeeded.
    static std::atomic<int> seq{0};
    fs::path tmp = fs::temp_directory_path() /
               ("pulp-mac-validator-cont-" +
                std::to_string(seq.fetch_add(1)) + ".app");
    auto plugins = tmp / "Contents" / "PlugIns";
    auto appex = plugins / "Synth.appex";
    fs::create_directories(appex / "Contents");
    std::ofstream(appex / "Contents" / "Info.plist").put('\0');
    StubEnv s;
    s.existing_paths.insert(tmp.string());
    s.existing_paths.insert(plugins.string());
    s.existing_paths.insert(appex.string());
    s.existing_paths.insert((appex / "Contents" / "Info.plist").string());
    s.path_to_executable["plutil"] = "/usr/bin/plutil";
    s.cmd_results["plutil"] = {0,
        R"("type" => "aufx" "subtype" => "Plpe" "manufacturer" => "Plpa")"};
    // No auval registered -> expect skip with auval message
    auto env = s.build();
    auto r = mr::run_auv3_validator(tmp, env);
    REQUIRE(r.status == "skip");
    REQUIRE(r.summary.find("auval") != std::string::npos);
    fs::remove_all(tmp);
}

TEST_CASE("auv3 validator: skips when auval not installed",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    fs::path bundle = "/tmp/Synth.appex";
    s.existing_paths.insert(bundle.string());
    s.existing_paths.insert((bundle / "Contents" / "Info.plist").string());
    s.path_to_executable["plutil"] = "/usr/bin/plutil";
    s.cmd_results["plutil"] = {0,
        R"("type" => "aufx" "subtype" => "Plpe" "manufacturer" => "Plpa")"};
    auto env = s.build();
    auto r = mr::run_auv3_validator(bundle, env);
    REQUIRE(r.status == "skip");
    REQUIRE(r.summary.find("auval") != std::string::npos);
}

TEST_CASE("auv3 validator: rejects unparseable Info.plist",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    fs::path bundle = "/tmp/Broken.appex";
    s.existing_paths.insert(bundle.string());
    s.existing_paths.insert((bundle / "Contents" / "Info.plist").string());
    s.path_to_executable["plutil"] = "/usr/bin/plutil";
    s.cmd_results["plutil"] = {0, "nothing useful here"};
    auto env = s.build();
    auto r = mr::run_auv3_validator(bundle, env);
    REQUIRE(r.status == "fail");
    REQUIRE(r.summary.find("AudioComponents") != std::string::npos);
}

TEST_CASE("extract_min_macos_version: LC_BUILD_VERSION minos",
          "[cli][mac-validators][phase5]") {
    std::string otool_out = R"(
        Load command 4
              cmd LC_BUILD_VERSION
          cmdsize 32
         platform 1
            minos 13.0
              sdk 14.0
        Load command 5
    )";
    REQUIRE(mr::extract_min_macos_version(otool_out) == "13.0");
}

TEST_CASE("extract_min_macos_version: LC_VERSION_MIN_MACOSX",
          "[cli][mac-validators][phase5]") {
    std::string otool_out = R"(
        Load command 2
              cmd LC_VERSION_MIN_MACOSX
          cmdsize 16
          version 12.3.1
              sdk 12.3
    )";
    REQUIRE(mr::extract_min_macos_version(otool_out) == "12.3.1");
}

TEST_CASE("extract_min_macos_version: no load command",
          "[cli][mac-validators][phase5]") {
    REQUIRE(mr::extract_min_macos_version("nothing").empty());
}

TEST_CASE("meets_macos_floor", "[cli][mac-validators][phase5]") {
    REQUIRE(mr::meets_macos_floor("13.0"));
    REQUIRE(mr::meets_macos_floor("13.4"));
    REQUIRE(mr::meets_macos_floor("14.1"));
    REQUIRE(mr::meets_macos_floor("15.0.0"));
    REQUIRE_FALSE(mr::meets_macos_floor("12.3"));
    REQUIRE_FALSE(mr::meets_macos_floor("11.0"));
    // Empty is permissive — paired with a warning row in the renderer.
    REQUIRE(mr::meets_macos_floor(""));
}

TEST_CASE("macho validator: skips on non-Apple",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    s.is_apple = false;
    auto env = s.build();
    auto r = mr::run_macho_validator("/tmp/Foo.app", env);
    REQUIRE(r.status == "skip");
}

TEST_CASE("macho validator: missing bundle fails",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    auto env = s.build();
    auto r = mr::run_macho_validator("/tmp/missing.app", env);
    REQUIRE(r.status == "fail");
}

TEST_CASE("macho validator: skips when no payloads found",
          "[cli][mac-validators][phase5]") {
    // Bundle exists but no files inside — enumerate returns empty
    // since the dir is empty / unreadable.
    static std::atomic<int> seq{0};
    fs::path tmp = fs::temp_directory_path() /
               ("pulp-mac-validator-empty-" +
                std::to_string(seq.fetch_add(1)));
    fs::create_directories(tmp);
    StubEnv s;
    s.existing_paths.insert(tmp.string());
    auto env = s.build();
    auto r = mr::run_macho_validator(tmp, env);
    REQUIRE(r.status == "skip");
    fs::remove_all(tmp);
}

TEST_CASE("enumerate_mach_o_payloads picks up dylib + executable",
          "[cli][mac-validators][phase5]") {
    static std::atomic<int> seq{0};
    fs::path tmp = fs::temp_directory_path() /
               ("pulp-mac-validator-payloads-" +
                std::to_string(seq.fetch_add(1)));
    auto macos = tmp / "Contents" / "MacOS";
    auto fwks = tmp / "Contents" / "Frameworks";
    fs::create_directories(macos);
    fs::create_directories(fwks);
    std::ofstream(macos / "Synth").put('\0');                // bare exec
    std::ofstream(fwks / "libsupport.dylib").put('\0');       // dylib
    std::ofstream(fwks / "license.txt").put('\0');            // ignore
    StubEnv s;
    s.existing_paths.insert(tmp.string());
    auto env = s.build();
    auto payloads = mr::enumerate_mach_o_payloads(tmp, env);
    bool found_exec = false, found_dylib = false;
    for (const auto& p : payloads) {
        if (p.filename() == "Synth") found_exec = true;
        if (p.filename() == "libsupport.dylib") found_dylib = true;
        // Must not include the .txt file
        REQUIRE(p.filename() != "license.txt");
    }
    REQUIRE(found_exec);
    REQUIRE(found_dylib);
    fs::remove_all(tmp);
}

TEST_CASE("macho validator: pass when codesign + otool happy",
          "[cli][mac-validators][phase5]") {
    static std::atomic<int> seq{0};
    fs::path tmp = fs::temp_directory_path() /
               ("pulp-mac-validator-pass-" +
                std::to_string(seq.fetch_add(1)));
    auto macos = tmp / "Contents" / "MacOS";
    fs::create_directories(macos);
    std::ofstream(macos / "Synth").put('\0');
    StubEnv s;
    s.existing_paths.insert(tmp.string());
    s.path_to_executable["codesign"] = "/usr/bin/codesign";
    s.path_to_executable["otool"] = "/usr/bin/otool";
    s.cmd_results["codesign"] = {0, ""};
    s.cmd_results["otool"] = {0,
        "cmd LC_BUILD_VERSION\n  minos 13.0\n"};
    auto env = s.build();
    auto r = mr::run_macho_validator(tmp, env);
    REQUIRE(r.status == "pass");
    fs::remove_all(tmp);
}

TEST_CASE("macho validator: fails when codesign --verify rejects",
          "[cli][mac-validators][phase5]") {
    static std::atomic<int> seq{0};
    fs::path tmp = fs::temp_directory_path() /
               ("pulp-mac-validator-fail-" +
                std::to_string(seq.fetch_add(1)));
    auto macos = tmp / "Contents" / "MacOS";
    fs::create_directories(macos);
    std::ofstream(macos / "Synth").put('\0');
    StubEnv s;
    s.existing_paths.insert(tmp.string());
    s.path_to_executable["codesign"] = "/usr/bin/codesign";
    s.path_to_executable["otool"] = "/usr/bin/otool";
    s.cmd_results["codesign"] = {1, "code object is not signed at all"};
    s.cmd_results["otool"] = {0,
        "cmd LC_BUILD_VERSION\n  minos 13.0\n"};
    auto env = s.build();
    auto r = mr::run_macho_validator(tmp, env);
    REQUIRE(r.status == "fail");
    REQUIRE(r.summary.find("not signed") != std::string::npos);
    fs::remove_all(tmp);
}

TEST_CASE("macho validator: rejects pre-13.0 minos",
          "[cli][mac-validators][phase5]") {
    static std::atomic<int> seq{0};
    fs::path tmp = fs::temp_directory_path() /
               ("pulp-mac-validator-floor-" +
                std::to_string(seq.fetch_add(1)));
    auto macos = tmp / "Contents" / "MacOS";
    fs::create_directories(macos);
    std::ofstream(macos / "Synth").put('\0');
    StubEnv s;
    s.existing_paths.insert(tmp.string());
    s.path_to_executable["codesign"] = "/usr/bin/codesign";
    s.path_to_executable["otool"] = "/usr/bin/otool";
    s.cmd_results["codesign"] = {0, ""};
    s.cmd_results["otool"] = {0,
        "cmd LC_BUILD_VERSION\n  minos 11.0\n"};
    auto env = s.build();
    auto r = mr::run_macho_validator(tmp, env);
    REQUIRE(r.status == "fail");
    REQUIRE(r.summary.find("macOS floor") != std::string::npos);
    fs::remove_all(tmp);
}

TEST_CASE("run_all_targets dispatches three validators in order",
          "[cli][mac-validators][phase5]") {
    StubEnv s;
    auto env = s.build();
    auto results = mr::run_all_targets("/tmp/missing.app", env);
    REQUIRE(results.size() == 3);
    REQUIRE(results[0].target == "standalone");
    REQUIRE(results[1].target == "auv3");
    REQUIRE(results[2].target == "macho");
}
