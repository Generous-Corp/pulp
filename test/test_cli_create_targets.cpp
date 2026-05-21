#include <catch2/catch_test_macros.hpp>

#include "tools/cli/create_targets.hpp"

#include <vector>

using pulp::cli::create_build_config;
using pulp::cli::create_default_build_targets;

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

    // Codex P2 on PR #1271 — an empty class_name (the slug
    // sanitizer can collapse names made only of separators to "")
    // must NOT produce the malformed target `"-test"`. That string
    // looks like a CMake CLI option to `cmake --build --target ...`
    // and can either fail confusingly or get mistaken for an
    // unknown option. Empty class_name → empty target list (caller's
    // contract: "no buildable targets" rather than a guaranteed-broken
    // build invocation).
    CHECK(targets == std::vector<std::string>{});
}

TEST_CASE("CLI create targets skip malformed targets when class_name is empty across formats",
          "[cli][create][issue-643]") {
    // Same Codex P2 case as above, generalised — none of the format
    // suffixes should produce a "_VST3" / "_CLAP" / "_Standalone"
    // string that starts with `_` either, for the same `cmake --build
    // --target` reason. Empty class_name → empty target list.
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

TEST_CASE("CLI create selects build config at build/test time for multi-config generators",
          "[cli][create][issue-2133]") {
    // Codex P1 on PR #2133: `pulp create` set only the configure-time
    // CMAKE_BUILD_TYPE, so on Visual Studio / Xcode multi-config generators
    // `cmake --build` and `ctest` defaulted to Debug regardless of --debug.
    // The config name must flow into `cmake --build --config <cfg>` and
    // `ctest -C <cfg>`.
    CHECK(create_build_config(false) == "Release");
    CHECK(create_build_config(true) == "Debug");

    // The fragments appended to the build/test commands resolve to the
    // multi-config selectors the generators require.
    CHECK((" --config " + create_build_config(false)) == " --config Release");
    CHECK((" -C " + create_build_config(true)) == " -C Debug");
}
