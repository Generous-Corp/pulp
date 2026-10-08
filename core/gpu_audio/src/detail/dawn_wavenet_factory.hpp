#pragma once

#include "dawn_shared_io_provider.hpp"
#include "dawn_shared_io_wavenet_program.hpp"
#include "shared_io_program_session.hpp"

#include <pulp/gpu_audio/gpu_wavenet.hpp>

#include <memory>
#include <utility>
#include <vector>

namespace pulp::gpu_audio::detail {

struct DawnWaveNetFactoryConfig {
    GpuWaveNetCompletionPolicy completion_policy = GpuWaveNetCompletionPolicy::ProcessEvents;
    std::uint64_t completion_wait_ns = 0;
    // Private diagnostic storage choice. Staged buffers keep the provider
    // boundary honest when an alternative WebGPU implementation cannot prove
    // imported host-pointer lifetime. The public WaveNet API remains on the
    // authenticated imported path by default.
    SharedIoArenaProvider::StorageKind storage_kind =
        SharedIoArenaProvider::StorageKind::ImportedHostPointer;
};

struct DawnWaveNetFactoryResult {
    std::unique_ptr<DawnSharedIoProvider> provider;
    std::unique_ptr<SharedIoPreparedProgram> program;
    DawnSharedIoProvider::CompletionPolicy effective_policy =
        DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
    DawnSharedIoProvider::CompletionPolicy requested_policy =
        DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
    // Captured before ownership moves into SharedIoProgramSession. These
    // snapshots are diagnostic receipt inputs; an unauthenticated provider
    // remains fail-closed and is never promoted by this factory.
    SharedIoProviderIdentity provider_identity;
    SharedIoProviderCapabilities provider_capabilities;

    // The session consumes the backend-neutral pair while tests and existing
    // Dawn probes may continue to inspect the concrete provider/program fields.
    SharedIoProgramSession::ProviderPair take_provider_pair() noexcept {
        return {std::move(provider), std::move(program)};
    }
};

DawnWaveNetFactoryResult create_dawn_wavenet(const WavenetProgramSpec& spec,
                                             const DawnWaveNetFactoryConfig& config) noexcept;

} // namespace pulp::gpu_audio::detail
