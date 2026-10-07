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

    static constexpr bool valid_contention(SharedIoHostContention value) noexcept {
        return value == SharedIoHostContention::Quiet || value == SharedIoHostContention::Cpu ||
               value == SharedIoHostContention::Graphics || value == SharedIoHostContention::Memory;
    }

    static constexpr bool valid_thermal(SharedIoHostThermal value) noexcept {
        return value == SharedIoHostThermal::Nominal || value == SharedIoHostThermal::Elevated ||
               value == SharedIoHostThermal::Critical;
    }

    // A receipt may only claim a complete host-policy observation when all
    // fields are present and enum values are recognized. This authenticates
    // metadata completeness only; it does not prove physical host provenance.
    constexpr bool authenticated() const noexcept {
        return captured_ns != 0 && callback_period_ns != 0 && scheduler_policy_id != 0 &&
               valid_contention(contention) && valid_thermal(thermal);
    }
};

static_assert(sizeof(SharedIoHostSchedulingSnapshot) <= 40);

} // namespace pulp::gpu_audio::detail
