#pragma once

/// @file spectral_cross_synthesis.hpp
/// Prepared source-filter cross-synthesis for coherent one-sided frames.
///
/// The carrier supplies phase and fine spectral structure. Separate cepstral
/// envelopes are estimated with `CepstralEnvelopeAnalyzerT`; the modulator
/// envelope replaces the carrier envelope in the log-magnitude domain. This is
/// deliberately not raw magnitude/phase interpolation (`SpectralMorphT`).

#include <pulp/signal/checked_allocation.hpp>
#include <pulp/signal/source_filter_analysis.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pulp::signal {

enum class SpectralCrossSynthesisNormalization {
    none,
    match_carrier_rms,
};

template <typename SampleType> struct SpectralCrossSynthesisPrepareConfigT {
    int channels = 1;
    int fft_size = 2048;
    /// Cepstral cutoff. -1 selects fft_size / 16; zero keeps only the mean.
    int lifter_order = -1;
    int true_envelope_iterations = 3;
    SampleType convergence_tolerance = SampleType{0};
};

template <typename SampleType> struct SpectralCrossSynthesisConfigT {
    /// Log-envelope transfer in [0,1]. Zero retains the carrier envelope.
    SampleType amount = SampleType{1};
    /// Final linear-magnitude dry/wet blend in [0,1].
    SampleType mix = SampleType{1};
    /// 63.2% envelope-history time constant in frames; one is instantaneous.
    int envelope_smoothing_frames = 1;
    /// Symmetric bound on the transferred envelope difference.
    SampleType maximum_transfer_db = SampleType{60};
    /// Absolute linear-magnitude floor used only for log-envelope analysis.
    SampleType magnitude_floor = SampleType{1e-12};
    SpectralCrossSynthesisNormalization normalization =
        SpectralCrossSynthesisNormalization::match_carrier_rms;
};

