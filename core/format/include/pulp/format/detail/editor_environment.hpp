#pragma once

#include <pulp/runtime/system.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace pulp::format::detail {

inline bool environment_flag_truthy(std::string_view name) {
    auto value = runtime::get_env(name);
    if (!value) return false;

    std::string normalized = *value;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (normalized.empty()) return false;
    return normalized != "0"
        && normalized != "false"
        && normalized != "no"
        && normalized != "off";
}

inline bool editor_launch_blocked_by_environment() {
    if (environment_flag_truthy("PULP_DISABLE_PLUGIN_EDITOR")
        || environment_flag_truthy("PULP_HEADLESS")
        || environment_flag_truthy("PULP_TEST_MODE")
        || environment_flag_truthy("CI")) {
        return true;
    }

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if (!runtime::get_env("DISPLAY") && !runtime::get_env("WAYLAND_DISPLAY"))
        return true;
#endif

    return false;
}

} // namespace pulp::format::detail
