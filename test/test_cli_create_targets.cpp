#include <catch2/catch_test_macros.hpp>

#include "tools/cli/create_build_commands.hpp"
#include "tools/cli/create_targets.hpp"

#include <filesystem>
#include <string>
#include <vector>

using pulp::cli::create_standalone_configure_command;
using pulp::cli::create_standalone_ctest_command;
using pulp::cli::create_build_config;
using pulp::cli::create_default_build_targets;

namespace fs = std::filesystem;

TEST_CASE("CLI create targets use format suffixes for plugin projects", "[cli][create]") {
    const auto targets =
        create_default_build_targets("PulpGain", "effect", "VST3 CLAP Standalone");

    CHECK(targets == std::vector<std::string>{
        "PulpGain-test",
        "PulpGain_VST3",
        "PulpGain_CLAP",
        "PulpGain_Standalone",
    });
}

TEST_CASE("CLI create targets use the app target name for standalone apps", "[cli][create]") {
    const auto targets = create_default_build_targets("PulpApp", "app", "Standalone");

    CHECK(targets == std::vector<std::string>{
        "PulpApp-test",
        "PulpApp",
    });
}

TEST_CASE("CLI create targets use the app target name for bare projects", "[cli][create]") {
    const auto targets = create_default_build_targets("PulpBare", "bare", "Standalone");

    CHECK(targets == std::vector<std::string>{
        "PulpBare-test",
        "PulpBare",
    });
}

TEST_CASE("CLI create targets include optional plugin format suffixes once",
          "[cli][create][issue-643]") {
    const auto targets =
        create_default_build_targets("PulpPlugin",
                                     "instrument",
                                     "VST3 CLAP AU AUv3 LV2 AAX Standalone VST3");

    CHECK(targets == std::vector<std::string>{
        "PulpPlugin-test",
        "PulpPlugin_VST3",
        "PulpPlugin_CLAP",
        "PulpPlugin_AU",
        "PulpPlugin_AUv3",
        "PulpPlugin_LV2",
        "PulpPlugin_AAX",
        "PulpPlugin_Standalone",
    });
}

TEST_CASE("CLI create targets skip empty standalone app targets",
          "[cli][create][issue-643]") {
    const auto targets = create_default_build_targets("", "app", "Standalone");

    // An empty class_name (the slug sanitizer can collapse names made only of
    // separators to "") must not produce the malformed target `"-test"`.
    // That string looks like a CMake CLI option to `cmake --build --target ...`
    // and can either fail confusingly or get mistaken for an unknown option.
    // Empty class_name means an empty target list: no buildable targets rather
    // than a guaranteed-broken build invocation.
    CHECK(targets == std::vector<std::string>{});
}

TEST_CASE("CLI create targets skip malformed targets when class_name is empty across formats",
          "[cli][create][issue-643]") {
    // No format suffix should produce a "_VST3" / "_CLAP" / "_Standalone"
    // target that starts with `_`; it has the same `cmake --build --target`
    // ambiguity as an option-like target. Empty class_name means an empty
    // target list.
    const auto targets = create_default_build_targets(
        "", "instrument", "VST3 CLAP AU Standalone");
    CHECK(targets == std::vector<std::string>{});
}

TEST_CASE("CLI create targets match complete format tokens only",
          "[cli][create][coverage]") {
    const auto auv3_only =
        create_default_build_targets("PulpPlugin", "instrument", "AUv3");
    CHECK(auv3_only == std::vector<std::string>{
        "PulpPlugin-test",
        "PulpPlugin_AUv3",
    });

    const auto standalone_prefix =
        create_default_build_targets("PulpPlugin", "instrument", "StandaloneKit");
    CHECK(standalone_prefix == std::vector<std::string>{
        "PulpPlugin-test",
    });
}

TEST_CASE("CLI create targets can omit the test target for template kit builds",
          "[cli][create][kit]") {
    const auto targets = create_default_build_targets(
        "KitGain", "effect", "CLAP Standalone", false);

    CHECK(targets == std::vector<std::string>{
        "KitGain_CLAP",
        "KitGain_Standalone",
    });
}

TEST_CASE("CLI create selects build config at build/test time for multi-config generators",
          "[cli][create][issue-2133]") {
    // Multi-config generators such as Visual Studio and Xcode ignore
    // CMAKE_BUILD_TYPE for build and test selection. The config name must flow
    // into `cmake --build --config <cfg>` and `ctest -C <cfg>`.
    CHECK(create_build_config(false) == "Release");
    CHECK(create_build_config(true) == "Debug");

    // The fragments appended to the build/test commands resolve to the
    // multi-config selectors the generators require.
    CHECK((" --config " + create_build_config(false)) == " --config Release");
    CHECK((" -C " + create_build_config(true)) == " -C Debug");
}

TEST_CASE("CLI create standalone commands quote paths with spaces",
          "[cli][create][shellout][coverage]") {
    const fs::path source_dir = fs::path("Generated Projects") / "My Plugin";
    const fs::path build_dir = source_dir / "build dir";
    const fs::path sdk_dir = fs::path("SDK Cache") / "Pulp SDK";

    const auto configure =
        create_standalone_configure_command(source_dir, build_dir, false, sdk_dir);
    CHECK(configure.find("-S \"" + source_dir.string() + "\"") != std::string::npos);
    CHECK(configure.find("-B \"" + build_dir.string() + "\"") != std::string::npos);
    CHECK(configure.find("-DCMAKE_BUILD_TYPE=\"Release\"") != std::string::npos);
    CHECK(configure.find("-DCMAKE_PREFIX_PATH=\"" + sdk_dir.string() + "\"")
          != std::string::npos);

    const auto ctest = create_standalone_ctest_command(build_dir, true);
    CHECK(ctest.find("--test-dir \"" + build_dir.string() + "\"") != std::string::npos);
    CHECK(ctest.find("-C \"Debug\"") != std::string::npos);
}
