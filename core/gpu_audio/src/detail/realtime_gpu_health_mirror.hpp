#pragma once

#include "realtime_gpu_audio_path.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace pulp::gpu_audio::detail {

// Coherent, allocation-free copy of one node's RealtimeGpuHealth. The node's
// health hook is a serialized non-RT read (transport worker or a quiescent
// host call); this mirror republishes it so host and UI threads can read every
// field from the same instant without touching worker-owned state.
//
// The guard is the sequence scheme used for SharedIoTelemetry's completion
// timing: an odd sequence marks an update in progress, both sides give up
// after a bounded number of attempts, and every field is an atomic so a torn
// attempt is a retry rather than a data race.
class RealtimeGpuHealthMirror {
  public:
    // Returns false when the sample was dropped because another writer held
    // the guard; that writer's update is the latest one instead.
    bool publish(const RealtimeGpuHealth& health) noexcept {
        if (!try_begin_update())
            return false;
        published_.store(true, std::memory_order_relaxed);
        fenced_.store(health.fenced, std::memory_order_relaxed);
        recovery_reason_.store(health.recovery_reason, std::memory_order_relaxed);
        recovery_sequence_.store(health.recovery_sequence, std::memory_order_relaxed);
        epoch_.store(health.epoch, std::memory_order_relaxed);
        delivery_epoch_active_.store(health.delivery_epoch_active, std::memory_order_relaxed);
        device_lost_.store(health.device_lost, std::memory_order_relaxed);
        for (std::size_t i = 0; i < terminal_counts_.size(); ++i)
            terminal_counts_[i].store(health.terminal_counts[i], std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
        return true;
    }

    // Host/quiescent lifecycle operation: it may wait for an in-progress
    // update so that the reset is never dropped.
    void reset() noexcept {
        while (!try_begin_update()) {
        }
        published_.store(false, std::memory_order_relaxed);
        fenced_.store(false, std::memory_order_relaxed);
        recovery_reason_.store(GpuAudioRecoveryReason::None, std::memory_order_relaxed);
        recovery_sequence_.store(std::numeric_limits<std::uint64_t>::max(),
                                 std::memory_order_relaxed);
        epoch_.store(0, std::memory_order_relaxed);
        delivery_epoch_active_.store(false, std::memory_order_relaxed);
        device_lost_.store(false, std::memory_order_relaxed);
        for (auto& count : terminal_counts_)
            count.store(0, std::memory_order_relaxed);
        sequence_.fetch_add(1, std::memory_order_release);
    }

    // False when nothing has been published since reset(), or when no stable
    // copy could be taken because a writer held the update open.
    bool read(RealtimeGpuHealth& out) const noexcept {
        for (int attempt = 0; attempt < kAttempts; ++attempt) {
            const auto sequence = sequence_.load(std::memory_order_acquire);
            if ((sequence & 1U) != 0U)
                continue;
            RealtimeGpuHealth copy;
            const bool published = published_.load(std::memory_order_relaxed);
            copy.fenced = fenced_.load(std::memory_order_relaxed);
            copy.recovery_reason = recovery_reason_.load(std::memory_order_relaxed);
            copy.recovery_sequence = recovery_sequence_.load(std::memory_order_relaxed);
            copy.epoch = epoch_.load(std::memory_order_relaxed);
            copy.delivery_epoch_active = delivery_epoch_active_.load(std::memory_order_relaxed);
            copy.device_lost = device_lost_.load(std::memory_order_relaxed);
            for (std::size_t i = 0; i < terminal_counts_.size(); ++i)
                copy.terminal_counts[i] = terminal_counts_[i].load(std::memory_order_relaxed);
            // Orders the relaxed data loads before the re-check; an acquire
            // load alone would let them be satisfied after it.
            std::atomic_thread_fence(std::memory_order_acquire);
            if (sequence_.load(std::memory_order_relaxed) == sequence) {
                if (!published)
                    return false;
                out = copy;
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
    std::atomic<bool> fenced_{false};
    std::atomic<GpuAudioRecoveryReason> recovery_reason_{GpuAudioRecoveryReason::None};
    std::atomic<std::uint64_t> recovery_sequence_{std::numeric_limits<std::uint64_t>::max()};
    std::atomic<std::uint64_t> epoch_{0};
    std::atomic<bool> delivery_epoch_active_{false};
    std::atomic<bool> device_lost_{false};
    std::array<std::atomic<std::uint64_t>, GpuAudioStatus::kTerminalDispositionCount>
        terminal_counts_{};
};

// Shared provider-state projection for every node health source. Loss of the
// provider (or a provider that stopped answering) cannot recover inside the
// current preparation, so it is Unavailable; every other latched cause leaves
// the prepared CPU fallback carrying the stream and is Degraded.
inline GpuAudioProviderState provider_state_for_health(const RealtimeGpuHealth& health) noexcept {
    if (health.device_lost || health.recovery_reason == GpuAudioRecoveryReason::ProviderLost ||
        health.recovery_reason == GpuAudioRecoveryReason::ProviderUnresponsive)
        return GpuAudioProviderState::Unavailable;
    if (health.fenced || health.recovery_reason != GpuAudioRecoveryReason::None)
        return GpuAudioProviderState::Degraded;
    return GpuAudioProviderState::Ready;
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
        const auto rank = static_cast<std::uint8_t>(lane->provider_state);
        const auto worst_rank = static_cast<std::uint8_t>(worst->provider_state);
        if (rank > worst_rank ||
            (rank == worst_rank && worst->recovery_reason == GpuAudioRecoveryReason::None &&
             lane->recovery_reason != GpuAudioRecoveryReason::None))
            worst = lane;
    }
    return worst;
}

} // namespace pulp::gpu_audio::detail
