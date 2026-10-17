#pragma once

#include "shared_io_arena.hpp"
#include "shared_io_execution_predictor.hpp"
#include "shared_io_execution_contract.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pulp::gpu_audio::detail {

class SharedIoComputePlan {
  public:
    // Fixed storage keeps the RT-facing admission path allocation-free. A
    // diagnostic campaign that exceeds this bound is invalidated explicitly.
    static constexpr std::size_t kLifecycleReceiptCapacity = 4096;
    struct Config {
        std::uint32_t slots = 0;
        std::size_t input_bytes_per_slot = 0;
        std::size_t output_bytes_per_slot = 0;
        SharedIoArenaProvider::StorageKind storage_kind =
            SharedIoArenaProvider::StorageKind::ImportedHostPointer;
        // Opt-in G3 calibration identity.  The default keeps the existing
        // private predictor behavior and does not require provider metadata.
        SharedIoCalibrationIdentity calibration_identity;
        bool authenticated_calibration = false;
        std::uint64_t calibration_max_age_ns = 0;
        std::string calibration_source_revision;
    };
    struct SubmitToken {
        SharedIoSlotLedger::SlotToken slot;
        std::uint64_t deadline_ns = 0;
        std::uint64_t submitted_ns = 0;
        std::uint64_t worker_ns = 0;
        std::uint64_t predicted_ns = 0;
    };
    struct Completion {
        SubmitToken token;
        SharedIoArena::CompletionStatus status = SharedIoArena::CompletionStatus::RetiredFailed;
        bool late = false;
        std::uint64_t gpu_elapsed_ns = 0;
        bool gpu_elapsed_available = false;
    };
    // Private diagnostic lifecycle evidence. GPU timestamps remain unavailable
    // until the provider supplies an authenticated timestamp query.
    struct LifecycleReceipt {
        std::uint64_t generation = 0;
        std::uint64_t sequence = 0;
        std::int64_t submit_ns = -1;
        std::int64_t terminal_ns = -1;
        std::int64_t service_ns = -1;
        std::int64_t delivery_ns = -1;
        std::int64_t worker_ns = -1;
        std::int64_t visible_ns = -1;
        std::int64_t gpu_elapsed_ns = -1;
        std::uint64_t predicted_ns = 0;
        SharedIoCalibrationIdentity calibration_identity;
        SharedIoCalibrationProvenance provenance;
        bool provider_authenticated = false;
        bool synchronized = false;
        bool predictor_underestimate = false;
        bool terminal_success = false;
        bool late = false;
        bool stale = false;
        bool terminal = false;
        bool output_acquired = false;
        bool output_released = false;
        bool discarded = false;
        bool device_lost = false;
        bool gpu_timestamp_available = false;
    };
    struct Telemetry {
        std::uint64_t payload_bytes_copied = 0;
        std::uint64_t encode_submit_ns = 0;
        std::uint64_t gpu_elapsed_ns = 0;
        std::uint64_t completion_wall_ns = 0;
        std::uint64_t gpu_timestamp_samples = 0;
        std::uint64_t misses = 0;
        std::uint64_t late_completions = 0;
        std::uint64_t high_water_in_flight = 0;
        std::uint64_t prediction_samples = 0;
        std::uint64_t prediction_refusals = 0;
        std::uint64_t prediction_underestimates = 0;
        std::uint64_t calibration_rows = 0;
        std::uint64_t invalid_calibration_rows = 0;
        std::uint64_t stale_calibration_rows = 0;
        std::uint64_t cpu_fallbacks = 0;
        std::uint64_t rejected_calibration_rows = 0;
        bool calibration_invalidated = false;
        bool prediction_enabled = false;
        SharedIoPredictionRefusal last_prediction_refusal = SharedIoPredictionRefusal::None;
        SharedIoFallbackReason last_fallback_reason = SharedIoFallbackReason::None;
        // The shared Dawn provider has no timestamp-query/occupancy certificate yet.
        bool gpu_timing_available = false;
        bool occupancy_available = false;
    };

    SharedIoComputePlan() = default;
    ~SharedIoComputePlan() = default;
    SharedIoComputePlan(const SharedIoComputePlan&) = delete;
    SharedIoComputePlan& operator=(const SharedIoComputePlan&) = delete;

