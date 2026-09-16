#pragma once

#include "shared_io_slot_ledger.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace pulp::gpu_audio::detail {

// These are provider-certified GPU retirement results, not delivery deadlines,
// Queue::OnSubmittedWorkDone statuses, or device-loss notifications by
// themselves. In particular, an early validation callback cannot free backing
// pages. Failed is emitted only after the provider's explicit drain/disposal
// boundary proves the submitted generation can no longer touch its resources.
enum class SharedIoTerminalStatus : std::uint8_t { RetiredSuccess, RetiredFailed };

// Independently owned terminal-completion inbox. Each physical slot contributes
// exactly one token-qualified credit. The dispatcher reserves it before submit;
// expiry does not return it. A provider callback turns that same credit Ready,
// and a stale or duplicate callback cannot consume another slot's reservation.
// Keeping this object alive is necessary but not sufficient to free resources:
// only the arena's terminal drain plus ledger quiescence permits destruction.
class SharedIoTerminalInbox {
  public:
    using SlotToken = SharedIoSlotLedger::SlotToken;

    struct Record {
        SlotToken token;
        SharedIoTerminalStatus status = SharedIoTerminalStatus::RetiredFailed;
    };

    enum class PushResult : std::uint8_t { Accepted, Rejected, Busy };

    struct CompletionClaim {
        std::uint32_t slot = 0;
        std::uint64_t completing_control = 0;
    };

    struct ClaimAttempt {
        PushResult result = PushResult::Busy;
        std::optional<CompletionClaim> claim;
    };

    struct CancellationClaim {
        std::uint32_t slot = 0;
        std::uint64_t cancelling_control = 0;
    };

    bool prepare(std::uint32_t capacity);
    bool reserve(const SlotToken& token) noexcept;
    bool cancel(const SlotToken& token) noexcept;
    std::optional<CancellationClaim> try_claim_cancellation(const SlotToken& token) noexcept;
    bool finish_cancellation(CancellationClaim claim) noexcept;
    ClaimAttempt try_claim(std::uint32_t slot) noexcept;
    PushResult finish_claim(CompletionClaim claim, const SlotToken& token,
                            SharedIoTerminalStatus status) noexcept;
    PushResult push(const SlotToken& token, SharedIoTerminalStatus status) noexcept;
    bool try_pop(Record& record) noexcept;
    bool quiescent() const noexcept;
    std::uint64_t rejected_callbacks() const noexcept {
        return rejected_callbacks_.load(std::memory_order_relaxed);
    }

  private:
    enum class CellState : std::uint8_t {
        Free,
        WritingReservation,
        Reserved,
        Completing,
        Publishing,
        Ready,
        Consuming,
        Cancelling,
    };
    struct Cell {
        std::atomic<std::uint64_t> control{0};
        SlotToken token;
        SharedIoTerminalStatus status = SharedIoTerminalStatus::RetiredFailed;
    };

    static constexpr std::uint64_t kStateBits = 3;
    static constexpr std::uint64_t kStateMask = (1u << kStateBits) - 1u;
    static constexpr std::uint64_t kMaxStamp =
        std::numeric_limits<std::uint64_t>::max() >> kStateBits;
    static constexpr std::uint64_t control(std::uint64_t stamp, CellState state) noexcept {
        return (stamp << kStateBits) | static_cast<std::uint64_t>(state);
    }
    static constexpr CellState state_of(std::uint64_t value) noexcept {
        return static_cast<CellState>(value & kStateMask);
    }
    static constexpr std::uint64_t stamp_of(std::uint64_t value) noexcept {
        return value >> kStateBits;
    }

    std::unique_ptr<Cell[]> cells_;
    std::uint32_t capacity_ = 0;
    std::uint32_t read_cursor_ = 0;
    std::atomic<std::uint64_t> rejected_callbacks_{0};
};

// Private provider boundary for the actual host allocations and wrapped GPU
// buffers. A Dawn implementation owns both allocations behind `opaque`; tests
// use the same transaction boundary with deterministic fake resources.
class SharedIoArenaProvider {
  public:
    using SlotToken = SharedIoSlotLedger::SlotToken;

    struct AllocationLifecycle {
        bool allocated = false;
        bool import_attempted = false;
        bool import_succeeded = false;
        bool dispose_observed = false;
        bool host_freed = false;
    };

    struct SlotResources {
        std::byte* input = nullptr;
        std::size_t input_size = 0;
        std::byte* output = nullptr;
        std::size_t output_size = 0;
        void* opaque = nullptr;
        AllocationLifecycle input_lifecycle;
        AllocationLifecycle output_lifecycle;
    };

    using CompletionStatus = SharedIoTerminalStatus;

    virtual ~SharedIoArenaProvider() = default;

    // Creation is a transaction. Both successful imports and partial failures
    // expose their allocation/import stages through `resources`; a validation or
    // fake-OOM rejection before import legitimately has no disposal callback.
    // Host memory remains provider-owned until retire_slot(), drain(), and then
    // destroy_slot() complete. A callback records disposal only; it never frees
    // host memory directly.
    virtual bool create_slot(std::uint32_t slot, std::size_t input_bytes, std::size_t output_bytes,
                             SlotResources& resources) noexcept = 0;
    virtual void retire_slot(SlotResources& resources) noexcept = 0;
    virtual void destroy_slot(SlotResources& resources) noexcept = 0;

