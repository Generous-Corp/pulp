#pragma once

#include "shared_io_arena.hpp"
#include "shared_io_execution_predictor.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
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
    };
    struct SubmitToken {
        SharedIoSlotLedger::SlotToken slot;
        std::uint64_t deadline_ns = 0;
        std::uint64_t submitted_ns = 0;
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
        std::int64_t gpu_elapsed_ns = -1;
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
        bool prediction_enabled = false;
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
    LifecycleReceipt* receipt_for(const SharedIoSlotLedger::SlotToken&) noexcept;

    SharedIoArena arena_;
    SharedIoArenaProvider* provider_ = nullptr;
    SharedIoExecutionPredictor predictor_;
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
