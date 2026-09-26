#pragma once

/// @file oversampling_fir.hpp
/// Linear-phase 2x FIR stage used by the realtime oversampler.

#include <pulp/signal/fir_filter.hpp>
#include <pulp/signal/windowed_sinc_design.hpp>
#include <pulp/simd/simd.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

/// Pulp's low-latency linear-phase half-band design. Character-delay
/// hysteresis and the drum output stage share these exact constants so the
/// "house pair" cannot drift into two almost-identical filters.
inline constexpr std::size_t kHouseHalfBandTaps = 65;
inline constexpr double kHouseHalfBandStopbandDb = 81.3;  // Kaiser beta ~= 8
inline constexpr double kHouseHalfBandStageOnePassband = 0.45;
inline constexpr double kHouseHalfBandStageTwoPassband = 0.225;

namespace detail {

// Fixed-storage linear-phase half-band stage for nonlinear processors whose
// configuration and processing paths must both remain allocation-free. The
// general LinearPhaseOversamplingStage2x below supports selectable quality and
// tap counts; this stage deliberately fixes the 65-tap prototype so its state
// can live entirely inline.
//
// Half-band structure: apart from the centre, only odd-offset taps are
// non-zero, and an odd offset always reaches a sample of the opposite time
// parity. The stage therefore keeps one linear history per parity; each
// output is the centre tap on its own-parity history plus one contiguous
// 32-tap pulp::simd::dot over the other parity's history. Histories move
// their newest samples back to the front once every kCapacity writes.
class FixedHalfBandFir65 {
  public:
    FixedHalfBandFir65() noexcept {
        // Force the shared coefficient tables to initialize off the audio path.
        static_cast<void>(coefficients());
        static_cast<void>(odd_taps_oldest_first());
    }

    double process(double input) noexcept {
        auto& own = parity_[phase_];
        auto& other = parity_[phase_ ^ 1u];
        if (own.head == kHistory)
            own.compact();
        own.samples[own.head++] = input;
        // x[n - 32] is 16 own-parity samples back; x[n - 1 - 2j] for
        // j = 0..31 are the other parity's newest 32, oldest first.
        const double centre = coefficients()[kCentre] * own.samples[own.head - 1 - kCentre / 2];
        const double odd = pulp::simd::dot(other.samples.data() + other.head - kOddTaps,
                                           odd_taps_oldest_first().data(), kOddTaps);
        phase_ ^= 1u;
        return centre + odd;
    }

    void reset() noexcept {
        for (auto& p : parity_) {
            p.samples.fill(0.0);
            p.head = kKeep;
        }
        phase_ = 0;
    }

    /// The 65 prototype taps (symmetric; centre index 32; even offsets zero).
    static const std::array<double, 65>& coefficients() noexcept {
        static const std::array<double, kTaps> result = design();
        return result;
    }

  private:
    static constexpr std::size_t kTaps = 65;
    static constexpr std::size_t kCentre = (kTaps - 1) / 2;
    static constexpr std::size_t kOddTaps = (kTaps - 1) / 2;
    // Each parity history must reach back kOddTaps samples (the dot) and
    // kCentre / 2 + 1 samples (the centre tap).
    static constexpr std::size_t kKeep = kOddTaps;
    static constexpr std::size_t kCapacity = 64;
    static constexpr std::size_t kHistory = kKeep + kCapacity;
    static constexpr double kBeta = 8.0;

    static double sinc(double x) noexcept {
        if (std::abs(x) < 1.0e-15)
            return 1.0;
        constexpr double kPi = 3.141592653589793238462643383279502884;
        const double pix = kPi * x;
        return std::sin(pix) / pix;
    }

