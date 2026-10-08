#include "dawn_wavenet_factory.hpp"

#ifndef PULP_GPU_AUDIO_EXPECTED_DAWN_SHA
#define PULP_GPU_AUDIO_EXPECTED_DAWN_SHA ""
#endif

namespace pulp::gpu_audio::detail {

DawnWaveNetFactoryResult create_dawn_wavenet(const WavenetProgramSpec& spec,
                                             const DawnWaveNetFactoryConfig& config) noexcept {
    DawnWaveNetFactoryResult result;
    try {
        auto policy = DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
        if (config.completion_policy == GpuWaveNetCompletionPolicy::WaitAny)
            policy = DawnSharedIoProvider::CompletionPolicy::WaitAny;
        else if (config.completion_policy == GpuWaveNetCompletionPolicy::TimedWaitAny)
            policy = DawnSharedIoProvider::CompletionPolicy::TimedWaitAny;
        result.requested_policy = policy;

        auto created = DawnSharedIoProvider::create(
            {.expected_dawn_revision = PULP_GPU_AUDIO_EXPECTED_DAWN_SHA,
             .completion_policy = policy,
             .completion_wait_ns = config.completion_wait_ns});
        if (!created.provider)
            return result;
        result.effective_policy = created.provider->completion_policy();
        result.program = created.provider->make_wavenet_program(spec);
        if (!result.program)
            return {};
        result.provider = std::move(created.provider);
    } catch (...) {
        return {};
    }
    return result;
}

} // namespace pulp::gpu_audio::detail
