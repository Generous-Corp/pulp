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

enum class GpuConvolverTrialPath : std::uint8_t { StagedSync, StagedAsync, SharedAsync };
enum class GpuConvolverTrialLoad : std::uint8_t {
    Quiet,
    GraphiteUi,
    GpuContention,
    Overload,
};

// Metadata owned by the benchmark, not inferred from a trace record. A raw
// writer must receive this context explicitly and reject a trial that cannot
// prove its geometry, deadline, transfer counters, and timing provenance.
struct GpuConvolverTrialContext {
    std::uint64_t trial_id = 0;
    std::uint64_t pair_id = 0;
    GpuConvolverTrialPath path = GpuConvolverTrialPath::SharedAsync;
    GpuConvolverTrialLoad load = GpuConvolverTrialLoad::Quiet;
    std::uint32_t block_frames = 0;
    std::uint32_t sample_rate_hz = 0;
    std::uint32_t channels = 0;
    std::uint32_t ir_frames = 0;
    std::uint32_t inflight_depth = 0;
    std::uint32_t lead_blocks = 0;
    std::uint64_t deadline_ns = 0;
    std::uint64_t watchdog_ns = 0;
    bool transfer_counters_direct = false;
    bool timing_provenance_direct = false;
};

constexpr bool valid_gpu_convolver_trial_context(const GpuConvolverTrialContext& context) noexcept {
    return context.trial_id != 0 && context.pair_id != 0 && context.block_frames != 0 &&
           context.sample_rate_hz != 0 && context.channels != 0 && context.ir_frames != 0 &&
           context.inflight_depth != 0 && context.lead_blocks != 0 && context.deadline_ns != 0 &&
           context.watchdog_ns > context.deadline_ns && context.transfer_counters_direct &&
           context.timing_provenance_direct;
}

// Host-only configuration for a single diagnostic preparation. This is private
// until the paired P4 provider contract is complete. It is consumed exactly
// once by GpuConvolver::prepare() and is never read by the callback.
struct GpuConvolverTrialConfig {
    SharedIoRequest requested_path = SharedIoRequest::Auto;
    // Preparation generation stamped into staged terminal records. Shared-I/O
    // records receive their generation from the prepared arena; keeping this
    // explicit makes a matched staged/shared pair comparable without inventing
    // identity after the fact.
    std::uint64_t generation = 1;
    bool enable_trace = false;
    bool capture_admissions = false;
    bool capture_callback_timing = false;
    // Explicit blocking staged reference for the P4 campaign. This disables
    // the asynchronous staged ledger and records the existing blocking
    // GpuCompute call directly. It is never used by the normal runtime path.
    bool staged_sync_reference = false;
    std::uint32_t success_stride = 1;
    DawnSharedIoProvider::CompletionPolicy completion_policy =
        DawnSharedIoProvider::CompletionPolicy::ProcessEvents;
    std::uint64_t completion_wait_ns = 0;
};

// Must be called while the node is quiescent, before the next prepare(). The
// normal staged path uses the authenticated asynchronous ledger; the explicit
// staged_sync_reference flag enables the blocking reference recorder for P4.
bool configure_gpu_convolver_trial(GpuConvolver&, const GpuConvolverTrialConfig&) noexcept;

// Quiescent diagnostic drain. Returns complete authenticated worker records;
// it never reads or mutates the callback path. The caller owns serialization
// and must stop the transport worker before calling it.
bool drain_gpu_convolver_trial_records(GpuConvolver&, std::vector<SharedIoTraceRecord>&) noexcept;

} // namespace pulp::gpu_audio::detail
