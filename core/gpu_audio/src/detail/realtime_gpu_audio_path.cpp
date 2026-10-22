#include "realtime_gpu_audio_path.hpp"
#include "worker_pipeline_hosting.hpp"
namespace pulp::gpu_audio::detail {

RealtimeGpuNodeBinding realtime_gpu_node_binding(GpuAudioNode* node) noexcept {
    RealtimeGpuNodeBinding binding;
    if (node == nullptr || !node->bind_realtime_path(binding))
        return {};
    return binding;
}

RealtimeGpuNodePath realtime_gpu_node_path(GpuAudioNode* node) noexcept {
    const auto binding = realtime_gpu_node_binding(node);
    return binding.path.active() ? binding.path : RealtimeGpuNodePath{};
}

bool requires_realtime_gpu_path(GpuAudioNode* node) noexcept {
    return realtime_gpu_node_binding(node).requires_realtime_path;
}

SharedIoProviderIdentity realtime_gpu_provider_identity(GpuAudioNode* node) noexcept {
    const auto binding = realtime_gpu_node_binding(node);
    return binding.identity != nullptr ? binding.identity(binding.identity_context)
                                       : SharedIoProviderIdentity{};
}

bool set_worker_pipeline_hosting(GpuAudioNode* node, bool hosted) noexcept {
    const auto binding = realtime_gpu_node_binding(node);
    return binding.pipeline_hosting != nullptr &&
           binding.pipeline_hosting(binding.pipeline_context, hosted);
}

GpuAudioProvider realtime_gpu_provider(GpuAudioNode* node) noexcept {
    // The provider names itself. A GPU provider counts only once it has
    // authenticated; a CPU provider needs no authentication to be CPU.
    const auto identity = realtime_gpu_provider_identity(node);
    if (identity.provider == GpuAudioProvider::Cpu)
        return GpuAudioProvider::Cpu;
    return identity.authenticated ? identity.provider : GpuAudioProvider::Unknown;
}

} // namespace pulp::gpu_audio::detail
