#pragma once

#include <pulp/signal/convolver_messages.hpp>
#include <pulp/signal/fft.hpp>
#include <pulp/signal/transition_mixer.hpp>

#include <algorithm>
#include <cassert>
#include <complex>
#include <cstddef>
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
class PartitionedConvolver {
public:
    PartitionedConvolver() = default;

    /// Load an impulse response. block_size should match your audio
    /// callback size. block_size must be a power of two (for the
    /// radix-2 FFT); if not, it is rounded up to the next one.
    ///
    /// Allocates and FFTs inline — call off the audio thread.
    void load_ir(const float* ir, std::size_t ir_length, std::size_t block_size) {
        state_ = detail::build_convolver_ir_state(ir, ir_length, block_size);
        partition_index_ = 0;
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
    bool try_swap_ir(ConvolverIrSwapper& swapper) {
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

        // A crossfade still running (item 2.1b): don't start another swap and
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

        const std::size_t prev_partition = partition_index_;
        auto previous = std::move(state_);
        state_ = std::move(next);
        partition_index_ = 0;

        if (previous) {
            // Crossfade path: keep the displaced IR rendering in parallel from
            // its own continuing history and blend old->new (item 2.1b). Only
            // when a fade is configured AND the scratch fits both IRs' block
            // size; otherwise fall back to the instant retire below.
            // Require MATCHING block sizes: the fade-out render reads a full
            // `previous->block_size` of `input`, but process() only guarantees
            // num_samples == state_->block_size — a larger old block would read
            // past the caller's input buffer. A mismatch falls back to instant.
            const bool can_fade =
                fade_samples_ > 0 &&
                previous->block_size == state_->block_size &&
                fade_scratch_.size() >= static_cast<std::size_t>(state_->block_size);
            if (can_fade) {
                fading_ = std::move(previous);
                fading_partition_index_ = prev_partition;
                fade_mixer_.configure(fade_samples_, fade_curve_);
            } else {
                // Instantaneous swap (default): park the displaced IR now.
                // Pre-checked capacity above; the false branch is defence-in-
                // depth — leaks rather than freeing on RT.
                const bool ok = swapper.retire(previous);
                (void)ok;
                (void)previous.release();
            }
        }
        return true;
    }

    /// Enable a click-free crossfade on IR swaps (item 2.1b, opt-in — default 0
    /// = instantaneous swap, exactly the prior behavior). When set, a swap keeps
    /// the OLD IR rendering in parallel with the new for `samples` and blends
    /// old→new via the shared TransitionMixer, so an IR change is audibly
    /// continuous (the old IR's tail fades out from its real history while the
    /// new fades in) rather than a hard cut. Call off the audio thread AFTER an
    /// IR is loaded (it sizes the parallel-render scratch from the block size).
    void set_crossfade(std::size_t samples, TransitionCurve curve = TransitionCurve::Smoothstep) {
        fade_samples_ = samples;
        fade_curve_ = curve;
        if (state_) fade_scratch_.assign(static_cast<std::size_t>(state_->block_size), 0.0f);
    }

    /// Process a block of audio. num_samples must equal block_size.
    void process(const float* input, float* output, std::size_t num_samples) {
        if (!state_ || state_->ir_spectra.empty()
            || static_cast<int>(num_samples) != state_->block_size) {
            std::copy_n(input, num_samples, output);
            return;
        }
        render_state(*state_, partition_index_, input, output, num_samples);

        // Crossfade the retiring IR (item 2.1b): render it in parallel from its
        // OWN continuing history into scratch and blend old→new. RT-safe: scratch
        // is pre-sized, no alloc; the fade-out IR is retired on the ring after
        // the fade (or opportunistically once done), never freed on the audio
        // thread. Only engages when a fade is configured + running.
        if (fading_ && fade_scratch_.size() >= num_samples &&
            static_cast<std::size_t>(fading_->block_size) == num_samples) {
            if (!fade_mixer_.done()) {
                render_state(*fading_, fading_partition_index_, input,
                             fade_scratch_.data(), num_samples);
                const std::size_t base = fade_mixer_.position();
                for (std::size_t i = 0; i < num_samples; ++i) {
                    float old_gain, new_gain;
                    fade_mixer_.gains_at(base + i, old_gain, new_gain);
                    output[i] = output[i] * new_gain + fade_scratch_[i] * old_gain;
                }
                fade_mixer_.advance(num_samples);
            }
            // When the fade completes the fade-out IR is simply held (not
            // rendered anymore); it is parked on the swapper's retire ring by the
            // next try_swap_ir() — never freed on the audio thread here.
        }
    }

    void reset() {
        if (!state_) return;
        for (auto& spec : state_->input_spectra)
            std::fill(spec.begin(), spec.end(), std::complex<float>{0.0f, 0.0f});
        std::fill(state_->input_buffer.begin(), state_->input_buffer.end(),
                  std::complex<float>{0.0f, 0.0f});
        partition_index_ = 0;
    }

    /// Returns the algorithmic latency in samples.
    /// Overlap-save produces valid output for the current block
    /// immediately (partition 0 is applied in the same callback), so
    /// latency is 0.
    std::size_t latency() const { return 0; }

    std::size_t num_partitions() const {
        return state_ ? state_->num_partitions : 0;
    }

    bool is_loaded() const {
        return state_ && !state_->ir_spectra.empty();
    }

private:
    // Overlap-save partitioned convolution for one IR state, advancing its own
    // partition index. Shared by the live path (state_) and the parallel
    // fade-out render (fading_). Caller guarantees num_samples == s.block_size
    // and s has a loaded IR.
    void render_state(ConvolverIrState& s, std::size_t& partition_index,
                      const float* input, float* output, std::size_t num_samples) {
        (void)num_samples;
        for (int i = 0; i < s.block_size; ++i)
            s.input_buffer[s.block_size + i] = {input[i], 0.0f};

        auto& current_spectrum = s.input_spectra[partition_index];
        std::copy(s.input_buffer.begin(), s.input_buffer.end(),
                  current_spectrum.begin());
        s.fft->forward(current_spectrum.data());

        std::fill(s.accum.begin(), s.accum.end(), std::complex<float>{0.0f, 0.0f});
        for (std::size_t p = 0; p < s.num_partitions; ++p) {
            const std::size_t idx =
                (partition_index + s.num_partitions - p) % s.num_partitions;
            for (int i = 0; i < s.fft_size; ++i)
                s.accum[i] += s.input_spectra[idx][i] * s.ir_spectra[p][i];
        }

        s.fft->inverse(s.accum.data());

        for (int i = 0; i < s.block_size; ++i)
            output[i] = s.accum[s.block_size + i].real();

        std::copy_n(s.input_buffer.begin() + s.block_size, s.block_size,
                    s.input_buffer.begin());
        std::fill(s.input_buffer.begin() + s.block_size, s.input_buffer.end(),
                  std::complex<float>{0.0f, 0.0f});

        partition_index = (partition_index + 1) % s.num_partitions;
    }

    std::unique_ptr<ConvolverIrState> state_;
    std::size_t partition_index_ = 0;

    // ── Crossfade state (item 2.1b; opt-in via set_crossfade) ─────────────────
    std::unique_ptr<ConvolverIrState> fading_;    // IR fading out (parallel render)
    std::size_t fading_partition_index_ = 0;      // its own overlap-save cursor
    signal::TransitionMixer fade_mixer_;          // shared click-free blend (item 2.1)
    std::size_t fade_samples_ = 0;                // configured fade length (0 = instant)
    TransitionCurve fade_curve_ = TransitionCurve::Smoothstep;
    std::vector<float> fade_scratch_;             // parallel-render output, pre-sized off-RT
};

} // namespace pulp::signal
