#pragma once

#include <pulp/signal/convolver_messages.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/transition_mixer.hpp>

#include <algorithm>
#include <cassert>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace pulp::signal {

/// Uniform partitioned convolution engine.
///
/// Processes audio through an impulse response using overlap-save with
/// uniform block partitioning. Zero latency (current block output is
/// immediate).
///
/// Two ways to load an IR:
///   1. `load_ir()` — synchronous; allocates + FFTs inline. Use at
///      `prepare()` time, never on the audio thread.
///   2. `try_swap_ir(ConvolverIrSwapper&)` — lock-free, allocation-free;
///      picks up a pre-built IR posted from a worker thread, swaps it
///      in at the next block boundary, and parks the displaced IR for
///      the worker thread to free. Designed for safe live IR swaps
///      during `process()`.
///
/// Usage (synchronous):
///   PartitionedConvolver conv;
///   conv.load_ir(ir_data, ir_length, block_size);
///   conv.process(input, output, num_samples);
///
/// Usage (background swap):
///   ConvolverIrSwapper swapper;
///   // background thread:
///   swapper.stage_ir(new_ir, new_len, block_size);
///   // audio thread, before/between process() calls:
///   conv.try_swap_ir(swapper);
///   // background thread, periodically:
///   swapper.drain_old();
template <typename SampleType = float>
class PartitionedConvolverT {
public:
    PartitionedConvolverT() = default;

    /// Load an impulse response. block_size should match your audio
    /// callback size. block_size must be a power of two (for the
    /// radix-2 FFT); if not, it is rounded up to the next one.
    ///
    /// Allocates and FFTs inline — call off the audio thread.
    void load_ir(const SampleType* ir, std::size_t ir_length, std::size_t block_size) {
        state_ = detail::build_convolver_ir_state(ir, ir_length, block_size);
        // Adopt the freshly-built state's spare as THE input history. Loading an
        // IR outright (as opposed to swapping one in) deliberately starts the
        // stream cold — this is a prepare()-time call, not a live change.
        history_ = state_ ? std::move(state_->history) : nullptr;
    }

