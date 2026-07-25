#pragma once

// Granular octave-up shifter, one per tank line, injected into the FEEDBACK
// path.
//
// Injecting into the feedback rather than the output is what makes shimmer
// bloom: each pass shifts what the previous pass already shifted, so the octave
// stack builds instead of sitting as a single fixed layer above the tail. It is
// also exactly why the injection weight must be energy-normalized and counted
// in the stability bound — an unnormalized recirculating shifter is a runaway.
//
// The shift ratio is fixed at 2x. One good interval, no ratio parameter: a
// variable ratio buys artifacts at every non-octave setting and buys nothing
// musical here.
//
// Four Hann grains staggered by a quarter period sum to a constant envelope
// (75% overlap), so the output has no amplitude ripple. Grain SIZE varies per
// channel across a 40-60 ms span, which is the cheap trick that makes sixteen
// mediocre shifters sound like one good one: their grain-boundary artifacts
// land at sixteen different periods instead of stacking into one buzz.
//
// The grains' READ offsets are deliberately NOT the same quarter-period stagger
// as their window phases. With both equal, the four taps are coherent copies of
// the source at fixed relative phases, and at the frequencies where those four
// phasors sum to zero the shifter outputs nothing at all — a measured total
// null, not a dip. Decoupling the read offset (pseudo-random, fixed per
// channel) from the window phase (which must stay at a quarter period for the
// windows to sum flat) breaks the structure that makes those nulls line up.
//
// RT contract: prepare() allocates; process/reset allocate nothing.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/modulation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace pulp::signal::fdn {

template <typename SampleType = float>
class ShimmerBank {
public:
    // Sized for the longest grain at the highest tank rate; configure() only
    // re-derives grain lengths.
    void prepare(double max_sample_rate) {
        const int cap =
            static_cast<int>(kShimmerGrainMaxMs * 0.001 * max_sample_rate) + kHermiteGuard;
        for (auto& line : lines_) line.prepare(cap * 2);
    }

    void configure(double tank_rate) {
        for (int i = 0; i < kNumChannels; ++i) {
            const double t = (kNumChannels > 1)
                                 ? static_cast<double>(i) /
                                       static_cast<double>(kNumChannels - 1)
                                 : 0.0;
            const double ms =
                kShimmerGrainMinMs + t * (kShimmerGrainMaxMs - kShimmerGrainMinMs);
            grain_[i] = std::max(16.0, ms * 0.001 * tank_rate);
            XorShift32 rng(kOffsetSeed + static_cast<std::uint32_t>(i) * kOffsetStride);
            for (int g = 0; g < kNumShimmerGrains; ++g)
                read_offset_[static_cast<std::size_t>(i)][static_cast<std::size_t>(g)] =
                    rng.next_unit() * grain_[i];
        }
        // One-pole lowpass after the shift. The shifter's artifacts live in the
        // top octave it creates; rolling them off there keeps the octave stack
        // from turning into hiss after a dozen passes.
        const double w = 6.283185307179586 * kShimmerToneHz / tank_rate;
        tone_coeff_ = std::exp(-w);
        reset_phases();
    }

    void reset() {
        for (auto& line : lines_) line.reset();
        for (auto& z : tone_state_) z = 0.0;
        reset_phases();
    }

    // Push one sample per channel and read back its octave-up image.
    SampleType process(int channel, SampleType x) {
        FracDelayT<SampleType>& line = lines_[static_cast<std::size_t>(channel)];
        line.push(x);

        const double period = grain_[static_cast<std::size_t>(channel)];
        if (period < 16.0) return SampleType{0};  // not configured yet
        double age = phase_[static_cast<std::size_t>(channel)];
        double sum = 0.0;
        for (int g = 0; g < kNumShimmerGrains; ++g) {
            // Grains are staggered by a quarter period; each one's read delay
            // falls from `period` to 0 over its life, which advances the read
            // pointer at 2x while the write pointer advances at 1x.
            double a = age + static_cast<double>(g) * period / kNumShimmerGrains;
            if (a >= period) a -= period;
            const double delay =
                period - a +
                read_offset_[static_cast<std::size_t>(channel)][static_cast<std::size_t>(g)];
            const double window =
                0.5 - 0.5 * std::cos(6.283185307179586 * a / period);
            sum += window * static_cast<double>(line.read(delay + 2.0));
        }
        // Four Hann grains at 75% overlap sum to 2.0.
        sum *= 0.5;

        age += 1.0;
        if (age >= period) age -= period;
        phase_[static_cast<std::size_t>(channel)] = age;

        double& z = tone_state_[static_cast<std::size_t>(channel)];
        z = snap_to_zero(sum * (1.0 - tone_coeff_) + z * tone_coeff_);
        return static_cast<SampleType>(z);
    }

    // The injection weight that keeps worst-case loop energy bounded: the
    // per-line contribution is divided across the active lines so sixteen
    // shifters inject the same total energy one would.
    static double injection_weight(double shimmer, int active_n) {
        const double n = static_cast<double>(std::max(active_n, 1));
        return std::clamp(shimmer, 0.0, 1.0) * kShimmerMaxWeight / std::sqrt(n);
    }

private:
    void reset_phases() {
        for (int i = 0; i < kNumChannels; ++i) {
            // Stagger the channels' grain clocks as well as the grains within a
            // channel, so no two lines ever cross a grain boundary together.
            phase_[static_cast<std::size_t>(i)] =
                grain_[static_cast<std::size_t>(i)] * static_cast<double>(i) /
                static_cast<double>(kNumChannels);
        }
    }

    static constexpr std::uint32_t kOffsetSeed = 0xA5A5F00Du;
    static constexpr std::uint32_t kOffsetStride = 0x27D4EB2Fu;

    std::array<FracDelayT<SampleType>, kNumChannels> lines_{};
    std::array<std::array<double, kNumShimmerGrains>, kNumChannels> read_offset_{};
    std::array<double, kNumChannels> grain_{};
    std::array<double, kNumChannels> phase_{};
    std::array<double, kNumChannels> tone_state_{};
    double tone_coeff_ = 0.0;
};

}  // namespace pulp::signal::fdn
