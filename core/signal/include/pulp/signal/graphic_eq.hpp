#pragma once

/// @file graphic_eq.hpp
/// Fixed-capacity, configurable graphic equalizer for one audio channel.

#include <pulp/signal/frequency_response.hpp>
#include <pulp/signal/sos_cascade.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class GraphicEqPrepareStatus {
    prepared,
    invalid_sample_rate,
    invalid_capacity,
};

enum class GraphicEqConfigureStatus {
    configured,
    not_prepared,
    over_capacity,
    invalid_transition,
    non_finite,
    frequency_out_of_range,
    frequencies_not_strictly_increasing,
    gain_out_of_range,
    q_out_of_range,
    unstable,
    transition_in_progress,
};

template <typename SampleType> struct GraphicEqBandT {
    SampleType frequency_hz = SampleType{1000};
    SampleType gain_db = SampleType{0};
    SampleType q = SampleType{1};

    friend constexpr bool operator==(const GraphicEqBandT&, const GraphicEqBandT&) = default;
};

using GraphicEqBand = GraphicEqBandT<float>;

/// A bounded cascade of configurable peaking bands for one audio channel.
///
/// This is the reusable variable-band container. `SixBandEqT` remains the
/// fixed-role low-shelf/four-peak/high-shelf compatibility surface, and this
/// type deliberately does not perform constant-Q analysis or choose band
/// frequencies. Callers provide a strictly increasing band layout.
///
/// `configure()` validates and designs the complete candidate before changing
/// live state. An invalid request leaves coefficients, recursive history, and
/// transition progress unchanged. A non-zero transition crossfades two stable
/// cascades; recursive coefficients are never interpolated. A second request
/// during that bounded transition is rejected so the audible endpoint is
/// deterministic.
///
/// RT contract: all storage is fixed. `prepare()`, `configure()`, `process()`,
/// `process_block()`, response inspection, and `reset()` allocate no memory.
/// Design and response inspection use transcendental functions, so callers
/// should keep those operations off the audio callback even though they are
/// allocation-free. The object is not a concurrent publication primitive:
/// call `configure()` while stopped or from the processing thread at a block
/// boundary. Instantiate one object per independently processed channel.
template <typename SampleType = float, std::size_t MaxBands = 31> class GraphicEqT {
    static_assert(std::is_floating_point_v<SampleType>,
                  "GraphicEqT requires a floating-point sample type");
    static_assert(MaxBands > 0, "GraphicEqT requires non-zero band storage");

  public:
    using Band = GraphicEqBandT<SampleType>;
    using Coefficients = BiquadCoefficientsT<SampleType>;

    static constexpr SampleType minimum_sample_rate_hz = SampleType{8000};
    static constexpr SampleType maximum_sample_rate_hz = SampleType{384000};
    static constexpr SampleType minimum_frequency_hz = SampleType{20};
    static constexpr SampleType maximum_frequency_hz = SampleType{20000};
    static constexpr SampleType minimum_gain_db = SampleType{-24};
    static constexpr SampleType maximum_gain_db = SampleType{24};
    static constexpr SampleType minimum_q = SampleType{0.1};
    static constexpr SampleType maximum_q = SampleType{12};

    GraphicEqPrepareStatus prepare(SampleType sample_rate,
                                   std::size_t band_capacity = MaxBands) noexcept {
        if (!(std::isfinite(sample_rate) && sample_rate >= minimum_sample_rate_hz &&
              sample_rate <= maximum_sample_rate_hz))
            return GraphicEqPrepareStatus::invalid_sample_rate;
        if (band_capacity == 0 || band_capacity > MaxBands)
            return GraphicEqPrepareStatus::invalid_capacity;

        // SosCascadeT uses fixed storage, so both calls are infallible after
        // the bounds above. Keep the mutation after validation nevertheless.
        (void)cascades_[0].prepare(band_capacity);
        (void)cascades_[1].prepare(band_capacity);
        sample_rate_ = sample_rate;
        capacity_ = band_capacity;
        active_index_ = 0;
        active_bands_ = {};
        requested_bands_ = {};
        requested_coefficients_ = {};
        active_count_ = 0;
        requested_count_ = 0;
        transition_total_ = 0;
        transition_remaining_ = 0;
        fault_count_ = 0;
        healthy_ = true;
        return GraphicEqPrepareStatus::prepared;
    }

    GraphicEqConfigureStatus configure(std::span<const Band> bands,
                                       std::size_t transition_samples = 0) noexcept {
        if (!prepared())
            return GraphicEqConfigureStatus::not_prepared;
        if (transitioning())
            return GraphicEqConfigureStatus::transition_in_progress;
        if (bands.size() > capacity_)
            return GraphicEqConfigureStatus::over_capacity;
        if (transition_samples == std::numeric_limits<std::size_t>::max())
            return GraphicEqConfigureStatus::invalid_transition;

        std::array<Band, MaxBands> candidate_bands{};
        std::array<Coefficients, MaxBands> candidate_coefficients{};
        SampleType previous_frequency = SampleType{0};
        const SampleType frequency_ceiling = supported_frequency_ceiling_hz();

        for (std::size_t index = 0; index < bands.size(); ++index) {
            const Band band = bands[index];
            if (!(std::isfinite(band.frequency_hz) && std::isfinite(band.gain_db) &&
                  std::isfinite(band.q)))
                return GraphicEqConfigureStatus::non_finite;
            if (band.frequency_hz < minimum_frequency_hz || band.frequency_hz > frequency_ceiling)
                return GraphicEqConfigureStatus::frequency_out_of_range;
            if (index != 0 && !(band.frequency_hz > previous_frequency))
                return GraphicEqConfigureStatus::frequencies_not_strictly_increasing;
            if (band.gain_db < minimum_gain_db || band.gain_db > maximum_gain_db)
                return GraphicEqConfigureStatus::gain_out_of_range;
            if (band.q < minimum_q || band.q > maximum_q)
                return GraphicEqConfigureStatus::q_out_of_range;

            candidate_bands[index] = band;
            candidate_coefficients[index] = design_band(band);
            if (!coefficients_are_finite(candidate_coefficients[index]))
                return GraphicEqConfigureStatus::non_finite;
            if (!biquad_is_stable(candidate_coefficients[index]))
                return GraphicEqConfigureStatus::unstable;
            previous_frequency = band.frequency_hz;
        }

        if (same_requested_configuration(bands))
            return GraphicEqConfigureStatus::configured;

        const std::size_t staging_index = 1 - active_index_;
        const auto installed = cascades_[staging_index].set_coefficients(
            std::span<const Coefficients>(candidate_coefficients.data(), bands.size()));
        switch (installed) {
            case SosCascadeInstallStatus::installed:
                break;
            case SosCascadeInstallStatus::not_prepared:
                return GraphicEqConfigureStatus::not_prepared;
            case SosCascadeInstallStatus::over_capacity:
                return GraphicEqConfigureStatus::over_capacity;
            case SosCascadeInstallStatus::non_finite:
                return GraphicEqConfigureStatus::non_finite;
            case SosCascadeInstallStatus::unstable:
                return GraphicEqConfigureStatus::unstable;
        }

        requested_bands_ = candidate_bands;
        requested_coefficients_ = candidate_coefficients;
        requested_count_ = bands.size();

        if (transition_samples == 0) {
            active_index_ = staging_index;
            commit_requested_endpoint();
            cascades_[1 - active_index_].reset();
        } else {
            transition_total_ = transition_samples;
            transition_remaining_ = transition_samples;
        }
        healthy_ = true;
        return GraphicEqConfigureStatus::configured;
    }

    SampleType process(SampleType input) noexcept {
        if (!prepared())
            return input;
        if (!std::isfinite(input))
            return recover_from_fault();

        const SampleType from = cascades_[active_index_].process(input);
        SampleType output = from;
        if (transitioning()) {
            const SampleType to = cascades_[1 - active_index_].process(input);
            const std::size_t completed = transition_total_ - transition_remaining_ + 1;
            const SampleType mix =
                static_cast<SampleType>(completed) / static_cast<SampleType>(transition_total_);
            // The final transition frame is the destination, not merely a
            // mathematically equivalent interpolation expression. Besides
            // making the endpoint exact, std::lerp avoids avoidable overflow
            // when two earlier finite endpoints have opposite signs.
            output = transition_remaining_ == 1 ? to : std::lerp(from, to, mix);
            if (--transition_remaining_ == 0) {
                active_index_ = 1 - active_index_;
                commit_requested_endpoint();
                cascades_[1 - active_index_].reset();
            }
        }

        if (!std::isfinite(output))
            return recover_from_fault();
        healthy_ = true;
        return output;
    }

    /// Process caller-owned storage in place. A null pointer is valid only for
    /// an empty block and otherwise fails without advancing any DSP state.
    bool process_block(SampleType* samples, std::size_t frames) noexcept {
        if (samples == nullptr && frames != 0)
            return false;
        for (std::size_t frame = 0; frame < frames; ++frame)
            samples[frame] = process(samples[frame]);
        return true;
    }

    /// Clear recursive history and, if a transition is active, select its
    /// requested endpoint. Configuration and fault telemetry are preserved.
    void reset() noexcept {
        if (!prepared())
            return;
        if (transitioning()) {
            active_index_ = 1 - active_index_;
            commit_requested_endpoint();
        }
        cascades_[0].reset();
        cascades_[1].reset();
        transition_total_ = 0;
        transition_remaining_ = 0;
        healthy_ = true;
    }

    bool prepared() const noexcept {
        return capacity_ != 0;
    }
    bool transitioning() const noexcept {
        return transition_remaining_ != 0;
    }
    bool healthy() const noexcept {
        return healthy_;
    }
    std::size_t fault_count() const noexcept {
        return fault_count_;
    }
    std::size_t capacity() const noexcept {
        return capacity_;
    }
    std::size_t band_count() const noexcept {
        return requested_count_;
    }
    SampleType sample_rate() const noexcept {
        return sample_rate_;
    }
    SampleType supported_frequency_ceiling_hz() const noexcept {
        return std::min(maximum_frequency_hz, sample_rate_ * SampleType{0.49});
    }
    Band band(std::size_t index) const noexcept {
        return index < requested_count_ ? requested_bands_[index] : Band{};
    }
    Coefficients coefficients(std::size_t index) const noexcept {
        return index < requested_count_ ? requested_coefficients_[index] : Coefficients{};
    }

    /// The cascade is causal IIR and adds no bulk delay. Any active non-neutral
    /// peaking band has an asymptotic recursive tail, reported with Pulp's -1
    /// infinite-tail sentinel. A bypass/all-zero-gain layout has no tail.
    static constexpr int latency_samples() noexcept {
        return 0;
    }
    int tail_samples() const noexcept {
        const auto has_non_neutral_band = [](const auto& bands, std::size_t count) {
            for (std::size_t index = 0; index < count; ++index)
                if (bands[index].gain_db != SampleType{0})
                    return true;
            return false;
        };
        return has_non_neutral_band(active_bands_, active_count_) ||
                       has_non_neutral_band(requested_bands_, requested_count_)
                   ? -1
                   : 0;
    }

    /// Stationary response of the requested endpoint. During a transition this
    /// describes the destination, not the instantaneous linear crossfade.
    double magnitude(double frequency_hz) const noexcept {
        if (!prepared() || !std::isfinite(frequency_hz) || frequency_hz < 0.0 ||
            frequency_hz > static_cast<double>(sample_rate_) * 0.5)
            return std::numeric_limits<double>::quiet_NaN();

        double result = 1.0;
        const double omega = angular_frequency(frequency_hz, static_cast<double>(sample_rate_));
        for (std::size_t index = 0; index < requested_count_; ++index)
            result *= section_magnitude(requested_coefficients_[index], omega);
        return result;
    }

    double magnitude_db(double frequency_hz) const noexcept {
        const double linear = magnitude(frequency_hz);
        if (!std::isfinite(linear))
            return linear;
        if (!(linear > 0.0))
            return static_cast<double>(min_response_db);
        const double db = 20.0 * std::log10(linear);
        return !std::isfinite(db) || db < static_cast<double>(min_response_db)
                   ? static_cast<double>(min_response_db)
                   : db;
    }

    static constexpr std::size_t storage_capacity() noexcept {
        return MaxBands;
    }

  private:
    static bool coefficients_are_finite(const Coefficients& c) noexcept {
        return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) &&
               std::isfinite(c.a1) && std::isfinite(c.a2);
    }

    Coefficients design_band(const Band& band) const noexcept {
        if (band.gain_db == SampleType{0})
            return Coefficients{};
        BiquadT<double> designer;
        designer.set_coefficients(BiquadT<double>::Type::peaking,
                                  static_cast<double>(band.frequency_hz),
                                  static_cast<double>(band.q), static_cast<double>(sample_rate_),
                                  static_cast<double>(band.gain_db));
        const auto c = designer.coefficients();
        return {static_cast<SampleType>(c.b0), static_cast<SampleType>(c.b1),
                static_cast<SampleType>(c.b2), static_cast<SampleType>(c.a1),
                static_cast<SampleType>(c.a2)};
    }

    bool same_requested_configuration(std::span<const Band> bands) const noexcept {
        if (bands.size() != requested_count_)
            return false;
        for (std::size_t index = 0; index < bands.size(); ++index)
            if (!(bands[index] == requested_bands_[index]))
                return false;
        return true;
    }

    void commit_requested_endpoint() noexcept {
        active_bands_ = requested_bands_;
        active_count_ = requested_count_;
        transition_total_ = 0;
        transition_remaining_ = 0;
    }

    SampleType recover_from_fault() noexcept {
        cascades_[0].reset();
        cascades_[1].reset();
        ++fault_count_;
        healthy_ = false;
        return SampleType{0};
    }

    std::array<SosCascadeT<SampleType, MaxBands>, 2> cascades_{};
    std::array<Band, MaxBands> active_bands_{};
    std::array<Band, MaxBands> requested_bands_{};
    std::array<Coefficients, MaxBands> requested_coefficients_{};
    SampleType sample_rate_ = SampleType{0};
    std::size_t capacity_ = 0;
    std::size_t active_index_ = 0;
    std::size_t active_count_ = 0;
    std::size_t requested_count_ = 0;
    std::size_t transition_total_ = 0;
    std::size_t transition_remaining_ = 0;
    std::size_t fault_count_ = 0;
    bool healthy_ = true;
};

using GraphicEq = GraphicEqT<float>;
using GraphicEq64 = GraphicEqT<double>;

} // namespace pulp::signal