    /// Try to consume the most recently staged IR from a swapper. If
    /// one is available, the current IR is parked on the swapper for
    /// the worker thread to free and the new IR is installed.
    ///
    /// Allocation-free, lock-free, RT-safe. Returns `true` if a swap
    /// happened on this call.
    ///
    /// Must be called at a block boundary (between `process()` calls)
    /// so the in-flight overlap buffers don't pick up midway through.
    bool try_swap_ir(ConvolverIrSwapperT<SampleType>& swapper) {
        // Gate the swap on retire-ring capacity FIRST so the audio
        // thread never has to free the displaced IR inline if the
        // ring is saturated. A single retired slot would let the audio
        // thread deallocate when 2+ swaps happened between drain_old()
        // calls. Refusing the swap when the ring is full is RT-safe:
        // pending stays in the swapper for the next try_swap_ir attempt;
        // in-flight state_ continues uninterrupted; worker thread catches
        // up on its next drain tick.
        if (state_ && !swapper.has_retire_capacity()) {
            return false;
        }

        // A crossfade still running: don't start another swap and
        // don't touch the in-flight fade-out — the pending IR waits for the next
        // call. Human-paced IR swaps rarely overlap the ~ms fade window.
        if (fading_ && !fade_mixer_.done())
            return false;
        // A completed fade-out can now be parked for the worker to free (never
        // on the audio thread). Refuse the swap if the ring is full so we never
        // free inline.
        if (fading_) {
            // Reserve TWO slots: one for this completed fade-out, one for the
            // just-displaced IR should the incoming swap fall back to an instant
            // retire below. Without this a near-full ring could accept the
            // fade-out then fail the second retire and strand (leak) the
            // displaced IR on the audio thread.
            if (!swapper.has_retire_capacity(2)) return false;
            (void)swapper.retire(fading_);
            (void)fading_.release();
        }

        auto next = swapper.try_consume();
        if (!next)
            return false;

        auto previous = std::move(state_);
        state_ = std::move(next);

        // The input delay line does NOT move with the swap. It belongs to the
        // input stream, whose identity is unchanged by an IR change, so the
        // convolver keeps the one it has and renders the incoming IR — and, under
        // a crossfade, the outgoing one too — against that same history. Only the
        // ring's SHAPE can force a change: a first load, a different block/FFT
        // size, or an IR with more partitions than the ring has slots.
        auto spare = std::move(state_->history);
        const bool ring_serves_new =
            history_ != nullptr &&
            history_->block_size == state_->block_size &&
            history_->fft_size == state_->fft_size &&
            history_->spectra.size() >= state_->num_partitions;
        if (!ring_serves_new) {
            // Adopt the incoming state's spare ring — allocated off the audio
            // thread precisely so this step never has to — carrying across
            // whatever the old ring held. If the incoming state has no spare (a
            // hand-built state that skipped it) the old ring is kept; process()
            // detects the too-short ring and fails safe rather than aliasing it.
            if (spare) {
                if (history_) detail::migrate_input_history(*history_, *spare);
                history_.swap(spare);
            }
        }

        if (previous) {
            // Crossfade path: keep the displaced IR rendering in parallel from the
            // shared history and blend old->new. Only when a fade is configured AND
            // the scratch fits both IRs' block size; otherwise fall back to the
            // instant retire below. Require MATCHING block sizes: the fade-out
            // render reads a full `previous->block_size` of `input`, but process()
            // only guarantees num_samples == state_->block_size — a larger old
            // block would read past the caller's input buffer. A mismatch falls
            // back to instant.
            const bool can_fade =
                fade_samples_ > 0 &&
                previous->block_size == state_->block_size &&
                fade_scratch_.size() >= static_cast<std::size_t>(state_->block_size);
            // Whichever ring lost the swap above is now rendered by nobody. Park
            // it on the displaced state so it is reclaimed off the audio thread
            // along with it — the audio thread must never free. The live state
            // never holds a spare (it is taken above on every path that installs
            // one), so this overwrites nothing and frees nothing.
            assert(!previous->history);
            previous->history = std::move(spare);
            if (can_fade) {
                fading_ = std::move(previous);
                fade_mixer_.configure(fade_samples_, fade_curve_);
            } else {
                // Pre-checked capacity above; the false branch is defense-in-
                // depth — leaks rather than freeing on RT.
                const bool ok = swapper.retire(previous);
                (void)ok;
                (void)previous.release();
            }
        } else {
            // `previous` is null only on a first load into an unloaded convolver,
            // where `history_` was null too — so the adopt branch above left
            // `spare` empty and nothing is freed on the audio thread here.
            assert(!spare);
        }
        return true;
    }

    /// Enable a click-free crossfade on IR swaps (opt-in — default 0 =
    /// instantaneous swap). When set, a swap keeps the OLD IR rendering in
    /// parallel with the new for `samples` and blends old→new via the shared
    /// TransitionMixer, so an IR change is audibly continuous rather than a hard
    /// cut. Call off the audio thread AFTER an IR is loaded (it sizes the
    /// parallel-render scratch from the block size).
    ///
    /// Both sides of the fade render from the convolver's single shared input
    /// history, so both are convolving against the same real past: the outgoing
    /// IR fades out from its tail and the incoming one fades in already warm. A
    /// fade is therefore continuous at any fade length — under the default
    /// Smoothstep curve, whose gain law is equal-gain, swapping in a copy of the
    /// IR already loaded is inaudible — and the choice of `samples` is purely a
    /// taste question about how fast the timbre should change. (`EqualPower`
    /// holds POWER rather than amplitude constant, so it bulges on a correlated
    /// pair by design; pick it when the two IRs are decorrelated.)
    ///
    /// A fade shorter than the IR is fine and costs nothing in continuity. Note
    /// only that a fade blends the outputs of two DIFFERENT impulse responses,
    /// which for a pair with opposing phase can dip in level mid-fade — that is
    /// inherent to blending, not a history artifact.
    void set_crossfade(std::size_t samples, TransitionCurve curve = TransitionCurve::Smoothstep) {
        fade_samples_ = samples;
        fade_curve_ = curve;
        if (state_) {
            fade_scratch_.assign(static_cast<std::size_t>(state_->block_size),
                                 SampleType{0.0f});
        }
    }

