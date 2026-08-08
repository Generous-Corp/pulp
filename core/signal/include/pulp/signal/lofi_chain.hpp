#pragma once

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/dither.hpp>
#include <pulp/signal/tpt_filter.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pulp::signal {

/// Quantises to `bits` of resolution, mid-tread, symmetric about zero.
///
/// `bits` is a continuous control rather than an integer: sweeping it is
/// musically useful and the rounding grid is well defined between whole bit
/// depths. Values at or above 24 return the input unchanged, because at that
/// point the quantisation step is below the noise floor of anything the caller
/// will play it through and the rounding only costs cycles.
inline double quantize_bits(double x, double bits) {
    if (bits >= 24.0) return x;
    const double levels = std::pow(2.0, std::max(bits, 1.0) - 1.0);
    return std::round(x * levels) / levels;
}

/// A dead-zone saturator: a hard gap around zero, then a soft-limited slope.
///
/// The gap is what distinguishes this from an ordinary soft clipper. Digital
/// audio chips of the 8- and 16-bit era produced no output at all for the
/// smallest input codes, so a decaying tail does not fade smoothly — it thins
/// out and then stops abruptly. That truncated tail is the characteristic
/// sound; a tanh alone cannot produce it because tanh is smooth through zero.
///
/// `dead_zone` is the fraction of full scale that produces silence. The region
/// above it is rescaled so the transfer curve stays continuous and still
/// reaches full scale, then soft-limited so the rescaling cannot clip.
inline double dead_zone_saturate(double x, double dead_zone) {
    const double dz = std::clamp(dead_zone, 0.0, 0.9);
    const double magnitude = std::fabs(x);
    if (magnitude <= dz) return 0.0;
    const double rescaled = (magnitude - dz) / (1.0 - dz);
    return std::copysign(std::tanh(1.5 * rescaled), x);
}

/// Sample-and-hold rate reducer with clock jitter and a reconstruction filter.
///
/// Holding a sample for several output samples is a zero-order hold, which
/// mirrors the signal around the hold rate. Those images are the point — they
/// are the grit that a "lo-fi" control is asked for — but they can be more
/// than a caller wants, so the class ends with a one-pole lowpass whose corner
/// tracks the hold rate and thins them out. Callers who want the raw folded
/// spectrum set the smoothing to zero.
///
/// The jitter perturbs the hold clock's period rather than its output. Early
/// samplers clocked their converters from free-running oscillators with no
/// phase relationship to the audio, so the hold boundaries drifted; that drift
/// broadens the image skirts and is why an emulation with a perfectly regular
/// hold sounds cleaner than the hardware it models. Jitter is driven by a
/// deterministic generator so a render stays reproducible.
///
/// RT contract: every member allocates nothing and takes no locks.
template <typename SampleType = float>
class SampleRateReducerT {
public:
    static constexpr std::uint32_t default_seed = 0x6A09E667u;

    void set_sample_rate(double sr) {
        sample_rate_ = sr > 0.0 ? sr : sample_rate_;
        update();
    }

    /// Rate the held samples are clocked at, in Hz. Clamped to the sample rate,
    /// at or above which the reducer is transparent. The default is unbounded
    /// rather than a fixed number of Hz, so an unconfigured reducer stays
    /// transparent at every sample rate instead of quietly decimating whenever
    /// the host runs faster than whatever default was chosen.
    void set_hold_rate_hz(double hz) {
        hold_rate_ = std::max(hz, 1.0);
        update();
    }

    /// Clock jitter as a fraction of the hold period, 0 to 1.
    void set_jitter(double amount) {
        jitter_ = std::clamp(amount, 0.0, 1.0);
    }

    /// Strength of the reconstruction lowpass, 0 (bypassed, raw images) to 1
    /// (corner at the hold rate's Nyquist). One pole is a gentle slope, so
    /// this attenuates the images rather than removing them — which is the
    /// behaviour of the reconstruction filters the stage models, and leaves
    /// enough of the hold's character to still be worth having.
    void set_smoothing(double amount) {
        smoothing_ = std::clamp(amount, 0.0, 1.0);
        update();
    }

    void set_seed(std::uint32_t seed) { seed_ = seed == 0 ? default_seed : seed; }

    /// Mirror another reducer's parameter configuration without copying its
    /// held sample, clock phase, filter history, or RNG position.
    void sync_configuration_from(const SampleRateReducerT& other) {
        if (sample_rate_ != other.sample_rate_)
            set_sample_rate(other.sample_rate_);
        if (hold_rate_ != other.hold_rate_)
            set_hold_rate_hz(other.hold_rate_);
        if (jitter_ != other.jitter_)
            set_jitter(other.jitter_);
        if (smoothing_ != other.smoothing_)
            set_smoothing(other.smoothing_);
        seed_ = other.seed_;
    }

    void reset() {
        phase_ = 1.0;  // latch on the first sample rather than emitting a zero
        held_ = 0.0;
        output_level_ = 0.0;
        smoother_.reset();
        rng_ = seed_;
    }

    bool has_tail() const noexcept {
        return (!hold_bypassed_ && std::fabs(held_) > 1.0e-12) ||
               (!bypass_smoothing_ && output_level_ > 1.0e-8);
    }

