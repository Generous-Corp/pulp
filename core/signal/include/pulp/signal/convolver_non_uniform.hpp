#pragma once

/// @file convolver_non_uniform.hpp
/// Two-stage Gardner-style non-uniform partitioned convolution.
///
/// `NonUniformPartitionedConvolver` splits an impulse response into:
///   • a small-block "head" stage processing the first `head_samples`
///     of the IR at the user-provided audio block size B (zero-latency
///     overlap-save partitioned convolution), and
///   • a large-block "tail" stage processing IR samples
///     [head_samples, ir_length) at a larger block size K · B (K ≥ 2),
///     which fires only every K audio blocks, amortizing the larger
///     FFT cost across many small blocks.
///
/// Why: a single uniform partitioned convolver at block size B pays
/// the per-block FFT cost on every block. A long IR (1 s @ 48 kHz =
/// 48 000 samples) needs `num_partitions = ceil(ir_len / B)` complex
/// multiplies per block in the frequency-domain accumulate; the FFTs
/// themselves stay O(B log B) per block. Switching the tail to a
/// larger block size collapses those tail-partition spectra into
/// fewer (and larger) FFT bins and runs the convolution loop on a
/// schedule of one out of every K blocks.
///
/// Latency: zero — head stage produces the current block immediately
/// via overlap-save. The tail stage's contribution to a given input
/// sample arrives at the output `head_samples` samples later, which
/// is exactly where tail IR taps would have contributed in a direct
/// convolution.
///
/// Acceptance pinned in `test/test_convolver_non_uniform.cpp`:
///   • impulse round-trip matches the IR within float epsilon,
///   • non-uniform output matches uniform `PartitionedConvolver`
///     output to within float epsilon over a sine sweep,
///   • short noise burst through a 4 096-tap IR matches the
///     direct-convolution reference within float epsilon.
///
/// Background IR swap: a follow-up will mirror the uniform
/// convolver's `ConvolverIrSwapper` for non-uniform IRs. Today,
/// `load_ir()` rebuilds both stages inline (off-RT-thread).

#include <pulp/signal/convolver.hpp>
#include <pulp/signal/convolver_messages.hpp>

#include <algorithm>
#include <cassert>
#include <complex>
#include <cstddef>
#include <vector>

namespace pulp::signal {

/// Two-stage non-uniform partitioned convolver. Block size B + tail
/// multiplier K (default 4). The "head" handles the first
/// `head_partitions × B` IR samples at block size B; the "tail"
/// handles the remainder at block size K·B.
class NonUniformPartitionedConvolver {
public:
    NonUniformPartitionedConvolver() = default;

    /// Reasonable default tail multiplier (K). The head stage covers
    /// the first K small-block partitions of the IR so that the tail
    /// stage's input-buffering latency (K small blocks) exactly
    /// cancels the IR-tap offset, producing seamless alignment.
    static constexpr std::size_t kDefaultTailMultiplier = 4;

    /// Load an impulse response into both stages.
    ///
    /// Allocates and FFTs inline — call off the audio thread.
    ///
    /// Architectural invariant: head covers the first
    /// `tail_multiplier × block_size` IR samples. This makes the
    /// buffering delay of the tail stage (K small blocks) coincide
    /// with the IR-tap offset of the tail (K · B samples), so the
    /// tail FFT output streams in with zero net latency. Callers may
    /// override `tail_multiplier` but cannot decouple head length
    /// from it — that's the whole point of the non-uniform layout.
    ///
    /// @param ir              IR samples.
    /// @param ir_length       IR length in samples.
    /// @param block_size      Audio callback block size (rounded up to
    ///                        the next power of two for the radix-2 FFT).
    /// @param tail_multiplier K factor for the tail-stage block size
    ///                        (default 4; rounded up to power of 2;
    ///                        K=1 degenerates to uniform partitioning).
    void load_ir(const float* ir, std::size_t ir_length,
                 std::size_t block_size,
                 std::size_t tail_multiplier = kDefaultTailMultiplier) {
        reset_state();
        if (ir == nullptr || ir_length == 0 || block_size == 0) return;

        // Pow-of-2 block size.
        block_size_ = round_up_pow2(block_size);
        if (tail_multiplier == 0) tail_multiplier = 1;
        // Tail multiplier must be a power of 2 so the larger FFT also
        // works on a pow-of-2 block.
        tail_multiplier_ = round_up_pow2(tail_multiplier);

        std::size_t head_samples = tail_multiplier_ * block_size_;
        head_samples = std::min(head_samples, ir_length);

        // ── head stage ──
        head_.load_ir(ir, head_samples, block_size_);
        head_samples_ = head_samples;

        // ── tail stage (optional) ──
        if (head_samples < ir_length) {
            std::size_t tail_block = block_size_ * tail_multiplier_;
            std::size_t tail_len = ir_length - head_samples;
            // The tail IR is the slice ir[head_samples .. ir_length)
            // with NO leading zero-padding. The K-small-block input
            // buffering delay equals the K·B = head_samples IR-tap
            // offset, so the natural sum lines up.
            tail_ir_slice_.assign(ir + head_samples, ir + ir_length);
            tail_.load_ir(tail_ir_slice_.data(), tail_len, tail_block);
            tail_block_ = tail_block;
            // Input/output ring buffers for the tail stage.
            tail_input_buf_.assign(tail_block_, 0.0f);
            tail_output_buf_.assign(tail_block_, 0.0f);
            tail_fill_ = 0;
            tail_stream_pos_ = tail_block_;  // nothing to stream yet
        }
        loaded_ = true;
    }