    /// Process a block of audio.
    ///
    /// **Precondition: `num_samples == block_size()` whenever an IR is loaded.**
    /// Overlap-save folds each block into a partition history sized to
    /// `block_size()`; a block of any other length cannot be folded into that
    /// history coherently.
    ///
    /// The three states are distinct, and only one of them passes audio through:
    ///
    /// - **No IR loaded (or an empty IR).** The convolver is a wire: input is
    ///   copied to output. This is the correct neutral behavior.
    /// - **IR loaded, `num_samples == block_size()`.** Normal convolution.
    /// - **IR loaded, wrong `num_samples`.** A precondition violation. It fails
    ///   *closed*: the block is emitted as silence, `block_size_violations()`
    ///   increments, and the history is marked torn so the next correctly-sized
    ///   block resets rather than continuing from a partially-advanced state.
    ///
    ///   It deliberately does NOT pass through. A pass-through here is audibly
    ///   indistinguishable from a working convolution with a near-unity IR, so
    ///   it turns a caller's block size bug into audio that merely sounds wrong
    ///   — the hardest kind of bug to find. Silence plus a counter is a bug you
    ///   can see, and `block_size_violations() == 0` is an invariant a test can
    ///   assert.
    ///
    /// RT-safe in every branch: the violation path bumps a counter and fills a
    /// buffer. It does not log, allocate, throw, or abort on the audio thread —
    /// a convolver must never take down its host over a caller bug.
    void process(const SampleType* input,
                 SampleType* output,
                 std::size_t num_samples) {
        // No IR to convolve with — or no input history long enough to serve it,
        // which only a hand-built state that skipped its `history` spare can
        // produce. Either way the convolver is a wire rather than something that
        // would read ring slots that do not exist.
        if (!state_ || state_->ir_spectra.empty() || !history_ ||
            history_->block_size != state_->block_size ||
            history_->fft_size != state_->fft_size ||
            history_->spectra.size() < state_->num_partitions) {
            std::copy_n(input, num_samples, output);
            return;
        }
        // Loaded, but the caller's block size does not match the IR partitioning.
        if (static_cast<int>(num_samples) != state_->block_size) {
            std::fill_n(output, num_samples, SampleType{0.0f});
            ++block_size_violations_;
            history_torn_ = true;
            return;
        }
        // Recover from a previous violation before folding a valid block in.
        if (history_torn_) reset();

        // Fold this block into the shared input history ONCE — one forward FFT
        // per block however many IRs render from it.
        capture_input(*history_, *state_->fft, input);

        render_ir(*state_, *history_, output);

        // Crossfade the retiring IR: render it in parallel from the SAME history
        // into scratch and blend old→new. RT-safe: scratch is pre-sized, no
        // alloc; the fade-out IR is retired on the ring after the fade (or
        // opportunistically once done), never freed on the audio thread.
        if (fading_ && !fade_mixer_.done()) {
            // Everything the parallel render needs. All three hold for any fade
            // try_swap_ir() actually starts — it only begins one when the scratch
            // fits and the block sizes match, and the shared ring is always at
            // least as long as the outgoing IR needs.
            const bool can_render =
                fade_scratch_.size() >= num_samples &&
                static_cast<std::size_t>(fading_->block_size) == num_samples &&
                history_->spectra.size() >= fading_->num_partitions;
            if (can_render) {
                render_ir(*fading_, *history_, fade_scratch_.data());
                const std::size_t base = fade_mixer_.position();
                for (std::size_t i = 0; i < num_samples; ++i) {
                    SampleType old_gain, new_gain;
                    fade_mixer_.gains_at(base + i, old_gain, new_gain);
                    output[i] = output[i] * new_gain + fade_scratch_[i] * old_gain;
                }
            }
            // Advance even when the render was skipped. A fade that cannot render
            // must still COMPLETE: `fading_` is only retired once the mixer is
            // done, and try_swap_ir() refuses every swap while a fade is running,
            // so a fade that never finishes would wedge the convolver on the IR
            // it happens to be holding.
            fade_mixer_.advance(num_samples);
        }
        // When the fade completes the fade-out IR is simply held (not rendered
        // anymore); it is parked on the swapper's retire ring by the next
        // try_swap_ir() — never freed on the audio thread here.

        // Step the shared ring once, after every IR has read this block.
        advance_input(*history_);
    }

