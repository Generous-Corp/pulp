#pragma once

#include "dawn_shared_io_provider.hpp"
#include "shared_io_execution_contract.hpp"
#include "shared_io_provider_identity.hpp"
#include "shared_io_trace.hpp"

#include <pulp/runtime/crypto.hpp>

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

// Host-observed machine state; this never implies GPU execution or success.
enum class GpuConvolverThermalState : std::uint8_t { Unavailable, Nominal, Warm, Throttled };

// Metadata owned by the benchmark, not inferred from a trace record. A raw
// writer must receive this context explicitly and reject a trial that cannot
// prove its geometry, deadline, transfer counters, and timing provenance.
constexpr bool valid_gpu_convolver_thermal_state(GpuConvolverThermalState state) noexcept {
    return state == GpuConvolverThermalState::Unavailable ||
           state == GpuConvolverThermalState::Nominal || state == GpuConvolverThermalState::Warm ||
           state == GpuConvolverThermalState::Throttled;
}

inline bool valid_provider_identity_component(const std::string& value) noexcept {
    // Identity values are serialized into JSON by the private receipt writer.
    // Reject control characters and quoting rather than risking an ambiguous
    // or forged provenance projection.
    for (const unsigned char c : value) {
        if (c < 0x20 || c == '\"' || c == '\\')
            return false;
    }
    return !value.empty();
}

inline std::string provider_receipt_digest(const SharedIoProviderIdentity& identity,
                                           std::string_view native_runtime_revision) {
    const auto receipt = std::string{"pulp.gpu-audio.provider.v1\n"} +
                         "authenticated=" + (identity.authenticated ? "true\n" : "false\n") +
                         "provider_revision=" + identity.provider_revision + "\n" +
                         "adapter_name=" + identity.adapter_name + "\n" +
                         "adapter_backend=" + identity.adapter_backend + "\n" +
                         "adapter_vendor_id=" + std::to_string(identity.adapter_vendor_id) +
                         "\nadapter_device_id=" + std::to_string(identity.adapter_device_id) +
                         "\nnative_runtime_name=" + identity.native_runtime_name +
                         "\nnative_runtime_backend=" + identity.native_runtime_backend +
                         "\nnative_runtime_authenticated=" +
                         (identity.native_runtime_authenticated ? "true\n" : "false\n") +
                         "\nnative_runtime_revision=" + std::string(native_runtime_revision) + "\n";
    return pulp::runtime::sha256_hex(receipt);
}

inline bool valid_provider_receipt_digest(const SharedIoProviderIdentity& identity,
                                          std::string_view native_runtime_revision,
                                          std::string_view immutable_receipt_digest) noexcept {
    if (immutable_receipt_digest.size() != 64)
        return false;
    for (const auto c : immutable_receipt_digest) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    try {
        return immutable_receipt_digest ==
               provider_receipt_digest(identity, native_runtime_revision);
    } catch (...) {
        // Receipt validation is a fail-closed boundary. Canonicalization uses
        // allocating std::string operations, so allocation or hashing failures
        // must reject the receipt rather than escape into the trace writer.
        return false;
    }
}

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
    std::uint32_t queue_capacity = 0;
    std::uint32_t max_inflight = 0;
    std::uint32_t lead_blocks = 0;
    std::uint64_t deadline_ns = 0;
    std::uint64_t watchdog_ns = 0;
    bool transfer_counters_direct = false;
    bool timing_provenance_direct = false;
    bool workgroup_requested = false;
    bool workgroup_joined = false;
    GpuConvolverThermalState thermal_state = GpuConvolverThermalState::Unavailable;
    // Receipts require an authenticated provider identity; missing identity
    // fails closed and cannot be interpreted as physical GPU evidence.
    SharedIoProviderIdentity provider_identity;
    // These receipt fields are private to the trial context. They must not be
    // added to SharedIoProviderIdentity, which crosses an existing ABI seam.
    std::string native_runtime_revision;
    std::string immutable_receipt_digest;
};

inline bool valid_gpu_convolver_trial_context(const GpuConvolverTrialContext& context) noexcept {
    return context.trial_id != 0 && context.pair_id != 0 && context.block_frames != 0 &&
           context.sample_rate_hz != 0 && context.channels != 0 && context.ir_frames != 0 &&
           context.inflight_depth != 0 && context.queue_capacity > context.lead_blocks &&
           context.max_inflight != 0 &&
           context.max_inflight <= context.queue_capacity - context.lead_blocks &&
           context.lead_blocks != 0 && context.deadline_ns != 0 &&
           context.watchdog_ns > context.deadline_ns && context.transfer_counters_direct &&
           context.timing_provenance_direct && context.provider_identity.authenticated &&
           valid_provider_identity_component(context.provider_identity.provider_revision) &&
           valid_provider_identity_component(context.provider_identity.adapter_name) &&
           valid_provider_identity_component(context.provider_identity.adapter_backend) &&
           context.provider_identity.adapter_vendor_id != 0 &&
           context.provider_identity.adapter_device_id != 0 &&
           // This value is projected verbatim into the private JSONL receipt;
           // reject unsafe bytes before digest validation or serialization.
           valid_provider_identity_component(context.native_runtime_revision) &&
           valid_provider_receipt_digest(context.provider_identity, context.native_runtime_revision,
                                         context.immutable_receipt_digest) &&
           context.provider_identity.native_runtime_authenticated &&
           valid_provider_identity_component(context.provider_identity.native_runtime_name) &&
           valid_provider_identity_component(context.provider_identity.native_runtime_backend) &&
           valid_gpu_convolver_thermal_state(context.thermal_state) &&
           (!context.workgroup_requested || context.workgroup_joined);
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
    // Diagnostic campaign slot capacity. Zero preserves the production
    // default. Non-zero values are accepted only by the private probe and are
    // passed through to the prepared shared-I/O session; the public runtime
    // ABI remains unchanged.
    std::uint32_t slots = 0;
    // Private diagnostic retention bound. A probe sets this to its complete
    // callback census size; zero leaves the ordinary runtime queue behavior.
    std::uint32_t retention_capacity = 0;
};

// Quiescent diagnostic drain for the complete admission census. This remains
// private/default-off and is intentionally separate from terminal records so
// consumers can prove admission/terminal/delivery identity multisets.
bool drain_gpu_convolver_trial_admissions(GpuConvolver&,
                                          std::vector<SharedIoTraceAdmission>&) noexcept;

// Quiescent diagnostic identity accessor. Zero means the prepared trace did
// not expose an authenticated engine identity and must fail closed.
std::uint64_t gpu_convolver_trial_engine_id(const GpuConvolver&) noexcept;

// Must be called while the node is quiescent, before the next prepare(). The
// normal staged path uses the authenticated asynchronous ledger; the explicit
// staged_sync_reference flag enables the blocking reference recorder for P4.
bool configure_gpu_convolver_trial(GpuConvolver&, const GpuConvolverTrialConfig&) noexcept;

// Quiescent diagnostic drain. Returns complete authenticated worker records;
// it never reads or mutates the callback path. The caller owns serialization
// and must stop the transport worker before calling it.
bool drain_gpu_convolver_trial_records(GpuConvolver&, std::vector<SharedIoTraceRecord>&) noexcept;

} // namespace pulp::gpu_audio::detail