    /// Process one audio block. `num_samples` must equal block_size().
    ///
    /// RT-safe: no allocation, no blocking. Pure compute on the
    /// frequency-domain accumulators of both stages.
    ///
    /// Tail-stage timing:
    ///   Accumulate K small blocks of input into `tail_input_buf_`,
    ///   then fire one large-block tail.process() that emits K·B
    ///   samples of tail contribution. Those samples stream out
    ///   over the NEXT K small blocks, mixed into the head output.
    ///   The K-block buffering delay coincides with the K·B IR-tap
    ///   offset of the tail slice, so the sum is sample-aligned with
    ///   the true linear convolution.
    void process(const float* input, float* output, std::size_t num_samples) {
        if (!loaded_ || num_samples != block_size_) {
            // Pass-through fallback (matches PartitionedConvolver).
            std::copy_n(input, num_samples, output);
            return;
        }

        // ── head stage: live ──
        head_.process(input, output, num_samples);

        if (tail_block_ == 0) return;

        // ── stream out previously-computed tail samples ──
        if (tail_stream_pos_ < tail_block_) {
            for (std::size_t i = 0; i < block_size_; ++i)
                output[i] += tail_output_buf_[tail_stream_pos_ + i];
            tail_stream_pos_ += block_size_;
        }

        // ── feed this block into the tail input buffer ──
        for (std::size_t i = 0; i < block_size_; ++i)
            tail_input_buf_[tail_fill_ + i] = input[i];
        tail_fill_ += block_size_;

        if (tail_fill_ >= tail_block_) {
            // Process one tail block worth of input. Result will be
            // streamed out over the NEXT K small blocks (starting on
            // the next process() call).
            tail_.process(tail_input_buf_.data(),
                          tail_output_buf_.data(), tail_block_);
            tail_fill_ = 0;
            tail_stream_pos_ = 0;
        }
    }

    void reset() {
        head_.reset();
        if (tail_block_ > 0) tail_.reset();
        std::fill(tail_input_buf_.begin(), tail_input_buf_.end(), 0.0f);
        std::fill(tail_output_buf_.begin(), tail_output_buf_.end(), 0.0f);
        tail_fill_ = 0;
        tail_stream_pos_ = tail_block_;
    }

    /// Algorithmic latency in samples. Always 0 — head stage produces
    /// the current block immediately (overlap-save).
    std::size_t latency() const { return 0; }

    std::size_t block_size() const { return block_size_; }
    std::size_t head_samples() const { return head_samples_; }
    std::size_t tail_block() const { return tail_block_; }
    std::size_t tail_multiplier() const { return tail_multiplier_; }

    bool is_loaded() const { return loaded_; }

private:
    static std::size_t round_up_pow2(std::size_t v) {
        if (v <= 1) return 1;
        if ((v & (v - 1)) == 0) return v;
        std::size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    void reset_state() {
        head_ = PartitionedConvolver{};
        tail_ = PartitionedConvolver{};
        tail_ir_slice_.clear();
        tail_input_buf_.clear();
        tail_output_buf_.clear();
        block_size_ = 0;
        tail_block_ = 0;
        tail_multiplier_ = 0;
        head_samples_ = 0;
        tail_fill_ = 0;
        tail_stream_pos_ = 0;
        loaded_ = false;
    }

    PartitionedConvolver head_;
    PartitionedConvolver tail_;
    std::vector<float> tail_ir_slice_;
    std::vector<float> tail_input_buf_;
    std::vector<float> tail_output_buf_;
    std::size_t block_size_ = 0;
    std::size_t tail_block_ = 0;
    std::size_t tail_multiplier_ = 0;
    std::size_t head_samples_ = 0;
    std::size_t tail_fill_ = 0;
    std::size_t tail_stream_pos_ = 0;  // tail_block_ ⇒ nothing to stream
    bool loaded_ = false;
};

} // namespace pulp::signal
