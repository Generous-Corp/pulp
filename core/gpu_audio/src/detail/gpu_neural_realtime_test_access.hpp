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
    // Replaces the prepare()-time shadow cost measurement (Auto policy).
    static void inject_shadow_cost(GpuNeuralRealtimeNode&, float pct) noexcept;
    // Before prepare(): run the Worker shadow without a thread; pump_shadow()
    // steps it on the caller.
    static void manual_shadow(GpuNeuralRealtimeNode&, bool manual) noexcept;
    static std::size_t pump_shadow(GpuNeuralRealtimeNode&, std::size_t max_blocks = 0) noexcept;
    static std::uint64_t shadow_processed(const GpuNeuralRealtimeNode&) noexcept;
};

} // namespace pulp::gpu_audio::detail
