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
/// IR loading: unlike the uniform convolver, this class does not provide a
/// background swapper. `load_ir()` rebuilds both stages inline and must be
/// called off the audio thread.

#include <pulp/signal/convolver.hpp>
#include <pulp/signal/convolver_messages.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

// The tail-multiplier table was measured on Apple Silicon Macs with the
// Accelerate backend in single precision; it applies only there.
#if defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE) && defined(__aarch64__) &&                       \
    defined(TARGET_OS_OSX) && TARGET_OS_OSX
#define PULP_NON_UNIFORM_MEASURED_TABLE 1
#else
#define PULP_NON_UNIFORM_MEASURED_TABLE 0
#endif

namespace pulp::signal {

/// Two-stage non-uniform partitioned convolver. Block size B + tail
/// multiplier K (default 4). The "head" handles the first
/// `head_partitions × B` IR samples at block size B; the "tail"
/// handles the remainder at block size K·B.
///
/// RT contract: load_ir() and invalidating IR swaps allocate/rebuild FFT
/// storage and are not audio-thread safe. process(), reset(), latency(), and
/// accessors are allocation-free after a valid IR is loaded and callers pass
/// exactly block_size() samples.
template <typename SampleType = float>
class NonUniformPartitionedConvolverT {
public:
    NonUniformPartitionedConvolverT() = default;

    /// Tail multiplier (K) used where choose_tail_multiplier() has no
    /// measurement. The head stage covers the first K small-block partitions
    /// of the IR so that the tail stage's input-buffering latency (K small
    /// blocks) exactly cancels the IR-tap offset, producing seamless
    /// alignment.
    static constexpr std::size_t kDefaultTailMultiplier = 4;

    /// The tail multiplier load_ir() uses when none is given: the head length
    /// (K · B samples) that minimises the mean per-frame cost for this IR
    /// length and block size.
    ///
    /// A longer head moves more of the IR into the per-block stage, whose
    /// multiply-accumulate grows with it; a shorter one fires the tail's
    /// large FFT more often. The balance point grows with both the IR and the
    /// block: about 1024 samples of head for a one-second IR at 16- to
    /// 32-frame blocks, up to 4096 at 512 frames. A fixed K of 4 sits far
    /// from it for long IRs (1.5-14x the cost at 48000-96000 taps).
    ///
    /// The table is measured in single precision on Apple Silicon Macs with
    /// the Accelerate backend (M1 Max over three runs, M5 Max over five) for IRs of 4096,
    /// 16384, 48000 and 96000 taps and blocks of 16 to 512 frames (receipt:
    /// docs/validation/nonuniform-head/; a test recomputes it from those
    /// files). Each entry is the K whose worst cost ratio to that chip's
    /// cheapest K is lowest over the chips, each chip's cost being its median over
    /// every round of its runs. An IR length uses the row of the
    /// nearest measured length on a log scale. Blocks outside 16-512 frames,
    /// other architectures and operating systems, other SIMD backends, and
    /// double precision keep kDefaultTailMultiplier until they are measured.
    static constexpr std::size_t choose_tail_multiplier(std::size_t ir_length,
                                                        std::size_t block_size) noexcept {
#if PULP_NON_UNIFORM_MEASURED_TABLE
        if constexpr (!std::is_same_v<SampleType, float>) {
            (void)ir_length;
            (void)block_size;
            return kDefaultTailMultiplier;
        } else {
            // Columns: IRs nearest 4096, 16384, 48000 and 96000 taps.
            constexpr std::array<std::uint64_t, 4> kMeasuredIr{4096, 16384, 48000, 96000};
            std::size_t column = 0;
            const auto n = static_cast<std::uint64_t>(ir_length);
            while (column + 1 < kMeasuredIr.size() &&
                   n * n > kMeasuredIr[column] * kMeasuredIr[column + 1])
                ++column;
            switch (detail::convolver_block_size(block_size)) {
            case 16:
                return std::array<std::size_t, 4>{16, 32, 64, 64}[column];
            case 32:
                return std::array<std::size_t, 4>{8, 32, 32, 64}[column];
            case 64:
                return std::array<std::size_t, 4>{8, 16, 32, 64}[column];
            case 128:
                return std::array<std::size_t, 4>{4, 8, 16, 32}[column];
            case 256:
                return std::array<std::size_t, 4>{16, 8, 16, 16}[column];
            case 512:
                return std::array<std::size_t, 4>{8, 8, 8, 16}[column];
            default:
                return kDefaultTailMultiplier;
            }
        }
#else
        (void)ir_length;
        (void)block_size;
        return kDefaultTailMultiplier;
#endif
    }

