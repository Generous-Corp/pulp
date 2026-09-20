#pragma once

#include "dawn_shared_io_provider.hpp"
#include "shared_io_execution_contract.hpp"
#include "shared_io_trace.hpp"

#include <cstdint>
#include <vector>

namespace pulp::gpu_audio {
class GpuConvolver;
}

namespace pulp::gpu_audio::detail {

// Host-only configuration for a single diagnostic preparation. This is private
// until the paired P4 provider contract is complete. It is consumed exactly
// once by GpuConvolver::prepare() and is never read by the callback.
struct GpuConvolverTrialConfig {
    SharedIoRequest requested_path = SharedIoRequest::Auto;
    bool enable_trace = false;
    bool capture_admissions = false;
    std::uint32_t success_stride = 1;
    DawnSharedIoProvider::CompletionPolicy completion_policy =
        DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
    std::uint64_t completion_wait_ns = 0;
};

// Must be called while the node is quiescent, before the next prepare(). A
// requested staged path currently fails closed because the legacy GpuCompute
// provider has no authenticated SharedIoTraceRecord bridge yet.
bool configure_gpu_convolver_trial(GpuConvolver&, const GpuConvolverTrialConfig&) noexcept;

// Quiescent diagnostic drain. Returns complete authenticated worker records;
// it never reads or mutates the callback path. The caller owns serialization
// and must stop the transport worker before calling it.
bool drain_gpu_convolver_trial_records(GpuConvolver&, std::vector<SharedIoTraceRecord>&) noexcept;

} // namespace pulp::gpu_audio::detail
