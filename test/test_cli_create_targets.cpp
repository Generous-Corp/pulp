#include <catch2/catch_test_macros.hpp>

#include "tools/cli/create_targets.hpp"

#include <vector>

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

    CHECK(targets == std::vector<std::string>{
        "-test",
    });
}
