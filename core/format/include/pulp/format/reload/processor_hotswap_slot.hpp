#pragma once

/// @file processor_hotswap_slot.hpp
/// RT-safe hot-swap slot for a `Processor` (v2 plan §4.4; adversarial P0-A).
///
/// The lifetime invariant: a `Processor` instance is destroyed only on the
/// control thread, and only after that thread has *proven* no audio callback is
/// inside the old instance's `process()` body. A `try_lock` around *entry* is
/// not enough — a reader already inside `process()` is invisible to the swapper.
///
/// Mechanism (the simplest correct one; RCU/epoch is a later optimization):
///   - Audio thread takes a NON-BLOCKING shared lock for the WHOLE `process()`
///     body. On contention (a swap is installing) it emits one passthrough block
///     and returns — it never blocks, never allocates, never destroys.
///   - Control thread takes the unique (writer) lock to install the new
///     instance. Acquiring it *proves* no shared reader is inside `process()`.
///     The displaced old instance is returned to the caller and destroyed on the
///     control thread after the lock is released — safe, because no audio reader
///     can still hold it.
///
/// A naive `atomic<shared_ptr<Processor>>` is intentionally NOT used: the last
/// reference can drop on the audio thread, running `~Processor()` in the
/// callback (an RT violation).

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>

namespace pulp::format::reload {

class ProcessorHotSwapSlot {
public:
    ProcessorHotSwapSlot() = default;
    explicit ProcessorHotSwapSlot(std::unique_ptr<Processor> initial)
        : active_(std::move(initial)) {}

    /// Audio-thread entry. Forwards to the active processor under a non-blocking
    /// shared lock; on swap contention (or no active processor) it passes the
    /// input through to the output for this block and counts it. Never blocks,
    /// allocates, or destroys.
    void process(audio::BufferView<float>& out,
                 const audio::BufferView<const float>& in,
                 midi::MidiBuffer& midi_in,
                 midi::MidiBuffer& midi_out,
                 const ProcessContext& ctx) {
        std::shared_lock<std::shared_mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || !active_) {
            passthrough(out, in);
            contention_blocks_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        active_->process(out, in, midi_in, midi_out, ctx);
    }

    /// Control-thread swap. Installs @p next and returns the displaced instance
    /// for the caller to destroy (on the control thread, after this returns) —
    /// acquiring the writer lock proves no audio reader is inside the old
    /// instance, so its later destruction can't race a callback.
    [[nodiscard]] std::unique_ptr<Processor> swap(std::unique_ptr<Processor> next) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        auto old = std::move(active_);
        active_ = std::move(next);
        return old;
    }

    /// Control-thread re-prepare of the live processor (e.g. a host sample-rate
    /// change). Takes the writer lock — the same one swap() uses — so it cannot
    /// race a reader inside process(). No-op when no processor is installed. Call
    /// only while audio is stopped, like prepare(): the writer lock guarantees
    /// mutual exclusion with process(), not that prepare() (which may allocate)
    /// is realtime.
    void reprepare_active(const PrepareContext& ctx) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (active_) active_->prepare(ctx);
    }

    /// True when a processor is installed. Control-thread/diagnostic use.
    bool has_active() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return active_ != nullptr;
    }

    /// Number of blocks that passed through due to swap contention / no active
    /// processor (dev metric).
    std::uint64_t contention_blocks() const {
        return contention_blocks_.load(std::memory_order_relaxed);
    }

private:
    static void passthrough(audio::BufferView<float>& out,
                            const audio::BufferView<const float>& in) {
        const std::size_t channels = std::min(out.num_channels(), in.num_channels());
        const std::size_t frames = out.num_samples();
        for (std::size_t ch = 0; ch < channels; ++ch) {
            auto o = out.channel(ch);
            auto i = in.channel(ch);
            for (std::size_t n = 0; n < frames; ++n) o[n] = i[n];
        }
        // Zero any output channels with no matching input.
        for (std::size_t ch = channels; ch < out.num_channels(); ++ch) {
            auto o = out.channel(ch);
            for (std::size_t n = 0; n < frames; ++n) o[n] = 0.0f;
        }
    }

    mutable std::shared_mutex mutex_;
    std::unique_ptr<Processor> active_;
    std::atomic<std::uint64_t> contention_blocks_{0};
};

} // namespace pulp::format::reload
