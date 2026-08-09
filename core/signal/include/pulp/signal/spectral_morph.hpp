#pragma once

/// @file spectral_morph.hpp
/// Real-time two-input morphing for live complex spectral frames.
///
/// `SpectralMorphT` operates directly on coherent one-sided frames from two
/// `SpectralFrameEngineT` instances. It owns no FFT, captures no audio, and
/// allocates no memory. Magnitude and phase have independent interpolation
/// amounts and policies. Phase interpolation is wrap-safe across the -pi/pi
/// seam; it never linearly interpolates raw angle values.

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <limits>
#include <type_traits>

namespace pulp::signal {

enum class SpectralMagnitudeInterpolation {
    linear,
    equal_power,
};

enum class SpectralPhaseInterpolation {
    shortest_arc,
    normalized_vector,
};

template <typename SampleType = float>
class SpectralMorphT {
public:
    static_assert(std::is_floating_point_v<SampleType>);

    static constexpr int maximum_channels = 64;
    static constexpr int minimum_bins = 129;
    static constexpr int maximum_bins = 8193;

    struct Config {
        SpectralMagnitudeInterpolation magnitude =
            SpectralMagnitudeInterpolation::linear;
        SpectralPhaseInterpolation phase = SpectralPhaseInterpolation::shortest_arc;
    };

    static bool supports_configuration(int channels, int num_bins) noexcept {
        if (channels <= 0 || channels > maximum_channels
            || num_bins < minimum_bins || num_bins > maximum_bins)
            return false;
        const int fft_size = 2 * (num_bins - 1);
        return (fft_size & (fft_size - 1)) == 0;
    }

    /// Fix the coherent frame geometry. This stores two integers and allocates
    /// nothing; invalid geometry leaves a prepared instance unchanged.
    bool prepare(int channels, int num_bins) noexcept {
        if (!supports_configuration(channels, num_bins)) return false;
        channels_ = channels;
        num_bins_ = num_bins;
        prepared_ = true;
        return true;
    }

    /// Stateless lifecycle symmetry for frame-processing chains. Configuration
    /// and prepared geometry are preserved.
    void reset() noexcept {}

    bool set_config(Config config) noexcept {
        if (!valid(config.magnitude) || !valid(config.phase)) return false;
        config_ = config;
        return true;
    }

    Config config() const noexcept { return config_; }
    bool prepared() const noexcept { return prepared_; }
    int channels() const noexcept { return channels_; }
    int num_bins() const noexcept { return num_bins_; }

    /// Morph an entire coherent frame group. Magnitude and phase amounts are
    /// finite normalized values and are independently clamped to [0,1]. Output
    /// may exactly alias either endpoint. A non-finite endpoint bin falls back
    /// to the other finite endpoint; two non-finite endpoints produce silence.
    bool process(const std::complex<SampleType>* const* a,
                 const std::complex<SampleType>* const* b,
                 std::complex<SampleType>* const* out,
                 int channels, int num_bins,
                 SampleType magnitude_amount,
                 SampleType phase_amount) const noexcept {
        if (channels != channels_ || num_bins != num_bins_) return false;
        return process_partition(a, b, out, channels, 0, num_bins,
                                 magnitude_amount, phase_amount);
    }

