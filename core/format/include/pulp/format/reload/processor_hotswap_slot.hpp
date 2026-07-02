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
///
/// ── Click-free crossfade (opt-in) ───────────────────────────────────────────
/// An instantaneous swap cuts from the old DSP's output to the new one's at a
/// block boundary — if they disagree there (different gain, phase, waveform) the
/// discontinuity clicks. With a crossfade configured (set_crossfade_samples() +
/// prepare_crossfade() to allocate the parallel-render scratch off the audio
/// thread), a swap instead keeps the OLD processor and runs it in parallel with
/// the new for a short window, mixing old→new along a smoothstep ramp (zero
/// slope at both ends, so neither the swap instant nor the fade end clicks). The
/// faded-out processor is then retired the RT-safe way: the audio thread only
/// marks the fade done; the control thread frees it in reclaim() (or the next
/// swap()/the destructor), never on the audio thread. With no crossfade
/// configured the swap is instantaneous and returns the displaced instance, as
/// before.

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace pulp::format::reload {

/// Amplitude law applied across a crossfade. Both are evaluated over the
/// smoothstepped ramp (zero slope at both ends), so both stay click-free at the
/// swap instant and the fade end; they differ only in the mid-fade sum:
///   - `Smoothstep` — equal-GAIN (old_gain + new_gain == 1). No power bump when
///     old and new are correlated (the usual hot-reload case: same input, a
///     small DSP tweak). Default.
///   - `EqualPower` — constant POWER (old_gain² + new_gain² == 1), cos/sin. Avoids
///     the mid-fade level dip when old and new are decorrelated (a big DSP
///     change). Mirrors `signal::DryWetMixer`'s EqualPower law.
enum class CrossfadeCurve { Smoothstep, EqualPower };

class ProcessorHotSwapSlot {
public:
    ProcessorHotSwapSlot() = default;
    explicit ProcessorHotSwapSlot(std::unique_ptr<Processor> initial)
        : active_(std::move(initial)) {}

    /// Audio-thread entry. Forwards to the active processor under a non-blocking
    /// shared lock; on swap contention (or no active processor) it passes the
    /// input through to the output for this block and counts it. When a previous
    /// processor is fading out (a crossfaded swap), it is rendered in parallel
    /// into the scratch buffer and mixed under a smoothstep ramp. Never blocks,
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
        mix_fade_out(out, in, ctx);
    }

    /// Control-thread swap. Without a crossfade configured this installs @p next
    /// and RETURNS the displaced instance for the caller to destroy (on the
    /// control thread) — acquiring the writer lock proves no audio reader is
    /// inside the old instance. With a crossfade configured (and scratch
    /// prepared) it instead RETAINS the displaced instance as the fading-out
    /// processor, returns nullptr, and the slot reclaims it later (reclaim() /
    /// the next swap() / the destructor). The very first install (no prior
    /// active processor) never fades — there is nothing to fade from.
    [[nodiscard]] std::unique_ptr<Processor> swap(std::unique_ptr<Processor> next) {
        std::unique_ptr<Processor> displaced;   // returned to caller (instant path)
        std::unique_ptr<Processor> superseded;  // a collapsed prior fade-out
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            // DSP-state carry (item 1.6): holding the writer lock proves no audio
            // reader is inside the old processor, so it is safe to serialize its
            // DSP state (delay tails / filter history / phase) and restore it into
            // the incoming one — off the audio thread. Opt-in + cold-start-safe:
            // an empty blob or a false restore leaves `next` freshly prepared and
            // never fails the swap. Skipped on the first install (no old to carry
            // from). The blob is copied while the lock is held, so a plugin that
            // opts in should keep it bounded (a longer copy → a few more audio
            // passthrough blocks during the swap, which the crossfade masks).
            if (active_ && next) {
                // Best-effort: a THROW (not just a false return) must also degrade
                // to cold-start, per the contract — otherwise a serialize/restore
                // bug would reject the whole reload (and a throwing serialize on
                // the already-live OLD processor would block every future reload).
                try {
                    const std::vector<std::byte> blob = active_->serialize_dsp_state();
                    if (!blob.empty()) next->restore_dsp_state(blob);
                } catch (...) {
                    // Swallow: state-carry failed → `next` stays freshly prepared.
                }
            }
            if (!crossfade_ready()) {
                displaced = std::move(active_);
                active_ = std::move(next);
            } else {
                // Collapse any still-pending fade-out: it is fully superseded.
                // We hold the writer lock so no audio reader is inside it; hand
                // it out to be freed AFTER unlock (not under the lock — a slow
                // ~Processor() would lengthen the audio thread's passthrough
                // contention window, as the instant path already avoids).
                // NOTE: collapsing an in-progress fade restarts at g=0, so a
                // re-swap WITHIN the ~12ms fade window can step the output (not
                // click-free). Hot-reload swaps are human-file-save paced (far
                // wider than the window), so a single fade slot is sufficient.
                superseded = std::move(fading_out_);
                fading_out_ = std::move(active_);   // keep the just-displaced DSP for the fade
                active_ = std::move(next);
                fade_total_ = fade_samples_;
                fade_pos_ = 0;
                // Nothing to fade from on the first install → mark done immediately.
                fade_done_.store(fading_out_ == nullptr, std::memory_order_release);
            }
        }
        return displaced;  // nullptr on the crossfade path (slot retains the fade-out)
    }

    /// Control-thread reclaim of a completed fade-out. Frees the retired
    /// processor on THIS (control) thread under the writer lock — which proves
    /// no audio reader is inside it. Call periodically (e.g. from the shell's
    /// watcher tick). No-op when nothing is pending or a fade is still running.
    void reclaim() {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (fading_out_ && fade_done_.load(std::memory_order_acquire)) {
            auto old = std::move(fading_out_);
            lock.unlock();
            old.reset();                      // ~Processor() here — control thread
        }
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

    /// Set the crossfade length in samples (0 = instantaneous swaps; the
    /// default). Control thread, before swaps. Crossfading also requires
    /// prepare_crossfade() to have allocated the parallel-render scratch.
    void set_crossfade_samples(std::size_t samples) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        fade_samples_ = samples;
    }

    /// Select the crossfade amplitude law (default `Smoothstep` = equal-gain).
    /// Control thread, before swaps. Takes effect on the next swap; changing it
    /// mid-fade is allowed (the running fade keeps its ramp position, only the
    /// gain mapping changes) but not typical.
    void set_crossfade_curve(CrossfadeCurve curve) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        fade_curve_ = curve;
    }

    /// Allocate the scratch the fade-out processor renders into, sized for the
    /// worst-case block. Control thread, off the audio path (called from the
    /// host's prepare()). Re-callable (audio stopped); sizes are set exactly to
    /// the arguments, not grown — call with the worst-case block.
    void prepare_crossfade(std::size_t max_frames, std::size_t max_channels) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        scratch_frames_ = max_frames;
        scratch_channels_ = max_channels;
        scratch_storage_.assign(max_frames * max_channels, 0.0f);
        scratch_ptrs_.resize(max_channels);
        for (std::size_t c = 0; c < max_channels; ++c)
            scratch_ptrs_[c] = scratch_storage_.data() + c * max_frames;
        // Bound the fade-out's MIDI buffers to their reserved capacity so a
        // MIDI-emitting DSP (arp/MIDI-fx) rendered during the fade drops-and-
        // counts on overflow instead of ALLOCATING on the audio thread. reserve()
        // alone does not bound growth; the realtime-capacity-limit flag does.
        fade_midi_in_.reserve(64, 0);
        fade_midi_out_.reserve(64, 0);
        fade_midi_in_.set_realtime_capacity_limit(true);
        fade_midi_out_.set_realtime_capacity_limit(true);
    }

    /// True when a processor is installed. Control-thread/diagnostic use.
    bool has_active() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return active_ != nullptr;
    }

    /// Invoke @p fn with the ACTIVE processor under the shared lock, returning
    /// its result (or a default-constructed result when no processor is
    /// installed). Control / UI thread only — fn may allocate (e.g. build a
    /// view); it runs concurrently with the audio thread's process() (also a
    /// shared reader) and only blocks a swap, which is rare. Kept as a template
    /// so the slot needs no dependency on whatever fn touches (e.g. pulp::view);
    /// the caller instantiates it where those types are complete.
    ///
    /// Lifetime note for view-building callers: the processor passed to fn is the
    /// one live RIGHT NOW. If it is later hot-swapped out and destroyed, a view
    /// that captured it would dangle — so a caller that keeps the editor across
    /// reloads must REBUILD on each swap (see ReloadableShell::set_on_reloaded),
    /// or have the logic return a self-contained view.
    ///
    /// Concurrency contract: fn runs concurrently with the audio thread's
    /// process() on the SAME processor (both shared readers). fn must therefore
    /// only READ — a create_view() that races a member process() mutates would
    /// be a data race. (Building a self-contained view that touches no mutable
    /// DSP state is fine.) Note too that while fn holds the shared lock, a swap
    /// waiting on the writer lock makes the audio thread's try-lock fail → one or
    /// more passthrough blocks for fn's duration; keep fn off the hot path's
    /// critical timing (editor open / post-swap rebuild, not per-block).
    template <class Fn>
    auto with_active(Fn&& fn) const -> decltype(fn(std::declval<Processor&>())) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (active_) return fn(*active_);
        return decltype(fn(std::declval<Processor&>())){};
    }

    /// True while a crossfade is in progress (a fade-out is rendering).
    bool crossfade_active() const {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        return fading_out_ != nullptr && !fade_done_.load(std::memory_order_acquire);
    }

    /// Number of blocks that passed through due to swap contention / no active
    /// processor (dev metric).
    std::uint64_t contention_blocks() const {
        return contention_blocks_.load(std::memory_order_relaxed);
    }

