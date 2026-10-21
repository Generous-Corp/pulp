#pragma once

#include <cstdint>

namespace pulp::gpu_audio {

/// First latched reason that a stamped shared-I/O node entered recovery.
/// `None` means that no recovery has been requested. This is diagnostic state;
/// it does not itself promise that the next callback will use the GPU.
enum class GpuAudioRecoveryReason : std::uint8_t {
    None = 0,
    SequenceGap = 1,
    InputSaturated = 2,
    ProviderFailure = 3,
    ProviderLost = 4,
    OfflineFence = 5,
    InvalidCallback = 6,
};

} // namespace pulp::gpu_audio