    bool prepare(SharedIoArenaProvider& provider, const Config& config);
    bool prepare(SharedIoArenaProvider& provider, const Config& config,
                 std::unique_ptr<SharedIoPreparedProgram> program);
    // Explicit opt-in. The default policy preserves the existing admission
    // behavior and records no predictor-based refusal.
    void set_prediction_policy(SharedIoExecutionPredictor::Config config) noexcept {
        predictor_.configure(config);
        predictor_.reset();
        telemetry_.prediction_samples = 0;
        telemetry_.prediction_refusals = 0;
        telemetry_.prediction_enabled = config.enabled;
    }
    SharedIoExecutionPredictor::Estimate prediction_estimate() const noexcept {
        return predictor_.estimate();
    }
    bool prepared() const noexcept {
        return arena_.prepared();
    }
    std::uint64_t preparation_epoch() const noexcept {
        return arena_.preparation_epoch();
    }
    std::optional<SharedIoArena::WriteLease> acquire_input(std::uint64_t sequence,
                                                           std::uint64_t deadline_ns) noexcept;
    void record_cpu_fallback(SharedIoFallbackReason reason) noexcept {
        telemetry_.last_fallback_reason = reason;
        if (telemetry_.cpu_fallbacks != UINT64_MAX)
            ++telemetry_.cpu_fallbacks;
    }
    bool submit(const SubmitToken& token) noexcept;
    // A pre-submit refusal must return the write lease to the fixed ledger.
    bool cancel(const SubmitToken& token) noexcept;
    std::size_t drain(std::uint64_t now_ns) noexcept;
    std::size_t drain_until(std::uint64_t now_ns, std::uint64_t service_deadline_ns) noexcept;
    std::optional<Completion> pop_completion() noexcept;
    std::vector<LifecycleReceipt> take_lifecycle_receipts() {
        // Receipt extraction is a quiescent diagnostic operation. Keeping the
        // live vector intact while work is pending prevents partial evidence.
        if (!arena_.quiescent())
            return {};
        std::vector<LifecycleReceipt> result(lifecycle_receipts_.begin(),
                                             lifecycle_receipts_.end());
        lifecycle_receipts_.clear();
        return result;
    }
    std::vector<SharedIoCalibrationObservation> take_calibration_observations() {
        if (!arena_.quiescent())
            return {};
        std::vector<SharedIoCalibrationObservation> result;
        result.reserve(lifecycle_receipts_.size());
        for (const auto& receipt : lifecycle_receipts_) {
            SharedIoCalibrationObservation observation;
            observation.identity = receipt.calibration_identity;
            observation.provenance = receipt.provenance;
            observation.generation = receipt.generation;
            observation.sequence = receipt.sequence;
            observation.worker_ns =
                receipt.worker_ns > 0 ? static_cast<std::uint64_t>(receipt.worker_ns) : 0;
            observation.submitted_ns =
                receipt.submit_ns > 0 ? static_cast<std::uint64_t>(receipt.submit_ns) : 0;
            observation.terminal_ns =
                receipt.terminal_ns > 0 ? static_cast<std::uint64_t>(receipt.terminal_ns) : 0;
            observation.visible_ns =
                receipt.visible_ns > 0 ? static_cast<std::uint64_t>(receipt.visible_ns) : 0;
            observation.predicted_ns = receipt.predicted_ns;
            observation.provider_authenticated = receipt.provider_authenticated;
            observation.terminal_success = receipt.terminal_success;
            observation.late = receipt.late;
            observation.stale = receipt.stale;
            observation.discarded = receipt.discarded;
            observation.device_lost = receipt.device_lost;
            observation.output_acquired = receipt.output_acquired;
            observation.output_released = receipt.output_released;
            observation.lifecycle_valid = lifecycle_receipt_valid();
            if (!observation.valid())
                ++telemetry_.invalid_calibration_rows;
            result.push_back(observation);
        }
        lifecycle_receipts_.clear();
        return result;
    }
    bool lifecycle_receipt_overflow() const noexcept {
        return lifecycle_receipt_overflow_;
    }
    bool lifecycle_receipt_valid() const noexcept {
        return !lifecycle_receipt_overflow_ && !completion_receipt_overflow_;
    }
    std::optional<SharedIoArena::OutputLease> acquire_output(const Completion&) noexcept;
    bool expire_delivery(const Completion&) noexcept;
    // Terminal failure and expired success retain storage until this explicit
    // non-RT disposition relinquishes the exact token.
    bool discard_completion(const Completion&) noexcept;
    // Host/quiescent only. Reuses the persistent slot allocations while
    // advancing epoch identity, so prior completion/token records are stale.
    bool reprime_when_quiescent() noexcept;
    bool release_output(const SharedIoArena::ReleaseRecord&) noexcept;
    bool release() noexcept {
        const bool released = arena_.release();
        if (released)
            provider_ = nullptr;
        return released;
    }
    const Telemetry& telemetry() const noexcept {
        return telemetry_;
    }
    std::size_t available_slots() const noexcept {
        return arena_.available_slots();
    }

  private:
    struct Pending {
        SubmitToken token;
        bool active = false;
    };
    static void on_terminal(void* context, const SharedIoSlotLedger::SlotToken& token,
                            SharedIoArena::CompletionStatus status) noexcept;
    void record_terminal(const SharedIoSlotLedger::SlotToken& token,
                         SharedIoArena::CompletionStatus status) noexcept;
    void record_calibration(LifecycleReceipt&) noexcept;
    LifecycleReceipt* receipt_for(const SharedIoSlotLedger::SlotToken&) noexcept;

    SharedIoArena arena_;
    SharedIoArenaProvider* provider_ = nullptr;
    SharedIoExecutionPredictor predictor_;
    SharedIoCalibrationIdentity calibration_identity_;
    SharedIoCalibrationProvenance calibration_provenance_;
    SharedIoProviderIdentity provider_identity_snapshot_;
    bool authenticated_calibration_ = false;
    std::uint64_t calibration_max_age_ns_ = 0;
    bool provider_authenticated_ = false;
    std::uint64_t last_calibration_ns_ = 0;
    std::vector<Pending> pending_;
    std::vector<Completion> completions_;
    std::vector<LifecycleReceipt> lifecycle_receipts_;
    std::vector<std::size_t> receipt_index_by_slot_;
    bool lifecycle_receipt_overflow_ = false;
    bool completion_receipt_overflow_ = false;
    std::size_t completion_read_ = 0;
    std::size_t completion_write_ = 0;
    Telemetry telemetry_;
};

} // namespace pulp::gpu_audio::detail
