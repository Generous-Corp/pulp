#pragma once

/// @file state_restore_gate.hpp
/// Proving the audio thread is outside `process()` while plugin state is
/// restored under it.
///
/// `Processor::deserialize_plugin_state()` is documented as being "called on a
/// host/main thread with the audio thread stopped". No format's host API
/// actually guarantees that: VST3 `setState`, AU `setFullState:`, AU v2
/// `kAudioUnitProperty_ClassInfo`, and CLAP `state.load` may all arrive while
/// the plug-in is active and rendering. Pulp's own host-quirk catalog records
/// row R6 for a DAW that calls the VST3 entry point at any time.
///
/// Without a gate the restoring thread mutates processor-owned state while the
/// audio thread reads it. `StateStore` parameters are atomic and survive that,
/// but a plug-in's own `deserialize_plugin_state()` payload — sample buffers,
/// filter coefficients, wavetables — has no such protection, and the contract
/// above tells plug-in authors they may assume there is none needed.
///
/// Mechanism:
///
///   - The audio thread registers itself as an in-flight render with one atomic
///     increment around its call into `Processor::process()`. If a restore has
///     already claimed the gate it backs out and the adapter emits silence (or
///     a passthrough) for that block. It never blocks, never takes a mutex,
///     never allocates.
///   - The restoring thread raises the "restoring" flag on the same atomic and
///     then waits for the in-flight render count to drain to zero. Both sides
///     read-modify-write one atomic word, so every render either registered
///     before the flag went up (and the restore waits for it) or sees the flag
///     (and refuses). Once the count is zero, no audio reader is inside
///     `process()` and the restore is safe.
///
/// The gate is deliberately separate from `ProcessorHotSwapSlot`: that type
/// owns and swaps the `Processor`, whereas a format adapter only borrows one it
/// neither owns nor replaces.

#include <pulp/audio/buffer.hpp>

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <thread>

namespace pulp::format {

/// Zero an output bus for a block the plug-in did not render.
///
/// The counterpart to `passthrough_block()` for sources with no input to pass
/// through — instruments, and any bus whose input side is absent.
template <typename Sample>
void silence_block(audio::BufferView<Sample>& out) {
    const std::size_t frames = out.num_samples();
    for (std::size_t ch = 0; ch < out.num_channels(); ++ch) {
        auto o = out.channel(ch);
        for (std::size_t n = 0; n < frames; ++n) o[n] = Sample{0};
    }
}

/// Copy input to output for a block the plug-in did not render.
///
/// Output channels with no matching input are zeroed, so a wider output bus
/// never carries whatever was left in the host's buffer. For an instrument —
/// no input — this is silence, which is the only correct answer there anyway.
///
/// Shared by every "the Processor is unavailable for this block" path so they
/// all degrade identically: the state-restore gate below and
/// `reload::ProcessorHotSwapSlot`'s swap contention.
template <typename Sample>
void passthrough_block(audio::BufferView<Sample>& out,
                       const audio::BufferView<const Sample>& in) {
    const std::size_t channels = std::min(out.num_channels(), in.num_channels());
    const std::size_t frames = out.num_samples();
    for (std::size_t ch = 0; ch < channels; ++ch) {
        auto o = out.channel(ch);
        auto i = in.channel(ch);
        for (std::size_t n = 0; n < frames; ++n) o[n] = i[n];
    }
    for (std::size_t ch = channels; ch < out.num_channels(); ++ch) {
        auto o = out.channel(ch);
        for (std::size_t n = 0; n < frames; ++n) o[n] = Sample{0};
    }
}

class StateRestoreGate {
public:
    /// Audio-thread pin. Contextually convertible to bool: false means a state
    /// restore holds the gate and this block must not enter `process()`.
    class [[nodiscard]] RenderLock {
    public:
        RenderLock(const RenderLock&) = delete;
        RenderLock& operator=(const RenderLock&) = delete;
        ~RenderLock() {
            if (gate_) gate_->state_.fetch_sub(1, std::memory_order_release);
        }

        explicit operator bool() const noexcept { return gate_ != nullptr; }

    private:
        friend class StateRestoreGate;
        explicit RenderLock(StateRestoreGate* gate) noexcept : gate_(gate) {}
        StateRestoreGate* gate_;
    };

    /// Control-thread hold. Keeps the audio thread out of `process()` until it
    /// is destroyed.
    class [[nodiscard]] RestoreLock {
    public:
        RestoreLock(const RestoreLock&) = delete;
        RestoreLock& operator=(const RestoreLock&) = delete;
        ~RestoreLock() {
            gate_.state_.fetch_and(~kRestoring, std::memory_order_release);
        }

    private:
        friend class StateRestoreGate;
        RestoreLock(StateRestoreGate& gate, std::unique_lock<std::mutex> writer) noexcept
            : gate_(gate), writer_(std::move(writer)) {}
        StateRestoreGate& gate_;
        // Released after the flag is cleared (members destroy after the body),
        // so a second restorer can never observe a half-released gate.
        std::unique_lock<std::mutex> writer_;
    };

    /// Audio-thread entry. Lock-free and wait-free; hold for the whole
    /// `process()` body, because a reader already inside `process()` is
    /// invisible to a gate taken only around entry.
    RenderLock lock_for_render() noexcept {
        const std::uint32_t prev = state_.fetch_add(1, std::memory_order_acquire);
        if ((prev & kRestoring) != 0) {
            state_.fetch_sub(1, std::memory_order_relaxed);
            contended_blocks_.fetch_add(1, std::memory_order_relaxed);
            return RenderLock(nullptr);
        }
        return RenderLock(this);
    }

    /// Control-thread entry. Blocks until every in-flight `process()` call has
    /// returned, then keeps the audio thread out until the returned lock is
    /// released. Restores are serialized against each other. Never call this
    /// from `process()`.
    RestoreLock lock_for_restore() {
        std::unique_lock<std::mutex> writer(writer_mutex_);
        state_.fetch_or(kRestoring, std::memory_order_acq_rel);
        while ((state_.load(std::memory_order_acquire) & kRenderMask) != 0)
            std::this_thread::yield();
        return RestoreLock(*this, std::move(writer));
    }

    /// Blocks that rendered silence because a restore held the gate. A healthy
    /// session sees a small number of these around each preset load; a growing
    /// count means something is holding the gate far too long.
    std::uint64_t contended_blocks() const noexcept {
        return contended_blocks_.load(std::memory_order_relaxed);
    }

private:
    static constexpr std::uint32_t kRestoring = 0x8000'0000u;
    static constexpr std::uint32_t kRenderMask = ~kRestoring;
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "the render side of the gate must be lock-free");

    // High bit: a restore holds (or is acquiring) the gate. Low bits: renders
    // currently inside process().
    std::atomic<std::uint32_t> state_{0};
    std::mutex writer_mutex_;
    std::atomic<std::uint64_t> contended_blocks_{0};
};

}  // namespace pulp::format
