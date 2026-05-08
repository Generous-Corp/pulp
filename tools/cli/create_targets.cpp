#include "create_targets.hpp"

#include <algorithm>

namespace pulp::cli {

std::vector<std::string> create_default_build_targets(const std::string& class_name,
                                                      const std::string& type,
                                                      const std::string& formats) {
    std::vector<std::string> targets;
    auto add_target = [&](const std::string& target) {
        if (!target.empty() &&
            std::find(targets.begin(), targets.end(), target) == targets.end()) {
            targets.push_back(target);
        }
    };

    // Skip the test target when class_name is empty — `cmake --build
    // --target -test` is a malformed flag (looks like a CMake CLI option,
    // not a target name) and would either fail confusingly or, on some
    // CMake builds, get mistaken for an unknown option. An empty
    // class_name means the caller's slug-sanitization collapsed the
    // input to nothing (e.g. a name made only of separators); the
    // create flow should fall back to "no buildable targets" rather
    // than emit a guaranteed-broken target string.
    // Codex P2 on PR #1271.
    if (!class_name.empty()) {
        add_target(class_name + "-test");
    }

    auto add_format_target = [&](const std::string& format, const std::string& suffix) {
        // Same defensive skip as the test target above — an empty
        // class_name would emit `_VST3`, `_CLAP`, etc. which look like
        // CMake CLI options to `cmake --build --target ...`.
        // Codex P2 on PR #1271.
        if (class_name.empty()) return;
        if (formats.find(format) != std::string::npos) {
            add_target(class_name + "_" + suffix);
        }
    };

    add_format_target("VST3", "VST3");
    add_format_target("CLAP", "CLAP");
    add_format_target("AU", "AU");
    add_format_target("AUv3", "AUv3");
    add_format_target("LV2", "LV2");
    add_format_target("AAX", "AAX");

    if (formats.find("Standalone") != std::string::npos && !class_name.empty()) {
        if (type == "app" || type == "bare") {
            add_target(class_name);
        } else {
            add_target(class_name + "_Standalone");
        }
    }

    return targets;
}

}  // namespace pulp::cli
