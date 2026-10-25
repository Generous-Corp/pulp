#pragma once

#include <algorithm>
#include <cstdint>

namespace pulp::gpu_audio::detail {

// Default ProviderUnresponsive window, in time. A false trip fences the GPU
// path for the rest of the preparation and tells the host it needs a new
// provider, while a late detection of a true hang costs only telemetry (the
// miss policy already carries every block), so the default is generous: more
// than twice the longest completion silence measured on real Dawn under load
// (86 ms on an M1 Max).
inline constexpr std::uint64_t kDefaultProviderWatchdogNs = 200'000'000;

struct ProviderWatchdogSizing {
    // Callback blocks without a completion, while work is in flight, before
    // the node latches ProviderUnresponsive.
    std::uint32_t window_blocks = 0;
    // Input ring capacity in blocks. With the default window the ring holds
    // the whole window plus the lead, so a stall the window tolerates cannot
    // fill the ring and a true hang trips the watchdog before saturation.
    std::uint32_t ring_blocks = 0;
};

// Converts the default window to callback blocks once, at prepare(), so the
// callback stays integer-only. An explicit `watchdog_blocks` is used as given
// and leaves the configured capacity unchanged.
constexpr ProviderWatchdogSizing size_provider_watchdog(std::uint32_t capacity,
                                                        std::uint32_t lead_blocks,
                                                        std::uint32_t watchdog_blocks,
                                                        std::uint32_t block_size,
                                                        std::uint32_t sample_rate) noexcept {
    if (watchdog_blocks != 0)
        return {watchdog_blocks, capacity};
    const std::uint64_t window_frames =
        (kDefaultProviderWatchdogNs * sample_rate + 999'999'999u) / 1'000'000'000u; // ceil
    const std::uint64_t blocks =
        block_size == 0 ? 1
                        : std::max<std::uint64_t>(1, (window_frames + block_size - 1) / block_size);
    const auto window = static_cast<std::uint32_t>(std::max<std::uint64_t>(
        capacity > 0 ? capacity - 1u : 1u, std::min<std::uint64_t>(blocks, UINT32_MAX / 4)));
    const auto ring = std::max<std::uint64_t>(capacity, std::uint64_t{window} + lead_blocks + 1u);
    return {window, static_cast<std::uint32_t>(std::min<std::uint64_t>(ring, UINT32_MAX))};
}

} // namespace pulp::gpu_audio::detail
