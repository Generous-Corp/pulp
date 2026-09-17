#pragma once

#include "shared_io_trace.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

namespace pulp::gpu_audio::detail {

enum class SharedIoRecoveryReason : std::uint8_t {
    None,
    SequenceGap,
    InputSaturated,
    ProviderFailure,
    ProviderLost,
    OfflineFence,
    InvalidCallback
};

// Fixed audio records between one callback producer/consumer and one serialized
// worker. The worker owns epoch changes, ingress consumption, and egress
// publication. Neither epoch changes nor output drops reclaim a consumer lease.
// Sample spans are planar [channel][frame], with exactly the prepared channel
// count times block size values.
class SharedIoStampedBridge {
  public:
    static constexpr std::uint64_t kSequenceLimit = std::uint64_t{1} << 63;
    static constexpr std::uint32_t kLeadBlocks = 2;
    struct Stamp {
        std::uint64_t epoch = 0;
        std::uint64_t sequence = 0;
        friend bool operator==(const Stamp&, const Stamp&) = default;
    };
    struct Config {
        std::uint32_t capacity = 0;
        std::uint32_t channels = 0;
        std::uint32_t block_size = 0;
        // Must be at least one and strictly less than capacity. The extra
        // capacity slot keeps the callback producer from colliding with the
        // lead window while a result is in flight.
        std::uint32_t lead_blocks = kLeadBlocks;
    };
    enum class Admission : std::uint8_t { Accepted, Full, CpuOnly, Invalid, SequenceExhausted };
    struct Callback {
        Stamp stamp;
        Admission admission = Admission::Invalid;
        bool valid() const noexcept {
            return admission == Admission::Accepted || admission == Admission::Full ||
                   admission == Admission::CpuOnly;
        }
    };
    enum class Delivery : std::uint8_t { Ready, Priming, Missing, EpochChanged, Invalid };
    enum class Publication : std::uint8_t { Published, DroppedFull, Invalid, CounterExhausted };

    class Lease {
      public:
        Stamp stamp() const noexcept {
            return stamp_;
        }
        std::span<const float> samples() const noexcept {
            return samples_;
        }

      private:
        friend class SharedIoStampedBridge;
        const void* queue_ = nullptr;
        std::uint64_t cursor_ = 0;
        Stamp stamp_;
        std::span<const float> samples_;
    };
    struct Claim {
        Delivery delivery = Delivery::Invalid;
        std::optional<Lease> lease;
        std::uint32_t stale_drained = 0;
    };

    // Host/quiescent only, one-shot. Destruction also requires both callers to
    // have stopped and returned all leases. Epoch zero means CPU-only.
    bool prepare(Config, std::uint64_t first_epoch, std::uint64_t first_sequence = 0);

    // Host/quiescent only; recorder lifetime covers both producers and the drain.
    void set_trace(SharedIoTraceRecorder* trace, SharedIoTelemetry* telemetry = nullptr) noexcept {
        trace_ = trace;
        trace_telemetry_ = telemetry;
    }

    // Callback only. Valid blocks advance absolute sequence even on a full
    // ingress or while GPU delivery is disabled. Finish each callback's output
    // before beginning another callback; fallback priming belongs to the owner.
    Callback begin_callback(std::span<const float> samples) noexcept {
        return begin_callback(samples, next_sequence_);
    }
    Callback begin_callback(std::span<const float> samples, std::uint64_t sequence) noexcept;
    void request_recovery(SharedIoRecoveryReason reason) noexcept;
    SharedIoRecoveryReason recovery_reason() const noexcept {
        return recovery_reason_.load(std::memory_order_acquire);
    }
    bool complete_callback_delivery(const Callback&, SharedIoDeliveryDisposition) noexcept;
    Claim claim_output(const Callback&) noexcept;
    Delivery finish_output(const Lease&, std::span<float> output) noexcept;
    Delivery consume_output(const Callback&, std::span<float> output, bool* finalized = nullptr,
                            bool defer_delivery = false) noexcept;

    // Worker only. Input and output samples stay immutable until the consumer
    // returns its lease. Output publication may skip sequences but never reorder
    // them; a future front record therefore cannot fill an earlier hole.
    // An admission reserved before a recovery request may finish. Closing the
    // gate prevents every later reservation without waiting on the callback.
    bool begin_worker_admission() noexcept;
    void end_worker_admission() noexcept;
    std::optional<Lease> acquire_input() noexcept;
    bool release_input(const Lease&) noexcept;
    Publication publish_output(Stamp, std::span<const float> samples) noexcept;
    std::optional<Stamp> finalized_through() const noexcept {
        return finalized_;
    }

    // Host/quiescent only, after stopping old submissions and callback invocation. Physical provider drain and
    // history reprime remain the owner's obligation before activate_epoch().
    // The first ingress record with the new epoch acknowledges the transition.
    void suspend_delivery() noexcept;
    bool activate_epoch(std::uint64_t epoch) noexcept;
    std::uint64_t delivery_epoch() const noexcept {
        return delivery_epoch_.load(std::memory_order_acquire);
    }
    // Callback-owned absolute stream position. The serialized worker uses this
    // at an epoch boundary so its collector starts at the exact record the
    // callback will stamp next; epochs never restart a sequence at zero.
    std::uint64_t next_sequence() const noexcept {
        return next_sequence_;
    }
    std::size_t samples_per_block() const noexcept {
        return sample_count_;
    }
    std::uint32_t lead_blocks() const noexcept {
        return lead_blocks_;
    }

  private:
    static constexpr std::uint64_t kNoLease = std::numeric_limits<std::uint64_t>::max();
    struct Queue {
        std::vector<Stamp> stamps;
        std::vector<float> samples;
        alignas(64) std::atomic<std::uint64_t> read{0};
        alignas(64) std::atomic<std::uint64_t> write{0};
        std::uint64_t claimed = kNoLease; // consumer only
    };
    Publication publish(Queue&, Stamp, std::span<const float>) noexcept;
    std::optional<Lease> acquire(Queue&) noexcept;
    bool release(Queue&, const Lease&) noexcept;
    bool current_callback(const Callback&) const noexcept;

    void trace_delivery(Delivery) noexcept;
    Queue ingress_, egress_;
    SharedIoTraceRecorder* trace_ = nullptr;
    SharedIoTelemetry* trace_telemetry_ = nullptr;
    std::uint64_t epoch_first_sequence_ = 0; // changes only while callback is quiescent
    bool defer_delivery_ = false;
    bool delivery_pending_ = false;
    Delivery observed_delivery_ = Delivery::Invalid;
    std::atomic<unsigned> admission_gate_{0}; // open bit 1, worker reservation bit 2
    std::atomic<SharedIoRecoveryReason> recovery_reason_{SharedIoRecoveryReason::None};
    bool trace_output_eligible_ = false; // callback only
    std::atomic<std::uint64_t> delivery_epoch_{0};
    std::uint64_t last_epoch_ = 0;    // worker only
    std::optional<Stamp> finalized_;  // worker/offline pump only
    std::uint64_t next_sequence_ = 0; // callback only
    Stamp current_;
    bool callback_open_ = false;
    std::size_t sample_count_ = 0;
    std::uint32_t capacity_ = 0;
    std::uint32_t lead_blocks_ = kLeadBlocks;
    bool prepared_ = false;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<SharedIoRecoveryReason>::is_always_lock_free);

} // namespace pulp::gpu_audio::detail
