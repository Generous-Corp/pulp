#pragma once

/// @file dither_noise_shaper.hpp
/// Deterministic output quantisation with optional TPDF dither and bounded
/// first- or second-order error-feedback noise shaping.
///
/// The quantiser models signed normalised PCM. For a bit depth `b`, one code
/// step is `2^(1-b)`, the lowest code is -1, and the highest is `1-step`.
/// Values are rounded to the nearest code with halfway cases away from zero,
/// then saturated to that asymmetric code range. TPDF dither is the difference
/// of two independent uniform draws, scaled to one code step.
///
/// Random draws are pure functions of `(seed, frame_coordinate, lane)`, rather
/// than a mutable PRNG stream. A caller may therefore divide a render into
/// blocks without changing its dither. Noise shaping is sequential per
/// instance: blocks must still be presented in time order, and each channel
/// needs its own instance.
///
/// RT contract: construction, scalar controls, `reset()`, `process()`, and
/// `process_block()` allocate no memory, take no locks, and perform no I/O.
/// Controls and reset are intended for block boundaries. The block method is
/// safe in place. A non-finite input clears the error history; NaN maps to zero
/// and infinities saturate to the signed endpoint. Quantiser overload also
/// clears history, so clipped transients cannot leave a noise-shaping hangover.

#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

enum class DitherMode {
    none,
    tpdf,
};

enum class NoiseShapingOrder {
    none,
    first,
    second,
};

template <typename SampleType = float> class DitherNoiseShaperT {
  public:
    static constexpr int kMinBitDepth = 2;
    static constexpr int kMaxBitDepth = 24;
    static constexpr int kDefaultBitDepth = 24;
    static constexpr std::uint64_t kDefaultSeed = 0xD17E4A5E9B37C261ull;

    void set_bit_depth(int bits) noexcept {
        bit_depth_ = std::clamp(bits, kMinBitDepth, kMaxBitDepth);
        update_step_();
        reset();
    }

    int bit_depth() const noexcept {
        return bit_depth_;
    }
    SampleType step() const noexcept {
        return step_;
    }
    SampleType minimum_code() const noexcept {
        return SampleType{-1};
    }
    SampleType maximum_code() const noexcept {
        return SampleType{1} - step_;
    }

    void set_dither_mode(DitherMode mode) noexcept {
        dither_mode_ = mode;
    }
    DitherMode dither_mode() const noexcept {
        return dither_mode_;
    }

    void set_noise_shaping_order(NoiseShapingOrder order) noexcept {
        order_ = order;
        reset();
    }
    NoiseShapingOrder noise_shaping_order() const noexcept {
        return order_;
    }

    /// Sets the purpose key used by all later coordinate-derived draws.
    /// Unlike stream generators, zero is a valid seed and is not remapped.
    void set_seed(std::uint64_t seed) noexcept {
        seed_ = seed;
    }
    std::uint64_t seed() const noexcept {
        return seed_;
    }

    /// Clears only sequential error-feedback history. Configuration, seed, and
    /// coordinate-derived dither remain unchanged.
    void reset() noexcept {
        error_1_ = SampleType{0};
        error_2_ = SampleType{0};
    }

    SampleType error_state_1() const noexcept {
        return error_1_;
    }
    SampleType error_state_2() const noexcept {
        return error_2_;
    }

    /// Returns the raw TPDF draw in code-step units, in the open range (-1, 1).
    SampleType tpdf_lsb(std::uint64_t frame_coordinate, std::uint64_t lane = 0) const noexcept {
        // Hash the lane into the purpose key. Multiplying it into a two-draw
        // field would make lane 0 and lane 2^63 alias after unsigned wrap.
        const std::uint64_t lane_key = mix64(seed_ ^ mix64(lane));
        const SampleType a = unit_from<SampleType>(mix64(lane_key, frame_coordinate, 0u));
        const SampleType b = unit_from<SampleType>(mix64(lane_key, frame_coordinate, 1u));
        return a - b;
    }

    SampleType process(SampleType input, std::uint64_t frame_coordinate,
                       std::uint64_t lane = 0) noexcept {
        if (!std::isfinite(input)) {
            reset();
            if (std::isnan(input))
                return SampleType{0};
            return std::signbit(input) ? minimum_code() : maximum_code();
        }

        const SampleType feedback = feedback_();
        const SampleType dither = dither_mode_ == DitherMode::tpdf
                                      ? tpdf_lsb(frame_coordinate, lane) * step_
                                      : SampleType{0};
        const SampleType driven = input + dither - feedback;
        const auto result = quantize_(driven);

        if (result.clipped || !std::isfinite(result.value)) {
            reset();
            return result.value;
        }

        // Store the quantiser's own error, not source minus output. With the
        // feedback signs above this gives (1-z^-1) and (1-z^-1)^2 noise
        // transfer functions. Away from clipping it is bounded by half a step.
        const SampleType current_error = result.value - driven;
        error_2_ = error_1_;
        error_1_ = std::clamp(current_error, -step_ * SampleType{0.5}, step_ * SampleType{0.5});
        return result.value;
    }

    void process_block(const SampleType* input, SampleType* output, std::size_t frames,
                       std::uint64_t first_frame_coordinate, std::uint64_t lane = 0) noexcept {
        for (std::size_t i = 0; i < frames; ++i) {
            const SampleType sample = input[i];
            output[i] =
                process(sample, first_frame_coordinate + static_cast<std::uint64_t>(i), lane);
        }
    }

    /// Pure undithered quantisation using the same range, tie, and saturation
    /// law as `process()`. Bit depth is clamped to the supported [2, 24] range.
    static SampleType quantize(SampleType input, int bits) noexcept {
        DitherNoiseShaperT quantizer;
        quantizer.set_bit_depth(bits);
        quantizer.set_dither_mode(DitherMode::none);
        quantizer.set_noise_shaping_order(NoiseShapingOrder::none);
        return quantizer.process(input, 0u);
    }

  private:
    struct QuantizeResult {
        SampleType value;
        bool clipped;
    };

    QuantizeResult quantize_(SampleType input) const noexcept {
        if (input <= minimum_code())
            return {minimum_code(), input < minimum_code()};
        if (input >= maximum_code())
            return {maximum_code(), input > maximum_code()};

        const SampleType rounded = std::round(input / step_) * step_;
        const SampleType saturated = std::clamp(rounded, minimum_code(), maximum_code());
        return {saturated, rounded != saturated};
    }

    SampleType feedback_() const noexcept {
        switch (order_) {
        case NoiseShapingOrder::none:
            return SampleType{0};
        case NoiseShapingOrder::first:
            return error_1_;
        case NoiseShapingOrder::second:
            return SampleType{2} * error_1_ - error_2_;
        }
        return SampleType{0};
    }

    void update_step_() noexcept {
        step_ = std::ldexp(SampleType{1}, 1 - bit_depth_);
    }

    int bit_depth_ = kDefaultBitDepth;
    SampleType step_ = std::ldexp(SampleType{1}, 1 - kDefaultBitDepth);
    DitherMode dither_mode_ = DitherMode::tpdf;
    NoiseShapingOrder order_ = NoiseShapingOrder::none;
    std::uint64_t seed_ = kDefaultSeed;
    SampleType error_1_ = SampleType{0};
    SampleType error_2_ = SampleType{0};
};

using DitherNoiseShaper = DitherNoiseShaperT<float>;
using DitherNoiseShaper64 = DitherNoiseShaperT<double>;

} // namespace pulp::signal