    // Accepted work completes exactly once through the independently retained
    // inbox. Rejection must not push. A provider must correlate the token with
    // live device/submission state; raw OnSubmittedWorkDone Success/Error/
    // CallbackCancelled is insufficient. Push only after actual retirement or
    // after drain/disposal proves the generation cannot access its resources.
    // Pulp-owned slot, callback-userdata, and terminal records must have been
    // preallocated while creating the fixed slots. Dawn/driver command encoder,
    // command buffer, event, and native submission allocations are permitted
    // only on this serialized non-RT dispatcher and are measured separately;
    // they are not covered by the arena's allocation-free claim. Callbacks may
    // be concurrent. If inbox->push() returns Busy, the provider retains the
    // terminal record and retries later; Accepted or Rejected is final.
    // A true result is required after any Queue::Submit attempt. False is only
    // for a proven pre-submit refusal; asynchronous validation/device errors are
    // accepted work and remain quarantined until certified terminal retirement.
    virtual bool submit(const SlotResources& resources, SlotToken token,
                        std::shared_ptr<SharedIoTerminalInbox> terminal_inbox) noexcept = 0;

    // Stops new provider activity, crosses the provider-specific loss/cancel
    // disposal boundary, makes every accepted submission terminal, pushes its
    // result, and returns only when no later callback can occur.
    virtual void drain() noexcept = 0;
};

// Prepared, fixed-capacity owner for shared CPU/GPU I/O slots. All methods other
// than the provider callback are called by one serialized non-RT dispatcher.
// prepare()/release() establish and destroy allocation ownership. Pulp-owned
// arena/ledger bookkeeping and preallocated callback/terminal records allocate
// nothing during the prepared runtime; provider/Dawn dispatcher allocations are
// outside that claim and must be measured separately. The provider must outlive
// the arena and every explicit release() call.
class SharedIoArena {
  public:
    using SlotToken = SharedIoSlotLedger::SlotToken;
    using CompletionStatus = SharedIoArenaProvider::CompletionStatus;

    struct Config {
        enum class PrepareFault : std::uint8_t {
            None,
            AfterLedger,
            AfterResourceTables,
            AfterInbox,
        };

        std::uint32_t slots = 0;
        std::size_t input_bytes_per_slot = 0;
        std::size_t output_bytes_per_slot = 0;
        // Deterministic private fault injection for the bool transaction.
        PrepareFault prepare_fault = PrepareFault::None;
    };

    struct WriteLease {
        SlotToken token;
        std::span<std::byte> bytes;
    };

    struct PublishRecord {
        SlotToken token;
    };

    struct OutputLease {
        SlotToken token;
        std::span<const std::byte> bytes;
    };

    struct ReleaseRecord {
        SlotToken token;
    };

    struct CompletionDrain {
        std::size_t accepted = 0;
        std::size_t rejected_stale_or_duplicate = 0;
    };

    SharedIoArena() = default;
    ~SharedIoArena();

    SharedIoArena(const SharedIoArena&) = delete;
    SharedIoArena& operator=(const SharedIoArena&) = delete;
    SharedIoArena(SharedIoArena&&) = delete;
    SharedIoArena& operator=(SharedIoArena&&) = delete;

    bool prepare(SharedIoArenaProvider& provider, const Config& config);
    // Host/quiescent teardown. Calling this explicitly asserts every producer
    // has returned its write lease and every consumer has stopped reading its
    // output lease. The provider is then terminally drained before resources are
    // destroyed. A false result deliberately preserves backing allocations.
    bool release() noexcept;

    bool prepared() const noexcept {
        return prepared_;
    }
    bool retiring() const noexcept {
        return ledger_.retiring();
    }
    bool quiescent() const noexcept {
        return ledger_.quiescent();
    }
    std::uint64_t preparation_epoch() const noexcept {
        return ledger_.preparation_epoch();
    }
    std::size_t available_slots() const noexcept {
        return ledger_.available_slots();
    }
    std::uint64_t rejected_terminal_callbacks() const noexcept {
        return terminal_inbox_ ? terminal_inbox_->rejected_callbacks() : 0;
    }

    // Dispatcher grants a known absolute sequence. The producer writes only the
    // lease bytes, then sends PublishRecord back; it never touches the ledger.
    std::optional<WriteLease> grant_write(std::uint64_t stream_sequence) noexcept;
    bool publish_written(const PublishRecord& record) noexcept {
        return ledger_.publish(record.token);
    }
    bool submit(const SlotToken& token) noexcept;
    bool expire_delivery(const SlotToken& token) noexcept {
        return ledger_.expire_delivery(token);
    }
    CompletionDrain drain_completions() noexcept;
    std::optional<OutputLease> acquire_output(std::uint64_t expected_epoch,
                                              std::uint64_t expected_sequence) noexcept;
    bool release_output(const ReleaseRecord& record) noexcept {
        return ledger_.release(record.token);
    }
    // Acquired write/output spans must already have been relinquished before
    // the dispatcher discards their generation.
    bool discard(const SlotToken& token) noexcept {
        return ledger_.discard(token);
    }

    void begin_retirement() noexcept;
    bool reset_when_quiescent() noexcept;

  private:
    struct RejectedSubmission {
        SlotToken token;
        bool pending = false;
    };

    bool rollback_rejected_submission(const SlotToken& token) noexcept;
    void retry_rejected_submissions() noexcept;
    void discard_cpu_owned_after_quiescence() noexcept;
    const SharedIoArenaProvider::SlotResources*
    resources_for(const SlotToken& token) const noexcept;

    SharedIoSlotLedger ledger_;
    SharedIoArenaProvider* provider_ = nullptr;
    std::vector<SharedIoArenaProvider::SlotResources> resources_;
    std::vector<RejectedSubmission> rejected_submissions_;
    std::shared_ptr<SharedIoTerminalInbox> terminal_inbox_;
    bool prepared_ = false;
};

} // namespace pulp::gpu_audio::detail
