#pragma once

#include <pulp/signal/biquad.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

namespace pulp::signal {

// Linkwitz-Riley crossover filter (4th order, -6dB at crossover)
// Provides lowpass and highpass outputs that sum flat.
//
// RT contract: coefficient updates, process, and reset are fixed-state only and
// allocate no memory.
template <typename SampleType = float> class LinkwitzRileyT {
  public:
    static constexpr SampleType legacy_butterworth_q = SampleType{0.707f};
    static constexpr SampleType exact_butterworth_q =
        static_cast<SampleType>(0.707106781186547524400844362104849039L);

    struct SectionCoefficients {
        BiquadCoefficientsT<SampleType> lowpass;
        BiquadCoefficientsT<SampleType> highpass;
    };

    void set_frequency(SampleType hz, SampleType sample_rate) {
        set_frequency_with_q(hz, sample_rate, legacy_butterworth_q);
    }

    /// Exact Butterworth alignment for new LR4 constructions. The two-argument
    /// overload retains its historical rounded-Q coefficients for render
    /// compatibility with existing consumers.
    void set_frequency_precise(SampleType hz, SampleType sample_rate) {
        set_frequency_with_q(hz, sample_rate, exact_butterworth_q);
    }

    /// Coefficients shared by both cascaded sections of each LR4 branch.
    SectionCoefficients section_coefficients() const {
        return {lp1_.coefficients(), hp1_.coefficients()};
    }

    /// Change coefficients without clearing recursive state. This lets several
    /// phase-related LR4 paths receive one identical coefficient design.
    void set_section_coefficients(const SectionCoefficients& coefficients) {
        lp1_.set_coefficients(coefficients.lowpass);
        lp2_.set_coefficients(coefficients.lowpass);
        hp1_.set_coefficients(coefficients.highpass);
        hp2_.set_coefficients(coefficients.highpass);
    }

  private:
    void set_frequency_with_q(SampleType hz, SampleType sample_rate, SampleType q) {
        lp1_.set_coefficients(BiquadT<SampleType>::Type::lowpass, hz, q, sample_rate);
        hp1_.set_coefficients(BiquadT<SampleType>::Type::highpass, hz, q, sample_rate);
        lp2_.set_coefficients(lp1_.coefficients());
        hp2_.set_coefficients(hp1_.coefficients());
    }

  public:
    struct BandSplit {
        SampleType low, high;
    };

    BandSplit process(SampleType input) {
        SampleType low = lp2_.process(lp1_.process(input));
        SampleType high = hp2_.process(hp1_.process(input));
        return {low, high};
    }

    void reset() {
        lp1_.reset();
        lp2_.reset();
        hp1_.reset();
        hp2_.reset();
    }

  private:
    BiquadT<SampleType> lp1_, lp2_, hp1_, hp2_;
};

using LinkwitzRiley = LinkwitzRileyT<float>;
using LinkwitzRiley64 = LinkwitzRileyT<double>;

/// Fixed-capacity Linkwitz-Riley crossover for two or more ordered bands.
///
/// Each split is fourth-order (LR4). Earlier bands pass through the all-pass
/// sum of every later split, so adding all returned bands reconstructs a flat
/// magnitude response with the common minimum-phase response of the cascade.
///
/// RT contract: prepare/configuration, process, reset, and response inspection
/// use fixed storage and allocate no memory. Band count is fixed by prepare();
/// a realtime retune may change only the existing ordered cutoffs. Smoothed
/// retunes use a topology-preserving-transform bank, avoiding both coefficient-
/// dependent state reinterpretation and cross-bank phase cancellation. Downward
/// moves must meet the bound reported by minimum_transition_samples().
template <typename SampleType = float, std::size_t MaxBands = 8> class LinkwitzRileyCrossoverT {
    static_assert(std::is_floating_point_v<SampleType>);
    static_assert(MaxBands >= 2);

  public:
    static constexpr std::size_t max_bands = MaxBands;

    struct Frame {
        std::array<SampleType, MaxBands> bands{};
        std::size_t count = 0;
        bool healthy = false;
    };

    /// Establish the sample rate, band count, and initial cutoff set.
    /// Cutoffs must be finite, strictly increasing, and inside (0, Nyquist).
    bool prepare(SampleType sample_rate, std::span<const SampleType> cutoffs) noexcept {
        if (!valid_configuration(sample_rate, cutoffs))
            return false;

        sample_rate_ = sample_rate;
        cutoff_count_ = cutoffs.size();
        copy_cutoffs(cutoffs, cutoffs_);
        Bank::warp_cutoffs(std::span<const double>(cutoffs_.data(), cutoff_count_), sample_rate_,
                           current_warped_cutoffs_);
        Bank::design_coefficients(current_warped_cutoffs_, cutoff_count_, current_coefficients_);
        target_warped_cutoffs_ = current_warped_cutoffs_;
        bank_.reset();
        transition_position_ = 0;
        transition_length_ = 0;
        prepared_ = true;
        healthy_ = true;
        return true;
    }

    /// Retune all cutoffs together. A zero-length transition changes the live
    /// coefficients immediately. A nonzero transition logarithmically moves
    /// the bilinear-warped cutoff design values for exactly transition_samples
    /// processed samples. Only bounded arithmetic occurs in process().
    bool set_cutoffs(std::span<const SampleType> cutoffs,
                     std::size_t transition_samples = 0) noexcept {
        if (!prepared_ || transition_length_ != 0 || cutoffs.size() != cutoff_count_ ||
            !valid_configuration(sample_rate_, cutoffs))
            return false;

        std::array<double, MaxBands - 1> proposed_cutoffs{};
        std::array<double, MaxBands - 1> proposed_warped_cutoffs{};
        copy_cutoffs(cutoffs, proposed_cutoffs);
        Bank::warp_cutoffs(std::span<const double>(proposed_cutoffs.data(), cutoff_count_),
                           sample_rate_, proposed_warped_cutoffs);
        if (transition_samples == 0) {
            cutoffs_ = proposed_cutoffs;
            target_cutoffs_ = proposed_cutoffs;
            current_warped_cutoffs_ = proposed_warped_cutoffs;
            target_warped_cutoffs_ = proposed_warped_cutoffs;
            Bank::design_coefficients(current_warped_cutoffs_, cutoff_count_,
                                      current_coefficients_);
            bank_.reset();
            return true;
        }

        const auto minimum = minimum_transition_samples_from_warped(proposed_warped_cutoffs);
        if (minimum == std::numeric_limits<std::size_t>::max() || transition_samples < minimum)
            return false;

        std::array<double, MaxBands - 1> proposed_multipliers{};
        bool changes_cutoff = false;
        for (std::size_t split = 0; split < cutoff_count_; ++split) {
            changes_cutoff =
                changes_cutoff || proposed_warped_cutoffs[split] != current_warped_cutoffs_[split];
            proposed_multipliers[split] =
                std::exp(std::log(proposed_warped_cutoffs[split] / current_warped_cutoffs_[split]) /
                         static_cast<double>(transition_samples));
            if (!(std::isfinite(proposed_multipliers[split]) &&
                  proposed_multipliers[split] > 0.0) ||
                (proposed_warped_cutoffs[split] != current_warped_cutoffs_[split] &&
                 proposed_multipliers[split] == 1.0))
                return false;
        }
        if (!changes_cutoff)
            return true;

        target_cutoffs_ = proposed_cutoffs;
        target_warped_cutoffs_ = proposed_warped_cutoffs;
        warped_cutoff_multipliers_ = proposed_multipliers;
        transition_position_ = 0;
        transition_length_ = transition_samples;
        return true;
    }

    Frame process(SampleType input) noexcept {
        Frame result;
        result.count = prepared_ ? cutoff_count_ + 1 : 0;
        if (!prepared_ || !std::isfinite(input))
            return recover(result);

        advance_transition();
        const auto rendered =
            bank_.process(static_cast<double>(input), cutoff_count_, current_coefficients_);
        for (std::size_t band = 0; band < result.count; ++band)
            result.bands[band] = static_cast<SampleType>(rendered[band]);

        for (std::size_t band = 0; band < result.count; ++band) {
            if (!std::isfinite(result.bands[band]))
                return recover(result);
        }
        healthy_ = true;
        result.healthy = true;
        return result;
    }

    void reset() noexcept {
        finish_transition();
        bank_.reset();
        transition_position_ = 0;
        transition_length_ = 0;
        healthy_ = prepared_;
    }

    std::size_t band_count() const noexcept {
        return prepared_ ? cutoff_count_ + 1 : 0;
    }
    std::size_t cutoff_count() const noexcept {
        return prepared_ ? cutoff_count_ : 0;
    }
    SampleType cutoff(std::size_t index) const noexcept {
        return index < cutoff_count_ ? static_cast<SampleType>(cutoffs_[index]) : SampleType{0};
    }
    SampleType sample_rate() const noexcept {
        return prepared_ ? sample_rate_ : SampleType{0};
    }
    bool transitioning() const noexcept {
        return transition_length_ != 0;
    }
    bool healthy() const noexcept {
        return healthy_;
    }
    std::uint64_t fault_count() const noexcept {
        return fault_count_;
    }

    /// Maximum accepted downward motion in the logarithm of the bilinear-warped
    /// cutoff. This parameter-rate guarantee does not imply a signal peak bound,
    /// which also depends on the recursive state established by prior input.
    static constexpr double maximum_downward_log_slew_nepers_per_second() noexcept {
        return 20.0;
    }

    /// Minimum safe nonzero transition for the requested cutoff set. Returning
    /// max() means the configuration is invalid for this prepared topology or
    /// that no nonzero transition length can represent the requested motion.
    std::size_t minimum_transition_samples(std::span<const SampleType> cutoffs) const noexcept {
        if (!prepared_ || cutoffs.size() != cutoff_count_ ||
            !valid_configuration(sample_rate_, cutoffs))
            return std::numeric_limits<std::size_t>::max();
        std::array<double, MaxBands - 1> warped{};
        std::array<double, MaxBands - 1> copied{};
        copy_cutoffs(cutoffs, copied);
        Bank::warp_cutoffs(std::span<const double>(copied.data(), cutoff_count_), sample_rate_,
                           warped);
        return minimum_transition_samples_from_warped(warped);
    }
    static constexpr int latency_samples() noexcept {
        return 0;
    }

    /// True when the exact double-precision internal realization has finite,
    /// non-degenerate coefficients and poles separated from the unit circle.
    /// This is the supported-domain predicate used by prepare() and retunes.
    static bool supports_configuration(SampleType sample_rate,
                                       std::span<const SampleType> cutoffs) noexcept {
        return valid_configuration(sample_rate, cutoffs);
    }

    /// Exact complex response of one configured band, including the all-pass
    /// compensation that makes the complete band sum flat.
    static std::complex<double> band_response(std::span<const SampleType> cutoffs, std::size_t band,
                                              double frequency_hz, double sample_rate) noexcept {
        if (band > cutoffs.size() ||
            !valid_configuration(static_cast<SampleType>(sample_rate), cutoffs) ||
            !(std::isfinite(frequency_hz) && frequency_hz >= 0.0 &&
              frequency_hz <= sample_rate * 0.5))
            return {};

        std::complex<double> response{1.0, 0.0};
        for (std::size_t split = 0; split < band; ++split)
            response *= split_response(cutoffs[split], frequency_hz, sample_rate, false);

        if (band < cutoffs.size())
            response *= split_response(cutoffs[band], frequency_hz, sample_rate, true);

        for (std::size_t split = band + 1; split < cutoffs.size(); ++split) {
            response *= split_response(cutoffs[split], frequency_hz, sample_rate, true) +
                        split_response(cutoffs[split], frequency_hz, sample_rate, false);
        }
        return response;
    }

    static std::complex<double> reconstruction_response(std::span<const SampleType> cutoffs,
                                                        double frequency_hz,
                                                        double sample_rate) noexcept {
        std::complex<double> response{};
        for (std::size_t band = 0; band <= cutoffs.size(); ++band)
            response += band_response(cutoffs, band, frequency_hz, sample_rate);
        return response;
    }

  private:
    using SectionCoefficients = LinkwitzRileyT<double>::SectionCoefficients;

    struct TptCoefficients {
        double a1 = 1.0;
        double a2 = 0.0;
        double a3 = 0.0;
    };

    struct TptSection {
        struct Outputs {
            double low = 0.0;
            double high = 0.0;
        };

        Outputs process(double input, const TptCoefficients& coefficients) noexcept {
            constexpr double inverse_q = 1.0 / LinkwitzRileyT<double>::exact_butterworth_q;
            const double v3 = input - state2;
            const double v1 = coefficients.a1 * state1 + coefficients.a2 * v3;
            const double v2 = state2 + coefficients.a2 * state1 + coefficients.a3 * v3;
            state1 = snap_to_zero(2.0 * v1 - state1);
            state2 = snap_to_zero(2.0 * v2 - state2);
            return {v2, input - inverse_q * v1 - v2};
        }

        void reset() noexcept {
            state1 = 0.0;
            state2 = 0.0;
        }

        double state1 = 0.0;
        double state2 = 0.0;
    };

    struct ModulatedLinkwitzRiley {
        LinkwitzRileyT<double>::BandSplit process(double input,
                                                  const TptCoefficients& coefficients) noexcept {
            const double low =
                low2.process(low1.process(input, coefficients).low, coefficients).low;
            const double high =
                high2.process(high1.process(input, coefficients).high, coefficients).high;
            return {low, high};
        }

        void reset() noexcept {
            low1.reset();
            low2.reset();
            high1.reset();
            high2.reset();
        }

        TptSection low1, low2, high1, high2;
    };

    struct Bank {
        std::array<ModulatedLinkwitzRiley, MaxBands - 1> splitters{};
        std::array<std::array<ModulatedLinkwitzRiley, MaxBands - 1>, MaxBands - 1>
            phase_compensators{};

        static void warp_cutoffs(std::span<const double> cutoffs, double sample_rate,
                                 std::array<double, MaxBands - 1>& destination) noexcept {
            for (std::size_t split = 0; split < cutoffs.size(); ++split) {
                destination[split] = std::tan(std::acos(-1.0) * cutoffs[split] / sample_rate);
            }
        }

        static SectionCoefficients coefficients_from_warped_cutoff(double warped) noexcept {
            const double square = warped * warped;
            const double inverse_q = 1.0 / LinkwitzRileyT<double>::exact_butterworth_q;
            const double normalization = 1.0 / (1.0 + inverse_q * warped + square);
            const double low_b0 = square * normalization;
            const double high_b0 = normalization;
            const double a1 = 2.0 * (square - 1.0) * normalization;
            const double a2 = (1.0 - inverse_q * warped + square) * normalization;
            return {{low_b0, 2.0 * low_b0, low_b0, a1, a2},
                    {high_b0, -2.0 * high_b0, high_b0, a1, a2}};
        }

        static void
        design_coefficients(const std::array<double, MaxBands - 1>& warped_cutoffs,
                            std::size_t cutoff_count,
                            std::array<TptCoefficients, MaxBands - 1>& destination) noexcept {
            for (std::size_t split = 0; split < cutoff_count; ++split) {
                constexpr double inverse_q = 1.0 / LinkwitzRileyT<double>::exact_butterworth_q;
                const double warped = warped_cutoffs[split];
                const double a1 = 1.0 / (1.0 + warped * (warped + inverse_q));
                destination[split] = {a1, warped * a1, warped * warped * a1};
            }
        }

        void reset() noexcept {
            for (auto& splitter : splitters)
                splitter.reset();
            for (auto& row : phase_compensators)
                for (auto& compensator : row)
                    compensator.reset();
        }

        std::array<double, MaxBands>
        process(double input, std::size_t cutoff_count,
                const std::array<TptCoefficients, MaxBands - 1>& coefficients) noexcept {
            std::array<double, MaxBands> bands{};
            double remainder = input;
            for (std::size_t split = 0; split < cutoff_count; ++split) {
                const auto divided = splitters[split].process(remainder, coefficients[split]);
                bands[split] = divided.low;
                remainder = divided.high;
            }
            bands[cutoff_count] = remainder;

            for (std::size_t band = 0; band < cutoff_count; ++band) {
                for (std::size_t split = band + 1; split < cutoff_count; ++split) {
                    const auto aligned =
                        phase_compensators[band][split].process(bands[band], coefficients[split]);
                    bands[band] = aligned.low + aligned.high;
                }
            }
            return bands;
        }
    };

    static bool valid_configuration(SampleType sample_rate,
                                    std::span<const SampleType> cutoffs) noexcept {
        if (!(std::isfinite(sample_rate) && sample_rate > SampleType{0}) || cutoffs.empty() ||
            cutoffs.size() >= MaxBands)
            return false;
        const SampleType nyquist = sample_rate * SampleType{0.5};
        SampleType previous = SampleType{0};
        for (const auto cutoff_hz : cutoffs) {
            if (!(std::isfinite(cutoff_hz) && cutoff_hz > previous && cutoff_hz < nyquist) ||
                !stable_section_design(static_cast<double>(cutoff_hz),
                                       static_cast<double>(sample_rate)))
                return false;
            previous = cutoff_hz;
        }
        return true;
    }

    static std::complex<double> section_response(const BiquadCoefficientsT<double>& c,
                                                 double frequency_hz, double sample_rate) noexcept {
        const double omega = 2.0 * std::acos(-1.0) * frequency_hz / sample_rate;
        const auto z1 = std::polar(1.0, -omega);
        const auto z2 = z1 * z1;
        return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    }

    static std::complex<double> split_response(SampleType cutoff_hz, double frequency_hz,
                                               double sample_rate, bool lowpass) noexcept {
        const double warped =
            std::tan(std::acos(-1.0) * static_cast<double>(cutoff_hz) / sample_rate);
        const auto coefficients = Bank::coefficients_from_warped_cutoff(warped);
        const auto once = section_response(lowpass ? coefficients.lowpass : coefficients.highpass,
                                           frequency_hz, sample_rate);
        return once * once;
    }

    static bool stable_section_design(double cutoff_hz, double sample_rate) noexcept {
        const double warped = std::tan(std::acos(-1.0) * cutoff_hz / sample_rate);
        const auto coefficients = Bank::coefficients_from_warped_cutoff(warped);
        const auto& lp = coefficients.lowpass;
        const auto& hp = coefficients.highpass;
        const auto finite = [](const auto& c) {
            return std::isfinite(c.b0) && std::isfinite(c.b1) && std::isfinite(c.b2) &&
                   std::isfinite(c.a1) && std::isfinite(c.a2);
        };
        if (!finite(lp) || !finite(hp) || !(lp.b0 > 0.0) || !(hp.b0 > 0.0))
            return false;

        const std::complex<double> discriminant{lp.a1 * lp.a1 - 4.0 * lp.a2, 0.0};
        const auto root = std::sqrt(discriminant);
        const double radius =
            std::max(std::abs((-lp.a1 + root) * 0.5), std::abs((-lp.a1 - root) * 0.5));
        constexpr double pole_margin = 64.0 * std::numeric_limits<double>::epsilon();
        return std::isfinite(radius) && radius < 1.0 - pole_margin;
    }

    static void copy_cutoffs(std::span<const SampleType> cutoffs,
                             std::array<double, MaxBands - 1>& destination) noexcept {
        for (std::size_t i = 0; i < cutoffs.size(); ++i)
            destination[i] = static_cast<double>(cutoffs[i]);
    }

    std::size_t minimum_transition_samples_from_warped(
        const std::array<double, MaxBands - 1>& target) const noexcept {
        // TPT state is invariant under coefficient changes, but a rapid downward
        // move can still release stored integrator energy into a much slower
        // pole. Limit that parameter motion explicitly. Signal peaks also depend
        // on prior input and are intentionally not described by this rate bound.
        double largest_downward_distance = 0.0;
        for (std::size_t split = 0; split < cutoff_count_; ++split) {
            if (target[split] < current_warped_cutoffs_[split]) {
                largest_downward_distance =
                    std::max(largest_downward_distance,
                             std::log(current_warped_cutoffs_[split] / target[split]));
            }
        }
        const double required = std::ceil(sample_rate_ * largest_downward_distance /
                                          maximum_downward_log_slew_nepers_per_second());
        if (!(std::isfinite(required) && required >= 0.0) ||
            required >= static_cast<double>(std::numeric_limits<std::size_t>::max()))
            return std::numeric_limits<std::size_t>::max();
        return static_cast<std::size_t>(required);
    }

    void advance_transition() noexcept {
        if (transition_length_ == 0)
            return;
        for (std::size_t split = 0; split < cutoff_count_; ++split)
            current_warped_cutoffs_[split] *= warped_cutoff_multipliers_[split];
        ++transition_position_;
        if (transition_position_ == transition_length_) {
            cutoffs_ = target_cutoffs_;
            current_warped_cutoffs_ = target_warped_cutoffs_;
            transition_position_ = 0;
            transition_length_ = 0;
        }
        Bank::design_coefficients(current_warped_cutoffs_, cutoff_count_, current_coefficients_);
    }

    void finish_transition() noexcept {
        if (transition_length_ == 0)
            return;
        cutoffs_ = target_cutoffs_;
        current_warped_cutoffs_ = target_warped_cutoffs_;
        Bank::design_coefficients(current_warped_cutoffs_, cutoff_count_, current_coefficients_);
        transition_position_ = 0;
        transition_length_ = 0;
    }

    Frame recover(Frame result) noexcept {
        finish_transition();
        bank_.reset();
        transition_position_ = 0;
        transition_length_ = 0;
        healthy_ = false;
        ++fault_count_;
        result.bands.fill(SampleType{0});
        result.healthy = false;
        return result;
    }

    Bank bank_{};
    std::array<double, MaxBands - 1> cutoffs_{};
    std::array<double, MaxBands - 1> target_cutoffs_{};
    std::array<double, MaxBands - 1> current_warped_cutoffs_{};
    std::array<double, MaxBands - 1> target_warped_cutoffs_{};
    std::array<double, MaxBands - 1> warped_cutoff_multipliers_{};
    std::array<TptCoefficients, MaxBands - 1> current_coefficients_{};
    double sample_rate_ = 0.0;
    std::size_t cutoff_count_ = 0;
    std::size_t transition_position_ = 0;
    std::size_t transition_length_ = 0;
    std::uint64_t fault_count_ = 0;
    bool prepared_ = false;
    bool healthy_ = false;
};

using LinkwitzRileyCrossover = LinkwitzRileyCrossoverT<float>;
using LinkwitzRileyCrossover64 = LinkwitzRileyCrossoverT<double>;

} // namespace pulp::signal
