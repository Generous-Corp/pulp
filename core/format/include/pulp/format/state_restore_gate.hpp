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
/// Mechanism, the same one `reload::ProcessorHotSwapSlot` uses for its own
/// "quiescence before publish" requirement:
///
///   - The audio thread takes a NON-BLOCKING shared lock around its call into
///     `Processor::process()`. On contention (a restore is installing) it
///     renders nothing and the adapter emits silence for that block. It never
///     blocks, never allocates.
///   - The restoring thread takes the unique lock. Acquiring it *proves* no
///     audio reader is inside `process()`, so the restore is safe.
///
/// The gate is deliberately separate from `ProcessorHotSwapSlot`: that type
/// owns and swaps the `Processor`, whereas a format adapter only borrows one it
/// neither owns nor replaces.

#include <pulp/audio/buffer.hpp>

#include <algorithm>
#include <cstdint>
#include <atomic>
#include <mutex>
#include <shared_mutex>

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
        explicit RenderLock(std::shared_mutex& m) noexcept
            : lock_(m, std::try_to_lock) {}

        explicit operator bool() const noexcept { return lock_.owns_lock(); }

    private:
        std::shared_lock<std::shared_mutex> lock_;
    };

    /// Audio-thread entry. Non-blocking; hold for the whole `process()` body,
    /// because a reader already inside `process()` is invisible to a gate taken
    /// only around entry.
    RenderLock lock_for_render() noexcept {
        RenderLock lock(mutex_);
        if (!lock) contended_blocks_.fetch_add(1, std::memory_order_relaxed);
        return lock;
    }

    /// Control-thread entry. Blocks until every in-flight `process()` call has
    /// returned, then keeps the audio thread out until the returned lock is
    /// released. Never call this from `process()`.
    [[nodiscard]] std::unique_lock<std::shared_mutex> lock_for_restore() {
        return std::unique_lock<std::shared_mutex>(mutex_);
    }

    /// Blocks that rendered silence because a restore held the gate. A healthy
    /// session sees a small number of these around each preset load; a growing
    /// count means something is holding the gate far too long.
    std::uint64_t contended_blocks() const noexcept {
        return contended_blocks_.load(std::memory_order_relaxed);
    }

private:
    std::shared_mutex mutex_;
    std::atomic<std::uint64_t> contended_blocks_{0};
};

}  // namespace pulp::format