template <typename SampleType = float> class SpectralCrossSynthesisT {
  public:
    static_assert(std::is_floating_point_v<SampleType>);

    using PrepareConfig = SpectralCrossSynthesisPrepareConfigT<SampleType>;
    using Config = SpectralCrossSynthesisConfigT<SampleType>;

    static constexpr int maximum_channels = 64;
    static constexpr int maximum_true_envelope_iterations = 32;
    static constexpr int maximum_smoothing_frames = 4096;
    static constexpr SampleType maximum_transfer_db = SampleType{120};

    static bool supports_configuration(const PrepareConfig& config) noexcept {
        if (config.channels <= 0 || config.channels > maximum_channels ||
            config.fft_size < kSpectralFrameEngineMinimumFftSize ||
            config.fft_size > kSpectralFrameEngineMaximumFftSize ||
            (config.fft_size & (config.fft_size - 1)) != 0 || config.lifter_order < -1 ||
            config.lifter_order > config.fft_size / 2 || config.true_envelope_iterations < 0 ||
            config.true_envelope_iterations > maximum_true_envelope_iterations ||
            !std::isfinite(config.convergence_tolerance) ||
            config.convergence_tolerance < SampleType{0})
            return false;
        return true;
    }

    /// Allocate the analyzer, envelope history, and all frame scratch off the
    /// audio thread. Rejection leaves an already prepared instance unchanged.
    bool prepare(PrepareConfig config) {
        if (!supports_configuration(config))
            return false;
        if (config.lifter_order < 0)
            config.lifter_order = config.fft_size / 16;

        typename CepstralEnvelopeAnalyzerT<SampleType>::Config analyzer_config;
        analyzer_config.fft_size = config.fft_size;
        analyzer_config.order = config.lifter_order;
        analyzer_config.true_envelope_iterations = config.true_envelope_iterations;
        analyzer_config.convergence_tolerance = config.convergence_tolerance;

        std::uint64_t analyzer_bytes = 0;
        if (!CepstralEnvelopeAnalyzerT<SampleType>::checked_retained_bytes(
                analyzer_config, kTargetAddressMaximumBytes, analyzer_bytes))
            return false;
        const auto bins = static_cast<std::uint64_t>(config.fft_size / 2 + 1);
        std::uint64_t elements = 0;
        if (!checked_capacity_product(static_cast<std::uint64_t>(config.channels), bins,
                                      std::numeric_limits<std::uint64_t>::max(), elements))
            return false;
        CheckedRetainedByteCharge charge(kTargetAddressMaximumBytes);
        if (!charge.add_retained_bytes(analyzer_bytes) || !charge.add<SampleType>(elements * 8u) ||
            !charge.add<std::uint8_t>(static_cast<std::uint64_t>(config.channels)))
            return false;

#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        try {
#else
        {
#endif
            CepstralEnvelopeAnalyzerT<SampleType> next_analyzer;
            if (next_analyzer.prepare(analyzer_config) != SourceFilterAnalysisStatus::Ok)
                return false;
            const auto count = static_cast<std::size_t>(elements);
            std::vector<SampleType> next_carrier_log(count, SampleType{0});
            std::vector<SampleType> next_modulator_log(count, SampleType{0});
            std::vector<SampleType> next_carrier_envelope(count, SampleType{0});
            std::vector<SampleType> next_modulator_envelope(count, SampleType{0});
            std::vector<SampleType> next_carrier_history(count, SampleType{0});
            std::vector<SampleType> next_modulator_history(count, SampleType{0});
            std::vector<SampleType> next_carrier_magnitude(count, SampleType{0});
            std::vector<SampleType> next_wet_magnitude(count, SampleType{0});
            std::vector<std::uint8_t> next_history_valid(static_cast<std::size_t>(config.channels),
                                                         std::uint8_t{0});

            analyzer_ = std::move(next_analyzer);
            carrier_log_ = std::move(next_carrier_log);
            modulator_log_ = std::move(next_modulator_log);
            carrier_envelope_ = std::move(next_carrier_envelope);
            modulator_envelope_ = std::move(next_modulator_envelope);
            carrier_history_ = std::move(next_carrier_history);
            modulator_history_ = std::move(next_modulator_history);
            carrier_magnitude_ = std::move(next_carrier_magnitude);
            wet_magnitude_ = std::move(next_wet_magnitude);
            history_valid_ = std::move(next_history_valid);
            prepare_config_ = config;
            num_bins_ = config.fft_size / 2 + 1;
            retained_bytes_ = charge.total();
            return true;
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS) || defined(_CPPUNWIND)
        } catch (const std::bad_alloc&) {
            return false;
        } catch (const std::length_error&) {
            return false;
        }
#else
        }
