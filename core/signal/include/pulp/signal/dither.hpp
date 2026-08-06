#pragma once

/// @file dither.hpp
/// Deterministic TPDF dither and bounded error-feedback quantization.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/rng.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::signal {

enum class DitherMode : std::uint8_t {
    none = 0,
    tpdf = 1,
};

enum class NoiseShapingOrder : std::uint8_t {
    none = 0,
    first = 1,
    second = 2,
};

/// Converts two independent uniform draws in [0, 1) to unit-peak TPDF noise.
/// The difference spelling preserves existing converter streams that already
/// use `u1 - u2`; the centered-sum spelling preserves streams using
/// `u1 + u2 - 1`. Both have zero mean and variance 1/6.
template <typename SampleType>
constexpr SampleType tpdf_difference(SampleType first, SampleType second) noexcept {
    return first - second;
}

template <typename SampleType>
constexpr SampleType tpdf_centered_sum(SampleType first, SampleType second) noexcept {
    return first + second - SampleType{1};
}

template <typename SampleType> struct NoiseShaperStateT {
    SampleType error_1{};
    SampleType error_2{};

    constexpr void reset() noexcept {
        error_1 = SampleType{};
        error_2 = SampleType{};
    }
};

/// Public intermediate returned by the pure quantizer transition. It lets
/// meters and tests inspect the shaped input and error without duplicating the
/// audio path's arithmetic.
template <typename SampleType> struct DitheredQuantizationResultT {
    SampleType output{};
    SampleType shaped_input{};
    SampleType dither{};
    SampleType quantization_error{};
    bool recovered = false;
};

/// Mid-tread quantization step for a continuous bit-depth control.
template <typename SampleType> inline SampleType quantization_step(SampleType bits) noexcept {
    if (!std::isfinite(bits))
        return SampleType{1};
    return std::exp2(-(std::max(bits, SampleType{1}) - SampleType{1}));
}

/// One allocation-free error-feedback transition.
///
/// First order realizes the noise-transfer function `1 - z^-1`; second order
/// realizes `(1 - z^-1)^2`. Error memory is clamped to four quantization steps
/// and snapped away from denormals. A non-finite input or arithmetic overflow
/// emits zero and clears the memory, so one bad frame cannot poison the tail.
template <typename SampleType>
inline DitheredQuantizationResultT<SampleType>
quantize_with_error_feedback(SampleType input, SampleType step, DitherMode dither_mode,
                             NoiseShapingOrder shaping, NoiseShaperStateT<SampleType>& state,
                             SampleType first_uniform = SampleType{},
                             SampleType second_uniform = SampleType{}) noexcept {
    DitheredQuantizationResultT<SampleType> result{};
    if (!(std::isfinite(input) && std::isfinite(step) && step > SampleType{})) {
        state.reset();
        result.recovered = true;
        return result;
    }

    SampleType feedback{};
    if (shaping == NoiseShapingOrder::first) {
        feedback = -state.error_1;
    } else if (shaping == NoiseShapingOrder::second) {
        feedback = SampleType{-2} * state.error_1 + state.error_2;
    }

    result.dither = dither_mode == DitherMode::tpdf
                        ? tpdf_difference(first_uniform, second_uniform) * step
                        : SampleType{};
    result.shaped_input = input + feedback + result.dither;
    const SampleType scaled = result.shaped_input / step;
    if (!std::isfinite(result.shaped_input) || !std::isfinite(scaled) ||
        std::abs(scaled) > static_cast<SampleType>(std::numeric_limits<std::int64_t>::max() / 2)) {
        state.reset();
        result.recovered = true;
        return result;
    }

    result.output = std::round(scaled) * step;
    if (!std::isfinite(result.output)) {
        state.reset();
        result = {};
        result.recovered = true;
        return result;
    }

    // Retain the current innovation: rounding residual plus non-subtractive
    // dither. Feeding this value back produces the FIR noise-transfer
    // functions documented above while shaping both sources of output noise.
    result.quantization_error = result.output - input - feedback;
    const SampleType error_limit = SampleType{4} * step;
    const SampleType bounded = std::clamp(result.quantization_error, -error_limit, error_limit);
    state.error_2 = snap_to_zero(state.error_1);
    state.error_1 = snap_to_zero(bounded);
    return result;
}

