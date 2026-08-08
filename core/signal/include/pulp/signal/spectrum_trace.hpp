#pragma once

/// @file spectrum_trace.hpp
/// Fixed-capacity conditioning for FFT magnitude frames.
///
/// Input is one-sided FFT magnitude in amplitude dBFS (`20*log10(|X|/ref)`),
/// ordered from DC through Nyquist, with exactly `fft_size / 2 + 1` bin centers
/// at `k * sample_rate / fft_size`. Band means convert those dB values to
/// linear power, average, then convert back to dB. DC and Nyquist participate
/// only when the configured band edges include their centers. A band containing
/// no FFT bin center remains at `floor_db`; it never borrows or duplicates a
/// nearby out-of-range bin.
///
/// This class owns band aggregation, additive A/C weighting, attack/release
/// smoothing, and peak hold/decay. Attack/release and peak decay are explicit
/// per-input-frame values; the defaults preserve the existing UI's 0.5/0.15
/// coefficients. Callers own the window/hop cadence and upstream analysis
/// latency because this class deliberately does not duplicate an FFT.
/// `current()` exposes same-thread state only; callers must provide their own
/// synchronized snapshot/mailbox when publishing frames across threads.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

enum class SpectrumBandScale : std::uint8_t { linear, logarithmic };
enum class SpectrumBandAggregation : std::uint8_t { mean_power, maximum };
enum class SpectrumFrequencyWeighting : std::uint8_t { none, a, c };

struct SpectrumTraceConfig {
    double sample_rate = 48000.0;
    std::uint32_t fft_size = 1024;
    std::size_t band_count = 128;
    double minimum_hz = 20.0;
    double maximum_hz = 20000.0;
    SpectrumBandScale band_scale = SpectrumBandScale::logarithmic;
    SpectrumBandAggregation aggregation = SpectrumBandAggregation::mean_power;
    SpectrumFrequencyWeighting weighting = SpectrumFrequencyWeighting::none;
    double floor_db = -120.0;
    double ceiling_db = 24.0;
    double attack = 0.5;
    double release = 0.15;
    std::uint32_t peak_hold_frames = 0;
    double peak_decay_db_per_frame = 1.0;
};

template <typename SampleType, std::size_t MaxBands> struct SpectrumTraceFrameT {
    static_assert(std::is_floating_point_v<SampleType>);

    std::array<SampleType, MaxBands> magnitude_db{};
    std::array<SampleType, MaxBands> peak_db{};
    std::array<SampleType, MaxBands> center_hz{};
    std::size_t band_count = 0;
    std::uint64_t sequence = 0;
};