    /// Drop the convolver's overlap-save input history so the next block starts
    /// from silence rather than continuing a previous stream. There is one
    /// history for every IR the convolver renders, so this clears a crossfade's
    /// fade-out side along with the live one. Also clears the torn-history flag
    /// and the block-size violation count.
    void reset() {
        clear_history(history_.get());
        history_torn_ = false;
        block_size_violations_ = 0;
    }

    /// Returns the algorithmic latency in samples.
    /// Overlap-save produces valid output for the current block
    /// immediately (partition 0 is applied in the same callback), so
    /// latency is 0.
    std::size_t latency() const { return 0; }

    /// The block size `process()` requires, in samples — the value handed to
    /// `load_ir()` rounded up to the next power of two (the radix-2 FFT needs
    /// it). Zero when no IR is loaded.
    ///
    /// Size the caller's blocks from THIS, not from the value passed to
    /// `load_ir()`: `load_ir(ir, len, 100)` yields a 128-sample convolver, and
    /// feeding it 100-sample blocks is a precondition violation.
    std::size_t block_size() const {
        return state_ ? static_cast<std::size_t>(state_->block_size) : 0;
    }

    /// How many times `process()` was called on a loaded convolver with
    /// `num_samples != block_size()`. Every one of those blocks was emitted as
    /// silence (see `process()`).
    ///
    /// This is a caller bug, not a runtime condition: a nonzero count means the
    /// audio host's block size and the size handed to `load_ir()` disagree.
    /// Assert it is zero in tests. Cleared by `reset()`.
    std::uint64_t block_size_violations() const { return block_size_violations_; }

    std::size_t num_partitions() const {
        return state_ ? state_->num_partitions : 0;
    }

    bool is_loaded() const {
        return state_ && !state_->ir_spectra.empty();
    }

private:
    // Fold one input block into the shared history: copy it into the overlap
    // buffer's upper half and take the forward FFT into the current ring slot.
    // Called ONCE per processed block, before any IR renders.
    static void capture_input(ConvolverInputHistoryT<SampleType>& h,
                              FftT<SampleType>& fft,
                              const SampleType* input) {
        for (int i = 0; i < h.block_size; ++i)
            h.overlap[h.block_size + i] = {input[i], SampleType{0.0f}};
        auto& current_spectrum = h.spectra[h.write_pos];
        std::copy(h.overlap.begin(), h.overlap.end(), current_spectrum.begin());
        // The transform is a property of the FFT size, which every IR rendering
        // against this history shares, so any loaded state's `fft` will do.
        fft.forward(current_spectrum.data());
    }

    // Slide the overlap buffer down and step the ring cursor. Called ONCE per
    // processed block, after every IR has read this block — a crossfade renders
    // twice from one history, so stepping per render would double-advance it.
    static void advance_input(ConvolverInputHistoryT<SampleType>& h) {
        std::copy_n(h.overlap.begin() + h.block_size, h.block_size,
                    h.overlap.begin());
        std::fill(h.overlap.begin() + h.block_size, h.overlap.end(),
                  std::complex<SampleType>{SampleType{0.0f}, SampleType{0.0f}});
        h.write_pos = (h.write_pos + 1) % h.spectra.size();
    }

