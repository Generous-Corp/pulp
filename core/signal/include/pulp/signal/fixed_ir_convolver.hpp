#pragma once

/// @file fixed_ir_convolver.hpp
/// Fixed-IR convolution that picks the cheaper engine at prepare time.

#include <pulp/signal/convolver.hpp>
#include <pulp/signal/convolver_messages.hpp>
#include <pulp/signal/fir_filter.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pulp::signal {

/// The engine a `FixedIrConvolverT` runs.
enum class ConvolutionBackend : std::uint8_t {
    none,            ///< No IR loaded; process() passes audio through.
    direct_fir,      ///< `FirFilterT`: a vectorized time-domain dot product.
    partitioned_fft, ///< `PartitionedConvolverT`: uniform overlap-save FFT.
};

/// Zero-latency convolution with one impulse response for the lifetime of a
/// preparation, running whichever engine is cheaper for that IR length and
/// block size.
///
/// `load_ir()` chooses the engine once, from the pure `choose_backend()`
/// policy; the choice never changes on the audio thread. Both engines are
/// zero latency, allocation-free in `process()` and `reset()`, and accept
/// exactly `block_size()` frames per call: any other count emits silence,
/// counts a violation, and the next valid block starts from fresh history.
///
/// Pick this over `PartitionedConvolverT` when the IR is fixed and the block
/// size is known; it has no live IR swap or crossfade. For long IRs at small
/// callbacks, `ZeroLatencyConvolverT` (direct head plus FFT tail) and
/// `NonUniformPartitionedConvolverT` partition the IR non-uniformly instead.
///
/// @code
/// FixedIrConvolver conv;
/// conv.load_ir(ir.data(), ir.size(), block_size);   // prepare(): allocates
/// conv.process(input, output, conv.block_size());   // process(): RT-safe
/// @endcode
template <typename SampleType = float> class FixedIrConvolverT {
  public:
    /// Load an IR on the engine `choose_backend()` selects. Prepare-time:
    /// allocates and, for the FFT engine, transforms the IR. A null or empty
    /// IR unloads.
    void load_ir(const SampleType* ir, std::size_t ir_length, std::size_t block_size) {
        load_ir(ir, ir_length, block_size, choose_backend(ir_length, block_size));
    }

    /// Load an IR on a named engine, for callers with their own measurements
    /// for the target. `ConvolutionBackend::none` unloads.
    void load_ir(const SampleType* ir, std::size_t ir_length, std::size_t block_size,
                 ConvolutionBackend engine) {
        direct_.set_coefficients({});
        partitioned_ = PartitionedConvolverT<SampleType>{};
        violations_ = 0;
        if (ir == nullptr || ir_length == 0 || engine == ConvolutionBackend::none) {
            backend_ = ConvolutionBackend::none;
            block_size_ = 0;
            return;
        }
        block_size_ = detail::convolver_block_size(block_size);
        backend_ = engine;
        if (backend_ == ConvolutionBackend::direct_fir)
            direct_.set_coefficients(std::vector<SampleType>(ir, ir + ir_length));
        else
            partitioned_.load_ir(ir, ir_length, block_size);
    }

    /// The engine policy: a pure function of IR length and block size, so
    /// the choice is deterministic and checkable without running audio.
    static constexpr ConvolutionBackend choose_backend(std::size_t ir_length,
                                                       std::size_t block_size) noexcept {
        return ir_length <= direct_fir_limit(block_size) ? ConvolutionBackend::direct_fir
                                                         : ConvolutionBackend::partitioned_fft;
    }

    /// The longest IR run on the direct engine for a block size.
    ///
    /// The table is measured on Apple Silicon with the Accelerate backend,
    /// single and multi-instance, for IRs of 8 to 4096 taps (receipt:
    /// docs/validation/convolver-crossover/). At 16-frame callbacks and below
    /// the direct engine beat the uniform FFT at every measured length, so
    /// the limit there is the end of the measured range; longer IRs keep the
    /// FFT engine until they are measured. Above 16 frames the crossover is
    /// not monotonic in block size, so it is a table rather than a formula.
    /// Other backends keep the FFT engine until they have their own
    /// measurements; a scalar direct kernel is an order of magnitude slower.
    static constexpr std::size_t direct_fir_limit(std::size_t block_size) noexcept {
#if defined(PULP_SIMD_DEFAULT_BACKEND_ACCELERATE)
        switch (detail::convolver_block_size(block_size)) {
        case 1:
        case 2:
        case 4:
        case 8:
        case 16:
            return 4096;
        case 32:
            return 448;
        case 64:
            return 256;
        case 128:
            return 512;
        case 256:
            return 384;
        default:
            return 256;
        }
#else
        (void)block_size;
        return 0;
#endif
    }

    /// Process one block. `input` and `output` may be the same buffer; any
    /// other overlap is unsupported, because the direct engine writes earlier
    /// outputs before it reads later inputs on blocks longer than its history
    /// slice.
    void process(const SampleType* input, SampleType* output, std::size_t num_samples) {
        switch (backend_) {
        case ConvolutionBackend::none:
            if (input != output)
                std::copy_n(input, num_samples, output);
            return;
        case ConvolutionBackend::partitioned_fft:
            // The FFT engine fails closed and recovers on its own; only the
            // count lives here, so both engines report it the same way.
            if (num_samples != block_size_)
                ++violations_;
            partitioned_.process(input, output, num_samples);
            return;
        case ConvolutionBackend::direct_fir:
            if (num_samples != block_size_) {
                std::fill_n(output, num_samples, SampleType{0});
                ++violations_;
                direct_.reset();
                return;
            }
            direct_.process(input, output, static_cast<int>(num_samples));
            return;
        }
    }

    /// Clear the history so the next block starts from silence. Also clears
    /// the block-size violation count.
    void reset() {
        direct_.reset();
        partitioned_.reset();
        violations_ = 0;
    }

    [[nodiscard]] bool is_loaded() const noexcept {
        return backend_ != ConvolutionBackend::none;
    }
    /// The block size `process()` requires: the `load_ir()` request rounded
    /// up to a power of two. Zero when no IR is loaded.
    [[nodiscard]] std::size_t block_size() const noexcept {
        return block_size_;
    }
    [[nodiscard]] std::size_t latency() const noexcept {
        return 0;
    }
    /// Calls to `process()` with a frame count other than `block_size()`
    /// since the last `load_ir()` or `reset()`. Nonzero means a caller bug.
    [[nodiscard]] std::uint64_t block_size_violations() const noexcept {
        return violations_;
    }
    [[nodiscard]] ConvolutionBackend backend() const noexcept {
        return backend_;
    }

  private:
    ConvolutionBackend backend_ = ConvolutionBackend::none;
    std::size_t block_size_ = 0;
    std::uint64_t violations_ = 0;
    FirFilterT<SampleType> direct_;
    PartitionedConvolverT<SampleType> partitioned_;
};

using FixedIrConvolver = FixedIrConvolverT<float>;
using FixedIrConvolver64 = FixedIrConvolverT<double>;

} // namespace pulp::signal
