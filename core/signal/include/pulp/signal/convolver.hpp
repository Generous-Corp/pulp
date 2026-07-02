#pragma once

#include <pulp/signal/convolver_messages.hpp>
#include <pulp/signal/fft.hpp>

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

        auto next = swapper.try_consume();
        if (!next)
            return false;

        auto previous = std::move(state_);
        // Carry the input delay line from the displaced IR into the incoming one so
        // the swap doesn't truncate the convolver's history (the tail dip the class
        // comment promises to avoid). The FDL is IR-independent; this moves the
        // overlapping recent history (buffer swaps, no per-sample copy) and returns
        // the ring cursor to resume at.
        partition_index_ =
            previous ? detail::carry_input_history(*previous, *next, partition_index_) : 0;
        state_ = std::move(next);

        if (previous) {
            // We pre-checked capacity above (single-producer ring +
            // single audio-thread consumer here, so no race between
            // the check and this push). The unreachable false branch
            // is defence-in-depth — leaks `previous` rather than
            // freeing on RT.
            const bool ok = swapper.retire(previous);
            (void)ok;
            (void)previous.release(); // ownership transferred on ok==true
        }
        return true;
    }

    /// Process a block of audio. num_samples must equal block_size.
    void process(const float* input, float* output, std::size_t num_samples) {
        if (!state_ || state_->ir_spectra.empty()
            || static_cast<int>(num_samples) != state_->block_size) {
            std::copy_n(input, num_samples, output);
            return;
        }
        auto& s = *state_;

        for (int i = 0; i < s.block_size; ++i)
            s.input_buffer[s.block_size + i] = {input[i], 0.0f};

        auto& current_spectrum = s.input_spectra[partition_index_];
        std::copy(s.input_buffer.begin(), s.input_buffer.end(),
                  current_spectrum.begin());
        s.fft->forward(current_spectrum.data());

        std::fill(s.accum.begin(), s.accum.end(),
                  std::complex<float>{0.0f, 0.0f});
        // Input and IR are FFTs of real data, so every spectrum is Hermitian
        // (bin N-i is the conjugate of bin i). The frequency-domain product is
        // therefore Hermitian too, so only the lower half + Nyquist (N/2+1 bins)
        // is independent — the upper half is reconstructed by conjugate symmetry
        // below instead of being multiply-accumulated, halving the dominant MAC.
        const int half = s.fft_size / 2;   // Nyquist bin; MAC bins [0, half]
        for (std::size_t p = 0; p < s.num_partitions; ++p) {
            const std::size_t idx =
                (partition_index_ + s.num_partitions - p) % s.num_partitions;
            const std::complex<float>* in = s.input_spectra[idx].data();
            const std::complex<float>* ir = s.ir_spectra[p].data();
            for (int i = 0; i <= half; ++i) {
                // Complex MAC written as explicit float ops: this is the
                // finite-value result of std::complex::operator* without its
                // libm NaN/Inf-recovery branch, so the loop is a plain float
                // multiply-add the compiler can vectorize. Matches operator*
                // for the finite spectra a convolver sees (any difference is a
                // sub-ulp FMA-contraction detail, well inside golden tolerance).
                const float ar = in[i].real(), ai = in[i].imag();
                const float br = ir[i].real(), bi = ir[i].imag();
                s.accum[i] += std::complex<float>(ar * br - ai * bi,
                                                  ar * bi + ai * br);
            }
        }
        // Mirror the lower half onto the conjugate-symmetric upper half so the
        // inverse transform sees a full spectrum. Bins 0 and `half` (DC and
        // Nyquist) have no distinct partner, so they keep their MAC'd values
        // unchanged — any residual imaginary part there is the same FFT-roundoff
        // term the full-spectrum path carried, not a new bias.
        for (int i = half + 1; i < s.fft_size; ++i)
            s.accum[i] = std::conj(s.accum[s.fft_size - i]);

        s.fft->inverse(s.accum.data());

        for (int i = 0; i < s.block_size; ++i)
            output[i] = s.accum[s.block_size + i].real();

        std::copy_n(s.input_buffer.begin() + s.block_size, s.block_size,
                    s.input_buffer.begin());
        std::fill(s.input_buffer.begin() + s.block_size,
                  s.input_buffer.end(),
                  std::complex<float>{0.0f, 0.0f});

        partition_index_ = (partition_index_ + 1) % s.num_partitions;
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
    std::unique_ptr<ConvolverIrState> state_;
    std::size_t partition_index_ = 0;
};

} // namespace pulp::signal