    /// Morph `[first_bin, first_bin + bin_count)`. Disjoint partitions produce
    /// the exact same result as `process()`, enabling caller-owned parallelism.
    bool process_partition(const std::complex<SampleType>* const* a,
                           const std::complex<SampleType>* const* b,
                           std::complex<SampleType>* const* out,
                           int channels, int first_bin, int bin_count,
                           SampleType magnitude_amount,
                           SampleType phase_amount) const noexcept {
        if (!prepared_ || channels != channels_ || first_bin < 0 || bin_count < 0
            || first_bin > num_bins_ || bin_count > num_bins_ - first_bin
            || !std::isfinite(magnitude_amount) || !std::isfinite(phase_amount)
            || a == nullptr || b == nullptr || out == nullptr)
            return false;
        for (int ch = 0; ch < channels_; ++ch)
            if (a[ch] == nullptr || b[ch] == nullptr || out[ch] == nullptr)
                return false;

        const SampleType magnitude_mix =
            std::clamp(magnitude_amount, SampleType{0}, SampleType{1});
        const SampleType phase_mix =
            std::clamp(phase_amount, SampleType{0}, SampleType{1});

        for (int ch = 0; ch < channels_; ++ch) {
            for (int bin = first_bin; bin < first_bin + bin_count; ++bin) {
                const bool self_conjugate = bin == 0 || bin == num_bins_ - 1;
                const auto input_a = a[ch][bin];
                const auto input_b = b[ch][bin];
                DecodedBin da = decode(input_a);
                DecodedBin db = decode(input_b);
                if (!da.finite && !db.finite) {
                    out[ch][bin] = {};
                    continue;
                }
                if (magnitude_mix == SampleType{0} && phase_mix == SampleType{0}) {
                    const auto selected = da.finite ? input_a : input_b;
                    const auto& selected_decoded = da.finite ? da : db;
                    if (!selected_decoded.saturated
                        && (!self_conjugate || selected.imag() == SampleType{0})) {
                        out[ch][bin] = selected;
                        continue;
                    }
                }
                if (magnitude_mix == SampleType{1} && phase_mix == SampleType{1}) {
                    const auto selected = db.finite ? input_b : input_a;
                    const auto& selected_decoded = db.finite ? db : da;
                    if (!selected_decoded.saturated
                        && (!self_conjugate || selected.imag() == SampleType{0})) {
                        out[ch][bin] = selected;
                        continue;
                    }
                }
                if (!da.finite) da = db;
                if (!db.finite) db = da;

                // Zero has no phase. Borrow the non-zero endpoint's phase so a
                // magnitude fade never rotates through an arbitrary angle.
                if (da.magnitude == SampleType{0} && db.magnitude > SampleType{0})
                    da.phase = db.phase;
                if (db.magnitude == SampleType{0} && da.magnitude > SampleType{0})
                    db.phase = da.phase;

                const SampleType magnitude = interpolate_magnitude(
                    da.magnitude, db.magnitude, magnitude_mix, config_.magnitude);
                if (magnitude == SampleType{0}) {
                    out[ch][bin] = {};
                    continue;
                }
                if (self_conjugate) {
                    // DC and Nyquist are their own conjugates and therefore
                    // cannot carry an imaginary phase. Opposite signs have no
                    // continuous real-only path at constant magnitude, so the
                    // phase amount selects A below 0.5 and B at/above 0.5.
                    const SampleType selected_phase =
                        phase_mix < SampleType{0.5} ? da.phase : db.phase;
                    const SampleType sign = std::cos(selected_phase) < SampleType{0}
                        ? SampleType{-1}
                        : SampleType{1};
                    out[ch][bin] = {sign * magnitude, SampleType{0}};
                    continue;
                }
                const SampleType phase = interpolate_phase(
                    da.phase, db.phase, phase_mix, config_.phase);
                out[ch][bin] = std::polar(magnitude, phase);
            }
        }
        return true;
    }

private:
    struct DecodedBin {
        SampleType magnitude = SampleType{0};
        SampleType phase = SampleType{0};
        bool finite = false;
        bool saturated = false;
    };

    static constexpr SampleType pi() noexcept {
        return static_cast<SampleType>(3.14159265358979323846264338327950288L);
    }

    static bool valid(SpectralMagnitudeInterpolation policy) noexcept {
        return policy == SpectralMagnitudeInterpolation::linear
            || policy == SpectralMagnitudeInterpolation::equal_power;
    }

    static bool valid(SpectralPhaseInterpolation policy) noexcept {
        return policy == SpectralPhaseInterpolation::shortest_arc
            || policy == SpectralPhaseInterpolation::normalized_vector;
    }

    static DecodedBin decode(std::complex<SampleType> value) noexcept {
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag())) return {};
        const SampleType real = value.real();
        const SampleType imag = value.imag();
        const SampleType scale = std::max(std::abs(real), std::abs(imag));
        if (scale == SampleType{0})
            return {SampleType{0}, SampleType{0}, true, false};

        const SampleType scaled_magnitude = std::hypot(real / scale, imag / scale);
        const SampleType maximum = std::numeric_limits<SampleType>::max();
        const bool saturated = scale > maximum / scaled_magnitude;
        const SampleType magnitude = saturated
            ? maximum
            : scale * scaled_magnitude;
        return {magnitude, std::atan2(imag, real), true, saturated};
    }

    static SampleType interpolate_magnitude(
        SampleType a, SampleType b, SampleType amount,
        SpectralMagnitudeInterpolation policy) noexcept {
        if (amount == SampleType{0}) return a;
        if (amount == SampleType{1}) return b;
        if (policy == SpectralMagnitudeInterpolation::linear)
            return std::fma(amount, b - a, a);

        const SampleType result = std::hypot(
            std::sqrt(SampleType{1} - amount) * a,
            std::sqrt(amount) * b);
        return std::isfinite(result)
            ? result
            : std::numeric_limits<SampleType>::max();
    }

    static SampleType shortest_phase(SampleType a, SampleType b,
                                     SampleType amount) noexcept {
        if (amount == SampleType{0}) return a;
        if (amount == SampleType{1}) return b;
        const SampleType delta = std::remainder(b - a, SampleType{2} * pi());
        return std::fma(amount, delta, a);
    }

    static SampleType interpolate_phase(
        SampleType a, SampleType b, SampleType amount,
        SpectralPhaseInterpolation policy) noexcept {
        if (policy == SpectralPhaseInterpolation::shortest_arc)
            return shortest_phase(a, b, amount);
        if (amount == SampleType{0}) return a;
        if (amount == SampleType{1}) return b;

        const SampleType real = std::fma(amount, std::cos(b) - std::cos(a),
                                         std::cos(a));
        const SampleType imag = std::fma(amount, std::sin(b) - std::sin(a),
                                         std::sin(a));
        if (std::hypot(real, imag) <= std::numeric_limits<SampleType>::epsilon())
            return shortest_phase(a, b, amount);
        return std::atan2(imag, real);
    }

    Config config_;
    int channels_ = 0;
    int num_bins_ = 0;
    bool prepared_ = false;
};

using SpectralMorph = SpectralMorphT<float>;
using SpectralMorph64 = SpectralMorphT<double>;

} // namespace pulp::signal
