#pragma once

#include <pulp/signal/denormal.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace pulp::signal {

/// Spectral tilt of a NoiseSourceT, named by the usual colour convention.
/// The number after each name is the power-spectral-density slope the colour
/// is defined by; the implementation is validated against those slopes rather
/// than against a particular filter topology.
enum class NoiseColor {
    white, ///<   0 dB/octave — the raw generator output.
    pink,  ///<  -3 dB/octave — equal power per octave.
    brown, ///<  -6 dB/octave — integrated white ("red" noise).
    blue,  ///<  +3 dB/octave — the mirror of pink.
    violet ///<  +6 dB/octave — differentiated white ("purple" noise).
};

/// A deterministic, colourable noise generator.
///
/// Two properties matter for percussion and any other one-shot voice:
///
/// 1. **Determinism.** `reset()` rewinds the generator to its seed, so the same
///    (seed, colour, sample rate) always produces the same sample sequence. A
///    drum voice that reseeds on every hit therefore renders bit-identically
///    for a given parameter set, which is what makes golden-file tests and
///    offline bounces reproducible. Voices that want per-hit variation advance
///    the seed deliberately (`set_seed`) rather than relying on a free-running
///    generator.
/// 2. **Sample-rate independence.** The colour filters are specified in Hz, not
///    in per-sample coefficients, so the measured spectral slope is the same at
///    44.1 kHz and at 192 kHz. Output level is equalised across colours by a
///    normalisation constant measured during `prepare()`, so switching colour
///    does not jump the level.
///
/// The generator is a 32-bit xorshift, chosen because its state is one word:
/// a voice can snapshot and restore it, and a whole drum kit's worth of
/// generators costs nothing.
///
/// RT contract: `prepare()` and `set_color()` measure the normalisation
/// constant by rendering a fixed-length burst. They allocate nothing, but they
/// are bounded work rather than constant work, so call them from `prepare()`
/// or a parameter-change path, not per sample. `process()`, `white()`,
/// `reset()`, and `set_seed()` allocate nothing and take no locks.
template <typename SampleType = float> class NoiseSourceT {
  public:
    /// Default seed. Any non-zero value works; xorshift is degenerate at zero,
    /// so `set_seed(0)` is remapped to this value.
    static constexpr std::uint32_t default_seed = 0x1D872B41u;

    /// A default-constructed source is immediately usable at 44.1 kHz, so a
    /// voice that forgets to call `prepare()` produces noise rather than
    /// silence and the mistake is audible instead of subtle.
    NoiseSourceT() {
        prepare(sample_rate_);
    }

    /// Fixes the sample rate and re-measures the per-colour normalisation.
    /// Leaves the generator rewound to its seed.
    void prepare(double sample_rate) {
        sample_rate_ = sample_rate > 0.0 ? sample_rate : sample_rate_;
        update_coefficients();
        const NoiseColor selected = color_;
        for (std::size_t index = 0; index < normalisations_.size(); ++index) {
            color_ = static_cast<NoiseColor>(index);
            measure_normalisation();
            normalisations_[index] = norm_;
        }
        color_ = selected;
        norm_ = normalisations_[static_cast<std::size_t>(color_)];
        reset();
    }

    /// Selects the spectral tilt. `prepare()` measures every colour at the
    /// current sample rate, so this RT setter only selects the cached gain and
    /// rewinds the colour-filter state.
    void set_color(NoiseColor c) {
        const auto index = std::min(static_cast<std::size_t>(c), normalisations_.size() - 1);
        color_ = static_cast<NoiseColor>(index);
        norm_ = normalisations_[index];
        reset_filters();
    }

    NoiseColor color() const {
        return color_;
    }

    /// Sets the seed used by `reset()`. Does not itself rewind — call `reset()`
    /// (or `note_on`-equivalent) to take the new seed into use.
    void set_seed(std::uint32_t seed) {
        seed_ = seed == 0 ? default_seed : seed;
    }

    std::uint32_t seed() const {
        return seed_;
    }

    /// Rewinds the generator to its seed and clears the colour filter state.
    void reset() {
        state_ = seed_;
        reset_filters();
    }

    /// Next sample, normalised so every colour has approximately the same RMS.
    SampleType process() {
        return static_cast<SampleType>(norm_ * colorise(white_next()));
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process();
    }

    /// Raw uniform white sample in [-1, 1), bypassing the colour filter and the
    /// normalisation. Voices that want an unfiltered burst (a click, a modal
    /// exciter) use this so they do not pay for a filter they will replace.
    SampleType white() {
        return static_cast<SampleType>(white_next());
    }

  private:
    // These poles are deliberately open-coded rather than built from
    // TptFilterT, which is the repository's shared one-pole. They are not
    // standalone filters: their weights are derived from the same
    // impulse-invariant coefficient the recurrence uses, and the whole bank is
    // normalised as one. TptFilterT pre-warps its corner through a tangent,
    // which would move the top pole enough to tilt the slope the bank exists
    // to produce. Anywhere a one-pole stands alone, use TptFilterT.
    //
    // Pink is approximated by summing one-pole lowpasses whose corners are
    // spaced by a constant ratio, each weighted by 1/sqrt(fc). Between two
    // adjacent corners the sum falls by exactly the corner ratio over exactly
    // that span of frequency, which is a 1/f power spectrum; the residual
    // ripple shrinks as the corners are packed closer. Five corners a factor of
    // eight apart cover roughly 8 Hz to 30 kHz with the ripple small enough to
    // stay inside a half-dB-per-octave slope tolerance.
    static constexpr std::size_t kPinkPoles = 5;
    static constexpr double kPinkBaseHz = 8.0;
    static constexpr double kPinkSpacing = 8.0;

    // Brown is a single one-pole well below the audio band, so its -6 dB/octave
    // region starts under the lowest frequency anyone measures.
    static constexpr double kBrownHz = 12.0;

    // Target RMS for every colour, matching a uniform white source in [-1, 1).
    static constexpr double kTargetRms = 0.57735026918962576; // 1/sqrt(3)

    double white_next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        // [0, 2^32) -> [-1, 1). The divisor is 2^31 so the sign is symmetric.
        return static_cast<double>(state_) * (1.0 / 2147483648.0) - 1.0;
    }

    double colorise(double w) {
        switch (color_) {
        case NoiseColor::white:
            return w;
        case NoiseColor::pink:
            return pink(w);
        case NoiseColor::brown:
            return brown(w);
        case NoiseColor::blue:
            return differentiate(pink(w));
        case NoiseColor::violet:
            return differentiate(w);
        }
        return w;
    }

    double pink(double w) {
        double sum = 0.0;
        for (std::size_t k = 0; k < kPinkPoles; ++k) {
            pink_state_[k] = snap_to_zero(pink_state_[k] + pink_a_[k] * (w - pink_state_[k]));
            sum += pink_w_[k] * pink_state_[k];
        }
        return sum;
    }

    double brown(double w) {
        brown_state_ = snap_to_zero(brown_state_ + brown_a_ * (w - brown_state_));
        return brown_state_;
    }

    double differentiate(double x) {
        const double y = x - diff_state_;
        diff_state_ = x;
        return y;
    }

    void reset_filters() {
        pink_state_.fill(0.0);
        brown_state_ = 0.0;
        diff_state_ = 0.0;
    }

    void update_coefficients() {
        double weight_sum = 0.0;
        for (std::size_t k = 0; k < kPinkPoles; ++k) {
            const double fc = kPinkBaseHz * std::pow(kPinkSpacing, static_cast<double>(k));
            // Keep every corner inside the band the sample rate can represent;
            // a corner at or above Nyquist would alias its own coefficient.
            const double clamped = std::min(fc, 0.45 * sample_rate_);
            pink_a_[k] = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * clamped / sample_rate_);
            pink_w_[k] = 1.0 / std::sqrt(clamped);
            weight_sum += pink_w_[k];
        }
        // Scale the weights to O(1) so the measured normalisation constant does
        // not have to absorb an arbitrary magnitude.
        for (std::size_t k = 0; k < kPinkPoles; ++k)
            pink_w_[k] /= weight_sum;

        brown_a_ = 1.0 - std::exp(-2.0 * 3.14159265358979323846 * kBrownHz / sample_rate_);
    }

    // Renders a fixed-length burst through a scratch copy of the current colour
    // and solves for the gain that lands its RMS on `kTargetRms`. Measuring
    // beats hand-tuning a table of per-colour constants: the same code stays
    // correct when the sample rate, the pole layout, or the colour set changes.
    void measure_normalisation() {
        // White needs no measurement -- the generator's own range is the
        // target -- and it is the default, so the common path costs nothing.
        if (color_ == NoiseColor::white) {
            norm_ = 1.0;
            return;
        }

        constexpr int kBurst = 16384;
        // Discard a lead-in so the slowest one-pole is past its step response
        // before any sample is counted.
        constexpr int kWarmup = 4096;

        const std::uint32_t saved_state = state_;
        const auto saved_pink = pink_state_;
        const double saved_brown = brown_state_;
        const double saved_diff = diff_state_;

        state_ = seed_;
        reset_filters();
        for (int i = 0; i < kWarmup; ++i)
            colorise(white_next());

        double sum_sq = 0.0;
        for (int i = 0; i < kBurst; ++i) {
            const double y = colorise(white_next());
            sum_sq += y * y;
        }
        const double rms = std::sqrt(sum_sq / kBurst);
        norm_ = rms > 1e-12 ? kTargetRms / rms : 1.0;

        state_ = saved_state;
        pink_state_ = saved_pink;
        brown_state_ = saved_brown;
        diff_state_ = saved_diff;
    }

    double sample_rate_ = 44100.0;
    NoiseColor color_ = NoiseColor::white;
    std::uint32_t seed_ = default_seed;
    std::uint32_t state_ = default_seed;
    double norm_ = 1.0;
    std::array<double, 5> normalisations_{1.0, 1.0, 1.0, 1.0, 1.0};

    std::array<double, kPinkPoles> pink_a_{};
    std::array<double, kPinkPoles> pink_w_{};
    std::array<double, kPinkPoles> pink_state_{};
    double brown_a_ = 0.0;
    double brown_state_ = 0.0;
    double diff_state_ = 0.0;
};

using NoiseSource = NoiseSourceT<float>;
using NoiseSource64 = NoiseSourceT<double>;

} // namespace pulp::signal
