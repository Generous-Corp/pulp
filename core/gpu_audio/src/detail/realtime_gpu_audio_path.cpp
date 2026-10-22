#include "realtime_gpu_audio_path.hpp"
#include "realtime_gpu_health_mirror.hpp"

#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/gpu_audio/gpu_neural_realtime_node.hpp>
#include <pulp/gpu_audio/gpu_wavenet_realtime_node.hpp>

namespace pulp::gpu_audio::detail {
namespace {

// Serialized non-RT read (transport worker or quiescent host). The public
// status projection is allocation-free and already carries the bridge's first
// latched cause; the transport republishes it for host readers.
bool wavenet_health(const void* self, RealtimeGpuHealth& out) noexcept {
    const auto* node = static_cast<const GpuWaveNetRealtimeNode*>(self);
    out = health_from_status(node->status_snapshot());
    out.fenced = out.fenced || node->fenced();
    return true;
}

} // namespace

RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode* node) noexcept {
    auto* wavenet = dynamic_cast<GpuWaveNetRealtimeNode*>(node);
    if (wavenet != nullptr && wavenet->ready()) {
        return {.context = wavenet,
                .process = &GpuWaveNetRealtimeNode::process,
                .service = &GpuWaveNetRealtimeNode::service,
                .fence = &GpuWaveNetRealtimeNode::fence,
                .delivered = &GpuWaveNetRealtimeNode::delivered,
                .next_sequence = &GpuWaveNetRealtimeNode::next_sequence,
                .health = &wavenet_health};
    }
    auto* neural = dynamic_cast<GpuNeuralRealtimeNode*>(node);
    if (neural != nullptr && neural->ready()) {
        return {.context = neural,
                .process = &GpuNeuralRealtimeNode::process,
                .service = &GpuNeuralRealtimeNode::service,
                .fence = &GpuNeuralRealtimeNode::fence,
                .delivered = &GpuNeuralRealtimeNode::delivered,
                .next_sequence = &GpuNeuralRealtimeNode::next_sequence,
                .health = &GpuNeuralRealtimeNode::health};
    }
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = dynamic_cast<GpuConvolver*>(node);
    if (convolver != nullptr && convolver->has_realtime_shared_io()) {
        return {.context = convolver,
                .process = &GpuConvolver::process_realtime_shared_io,
                .service = &GpuConvolver::service_realtime_shared_io,
                .fence = &GpuConvolver::fence_realtime_shared_io,
                .delivered = &GpuConvolver::complete_realtime_shared_io,
                .next_sequence = &GpuConvolver::next_realtime_shared_io_sequence,
                .health = &GpuConvolver::realtime_shared_io_health};
    }
#else
    (void)node;
#endif
    return {};
}

bool requires_realtime_gpu_path(GpuAudioNode* node) noexcept {
    return dynamic_cast<GpuWaveNetRealtimeNode*>(node) != nullptr ||
           dynamic_cast<GpuNeuralRealtimeNode*>(node) != nullptr;
}

GpuAudioProvider realtime_gpu_provider(GpuAudioNode* node) noexcept {
    if (auto* wavenet = dynamic_cast<GpuWaveNetRealtimeNode*>(node))
        return wavenet->authenticated_provider() ? GpuAudioProvider::Dawn
                                                 : GpuAudioProvider::Unknown;
    if (auto* neural = dynamic_cast<GpuNeuralRealtimeNode*>(node))
        return neural->transport_provider();
    // The existing friend builds this path only for the concrete, prepared
    // Dawn convolver. Reuse that decision without exposing node internals.
    return realtime_gpu_node_path(node).active() ? GpuAudioProvider::Dawn
                                                 : GpuAudioProvider::Unknown;
}

SharedIoProviderIdentity realtime_gpu_provider_identity(GpuAudioNode* node) noexcept {
#if defined(PULP_GPU_AUDIO_HAS_DAWN_SHARED_IO)
    auto* convolver = dynamic_cast<GpuConvolver*>(node);
    if (convolver != nullptr)
        return convolver->provider_identity_for_diagnostics();
#else
    (void)node;
#endif
    return {};
}

} // namespace pulp::gpu_audio::detail
