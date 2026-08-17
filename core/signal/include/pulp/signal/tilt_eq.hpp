#pragma once

/// @file tilt_eq.hpp
/// Fixed-state, pivot-normalized spectral tilt equalizer.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/frequency_response.hpp>
#include <pulp/signal/sos_cascade.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace pulp::signal {

/// A reusable spectral slope built from a fixed cascade of high shelves.
///
/// `tilt_db_per_octave` is the signed magnitude-response slope across the
/// design band `[31.25 Hz, min(16 kHz, 0.4 * sample_rate)]`. Positive values
/// brighten and negative values darken. `pivot_hz` selects the frequency whose
/// gain is normalized to exactly unity; changing the pivot changes only that
/// normalization, not the slope. Zero tilt bypasses the cascade so every
/// finite input sample is returned bit-for-bit.
///
/// Configuration is a bounded control-side operation. A request is validated
/// and fully designed before any coefficient or recursive state changes; a
/// rejected request leaves the object unchanged. An accepted non-no-op retune
/// clears every channel's recursive history. Process, block, reset, response,
/// and configuration paths allocate no memory and lock nothing.
template <typename SampleType = float, std::size_t Channels = 2> class TiltEqT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "TiltEqT requires a floating-point sample type");
    static_assert(Channels > 0, "TiltEqT requires at least one channel");

  public:
    static constexpr std::size_t channel_count = Channels;
    static constexpr std::size_t shelf_count = 9;
    static constexpr double min_sample_rate = 8000.0;
    static constexpr double max_sample_rate = 384000.0;
    static constexpr double min_tilt_db_per_octave = -6.0;
    static constexpr double max_tilt_db_per_octave = 6.0;
    static constexpr double min_pivot_hz = 31.25;
    static constexpr double max_design_hz = 16000.0;
    static constexpr double default_sample_rate = 48000.0;

    using Coefficients = BiquadCoefficientsT<SampleType>;

    struct Config {
        double pivot_hz = 1000.0;
        double tilt_db_per_octave = 0.0;
    };

    TiltEqT() noexcept {
        for (auto& cascade : cascades_)
            static_cast<void>(cascade.prepare(shelf_count));
        std::array<Coefficients, shelf_count> designed{};
        double normalization = 1.0;
        static_cast<void>(design(config_, sample_rate_, designed, normalization));
        commit(designed, normalization);
    }

    /// Change sample rate and clear history. Invalid rates reject atomically.
    bool prepare(double sample_rate) noexcept {
        if (!valid_sample_rate(sample_rate))
            return false;
        std::array<Coefficients, shelf_count> designed{};
        double normalization = 1.0;
        if (!design(config_, sample_rate, designed, normalization))
            return false;
        sample_rate_ = sample_rate;
        commit(designed, normalization);
        return true;
    }

    /// Apply one coherent pivot/slope request at a block boundary.
    /// Non-finite or out-of-domain fields reject the whole request.
    bool set_config(Config requested) noexcept {
        if (same_config(requested, config_))
            return true;
        std::array<Coefficients, shelf_count> designed{};
        double normalization = 1.0;
        if (!design(requested, sample_rate_, designed, normalization))
            return false;
        config_ = requested;
        commit(designed, normalization);
        return true;
    }

    Config config() const noexcept {
        return config_;
    }
    double sample_rate() const noexcept {
        return sample_rate_;
    }
    double design_high_hz() const noexcept {
        return std::min(max_design_hz, 0.4 * sample_rate_);
    }
    double normalization_gain() const noexcept {
        return normalization_gain_;
    }
    std::uint64_t fault_count() const noexcept {
        return fault_count_;
    }

    Coefficients coefficients(std::size_t index) const noexcept {
        return index < shelf_count ? coefficients_[index] : Coefficients{};
    }

    /// Process one channel. Invalid channels fail transparent. A non-finite
    /// sample or result clears that channel, increments the fault count, and
    /// emits zero so recursive state cannot remain poisoned.
    SampleType process(SampleType input, std::size_t channel = 0) noexcept {
        if (channel >= Channels)
            return input;
        if (!std::isfinite(static_cast<double>(input)))
            return recover(channel);
        if (config_.tilt_db_per_octave == 0.0)
            return input;
        const SampleType output =
            cascades_[channel].process(input) * static_cast<SampleType>(normalization_gain_);
        if (!std::isfinite(static_cast<double>(output)))
            return recover(channel);
        return output;
    }

    /// Process a caller-owned channel in place. Invalid arguments reject
    /// without changing state; a null pointer is valid only for zero frames.
    bool process_block(SampleType* samples, std::size_t frames, std::size_t channel = 0) noexcept {
        if (channel >= Channels || (samples == nullptr && frames != 0))
            return false;
        for (std::size_t frame = 0; frame < frames; ++frame)
            samples[frame] = process(samples[frame], channel);
        return true;
    }

    /// Process one channel into separate caller-owned storage. Exact in-place
    /// aliasing is supported; other overlapping ranges are not.
    bool process_block(const SampleType* input, SampleType* output, std::size_t frames,
                       std::size_t channel = 0) noexcept {
        if (channel >= Channels || ((input == nullptr || output == nullptr) && frames != 0))
            return false;
        for (std::size_t frame = 0; frame < frames; ++frame)
            output[frame] = process(input[frame], channel);
        return true;
    }

    /// Clear all recursive history and the fault diagnostic without changing
    /// sample rate or configuration.
    void reset() noexcept {
        reset_history();
        fault_count_ = 0;
    }

    /// Clear one channel's recursive history. Invalid channels are rejected.
    bool reset(std::size_t channel) noexcept {
        if (channel >= Channels)
            return false;
        cascades_[channel].reset();
        return true;
    }

    /// Magnitude response of the active coefficient endpoint.
    double magnitude(double frequency_hz) const noexcept {
        if (config_.tilt_db_per_octave == 0.0)
            return 1.0;
        if (!std::isfinite(frequency_hz))
            frequency_hz = 0.0;
        frequency_hz = std::clamp(frequency_hz, 0.0, sample_rate_ * 0.5);
        const double omega = 2.0 * std::acos(-1.0) * frequency_hz / sample_rate_;
        double result = normalization_gain_;
        for (const auto& section : coefficients_)
            result *= section_magnitude(section, omega);
        return std::isfinite(result) ? result : 0.0;
    }

    double magnitude_db(double frequency_hz) const noexcept {
        const double linear = magnitude(frequency_hz);
        if (!(linear > 0.0))
            return -200.0;
        const double db = 20.0 * std::log10(linear);
        return std::isfinite(db) ? std::max(db, -200.0) : -200.0;
    }

    static constexpr int latency_samples() noexcept {
        return 0;
    }

    /// Recursive shelves decay asymptotically when active. `-1` means the
    /// active configuration has no finite advertised tail.
    int tail_samples() const noexcept {
        return config_.tilt_db_per_octave == 0.0 ? 0 : -1;
    }

  private:
    using Cascade = SosCascadeT<SampleType, shelf_count>;

    static bool valid_sample_rate(double sample_rate) noexcept {
        return std::isfinite(sample_rate) && sample_rate >= min_sample_rate &&
               sample_rate <= max_sample_rate;
    }

    static bool same_config(const Config& lhs, const Config& rhs) noexcept {
        return lhs.pivot_hz == rhs.pivot_hz && lhs.tilt_db_per_octave == rhs.tilt_db_per_octave;
    }

    static bool finite(const Coefficients& c) noexcept {
        return std::isfinite(static_cast<double>(c.b0)) &&
               std::isfinite(static_cast<double>(c.b1)) &&
               std::isfinite(static_cast<double>(c.b2)) &&
               std::isfinite(static_cast<double>(c.a1)) && std::isfinite(static_cast<double>(c.a2));
    }

    static bool design_sections(double stage_gain_db, double sample_rate, double high_hz,
                                std::array<Coefficients, shelf_count>& designed) noexcept {
        const double ratio = high_hz / min_pivot_hz;
        for (std::size_t index = 0; index < shelf_count; ++index) {
            const double position =
                (static_cast<double>(index) + 0.5) / static_cast<double>(shelf_count);
            const double center_hz = min_pivot_hz * std::pow(ratio, position);
            BiquadT<SampleType> designer;
            designer.set_coefficients(
                BiquadT<SampleType>::Type::high_shelf, static_cast<SampleType>(center_hz),
                static_cast<SampleType>(0.7071067811865476), static_cast<SampleType>(sample_rate),
                static_cast<SampleType>(stage_gain_db));
            designed[index] = designer.coefficients();
            if (!finite(designed[index]) || !biquad_is_stable(designed[index]))
                return false;
        }
        return true;
    }

    static double endpoint_delta_db(const std::array<Coefficients, shelf_count>& designed,
                                    double sample_rate, double high_hz) noexcept {
        const double scale = 2.0 * std::acos(-1.0) / sample_rate;
        double low_magnitude = 1.0;
        double high_magnitude = 1.0;
        for (const auto& section : designed) {
            low_magnitude *= section_magnitude(section, scale * min_pivot_hz);
            high_magnitude *= section_magnitude(section, scale * high_hz);
        }
        return 20.0 * std::log10(high_magnitude / low_magnitude);
    }

    static bool design(const Config& requested, double sample_rate,
                       std::array<Coefficients, shelf_count>& designed,
                       double& normalization) noexcept {
        const double high_hz = std::min(max_design_hz, 0.4 * sample_rate);
        if (!valid_sample_rate(sample_rate) || !std::isfinite(requested.pivot_hz) ||
            !std::isfinite(requested.tilt_db_per_octave) || requested.pivot_hz < min_pivot_hz ||
            requested.pivot_hz > high_hz || requested.tilt_db_per_octave < min_tilt_db_per_octave ||
            requested.tilt_db_per_octave > max_tilt_db_per_octave)
            return false;

        const double octaves = std::log2(high_hz / min_pivot_hz);
        const double target_delta_db = requested.tilt_db_per_octave * octaves;
        if (target_delta_db == 0.0) {
            if (!design_sections(0.0, sample_rate, high_hz, designed))
                return false;
        } else {
            const double direction = target_delta_db > 0.0 ? 1.0 : -1.0;
            const double target_magnitude = std::abs(target_delta_db);
            double low_gain = 0.0;
            double high_gain = target_magnitude * 2.0 / static_cast<double>(shelf_count) + 0.5;
            if (!design_sections(direction * high_gain, sample_rate, high_hz, designed) ||
                std::abs(endpoint_delta_db(designed, sample_rate, high_hz)) < target_magnitude)
                return false;
            for (int iteration = 0; iteration < 48; ++iteration) {
                const double candidate_gain = (low_gain + high_gain) * 0.5;
                if (!design_sections(direction * candidate_gain, sample_rate, high_hz, designed))
                    return false;
                if (std::abs(endpoint_delta_db(designed, sample_rate, high_hz)) < target_magnitude)
                    low_gain = candidate_gain;
                else
                    high_gain = candidate_gain;
            }
            if (!design_sections(direction * (low_gain + high_gain) * 0.5, sample_rate, high_hz,
                                 designed))
                return false;
        }

        const double pivot_omega = 2.0 * std::acos(-1.0) * requested.pivot_hz / sample_rate;
        double pivot_magnitude = 1.0;
        for (const auto& section : designed)
            pivot_magnitude *= section_magnitude(section, pivot_omega);
        if (!(std::isfinite(pivot_magnitude) && pivot_magnitude > 0.0))
            return false;
        normalization = 1.0 / pivot_magnitude;
        return std::isfinite(normalization) && normalization > 0.0;
    }

    void commit(const std::array<Coefficients, shelf_count>& designed,
                double normalization) noexcept {
        coefficients_ = designed;
        normalization_gain_ = normalization;
        for (auto& cascade : cascades_)
            static_cast<void>(
                cascade.set_coefficients(std::span<const Coefficients, shelf_count>(coefficients_),
                                         SosCascadeTransition::reset_state));
    }

    void reset_history() noexcept {
        for (auto& cascade : cascades_)
            cascade.reset();
    }

    SampleType recover(std::size_t channel) noexcept {
        cascades_[channel].reset();
        ++fault_count_;
        return SampleType{0};
    }

    std::array<Cascade, Channels> cascades_{};
    std::array<Coefficients, shelf_count> coefficients_{};
    Config config_{};
    double sample_rate_ = default_sample_rate;
    double normalization_gain_ = 1.0;
    std::uint64_t fault_count_ = 0;
};

using TiltEq = TiltEqT<float>;
using TiltEq64 = TiltEqT<double>;

} // namespace pulp::signal
