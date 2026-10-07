#pragma once

#include <cstdint>

namespace pulp::gpu_audio::detail {

// A private, immutable-at-publication snapshot of host conditions used by a
// scheduling receipt.  It is diagnostic metadata only: it does not change
// dispatch policy and it must never be sampled from the realtime callback.
enum class SharedIoHostContention : std::uint8_t {
    Unavailable = 0,
    Quiet = 1,
    Cpu = 2,
    Graphics = 3,
    Memory = 4,
};

enum class SharedIoHostThermal : std::uint8_t {
    Unavailable = 0,
    Nominal = 1,
    Elevated = 2,
    Critical = 3,
};

struct SharedIoHostSchedulingSnapshot {
    // Monotonic timestamp at which the host observation was captured.
    std::uint64_t captured_ns = 0;
    // Callback budget represented by the active block size and sample rate.
    std::uint64_t callback_period_ns = 0;
    // Stable policy identifier from the campaign/controller. Zero is unknown.
    std::uint64_t scheduler_policy_id = 0;
    SharedIoHostContention contention = SharedIoHostContention::Unavailable;
    SharedIoHostThermal thermal = SharedIoHostThermal::Unavailable;
    bool audio_workgroup_joined = false;

    // A receipt may only claim a host-policy observation when all identity and
    // timing fields are present. Missing host state remains explicitly
    // unavailable and therefore fails closed for promotion.
    constexpr bool authenticated() const noexcept {
        return captured_ns != 0 && callback_period_ns != 0 && scheduler_policy_id != 0 &&
               contention != SharedIoHostContention::Unavailable &&
               thermal != SharedIoHostThermal::Unavailable;
    }
};

static_assert(sizeof(SharedIoHostSchedulingSnapshot) <= 40);

} // namespace pulp::gpu_audio::detail