private:
    bool crossfade_ready() const {  // caller holds the lock
        return fade_samples_ > 0 && scratch_frames_ > 0 && scratch_channels_ > 0;
    }

    // Render the fading-out processor into scratch and mix it under a smoothstep
    // ramp (old→new). Audio thread, under the shared lock. RT-safe: no alloc
    // (scratch + midi reserved at prepare), no destroy (control thread reclaims).
    void mix_fade_out(audio::BufferView<float>& out,
                      const audio::BufferView<const float>& in,
                      const ProcessContext& ctx) {
        if (!fading_out_ || fade_done_.load(std::memory_order_relaxed)) return;
        const std::size_t frames = out.num_samples();
        if (fade_total_ == 0 || frames > scratch_frames_) {
            // Safety floor: a block larger than the scratch (the host exceeded
            // its declared max_buffer_size) can't be faded — finish now. This
            // snaps to full-new mid-ramp (a click), but it only triggers on a
            // misdeclared block size; prepare_crossfade() is sized to the host's
            // worst case so the normal path never hits this.
            fade_done_.store(true, std::memory_order_release);
            return;
        }
        const std::size_t ch =
            std::min({out.num_channels(), in.num_channels(), scratch_channels_});
        audio::BufferView<float> sv(scratch_ptrs_.data(), ch, frames);
        fade_midi_in_.clear();
        fade_midi_out_.clear();
        fading_out_->process(sv, in, fade_midi_in_, fade_midi_out_, ctx);

        const float inv_total = 1.0f / static_cast<float>(fade_total_);
        const bool equal_power = fade_curve_ == CrossfadeCurve::EqualPower;
        constexpr float kHalfPi = 1.57079632679489661923f;
        for (std::size_t c = 0; c < ch; ++c) {
            auto o = out.channel(c);
            auto old_ch = sv.channel(c);                      // fading-out (old) DSP output
            for (std::size_t n = 0; n < frames; ++n) {
                float t = static_cast<float>(fade_pos_ + n) * inv_total;
                if (t > 1.0f) t = 1.0f;
                const float ramp = t * t * (3.0f - 2.0f * t); // smoothstep: 0 slope at 0 and 1
                float old_gain, new_gain;
                if (equal_power) {                            // cos/sin over the ramp:
                    const float theta = ramp * kHalfPi;       // click-free (ramp ends flat)
                    old_gain = std::cos(theta);               // AND old²+new² == 1
                    new_gain = std::sin(theta);
                } else {                                      // equal-gain: old+new == 1
                    old_gain = 1.0f - ramp;
                    new_gain = ramp;
                }
                o[n] = old_ch[n] * old_gain + o[n] * new_gain;  // old→new, click-free
            }
        }
        fade_pos_ += frames;
        if (fade_pos_ >= fade_total_)
            fade_done_.store(true, std::memory_order_release);
    }

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

    // ── Crossfade state ──────────────────────────────────────────────────────
    std::unique_ptr<Processor> fading_out_;       // retained old DSP during a fade
    std::size_t fade_samples_ = 0;                // configured fade length (0 = instant)
    CrossfadeCurve fade_curve_ = CrossfadeCurve::Smoothstep;  // amplitude law
    std::size_t fade_total_ = 0;                  // active fade length
    std::size_t fade_pos_ = 0;                    // samples elapsed in the active fade
    std::atomic<bool> fade_done_{true};           // audio thread sets; reclaim() reads
    std::vector<float> scratch_storage_;          // [channels * frames], allocated off-RT
    std::vector<float*> scratch_ptrs_;
    std::size_t scratch_frames_ = 0;
    std::size_t scratch_channels_ = 0;
    midi::MidiBuffer fade_midi_in_;               // empty MIDI for the fading-out tail
    midi::MidiBuffer fade_midi_out_;              // discarded
};

} // namespace pulp::format::reload
