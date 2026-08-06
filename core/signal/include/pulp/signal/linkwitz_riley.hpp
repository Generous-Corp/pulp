#pragma once

#include <pulp/signal/biquad.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
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

    void set_frequency(SampleType hz, SampleType sample_rate) {
        set_frequency_with_q(hz, sample_rate, legacy_butterworth_q);
    }

    /// Exact Butterworth alignment for new LR4 constructions. The two-argument
    /// overload retains its historical rounded-Q coefficients for render
    /// compatibility with existing consumers.
    void set_frequency_precise(SampleType hz, SampleType sample_rate) {
        set_frequency_with_q(hz, sample_rate, exact_butterworth_q);
    }

  private:
    void set_frequency_with_q(SampleType hz, SampleType sample_rate, SampleType q) {
        lp1_.set_coefficients(BiquadT<SampleType>::Type::lowpass, hz, q, sample_rate);
        lp2_.set_coefficients(BiquadT<SampleType>::Type::lowpass, hz, q, sample_rate);
        hp1_.set_coefficients(BiquadT<SampleType>::Type::highpass, hz, q, sample_rate);
        hp2_.set_coefficients(BiquadT<SampleType>::Type::highpass, hz, q, sample_rate);
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
/// a realtime retune may change only the existing ordered cutoffs. A smoothed
/// retune runs two fixed banks in parallel and rejects another retune until the
/// requested transition has completed.
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
        copy_cutoffs(cutoffs);
        banks_[0].configure(cutoffs, sample_rate_);
        banks_[1].configure(cutoffs, sample_rate_);
        banks_[0].reset();
        banks_[1].reset();
        active_bank_ = 0;
        transition_position_ = 0;
        transition_length_ = 0;
        prepared_ = true;
        healthy_ = true;
        return true;
    }

    /// Retune all cutoffs together. A zero-length transition updates the active
    /// coefficients at the next call site. A nonzero transition crossfades two
    /// complete, phase-aligned banks for exactly transition_samples samples.
    bool set_cutoffs(std::span<const SampleType> cutoffs,
                     std::size_t transition_samples = 0) noexcept {
        if (!prepared_ || transition_length_ != 0 || cutoffs.size() != cutoff_count_ ||
            !valid_configuration(sample_rate_, cutoffs))
            return false;

        copy_cutoffs(cutoffs);
        if (transition_samples == 0) {
            banks_[active_bank_].configure(cutoffs, sample_rate_);
            return true;
        }

        const std::size_t next = active_bank_ ^ std::size_t{1};
        banks_[next].configure(cutoffs, sample_rate_);
        banks_[next].reset();
        transition_position_ = 0;
        transition_length_ = transition_samples;
        return true;
    }

    Frame process(SampleType input) noexcept {
        Frame result;
        result.count = prepared_ ? cutoff_count_ + 1 : 0;
        if (!prepared_ || !std::isfinite(input))
            return recover(result);

        auto old_frame = banks_[active_bank_].process(input, cutoff_count_);
        if (transition_length_ == 0) {
            result.bands = old_frame;
        } else {
            const std::size_t next = active_bank_ ^ std::size_t{1};
            const auto new_frame = banks_[next].process(input, cutoff_count_);
            const SampleType mix = static_cast<SampleType>(transition_position_ + 1) /
                                   static_cast<SampleType>(transition_length_);
            const SampleType dry = SampleType{1} - mix;
            for (std::size_t band = 0; band <= cutoff_count_; ++band)
                result.bands[band] = dry * old_frame[band] + mix * new_frame[band];

            ++transition_position_;
            if (transition_position_ == transition_length_) {
                active_bank_ = next;
                transition_position_ = 0;
                transition_length_ = 0;
            }
        }

        for (std::size_t band = 0; band < result.count; ++band) {
            if (!std::isfinite(result.bands[band]))
                return recover(result);
        }
        healthy_ = true;
        result.healthy = true;
        return result;
    }

    void reset() noexcept {
        if (transition_length_ != 0)
            active_bank_ ^= std::size_t{1};
        banks_[0].reset();
        banks_[1].reset();
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
        return index < cutoff_count_ ? cutoffs_[index] : SampleType{0};
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
    static constexpr int latency_samples() noexcept {
        return 0;
    }

    /// Exact complex response of one configured band, including the all-pass
    /// compensation that makes the complete band sum flat.
    static std::complex<double> band_response(std::span<const SampleType> cutoffs, std::size_t band,
                                              double frequency_hz, double sample_rate) noexcept {
        if (band > cutoffs.size() ||
            !valid_configuration(static_cast<SampleType>(sample_rate), cutoffs) ||
            !(std::isfinite(frequency_hz) && frequency_hz >= 0.0))
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
    struct Bank {
        std::array<LinkwitzRileyT<SampleType>, MaxBands - 1> splitters{};
        std::array<std::array<LinkwitzRileyT<SampleType>, MaxBands - 1>, MaxBands - 1>
            phase_compensators{};

        void configure(std::span<const SampleType> cutoffs, SampleType sample_rate) noexcept {
            for (std::size_t split = 0; split < cutoffs.size(); ++split) {
                splitters[split].set_frequency_precise(cutoffs[split], sample_rate);
                for (std::size_t band = 0; band < split; ++band)
                    phase_compensators[band][split].set_frequency_precise(cutoffs[split],
                                                                          sample_rate);
            }
        }

        void reset() noexcept {
            for (auto& splitter : splitters)
                splitter.reset();
            for (auto& row : phase_compensators)
                for (auto& compensator : row)
                    compensator.reset();
        }

        std::array<SampleType, MaxBands> process(SampleType input,
                                                 std::size_t cutoff_count) noexcept {
            std::array<SampleType, MaxBands> bands{};
            SampleType remainder = input;
            for (std::size_t split = 0; split < cutoff_count; ++split) {
                const auto divided = splitters[split].process(remainder);
                bands[split] = divided.low;
                remainder = divided.high;
            }
            bands[cutoff_count] = remainder;

            for (std::size_t band = 0; band < cutoff_count; ++band) {
                for (std::size_t split = band + 1; split < cutoff_count; ++split) {
                    const auto aligned = phase_compensators[band][split].process(bands[band]);
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
            if (!(std::isfinite(cutoff_hz) && cutoff_hz > previous && cutoff_hz < nyquist))
                return false;
            previous = cutoff_hz;
        }
        return true;
    }

    static std::complex<double> section_response(const BiquadCoefficientsT<double>& c,
                                                 double frequency_hz, double sample_rate) noexcept {
        const double omega =
            std::clamp(2.0 * std::acos(-1.0) * frequency_hz / sample_rate, 0.0, std::acos(-1.0));
        const auto z1 = std::polar(1.0, -omega);
        const auto z2 = z1 * z1;
        return (c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2);
    }

    static std::complex<double> split_response(SampleType cutoff_hz, double frequency_hz,
                                               double sample_rate, bool lowpass) noexcept {
        BiquadT<double> section;
        section.set_coefficients(
            lowpass ? BiquadT<double>::Type::lowpass : BiquadT<double>::Type::highpass,
            static_cast<double>(cutoff_hz),
            static_cast<double>(LinkwitzRileyT<SampleType>::exact_butterworth_q), sample_rate);
        const auto once = section_response(section.coefficients(), frequency_hz, sample_rate);
        return once * once;
    }

    void copy_cutoffs(std::span<const SampleType> cutoffs) noexcept {
        for (std::size_t i = 0; i < cutoffs.size(); ++i)
            cutoffs_[i] = cutoffs[i];
    }

    Frame recover(Frame result) noexcept {
        if (transition_length_ != 0)
            active_bank_ ^= std::size_t{1};
        banks_[0].reset();
        banks_[1].reset();
        transition_position_ = 0;
        transition_length_ = 0;
        healthy_ = false;
        ++fault_count_;
        result.bands.fill(SampleType{0});
        result.healthy = false;
        return result;
    }

    std::array<Bank, 2> banks_{};
    std::array<SampleType, MaxBands - 1> cutoffs_{};
    SampleType sample_rate_ = SampleType{0};
    std::size_t cutoff_count_ = 0;
    std::size_t active_bank_ = 0;
    std::size_t transition_position_ = 0;
    std::size_t transition_length_ = 0;
    std::uint64_t fault_count_ = 0;
    bool prepared_ = false;
    bool healthy_ = false;
};

using LinkwitzRileyCrossover = LinkwitzRileyCrossoverT<float>;
using LinkwitzRileyCrossover64 = LinkwitzRileyCrossoverT<double>;

} // namespace pulp::signal