/// Stateful deterministic wrapper for per-sample and block processing.
///
/// RT contract: all state is fixed-size scalar storage. `process`, `reset`, and
/// every setter allocate nothing, take no locks, and do not throw. The random
/// stream advances only when TPDF is enabled and the quantizer is active.
template <typename SampleType = float> class DitherQuantizerT {
  public:
    static constexpr std::uint32_t default_seed = 0x2C1B3A5Du;

    void set_bits(SampleType bits) noexcept {
        if (!std::isfinite(bits)) {
            bits_ = SampleType{24};
            state_.reset();
            return;
        }
        const SampleType sanitized = std::clamp(bits, SampleType{1}, SampleType{24});
        if (sanitized != bits_) {
            // A continuous bit-depth control changes the quantizer step every
            // sample. Preserve the error history in signal units so shaping
            // remains active through that glide, while applying the same
            // four-step safety bound used by the quantizer transition. This
            // also makes a coarse-to-fine jump safe without turning every
            // ordinary automation sample into a state reset.
            const SampleType error_limit = SampleType{4} * quantization_step(sanitized);
            state_.error_1 = snap_to_zero(
                std::clamp(state_.error_1, -error_limit, error_limit));
            state_.error_2 = snap_to_zero(
                std::clamp(state_.error_2, -error_limit, error_limit));
        }
        bits_ = sanitized;
    }
    void set_dither_mode(DitherMode mode) noexcept {
        if (mode != dither_mode_) state_.reset();
        dither_mode_ = mode;
    }
    void set_noise_shaping(NoiseShapingOrder order) noexcept {
        if (order != shaping_) state_.reset();
        shaping_ = order;
    }
    void set_seed(std::uint32_t seed) noexcept {
        seed_ = seed == 0u ? default_seed : seed;
        rng_.seed(seed_);
    }

    /// Copy policy and reset seed without copying another instance's live
    /// error memory or random-stream position.
    void sync_configuration_from(const DitherQuantizerT& other) noexcept {
        bits_ = other.bits_;
        dither_mode_ = other.dither_mode_;
        shaping_ = other.shaping_;
        seed_ = other.seed_;
    }

    void reset() noexcept {
        state_.reset();
        rng_.seed(seed_);
    }

    SampleType process(SampleType input) noexcept {
        if (bits_ >= SampleType{24})
            return input;
        SampleType first{};
        SampleType second{};
        if (dither_mode_ == DitherMode::tpdf) {
            first = rng_.template next_unit<SampleType>();
            second = rng_.template next_unit<SampleType>();
        }
        return quantize_with_error_feedback(input, quantization_step(bits_), dither_mode_, shaping_,
                                            state_, first, second)
            .output;
    }

    void process(SampleType* samples, int count) noexcept {
        if (samples == nullptr || count <= 0)
            return;
        for (int i = 0; i < count; ++i)
            samples[i] = process(samples[i]);
    }

    SampleType bits() const noexcept {
        return bits_;
    }
    DitherMode dither_mode() const noexcept {
        return dither_mode_;
    }
    NoiseShapingOrder noise_shaping() const noexcept {
        return shaping_;
    }
    const NoiseShaperStateT<SampleType>& state() const noexcept {
        return state_;
    }
    std::uint32_t rng_state() const noexcept {
        return rng_.state();
    }
    std::uint32_t seed() const noexcept {
        return seed_;
    }
    static constexpr int latency_samples() noexcept {
        return 0;
    }

  private:
    SampleType bits_{24};
    DitherMode dither_mode_ = DitherMode::none;
    NoiseShapingOrder shaping_ = NoiseShapingOrder::none;
    NoiseShaperStateT<SampleType> state_{};
    std::uint32_t seed_ = default_seed;
    Xorshift32 rng_{default_seed};
};

using DitherQuantizer = DitherQuantizerT<float>;
using DitherQuantizer64 = DitherQuantizerT<double>;

} // namespace pulp::signal
