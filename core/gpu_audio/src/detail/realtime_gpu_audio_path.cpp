#include "realtime_gpu_audio_path.hpp"

#include <pulp/gpu_audio/gpu_convolver.hpp>

namespace pulp::gpu_audio::detail {

RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode* node) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = dynamic_cast<GpuConvolver*>(node);
    if (convolver != nullptr && convolver->has_realtime_shared_io()) {
        return {.context = convolver,
                .process = &GpuConvolver::process_realtime_shared_io,
                .service = &GpuConvolver::service_realtime_shared_io,
                .fence = &GpuConvolver::fence_realtime_shared_io,
                .delivered = &GpuConvolver::complete_realtime_shared_io,
                .next_sequence = &GpuConvolver::next_realtime_shared_io_sequence};
    }
#else
    (void)node;
#endif
    return {};
}

GpuAudioProvider realtime_gpu_provider(GpuAudioNode* node) noexcept {
    // The existing friend builds this path only for the concrete, prepared
    // Dawn convolver. Reuse that decision without exposing node internals.
    return realtime_gpu_node_path(node).active() ? GpuAudioProvider::Dawn
                                                 : GpuAudioProvider::Unknown;
}

} // namespace pulp::gpu_audio::detail
