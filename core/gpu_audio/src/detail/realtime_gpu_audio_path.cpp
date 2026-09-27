#include "realtime_gpu_audio_path.hpp"

#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>

namespace pulp::gpu_audio::detail {

RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode* node) noexcept {
    auto* wavenet = dynamic_cast<GpuWaveNetRealtimeNode*>(node);
    if (wavenet != nullptr && wavenet->ready()) {
        return {.context = wavenet,
                .process = &GpuWaveNetRealtimeNode::process,
                .service = &GpuWaveNetRealtimeNode::service,
                .fence = &GpuWaveNetRealtimeNode::fence,
                .delivered = &GpuWaveNetRealtimeNode::delivered,
                .next_sequence = &GpuWaveNetRealtimeNode::next_sequence};
    }
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

bool requires_realtime_gpu_path(GpuAudioNode* node) noexcept {
    return dynamic_cast<GpuWaveNetRealtimeNode*>(node) != nullptr;
}

GpuAudioProvider realtime_gpu_provider(GpuAudioNode* node) noexcept {
    if (auto* wavenet = dynamic_cast<GpuWaveNetRealtimeNode*>(node))
        return wavenet->authenticated_provider() ? GpuAudioProvider::Dawn
                                                 : GpuAudioProvider::Unknown;
    // The existing friend builds this path only for the concrete, prepared
    // Dawn convolver. Reuse that decision without exposing node internals.
    return realtime_gpu_node_path(node).active() ? GpuAudioProvider::Dawn
                                                 : GpuAudioProvider::Unknown;
}

} // namespace pulp::gpu_audio::detail
