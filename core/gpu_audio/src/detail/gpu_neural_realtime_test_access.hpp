#pragma once

#include "realtime_gpu_audio_path.hpp"
#include "shared_io_stamped_bridge.hpp"

#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>

namespace pulp::gpu_audio::detail {

// Private test seams for GpuNeuralRealtimeNode. No plugin-facing hooks.
struct NeuralRealtimeTestAccess {
    // Planted negative: prepare() leaves the shadow processors unpublished.
    static void skip_shadow_publish(GpuNeuralRealtimeNode&, bool skip) noexcept;
    // Latches a recovery reason as a provider fault would.
    static void request_recovery(GpuNeuralRealtimeNode&, SharedIoRecoveryReason) noexcept;
    static RealtimeGpuNodePath path(GpuNeuralRealtimeNode&) noexcept;
};

} // namespace pulp::gpu_audio::detail
