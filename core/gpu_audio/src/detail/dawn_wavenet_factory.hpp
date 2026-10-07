#pragma once

#include "dawn_shared_io_provider.hpp"
#include "dawn_shared_io_wavenet_program.hpp"

#include <pulp/gpu_audio/gpu_wavenet.hpp>

#include <memory>
#include <vector>

namespace pulp::gpu_audio::detail {

struct DawnWaveNetFactoryConfig {
    GpuWaveNetCompletionPolicy completion_policy = GpuWaveNetCompletionPolicy::ProcessEvents;
    std::uint64_t completion_wait_ns = 0;
};

struct DawnWaveNetFactoryResult {
    std::unique_ptr<DawnSharedIoProvider> provider;
    std::unique_ptr<SharedIoPreparedProgram> program;
    DawnSharedIoProvider::CompletionPolicy effective_policy =
        DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
    DawnSharedIoProvider::CompletionPolicy requested_policy =
        DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
};

DawnWaveNetFactoryResult create_dawn_wavenet(const DawnSharedIoWavenetProgramSpec& spec,
                                             const DawnWaveNetFactoryConfig& config) noexcept;

} // namespace pulp::gpu_audio::detail
