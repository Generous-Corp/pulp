#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

inline std::uint32_t plugin_inspect_timeout_ms(
    std::optional<std::uint32_t> requested_timeout_ms,
    std::uint32_t warmup_ms) {
    if (requested_timeout_ms) return *requested_timeout_ms;
    const auto derived = std::max<std::uint64_t>(
        30'000u, static_cast<std::uint64_t>(warmup_ms) * 10u + 10'000u);
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        derived, std::numeric_limits<std::uint32_t>::max()));
}

// `pulp audio plugin-inspect` — isolated parameter/API discovery for one
// hosted third-party plugin.
int cmd_audio_plugin_inspect(const std::vector<std::string>& args);
int cmd_audio_plugin_inspect_worker(const std::vector<std::string>& args);
