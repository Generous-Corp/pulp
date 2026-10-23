#pragma once

#include "realtime_gpu_audio_path.hpp"
#include "shared_io_stamped_bridge.hpp"

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace pulp::gpu_audio::detail {

// Coherent, allocation-free copy of one trivially copyable value, published by
// a serialized writer and read from any host or UI thread.
//
// The guard is the sequence scheme used for SharedIoTelemetry's completion
// timing: an odd sequence marks an update in progress, both sides give up
// after a bounded number of attempts, and the value is stored as atomic words
// so a torn attempt is a retry rather than a data race.
template <class T> class SeqlockCell {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::is_default_constructible_v<T>);
    static constexpr std::size_t kWords =
        (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

  public:
    // Returns false when the sample was dropped because another writer held
    // the guard; that writer's update is the latest one instead.
    bool publish(const T& value) noexcept {
        if (!try_begin_update())
            return false;
        std::array<std::uint64_t, kWords> words{};
        std::memcpy(words.data(), &value, sizeof(T));
        for (std::size_t i = 0; i < kWords; ++i)
            words_[i].store(words[i], std::memory_order_relaxed);
        published_.store(true, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // Host/quiescent lifecycle operation: it may wait for an in-progress
    // update so that the reset is never dropped.
    void reset() noexcept {
        while (!try_begin_update()) {
        }
        published_.store(false, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }

    // False when nothing has been published since reset(), or when no stable
    // copy could be taken because a writer held the update open.
    bool read(T& out) const noexcept {
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            const auto sequence = sequence_.load(std::memory_order_acquire);
            if ((sequence & 1U) != 0U)
                continue;
            std::array<std::uint64_t, kWords> words{};
            for (std::size_t i = 0; i < kWords; ++i)
                words[i] = words_[i].load(std::memory_order_relaxed);
            const bool published = published_.load(std::memory_order_relaxed);
            // Orders the relaxed data loads before the re-check; an acquire
            // load alone would let them be satisfied after it.
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == sequence) {
                if (!published)
                    return false;
                std::memcpy(&out, words.data(), sizeof(T));
                return true;
            }
        }
        return false;
    }

  private:
    static constexpr int kAttempts = 64;

    bool try_begin_update() noexcept {
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            auto sequence = sequence_.load(std::memory_order_relaxed);
            if ((sequence & 1U) == 0U &&
                sequence_.compare_exchange_weak(sequence, sequence + 1, std::memory_order_acquire,
                                                std::memory_order_relaxed)) {
                // Keeps the following data stores from becoming visible before
                // the odd sequence value that marks the update as in progress.
                std::atomic_thread_fence(std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<bool> published_{false};
    std::array<std::atomic<std::uint64_t>, kWords> words_{};
};

// The node's health hook is a serialized non-RT read (transport worker or a
// quiescent host); this mirror republishes it for host and UI readers.
using RealtimeGpuHealthMirror = SeqlockCell<RealtimeGpuHealth>;

inline bool provider_loss_reason(GpuAudioRecoveryReason reason) noexcept {
    return reason == GpuAudioRecoveryReason::ProviderLost ||
           reason == GpuAudioRecoveryReason::ProviderUnresponsive;
}

// Shared provider-state projection for every node health source. Loss of the
// provider (or a provider that stopped answering) cannot recover inside the
// current preparation, so it is Unavailable; every other latched cause leaves
// the prepared CPU fallback carrying the stream and is Degraded.
inline GpuAudioProviderState provider_state_for_health(const RealtimeGpuHealth& health) noexcept {
    if (health.device_lost || provider_loss_reason(health.recovery_reason))
        return GpuAudioProviderState::Unavailable;
    // A re-acquire is informational: the new provider's own state applies.
    if (health.fenced || (health.recovery_reason != GpuAudioRecoveryReason::None &&
                          health.recovery_reason != GpuAudioRecoveryReason::ProviderReacquired))
        return GpuAudioProviderState::Degraded;
    return GpuAudioProviderState::Ready;
}

// The latched cause as the status reports it. The bridge keeps the first
// cause, but a status may not be Unavailable for sustained misses, and a
// failure or stall the lost device caused should not hide the loss.
inline GpuAudioRecoveryReason reported_recovery_reason(const RealtimeGpuHealth& health) noexcept {
    // A lost device is the root cause of the failures and stalls it induces,
    // so loss observed after any of them is reported as the loss; the latched
    // sequence still marks where delivery first stopped. Independent causes
    // (sequence gaps, saturation, invalid callbacks, offline fences) stay first.
    if (health.device_lost &&
        (health.recovery_reason == GpuAudioRecoveryReason::ProviderFailure ||
         health.recovery_reason == GpuAudioRecoveryReason::ProviderUnresponsive ||
         health.recovery_reason == GpuAudioRecoveryReason::SustainedDeadlineMisses))
        return GpuAudioRecoveryReason::ProviderLost;
    return health.recovery_reason;
}

// The engine actually producing audio. A provider that is not Ready does not
// deliver GPU blocks: the callback runs the prepared CPU fallback, or emits
// silence or dry input, which no engine produces.
inline GpuAudioEngine delivering_engine(GpuAudioProviderState state, GpuAudioEngine selected,
                                        MissPolicy fallback_policy,
                                        bool fallback_available = true) noexcept {
    if (state == GpuAudioProviderState::Ready || selected != GpuAudioEngine::Gpu)
        return selected;
    return fallback_policy == MissPolicy::CpuFallback && fallback_available
               ? GpuAudioEngine::Cpu
               : GpuAudioEngine::Unknown;
}

// The one private-to-public recovery-reason mapping, shared by every node
// that reads a stamped bridge.
inline GpuAudioRecoveryReason public_recovery_reason(SharedIoRecoveryReason reason) noexcept {
    switch (reason) {
    case SharedIoRecoveryReason::None:
        return GpuAudioRecoveryReason::None;
    case SharedIoRecoveryReason::SequenceGap:
        return GpuAudioRecoveryReason::SequenceGap;
    case SharedIoRecoveryReason::InputSaturated:
        return GpuAudioRecoveryReason::InputSaturated;
    case SharedIoRecoveryReason::ProviderFailure:
        return GpuAudioRecoveryReason::ProviderFailure;
    case SharedIoRecoveryReason::ProviderLost:
        return GpuAudioRecoveryReason::ProviderLost;
    case SharedIoRecoveryReason::OfflineFence:
        return GpuAudioRecoveryReason::OfflineFence;
    case SharedIoRecoveryReason::InvalidCallback:
        return GpuAudioRecoveryReason::InvalidCallback;
    case SharedIoRecoveryReason::ProviderUnresponsive:
        return GpuAudioRecoveryReason::ProviderUnresponsive;
    }
    return GpuAudioRecoveryReason::None;
}

// Which recent callback blocks the GPU delivered, so a status does not read
// Ready on the GPU while the miss substitute carries the stream (a provider
// that keeps completing, but too late for every consumer, latches no cause).
// One realtime writer records each block; any thread reads. The word holds the
// last 32 counted blocks: the low half marks GPU deliveries, the high half
// marks counted blocks. Priming and rejected blocks are not counted.
class RecentGpuDelivery {
  public:
    static constexpr int kWindowBlocks = 32;
    // Fewer counted blocks than this are not evidence either way.
    static constexpr int kMinBlocks = 8;

    // Realtime, single writer: two relaxed word accesses, no read-modify-write.
    void record(SharedIoDeliveryDisposition disposition) noexcept {
        std::uint64_t gpu = 0;
        switch (disposition) {
        case SharedIoDeliveryDisposition::GpuDelivered:
            gpu = 1;
            break;
        case SharedIoDeliveryDisposition::CpuFallbackDelivered:
        case SharedIoDeliveryDisposition::SilenceDelivered:
        case SharedIoDeliveryDisposition::PassthroughDelivered:
            break;
        default:
            return;
        }
        constexpr std::uint64_t kHalf = 0xffff'ffffu;
        const auto bits = bits_.load(std::memory_order_relaxed);
        const auto delivered = (((bits & kHalf) << 1) | gpu) & kHalf;
        const auto counted = (((bits >> 32) << 1) | 1u) & kHalf;
        bits_.store((counted << 32) | delivered, std::memory_order_relaxed);
    }
    // Any thread. Against a concurrent record() one of the two may be lost,
    // which only delays the window by a block; the word is never torn.
    void reset() noexcept {
        bits_.store(0, std::memory_order_relaxed);
    }
    // The GPU delivered fewer than half of enough recent blocks.
    bool lagging() const noexcept {
        const auto bits = bits_.load(std::memory_order_relaxed);
        const int counted = std::popcount(static_cast<std::uint32_t>(bits >> 32));
        const int delivered = std::popcount(static_cast<std::uint32_t>(bits));
        return counted >= kMinBlocks && delivered * 2 < counted;
    }

  private:
    std::atomic<std::uint64_t> bits_{0};
};

// A provider that is otherwise Ready but whose GPU is not carrying the stream
// reads Degraded, so its engine is the miss substitute that delivers.
inline GpuAudioProviderState lower_for_lagging(GpuAudioProviderState state,
                                               bool gpu_lagging) noexcept {
    return gpu_lagging && state == GpuAudioProviderState::Ready ? GpuAudioProviderState::Degraded
                                                                : state;
}

// A node's provider state and delivering engine from its own facts, the same
// projection the transport applies to a health sample: Unavailable when the
// provider is lost or stalled, Ready only for an authenticated, unfenced
// provider whose GPU delivers most recent blocks, Degraded otherwise.
struct NodeStateProjection {
    GpuAudioProviderState provider_state = GpuAudioProviderState::Degraded;
    GpuAudioEngine selected_engine = GpuAudioEngine::Unknown;
};
inline NodeStateProjection project_node_state(bool device_lost, GpuAudioRecoveryReason reason,
                                              bool authenticated_unfenced,
                                              MissPolicy fallback_policy, bool fallback_available,
                                              bool gpu_lagging = false) noexcept {
    RealtimeGpuHealth health;
    health.device_lost = device_lost;
    health.recovery_reason = reason;
    health.fenced = !authenticated_unfenced;
    NodeStateProjection projection;
    projection.provider_state = lower_for_lagging(provider_state_for_health(health), gpu_lagging);
    projection.selected_engine = delivering_engine(projection.provider_state, GpuAudioEngine::Gpu,
                                                   fallback_policy, fallback_available);
    return projection;
}

// Health carried by a node's own status projection. Loss is the latched
// reason, or an Unavailable state reported after an earlier recoverable cause
// (the first cause stays latched while the provider is lost).
inline RealtimeGpuHealth health_from_status(const GpuAudioStatus& status) noexcept {
    RealtimeGpuHealth health;
    health.recovery_reason = status.recovery_reason;
    health.recovery_sequence = status.recovery_sequence;
    health.epoch = status.epoch;
    health.delivery_epoch_active = status.delivery_epoch_active;
    health.device_lost = provider_loss_reason(status.recovery_reason) ||
                         status.provider_state == GpuAudioProviderState::Unavailable;
    health.fenced = health.device_lost ||
                    (status.recovery_reason != GpuAudioRecoveryReason::None &&
                     status.recovery_reason != GpuAudioRecoveryReason::ProviderReacquired);
    for (std::size_t i = 0; i < GpuAudioStatus::kTerminalDispositionCount; ++i)
        health.terminal_counts[i] = status.terminal_counts[i];
    health.shadow_mode = status.shadow_mode;
    health.shadow_inline_cost_pct = status.shadow_inline_cost_pct;
    health.shadow_delivered_blocks = status.shadow_delivered_blocks;
    health.shadow_late_blocks = status.shadow_late_blocks;
    health.shadow_double_miss_blocks = status.shadow_double_miss_blocks;
    health.shadow_model_differs = status.shadow_model_differs;
    health.gpu_kernel_available = status.gpu_kernel_available;
    health.completion_mode = status.completion_mode;
    health.timestamps_available = status.timestamps_available;
    health.last_delivery_ns = status.last_delivery_ns;
    return health;
}

// An unprepared lane is worse than any prepared one, because the route cannot
// deliver it at all.
inline std::uint8_t lane_health_rank(GpuAudioProviderState state) noexcept {
    switch (state) {
    case GpuAudioProviderState::Ready:
        return 0;
    case GpuAudioProviderState::Degraded:
        return 1;
    case GpuAudioProviderState::Unavailable:
        return 2;
    case GpuAudioProviderState::Uninitialized:
        return 3;
    }
    return 3;
}

// The least healthy of several lane statuses (the first on a tie). Within one
// provider state, a lane with a latched cause explains more than one without.
inline const GpuAudioStatus* least_healthy_status(const GpuAudioStatus* const* lanes,
                                                  std::size_t count) noexcept {
    const GpuAudioStatus* worst = nullptr;
    for (std::size_t i = 0; i < count; ++i) {
        const auto* lane = lanes[i];
        if (lane == nullptr)
            continue;
        if (worst == nullptr) {
            worst = lane;
            continue;
        }
        const auto rank = lane_health_rank(lane->provider_state);
        const auto worst_rank = lane_health_rank(worst->provider_state);
        if (rank > worst_rank ||
            (rank == worst_rank && worst->recovery_reason == GpuAudioRecoveryReason::None &&
             lane->recovery_reason != GpuAudioRecoveryReason::None))
            worst = lane;
    }
    return worst;
}

} // namespace pulp::gpu_audio::detail
