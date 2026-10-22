#pragma once

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fast_math.hpp>

#include <cmath>
#include <algorithm>
#include <type_traits>

namespace pulp::signal {

// Nonlinear ladder filter (4-pole, 24 dB/octave)
// Non-linear, self-oscillating at high resonance.
//
// RT contract: setters, process paths, and reset are scalar/fixed-array only
// and allocate no memory.
template <typename SampleType = float>
class LadderFilterT {
public:
    void set_sample_rate(SampleType sr) { sample_rate_ = sr; }
    void set_frequency(SampleType hz) { cutoff_ = hz; update(); }
    void set_resonance(SampleType r) {
        resonance_ = std::clamp(r, SampleType{0.0f}, SampleType{1.0f});
        update();
    }

    /// Process one sample with the default saturator (see LadderSaturation).
    SampleType process(SampleType input) {
        return process_with_profile<kDefaultLadderSaturation>(input);
    }

    /// Process one sample with exact std::tanh stages.
    SampleType process_reference(SampleType input) {
        return process_with_profile<LadderSaturation::exact>(input);
    }

    /// Process one sample with the Padé `FastMath::tanh` stages.
    SampleType process_realtime_efficient(SampleType input) {
        return process_with_profile<LadderSaturation::realtime_efficient>(input);
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() {
        for (auto& s : stage_) s = 0;
        for (auto& s : saturated_stage_) s = 0;
    }

    /// One-pole stage coefficient for a cutoff and sample rate:
    /// g = 1 - exp(-2 pi fc / fs). Each stage moves g of the way from its
    /// saturated state toward its saturated input per sample.
    static SampleType stage_coefficient(SampleType cutoff_hz, SampleType sample_rate) {
        return SampleType{1.0f} -
               std::exp(SampleType{-2.0f} * SampleType{3.14159265f} * cutoff_hz / sample_rate);
    }

    /// Saturating non-linearity. The `float` path goes through
    /// `signal::ladder_tanh_profiled` (see LadderSaturation in fast_math.hpp
    /// for the choices and their measured deviation from exact tanh).
    /// `LadderFilterT<double>` always uses libm: the float approximations do
    /// not reach double precision and it is not on a hot path.
    template <LadderSaturation Saturation>
    static SampleType saturate(SampleType x) {
        if constexpr (std::is_same_v<SampleType, float>)
            return ladder_tanh_profiled<Saturation>(x);
        else
            return std::tanh(x);
    }

private:
    template <LadderSaturation Saturation>
    SampleType process_with_profile(SampleType input) {
        // Non-linear feedback
        SampleType feedback =
            resonance_ * SampleType{4.0f} * (stage_[3] - input * SampleType{0.5f});
        SampleType x = input - feedback;

        // Cache each snapped stage's saturation. The next stage and next sample
        // reuse exactly that value, avoiding repeated transcendental calls.
        SampleType saturated_prev = saturate<Saturation>(x);
        for (int i = 0; i < 4; ++i) {
            // Snap each ladder stage: at high resonance the stages self-
            // oscillate and their tails otherwise decay into denormals with
            // no FTZ guard. No-op above 1e-15.
            stage_[i] = snap_to_zero(
                stage_[i] + g_ * (saturated_prev - saturated_stage_[i]));
            saturated_stage_[i] = saturate<Saturation>(stage_[i]);
            saturated_prev = saturated_stage_[i];
        }

        return stage_[3];
    }

    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType cutoff_ = SampleType{1000.0f};
    SampleType resonance_ = SampleType{0.0f};
    SampleType g_ = 0;
    SampleType stage_[4] = {};
    SampleType saturated_stage_[4] = {};

    void update() { g_ = stage_coefficient(cutoff_, sample_rate_); }
};

using LadderFilter = LadderFilterT<float>;
using LadderFilter64 = LadderFilterT<double>;

} // namespace pulp::signal