    static std::array<double, kTaps> design() noexcept {
        std::array<double, kTaps> result{};
        const double denominator = bessel_i0(kBeta);
        double sum = 0.0;
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            const int offset =
                static_cast<int>(tap) - static_cast<int>(kCentre);
            if (offset != 0 && (offset & 1) == 0) {
                result[tap] = 0.0;
                continue;
            }
            const double position =
                static_cast<double>(offset) / static_cast<double>(kCentre);
            const double window =
                bessel_i0(kBeta * std::sqrt(
                                      std::max(0.0, 1.0 - position * position))) /
                denominator;
            result[tap] =
                0.5 * sinc(0.5 * static_cast<double>(offset)) * window;
            sum += result[tap];
        }
        for (double& coefficient : result)
            coefficient /= sum;
        return result;
    }

    // Odd-offset taps t = 63, 61, ..., 1 (oldest sample first).
    static const std::array<double, kOddTaps>& odd_taps_oldest_first() noexcept {
        static const std::array<double, kOddTaps> result = [] {
            std::array<double, kOddTaps> taps{};
            for (std::size_t j = 0; j < kOddTaps; ++j)
                taps[j] = coefficients()[kTaps - 2 - 2 * j];
            return taps;
        }();
        return result;
    }

    struct ParityHistory {
        // [0, head) holds past same-parity input, oldest first; head >= kKeep.
        std::array<double, kHistory> samples{};
        std::size_t head = kKeep;

        void compact() noexcept {
            std::copy(samples.begin() + static_cast<std::ptrdiff_t>(head - kKeep),
                      samples.begin() + static_cast<std::ptrdiff_t>(head), samples.begin());
            head = kKeep;
        }
    };

    std::array<ParityHistory, 2> parity_{};
    unsigned phase_ = 0;
};

template <typename SampleType> class LinearPhaseOversamplingStage2x {
  public:
    void configure(double passband_edge_fraction, double stopband_db, std::size_t taps) {
        // Fractions are expressed against the input rate. This filter runs at
        // twice that rate, so base Nyquist is 0.25 cycles/output-sample.
        const double passband = 0.5 * passband_edge_fraction;
        constexpr double stopband = 0.25;
        const double transition = stopband - passband;
        const double cutoff = passband + 0.5 * transition;
        const double beta = kaiser_beta_for_stopband(stopband_db);
        auto prototype = design_windowed_sinc(taps, cutoff, beta);

        std::vector<SampleType> up_even;
        std::vector<SampleType> up_odd;
        std::vector<SampleType> down_even;
        std::vector<SampleType> down_odd;
        up_even.reserve((prototype.size() + 1) / 2);
        up_odd.reserve(prototype.size() / 2);
        down_even.reserve((prototype.size() + 1) / 2);
        down_odd.reserve(prototype.size() / 2);
        for (std::size_t i = 0; i < prototype.size(); ++i) {
            const auto coefficient = static_cast<SampleType>(prototype[i]);
            auto& up_phase = (i & 1u) == 0u ? up_even : up_odd;
            auto& down_phase = (i & 1u) == 0u ? down_even : down_odd;
            up_phase.push_back(SampleType{2} * coefficient);
            down_phase.push_back(coefficient);
        }
        up_even_.set_coefficients(std::move(up_even));
        up_odd_.set_coefficients(std::move(up_odd));
        down_even_.set_coefficients(std::move(down_even));
        down_odd_.set_coefficients(std::move(down_odd));
        taps_ = taps;
    }

    void reset() {
        up_even_.reset();
        up_odd_.reset();
        down_even_.reset();
        down_odd_.reset();
        previous_odd_ = SampleType{0};
    }

    void upsample(SampleType input, SampleType& even, SampleType& odd) {
        even = up_even_.process(input);
        odd = up_odd_.process(input);
    }

    SampleType downsample(SampleType even, SampleType odd) {
        const SampleType output = down_even_.process(even) + down_odd_.process(previous_odd_);
        previous_odd_ = odd;
        return output;
    }

    std::size_t taps() const noexcept {
        return taps_;
    }

  private:
    FirFilterT<SampleType> up_even_;
    FirFilterT<SampleType> up_odd_;
    FirFilterT<SampleType> down_even_;
    FirFilterT<SampleType> down_odd_;
    SampleType previous_odd_ = SampleType{0};
    std::size_t taps_ = 0;
};

} // namespace detail

} // namespace pulp::signal