    /// Load an impulse response with the tail multiplier
    /// choose_tail_multiplier() picks for its length and the block size.
    /// Allocates and FFTs inline — call off the audio thread.
    void load_ir(const SampleType* ir, std::size_t ir_length, std::size_t block_size) {
        load_ir(ir, ir_length, block_size, choose_tail_multiplier(ir_length, block_size));
    }

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
    ///                        (rounded up to power of 2; K=1 degenerates to
    ///                        uniform partitioning). The three-argument
    ///                        overload picks it with choose_tail_multiplier().
    void load_ir(const SampleType* ir, std::size_t ir_length, std::size_t block_size,
                 std::size_t tail_multiplier) {
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
            tail_input_buf_.assign(tail_block_, SampleType{0.0f});
            tail_output_buf_.assign(tail_block_, SampleType{0.0f});
            tail_fill_ = 0;
            tail_stream_pos_ = tail_block_;  // nothing to stream yet
        }
        loaded_ = true;
    }

    /// Process one audio block.
    ///
    /// `input` and `output` may be the same buffer (in-place processing).
    ///
    /// **Precondition: `num_samples == block_size()` whenever an IR is loaded.**
    /// The head/tail stages advance interlocked cursors sized to `block_size()`;
    /// a block of any other length desynchronizes them.
    ///
    /// Fails closed on violation, exactly as `PartitionedConvolver` does: an
    /// unloaded convolver passes through (it is a wire), but a *loaded* one
    /// handed the wrong block size emits silence, bumps
    /// `block_size_violations()`, and marks its history torn so the next
    /// correctly-sized block resets both stages. It never passes through,
    /// because a pass-through is audibly indistinguishable from a working
    /// convolution and would hide the caller's block size bug.
    ///
    /// RT-safe: no allocation, no blocking, no logging, no abort. Pure compute
    /// on the frequency-domain accumulators of both stages.
    ///
    /// Tail-stage timing:
    ///   Accumulate K small blocks of input into `tail_input_buf_`,
    ///   then fire one large-block tail.process() that emits K·B
    ///   samples of tail contribution. Those samples stream out
    ///   over the NEXT K small blocks, mixed into the head output.
    ///   The K-block buffering delay coincides with the K·B IR-tap
    ///   offset of the tail slice, so the sum is sample-aligned with
    ///   the true linear convolution.
    void process(const SampleType* input,
                 SampleType* output,
                 std::size_t num_samples) {
        // No IR to convolve with: pass through.
        if (!loaded_) {
            if (input != output)
                std::copy_n(input, num_samples, output);
            return;
        }
        // Loaded, but the caller's block size does not match the partitioning.
        if (num_samples != block_size_) {
            std::fill_n(output, num_samples, SampleType{0.0f});
            ++block_size_violations_;
            history_torn_ = true;
            return;
        }
        // Recover from a previous violation before folding a valid block in.
        if (history_torn_) reset();

        // ── feed this block into the tail input buffer ──
        // Captured before the head writes `output`, so `input` may be the
        // same buffer as `output`.
        if (tail_block_ > 0) {
            std::copy_n(input, block_size_, tail_input_buf_.data() + tail_fill_);
            tail_fill_ += block_size_;
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
        std::fill(tail_input_buf_.begin(), tail_input_buf_.end(), SampleType{0.0f});
        std::fill(tail_output_buf_.begin(), tail_output_buf_.end(), SampleType{0.0f});
        tail_fill_ = 0;
        tail_stream_pos_ = tail_block_;
        history_torn_ = false;
        block_size_violations_ = 0;
    }

    /// Algorithmic latency in samples. Always 0 — head stage produces
    /// the current block immediately (overlap-save).
    std::size_t latency() const { return 0; }

    std::size_t block_size() const { return block_size_; }

    /// How many times `process()` was called on a loaded convolver with
    /// `num_samples != block_size()`. Every one of those blocks was emitted as
    /// silence. A nonzero count is a caller bug — the host's block size and the
    /// size handed to `load_ir()` disagree. Assert it is zero in tests. Cleared
    /// by `reset()`.
    std::uint64_t block_size_violations() const { return block_size_violations_; }

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
        head_ = PartitionedConvolverT<SampleType>{};
        tail_ = PartitionedConvolverT<SampleType>{};
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

    PartitionedConvolverT<SampleType> head_;
    PartitionedConvolverT<SampleType> tail_;
    std::vector<SampleType> tail_ir_slice_;
    std::vector<SampleType> tail_input_buf_;
    std::vector<SampleType> tail_output_buf_;
    std::size_t block_size_ = 0;
    std::size_t tail_block_ = 0;
    std::size_t tail_multiplier_ = 0;
    std::size_t head_samples_ = 0;
    std::size_t tail_fill_ = 0;
    std::size_t tail_stream_pos_ = 0;  // tail_block_ ⇒ nothing to stream
    bool loaded_ = false;
    // Set when process() is handed a block whose size does not match the loaded
    // IR partitioning. The next valid block resets both stages before folding
    // itself in.
    bool history_torn_ = false;
    std::uint64_t block_size_violations_ = 0;
};

using NonUniformPartitionedConvolver = NonUniformPartitionedConvolverT<float>;
using NonUniformPartitionedConvolver64 = NonUniformPartitionedConvolverT<double>;

} // namespace pulp::signal