    SampleType process(SampleType input) {
        // Latch before advancing. Advancing first would make the very first
        // hold one sample short, because `reset()` parks the phase at the
        // latch point so the first sample is captured rather than emitted as
        // a leftover zero.
        if (phase_ >= 1.0) {
            phase_ -= std::floor(phase_);
            held_ = static_cast<double>(input);
        }
        phase_ += step_ * (1.0 + jitter_ * next_jitter());
        const SampleType output =
            bypass_smoothing_
                ? static_cast<SampleType>(held_)
                : smoother_.process_lowpass(static_cast<SampleType>(held_));
        output_level_ = std::fabs(static_cast<double>(output));
        return output;
    }

private:
    double next_jitter() {
        rng_ ^= rng_ << 13;
        rng_ ^= rng_ >> 17;
        rng_ ^= rng_ << 5;
        return static_cast<double>(rng_) * (1.0 / 2147483648.0) - 1.0;
    }

    void update() {
        const double rate = std::min(hold_rate_, sample_rate_);
        step_ = rate / sample_rate_;
        hold_bypassed_ = rate >= sample_rate_;
        smoother_.prepare(static_cast<SampleType>(sample_rate_));
        bypass_smoothing_ = smoothing_ <= 0.0;
        if (!bypass_smoothing_) {
            // Sweep the corner from the host Nyquist down to the hold rate's
            // Nyquist, so smoothing=1 thins the images the most and smoothing
            // just above zero barely touches the top of the band.
            const double top = 0.5 * sample_rate_;
            const double bottom = 0.5 * rate;
            smoother_.set_cutoff(static_cast<SampleType>(top * std::pow(bottom / top, smoothing_)));
        }
    }

    double sample_rate_ = 44100.0;
    double hold_rate_ = std::numeric_limits<double>::max();
    double jitter_ = 0.0;
    double smoothing_ = 0.0;

    double step_ = 1.0;
    double phase_ = 1.0;
    double held_ = 0.0;
    double output_level_ = 0.0;
    TptFilterT<SampleType> smoother_;
    bool hold_bypassed_ = true;
    bool bypass_smoothing_ = true;

    std::uint32_t seed_ = default_seed;
    std::uint32_t rng_ = default_seed;
};

/// The three lo-fi stages in the order the hardware they model applies them:
/// quantise the word, hold it at a lower clock, then push it through an output
/// stage that cannot represent the smallest codes.
///
/// Keeping them in one class rather than three loose calls is what makes the
/// order a property of the code instead of a convention each caller has to
/// remember. Any stage left at its default is a pass-through, so a voice can
/// use one of the three without paying for the others.
///
/// This chain distorts, so it generates harmonics above the input's bandwidth
/// and will alias if run at the host rate. Callers that care run it inside an
/// oversampled section (`pulp::signal::Oversampling`); callers modelling a
/// device that itself aliased run it at the host rate deliberately.
///
/// RT contract: every member allocates nothing and takes no locks.
template <typename SampleType = float>
class LofiChainT {
public:
    void set_sample_rate(double sr) { reducer_.set_sample_rate(sr); }

    /// Word length in bits. 24 or above bypasses the quantiser.
    void set_bits(double bits) {
        bits_ = std::isfinite(bits) ? std::clamp(bits, 1.0, 24.0) : 24.0;
        quantizer_.set_bits(static_cast<SampleType>(bits_));
    }

    /// Quantizer dither is opt-in so existing chains remain bit-exact.
    void set_dither_mode(DitherMode mode) { quantizer_.set_dither_mode(mode); }

    /// Error feedback is opt-in and may be used with or without dither.
    void set_noise_shaping(NoiseShapingOrder order) {
        quantizer_.set_noise_shaping(order);
    }

    /// Hold clock in Hz. At or above the sample rate the reducer is
    /// transparent.
    void set_hold_rate_hz(double hz) { reducer_.set_hold_rate_hz(hz); }

    void set_jitter(double amount) { reducer_.set_jitter(amount); }

    void set_smoothing(double amount) { reducer_.set_smoothing(amount); }

    /// Fraction of full scale the output stage cannot represent, 0 to 0.9.
    void set_dead_zone(double amount) { dead_zone_ = std::clamp(amount, 0.0, 0.9); }

    void set_seed(std::uint32_t seed) {
        reducer_.set_seed(seed);
        quantizer_.set_seed(seed);
    }

    /// Mirror parameter configuration while preserving this chain's
    /// independent reducer/filter state.
    void sync_configuration_from(const LofiChainT& other) {
        bits_ = other.bits_;
        dead_zone_ = other.dead_zone_;
        reducer_.sync_configuration_from(other.reducer_);
        quantizer_.sync_configuration_from(other.quantizer_);
    }

    void reset() {
        reducer_.reset();
        quantizer_.reset();
    }
    bool has_tail() const noexcept { return reducer_.has_tail(); }

    SampleType process(SampleType input) {
        double x;
        if (quantizer_.dither_mode() == DitherMode::none &&
            quantizer_.noise_shaping() == NoiseShapingOrder::none) {
            // Preserve the original double-precision arithmetic exactly for
            // callers that do not opt into the output-correctness policy.
            x = quantize_bits(static_cast<double>(input), bits_);
        } else {
            x = static_cast<double>(quantizer_.process(input));
        }
        x = static_cast<double>(reducer_.process(static_cast<SampleType>(x)));
        if (dead_zone_ > 0.0) x = dead_zone_saturate(x, dead_zone_);
        return static_cast<SampleType>(x);
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i) buffer[i] = process(buffer[i]);
    }

private:
    SampleRateReducerT<SampleType> reducer_;
    DitherQuantizerT<SampleType> quantizer_;
    double bits_ = 24.0;
    double dead_zone_ = 0.0;
};

using SampleRateReducer = SampleRateReducerT<float>;
using SampleRateReducer64 = SampleRateReducerT<double>;
using LofiChain = LofiChainT<float>;
using LofiChain64 = LofiChainT<double>;

}  // namespace pulp::signal