    // Multiply-accumulate one IR's partitions against the shared input history
    // and write the block's time-domain result. Shared by the live path (state_)
    // and the parallel fade-out render (fading_). Reads the history; never writes
    // or advances it. Caller guarantees `s` has a loaded IR and that the ring is
    // at least `s.num_partitions` long.
    static void render_ir(ConvolverIrStateT<SampleType>& s,
                          const ConvolverInputHistoryT<SampleType>& h,
                          SampleType* output) {
        std::fill(s.accum.begin(), s.accum.end(),
                  std::complex<SampleType>{SampleType{0.0f}, SampleType{0.0f}});
        // Input and IR are FFTs of real data, so every spectrum is Hermitian
        // (bin N-i is the conjugate of bin i). The frequency-domain product is
        // therefore Hermitian too, so only the lower half + Nyquist (N/2+1 bins)
        // is independent — the upper half is reconstructed by conjugate symmetry
        // below instead of being multiply-accumulated, halving the dominant MAC.
        const int half = s.fft_size / 2;   // Nyquist bin; MAC bins [0, half]
        const std::size_t capacity = h.spectra.size();
        for (std::size_t p = 0; p < s.num_partitions; ++p) {
            // Partition p convolves with the input block of age p. `write_pos`
            // holds the block being processed, so age p is p slots back.
            const std::size_t idx = (h.write_pos + capacity - p) % capacity;
            const std::complex<SampleType>* in = h.spectra[idx].data();
            const std::complex<SampleType>* ir = s.ir_spectra[p].data();
            for (int i = 0; i <= half; ++i) {
                // Complex MAC as explicit float ops: the finite-value result of
                // std::complex::operator* without its libm NaN/Inf-recovery
                // branch, so the loop vectorizes. Matches operator* for the
                // finite spectra a convolver sees (differences are sub-ulp).
                const SampleType ar = in[i].real(), ai = in[i].imag();
                const SampleType br = ir[i].real(), bi = ir[i].imag();
                s.accum[i] += std::complex<SampleType>(ar * br - ai * bi,
                                                       ar * bi + ai * br);
            }
        }
        // Mirror the lower half onto the conjugate-symmetric upper half so the
        // inverse transform sees a full spectrum. Bins 0 and `half` (DC and
        // Nyquist) have no distinct partner, so they keep their MAC'd values.
        for (int i = half + 1; i < s.fft_size; ++i)
            s.accum[i] = std::conj(s.accum[s.fft_size - i]);

        s.fft->inverse(s.accum.data());

        for (int i = 0; i < s.block_size; ++i)
            output[i] = s.accum[s.block_size + i].real();
    }

    // Zero the shared input history so the next block starts a new stream.
    // Null-tolerant so reset() can call it on an unloaded convolver.
    static void clear_history(ConvolverInputHistoryT<SampleType>* h) {
        if (!h) return;
        constexpr std::complex<SampleType> kZero{SampleType{0.0f}, SampleType{0.0f}};
        for (auto& spec : h->spectra)
            std::fill(spec.begin(), spec.end(), kZero);
        std::fill(h->overlap.begin(), h->overlap.end(), kZero);
        h->write_pos = 0;
    }

    std::unique_ptr<ConvolverIrStateT<SampleType>> state_;
    // The input delay line, shared by every IR this convolver renders. Outlives
    // any one IR: a swap replaces `state_`, never this. Its ring capacity is the
    // high-water mark of partition counts seen since the last `load_ir()`, since
    // growing it on the audio thread means adopting an incoming state's spare and
    // shrinking it would mean throwing history away for nothing.
    std::unique_ptr<ConvolverInputHistoryT<SampleType>> history_;
    // Set when process() is handed a block whose size does not match the loaded
    // IR partitioning. The next valid block resets before folding itself in.
    bool history_torn_ = false;
    std::uint64_t block_size_violations_ = 0;

    // ── Crossfade state (opt-in via set_crossfade) ────────────────────────────
    std::unique_ptr<ConvolverIrStateT<SampleType>> fading_; // IR fading out (parallel render)
    signal::TransitionMixerT<SampleType> fade_mixer_; // shared click-free blend
    std::size_t fade_samples_ = 0;                // configured fade length (0 = instant)
    TransitionCurve fade_curve_ = TransitionCurve::Smoothstep;
    std::vector<SampleType> fade_scratch_;        // parallel-render output, pre-sized off-RT
};

using PartitionedConvolver = PartitionedConvolverT<float>;
using PartitionedConvolver64 = PartitionedConvolverT<double>;

} // namespace pulp::signal