#endif
    }

    /// Replace every runtime control transactionally. Rejection retains the
    /// previous config and history coefficient.
    bool set_config(const Config& config) noexcept {
        if (!valid_config(config))
            return false;
        const SampleType next_retain =
            config.envelope_smoothing_frames <= 1
                ? SampleType{0}
                : std::exp(-SampleType{1} /
                           static_cast<SampleType>(config.envelope_smoothing_frames));
        config_ = config;
        envelope_retain_ = next_retain;
        return true;
    }

    Config config() const noexcept {
        return config_;
    }
    bool prepared() const noexcept {
        return num_bins_ > 0;
    }
    int channels() const noexcept {
        return prepared() ? prepare_config_.channels : 0;
    }
    int fft_size() const noexcept {
        return prepared() ? prepare_config_.fft_size : 0;
    }
    int num_bins() const noexcept {
        return num_bins_;
    }
    int lifter_order() const noexcept {
        return prepared() ? prepare_config_.lifter_order : 0;
    }
    std::uint64_t retained_bytes() const noexcept {
        return retained_bytes_;
    }

    /// Clear temporal envelope history while preserving prepared capacity and
    /// both configs. The next accepted frame initializes history from itself.
    void reset() noexcept {
        std::fill(carrier_history_.begin(), carrier_history_.end(), SampleType{0});
        std::fill(modulator_history_.begin(), modulator_history_.end(), SampleType{0});
        std::fill(history_valid_.begin(), history_valid_.end(), std::uint8_t{0});
    }

    /// Cross-synthesize one coherent frame group. Output may alias corresponding
    /// carrier or modulator channel storage. Invalid geometry is rejected before
    /// output/history mutation. Non-finite bins enter envelope analysis at the
    /// configured floor; a non-finite carrier bin itself becomes silence.
    bool process(const std::complex<SampleType>* const* carrier,
                 const std::complex<SampleType>* const* modulator,
                 std::complex<SampleType>* const* output, int channels, int num_bins) noexcept {
        if (!prepared() || channels != prepare_config_.channels || num_bins != num_bins_ ||
            carrier == nullptr || modulator == nullptr || output == nullptr)
            return false;
        for (int ch = 0; ch < channels; ++ch) {
            if (carrier[ch] == nullptr || modulator[ch] == nullptr || output[ch] == nullptr)
                return false;
        }

        for (int ch = 0; ch < channels; ++ch) {
            const auto offset = static_cast<std::size_t>(ch) * static_cast<std::size_t>(num_bins_);
            for (int bin = 0; bin < num_bins_; ++bin) {
                const auto index = offset + static_cast<std::size_t>(bin);
                const bool endpoint = bin == 0 || bin == num_bins_ - 1;
                const SampleType carrier_magnitude = decoded_magnitude(carrier[ch][bin], endpoint);
                const SampleType modulator_magnitude =
                    decoded_magnitude(modulator[ch][bin], endpoint);
                carrier_magnitude_[index] = carrier_magnitude;
                carrier_log_[index] =
                    std::log(std::max(carrier_magnitude, config_.magnitude_floor));
                modulator_log_[index] =
                    std::log(std::max(modulator_magnitude, config_.magnitude_floor));
            }
            const auto carrier_result =
                analyzer_.estimate(std::span<const SampleType>{carrier_log_.data() + offset,
                                                               static_cast<std::size_t>(num_bins_)},
                                   std::span<SampleType>{carrier_envelope_.data() + offset,
                                                         static_cast<std::size_t>(num_bins_)});
            if (!carrier_result.ok())
                return false;
            const auto modulator_result =
                analyzer_.estimate(std::span<const SampleType>{modulator_log_.data() + offset,
                                                               static_cast<std::size_t>(num_bins_)},
                                   std::span<SampleType>{modulator_envelope_.data() + offset,
                                                         static_cast<std::size_t>(num_bins_)});
            if (!modulator_result.ok())
                return false;
        }

        const SampleType maximum_transfer_ln =
            config_.maximum_transfer_db * static_cast<SampleType>(0.1151292546497022842L);
        for (int ch = 0; ch < channels; ++ch) {
            const auto offset = static_cast<std::size_t>(ch) * static_cast<std::size_t>(num_bins_);
            const bool initialize = history_valid_[static_cast<std::size_t>(ch)] == 0;
            for (int bin = 0; bin < num_bins_; ++bin) {
                const auto index = offset + static_cast<std::size_t>(bin);
                if (initialize) {
                    carrier_history_[index] = carrier_envelope_[index];
                    modulator_history_[index] = modulator_envelope_[index];
                } else {
                    carrier_history_[index] = std::fma(
                        envelope_retain_, carrier_history_[index] - carrier_envelope_[index],
                        carrier_envelope_[index]);
                    modulator_history_[index] = std::fma(
                        envelope_retain_, modulator_history_[index] - modulator_envelope_[index],
                        modulator_envelope_[index]);
                }
                const SampleType transfer = std::clamp(
                    config_.amount * (modulator_history_[index] - carrier_history_[index]),
                    -maximum_transfer_ln, maximum_transfer_ln);
                wet_magnitude_[index] = transferred_magnitude(carrier_magnitude_[index], transfer);
            }
            history_valid_[static_cast<std::size_t>(ch)] = 1;

            SampleType normalization_gain = SampleType{1};
            if (config_.normalization == SpectralCrossSynthesisNormalization::match_carrier_rms)
                normalization_gain = rms_ratio(carrier_magnitude_.data() + offset,
                                               wet_magnitude_.data() + offset, num_bins_);

            for (int bin = 0; bin < num_bins_; ++bin) {
                const auto index = offset + static_cast<std::size_t>(bin);
                const auto carrier_value = carrier[ch][bin];
                if (!finite(carrier_value) || carrier_magnitude_[index] == SampleType{0}) {
                    output[ch][bin] = {};
                    continue;
                }
                const SampleType magnitude = mixed_magnitude(
                    carrier_magnitude_[index], wet_magnitude_[index], normalization_gain,
                    config_.mix);
                const bool endpoint = bin == 0 || bin == num_bins_ - 1;
                if (endpoint) {
                    const SampleType sign =
                        carrier_value.real() < SampleType{0} ? SampleType{-1} : SampleType{1};
                    output[ch][bin] = {sign * magnitude, SampleType{0}};
                } else {
                    output[ch][bin] = unit_phase(carrier_value) * magnitude;
                }
            }
        }
        return true;
    }

    /// Analysis/synthesis buffering belongs to the surrounding frame engine;
    /// retained envelope history does not emit signal by itself.
    static constexpr int added_latency_samples() noexcept {
        return 0;
    }
    static constexpr int added_tail_samples() noexcept {
        return 0;
    }

  private:
    struct ScaledSquares {
        long double scale = 0.0L;
        long double sum = 0.0L;

        void add(SampleType value) noexcept {
            const long double magnitude = std::abs(static_cast<long double>(value));
            if (magnitude == 0.0L)
                return;
            if (scale < magnitude) {
                const long double ratio = scale / magnitude;
                sum = 1.0L + sum * ratio * ratio;
                scale = magnitude;
            } else {
                const long double ratio = magnitude / scale;
                sum += ratio * ratio;
            }
        }
    };

    static bool valid_config(const Config& config) noexcept {
        const bool normalization_valid =
            config.normalization == SpectralCrossSynthesisNormalization::none ||
            config.normalization == SpectralCrossSynthesisNormalization::match_carrier_rms;
        return std::isfinite(config.amount) && config.amount >= SampleType{0} &&
               config.amount <= SampleType{1} && std::isfinite(config.mix) &&
               config.mix >= SampleType{0} && config.mix <= SampleType{1} &&
               config.envelope_smoothing_frames >= 1 &&
               config.envelope_smoothing_frames <= maximum_smoothing_frames &&
               std::isfinite(config.maximum_transfer_db) &&
               config.maximum_transfer_db >= SampleType{0} &&
               config.maximum_transfer_db <= maximum_transfer_db &&
               std::isfinite(config.magnitude_floor) && config.magnitude_floor > SampleType{0} &&
               normalization_valid;
    }

    static bool finite(std::complex<SampleType> value) noexcept {
        return std::isfinite(value.real()) && std::isfinite(value.imag());
    }

    static SampleType decoded_magnitude(std::complex<SampleType> value, bool endpoint) noexcept {
        if (!finite(value))
            return SampleType{0};
        if (endpoint)
            return std::abs(value.real());
        const SampleType scale = std::max(std::abs(value.real()), std::abs(value.imag()));
        if (scale == SampleType{0})
            return SampleType{0};
        const SampleType normalized = std::hypot(value.real() / scale, value.imag() / scale);
        const SampleType maximum = std::numeric_limits<SampleType>::max();
        return scale > maximum / normalized ? maximum : scale * normalized;
    }

    static std::complex<SampleType> unit_phase(std::complex<SampleType> value) noexcept {
        const SampleType scale = std::max(std::abs(value.real()), std::abs(value.imag()));
        const SampleType real = value.real() / scale;
        const SampleType imag = value.imag() / scale;
        const SampleType length = std::hypot(real, imag);
        return {real / length, imag / length};
    }

    static SampleType saturate_positive(long double value) noexcept {
        if (!(value > 0.0L))
            return SampleType{0};
        const long double maximum =
            static_cast<long double>(std::numeric_limits<SampleType>::max());
        return static_cast<SampleType>(std::min(value, maximum));
    }

    static SampleType saturating_positive_product(SampleType a, SampleType b,
                                                   SampleType c) noexcept {
        std::array<SampleType, 3> factors{a, b, c};
        std::sort(factors.begin(), factors.end());
        if (!(factors[0] > SampleType{0}))
            return SampleType{0};
        const SampleType maximum = std::numeric_limits<SampleType>::max();
        if (factors[0] > maximum / factors[1])
            return maximum;
        const SampleType partial = factors[0] * factors[1];
        return partial > maximum / factors[2] ? maximum : partial * factors[2];
    }

    static SampleType mixed_magnitude(SampleType dry, SampleType wet,
                                      SampleType normalization_gain, SampleType mix) noexcept {
        if (mix == SampleType{0})
            return dry;
        if (mix == SampleType{1})
            return saturating_positive_product(wet, normalization_gain, SampleType{1});
        const SampleType dry_component = (SampleType{1} - mix) * dry;
        const SampleType wet_component = saturating_positive_product(wet, normalization_gain, mix);
        const SampleType maximum = std::numeric_limits<SampleType>::max();
        return dry_component > maximum - wet_component ? maximum
                                                        : dry_component + wet_component;
    }

    static SampleType transferred_magnitude(SampleType carrier_magnitude,
                                            SampleType transfer) noexcept {
        if (carrier_magnitude == SampleType{0})
            return SampleType{0};
        const long double log_target = std::log(static_cast<long double>(carrier_magnitude)) +
                                       static_cast<long double>(transfer);
        const long double maximum_log =
            std::log(static_cast<long double>(std::numeric_limits<SampleType>::max()));
        return saturate_positive(std::exp(std::min(log_target, maximum_log)));
    }

    static SampleType rms_ratio(const SampleType* dry, const SampleType* wet, int count) noexcept {
        ScaledSquares dry_energy;
        ScaledSquares wet_energy;
        for (int index = 0; index < count; ++index) {
            dry_energy.add(dry[index]);
            wet_energy.add(wet[index]);
        }
        if (!(dry_energy.scale > 0.0L) || !(wet_energy.scale > 0.0L))
            return SampleType{1};
        const long double ratio =
            (dry_energy.scale / wet_energy.scale) * std::sqrt(dry_energy.sum / wet_energy.sum);
        return saturate_positive(ratio);
    }

    PrepareConfig prepare_config_{};
    Config config_{};
    CepstralEnvelopeAnalyzerT<SampleType> analyzer_{};
    int num_bins_ = 0;
    SampleType envelope_retain_ = SampleType{0};
    std::uint64_t retained_bytes_ = 0;
    std::vector<SampleType> carrier_log_;
    std::vector<SampleType> modulator_log_;
    std::vector<SampleType> carrier_envelope_;
    std::vector<SampleType> modulator_envelope_;
    std::vector<SampleType> carrier_history_;
    std::vector<SampleType> modulator_history_;
    std::vector<SampleType> carrier_magnitude_;
    std::vector<SampleType> wet_magnitude_;
    std::vector<std::uint8_t> history_valid_;
};

using SpectralCrossSynthesisPrepareConfig = SpectralCrossSynthesisPrepareConfigT<float>;
using SpectralCrossSynthesisPrepareConfig64 = SpectralCrossSynthesisPrepareConfigT<double>;
using SpectralCrossSynthesisConfig = SpectralCrossSynthesisConfigT<float>;
using SpectralCrossSynthesisConfig64 = SpectralCrossSynthesisConfigT<double>;
using SpectralCrossSynthesis = SpectralCrossSynthesisT<float>;
using SpectralCrossSynthesis64 = SpectralCrossSynthesisT<double>;

} // namespace pulp::signal