template <typename SampleType = float, std::size_t MaxInputBins = 4097, std::size_t MaxBands = 256>
class SpectrumTraceT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxInputBins >= 2);
    static_assert(MaxInputBins <= std::numeric_limits<std::uint32_t>::max() / 2u + 1u);
    static_assert(MaxBands > 0);

  public:
    using Frame = SpectrumTraceFrameT<SampleType, MaxBands>;

    SpectrumTraceT() noexcept {
        SpectrumTraceConfig config;
        config.fft_size =
            static_cast<std::uint32_t>(std::min<std::size_t>(1024, 2 * (MaxInputBins - 1)));
        config.band_count = std::min<std::size_t>(128, MaxBands);
        (void)configure(config);
    }

    /// Control-thread operation. Invalid configurations are rejected without
    /// changing the current configuration or trace.
    [[nodiscard]] bool configure(const SpectrumTraceConfig& config) noexcept {
        std::array<std::size_t, MaxBands> first_bin{};
        std::array<std::size_t, MaxBands> end_bin{};
        std::array<SampleType, MaxBands> center_hz{};
        std::array<SampleType, MaxInputBins> weighting_db{};

        const auto scale_valid = config.band_scale == SpectrumBandScale::linear ||
                                 config.band_scale == SpectrumBandScale::logarithmic;
        const auto aggregation_valid = config.aggregation == SpectrumBandAggregation::mean_power ||
                                       config.aggregation == SpectrumBandAggregation::maximum;
        const auto weighting_valid = config.weighting == SpectrumFrequencyWeighting::none ||
                                     config.weighting == SpectrumFrequencyWeighting::a ||
                                     config.weighting == SpectrumFrequencyWeighting::c;
        if (!std::isfinite(config.sample_rate) || config.sample_rate < 1.0 ||
            config.sample_rate > 384000.0 || config.fft_size < 2 || (config.fft_size & 1u) != 0 ||
            config.fft_size / 2u + 1u > MaxInputBins || config.band_count == 0 ||
            config.band_count > MaxBands || !std::isfinite(config.minimum_hz) ||
            !std::isfinite(config.maximum_hz) || config.minimum_hz < 0.0 ||
            config.maximum_hz <= config.minimum_hz ||
            config.maximum_hz > config.sample_rate * 0.5 ||
            (config.band_scale == SpectrumBandScale::logarithmic && config.minimum_hz <= 0.0) ||
            !std::isfinite(config.floor_db) || !std::isfinite(config.ceiling_db) ||
            config.ceiling_db <= config.floor_db || !std::isfinite(config.attack) ||
            !std::isfinite(config.release) || config.attack < 0.0 || config.attack > 1.0 ||
            config.release < 0.0 || config.release > 1.0 ||
            !std::isfinite(config.peak_decay_db_per_frame) ||
            config.peak_decay_db_per_frame < 0.0 || !scale_valid || !aggregation_valid ||
            !weighting_valid || !representable_(config.floor_db) ||
            !representable_(config.ceiling_db) || !representable_(config.peak_decay_db_per_frame)) {
            return false;
        }

        const auto input_bins = static_cast<std::size_t>(config.fft_size / 2u + 1u);
        const double bin_hz = config.sample_rate / static_cast<double>(config.fft_size);
        for (std::size_t band = 0; band < config.band_count; ++band) {
            const double t0 = static_cast<double>(band) / static_cast<double>(config.band_count);
            const double t1 =
                static_cast<double>(band + 1) / static_cast<double>(config.band_count);
            const double low = band_edge_(config, t0);
            const double high = band_edge_(config, t1);
            const auto first = static_cast<std::size_t>(std::ceil(low / bin_hz));
            const auto end = band + 1 == config.band_count
                                 ? static_cast<std::size_t>(std::floor(high / bin_hz)) + 1
                                 : static_cast<std::size_t>(std::ceil(high / bin_hz));
            first_bin[band] = std::min(first, input_bins);
            end_bin[band] = std::min(end, input_bins);
            const double center = config.band_scale == SpectrumBandScale::logarithmic
                                      ? std::sqrt(low * high)
                                      : (low + high) * 0.5;
            center_hz[band] = static_cast<SampleType>(center);
        }
        for (std::size_t bin = 0; bin < input_bins; ++bin) {
            weighting_db[bin] = static_cast<SampleType>(
                calculate_weighting_db_(static_cast<double>(bin) * bin_hz, config.weighting));
        }

        config_ = config;
        input_bin_count_ = input_bins;
        first_bin_ = first_bin;
        end_bin_ = end_bin;
        weighting_db_ = weighting_db;
        centers_hz_ = center_hz;
        reset();
        return true;
    }

    /// Rejected frames leave the current trace and sequence unchanged.
    [[nodiscard]] bool process_frame(std::span<const SampleType> magnitude_db) noexcept {
        if (magnitude_db.size() != input_bin_count_)
            return false;

        for (std::size_t band = 0; band < config_.band_count; ++band) {
            SampleType target = static_cast<SampleType>(config_.floor_db);
            const bool band_has_bins = first_bin_[band] < end_bin_[band];
            if (band_has_bins && config_.aggregation == SpectrumBandAggregation::maximum) {
                for (std::size_t bin = first_bin_[band]; bin < end_bin_[band]; ++bin)
                    target = std::max(target, sanitize_bin_(magnitude_db[bin], bin));
            } else if (band_has_bins) {
                long double power = 0.0L;
                for (std::size_t bin = first_bin_[band]; bin < end_bin_[band]; ++bin) {
                    const auto db = sanitize_bin_(magnitude_db[bin], bin);
                    power += std::pow(10.0L, static_cast<long double>(db) / 10.0L);
                }
                power /= static_cast<long double>(end_bin_[band] - first_bin_[band]);
                const auto aggregated_db = 10.0L * std::log10(power);
                target = static_cast<SampleType>(
                    std::clamp(aggregated_db, static_cast<long double>(config_.floor_db),
                               static_cast<long double>(config_.ceiling_db)));
            }
            target = std::clamp(target, static_cast<SampleType>(config_.floor_db),
                                static_cast<SampleType>(config_.ceiling_db));

            if (!initialized_) {
                frame_.magnitude_db[band] = target;
                frame_.peak_db[band] = target;
                peak_hold_remaining_[band] = config_.peak_hold_frames;
                continue;
            }

            auto& smoothed = frame_.magnitude_db[band];
            const auto coefficient =
                static_cast<SampleType>(target > smoothed ? config_.attack : config_.release);
            smoothed = std::lerp(smoothed, target, coefficient);

            auto& peak = frame_.peak_db[band];
            if (smoothed >= peak) {
                peak = smoothed;
                peak_hold_remaining_[band] = config_.peak_hold_frames;
            } else if (peak_hold_remaining_[band] > 0) {
                --peak_hold_remaining_[band];
            } else {
                const auto decay = static_cast<SampleType>(config_.peak_decay_db_per_frame);
                const auto lowest = std::numeric_limits<SampleType>::lowest();
                const auto decayed =
                    peak < lowest + decay ? lowest : static_cast<SampleType>(peak - decay);
                peak = std::max(smoothed, decayed);
            }
        }

        initialized_ = true;
        frame_.band_count = config_.band_count;
        frame_.sequence = ++sequence_;
        return true;
    }

    void reset() noexcept {
        initialized_ = false;
        sequence_ = 0;
        frame_ = {};
        frame_.band_count = config_.band_count;
        for (std::size_t band = 0; band < config_.band_count; ++band) {
            frame_.magnitude_db[band] = static_cast<SampleType>(config_.floor_db);
            frame_.peak_db[band] = static_cast<SampleType>(config_.floor_db);
            frame_.center_hz[band] = centers_hz_[band];
        }
        peak_hold_remaining_.fill(0);
    }

    /// Same-thread view. This reference is not a concurrent publication API.
    [[nodiscard]] const Frame& current() const noexcept {
        return frame_;
    }
    [[nodiscard]] const SpectrumTraceConfig& config() const noexcept {
        return config_;
    }
    [[nodiscard]] std::size_t input_bin_count() const noexcept {
        return input_bin_count_;
    }
    [[nodiscard]] static constexpr std::uint32_t algorithmic_latency_samples() noexcept {
        return 0;
    }

  private:
    static bool representable_(double value) noexcept {
        return value >= static_cast<double>(std::numeric_limits<SampleType>::lowest()) &&
               value <= static_cast<double>(std::numeric_limits<SampleType>::max());
    }

    static double band_edge_(const SpectrumTraceConfig& config, double t) noexcept {
        if (config.band_scale == SpectrumBandScale::linear)
            return std::lerp(config.minimum_hz, config.maximum_hz, t);
        return std::exp(std::lerp(std::log(config.minimum_hz), std::log(config.maximum_hz), t));
    }

    static double calculate_weighting_db_(double frequency_hz,
                                          SpectrumFrequencyWeighting weighting) noexcept {
        if (weighting == SpectrumFrequencyWeighting::none)
            return 0.0;
        if (!(frequency_hz > 0.0) || !std::isfinite(frequency_hz))
            return -200.0;
        const double f2 = frequency_hz * frequency_hz;
        constexpr double f1 = 20.6;
        constexpr double f2a = 107.7;
        constexpr double f3 = 737.9;
        constexpr double f4 = 12200.0;
        if (weighting == SpectrumFrequencyWeighting::a) {
            const double numerator = f4 * f4 * f2 * f2;
            const double denominator =
                (f2 + f1 * f1) * std::sqrt((f2 + f2a * f2a) * (f2 + f3 * f3)) * (f2 + f4 * f4);
            return 20.0 * std::log10(numerator / denominator) + 2.0;
        }
        const double numerator = f4 * f4 * f2;
        const double denominator = (f2 + f1 * f1) * (f2 + f4 * f4);
        return 20.0 * std::log10(numerator / denominator) + 0.06;
    }

    SampleType sanitize_bin_(SampleType value, std::size_t bin) const noexcept {
        const auto floor = static_cast<SampleType>(config_.floor_db);
        const auto ceiling = static_cast<SampleType>(config_.ceiling_db);
        if (!std::isfinite(value))
            return floor;
        if (bin == 0 && config_.weighting != SpectrumFrequencyWeighting::none)
            return floor;
        const auto correction = weighting_db_[bin];
        const auto maximum = std::numeric_limits<SampleType>::max();
        const auto lowest = std::numeric_limits<SampleType>::lowest();
        if (correction > SampleType{0} && value > SampleType{0} && correction > maximum - value)
            return ceiling;
        if (correction < SampleType{0} && value < SampleType{0} && correction < lowest - value)
            return floor;
        const auto weighted = static_cast<SampleType>(value + correction);
        return std::clamp(weighted, floor, ceiling);
    }

    SpectrumTraceConfig config_{};
    std::size_t input_bin_count_ = 0;
    std::array<std::size_t, MaxBands> first_bin_{};
    std::array<std::size_t, MaxBands> end_bin_{};
    std::array<SampleType, MaxInputBins> weighting_db_{};
    std::array<SampleType, MaxBands> centers_hz_{};
    std::array<std::uint32_t, MaxBands> peak_hold_remaining_{};
    Frame frame_{};
    std::uint64_t sequence_ = 0;
    bool initialized_ = false;
};

using SpectrumTrace = SpectrumTraceT<float>;
using SpectrumTrace64 = SpectrumTraceT<double>;

} // namespace pulp::signal
