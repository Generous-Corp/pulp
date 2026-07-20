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

    SampleType process(SampleType input) {
        // Non-linear feedback
        SampleType feedback =
            resonance_ * SampleType{4.0f} * (stage_[3] - input * SampleType{0.5f});
        SampleType x = input - feedback;

        // Cache each snapped stage's saturation. The next stage and next sample
        // reuse exactly that value, avoiding repeated transcendental calls.
        SampleType saturated_prev = saturate(x);
        for (int i = 0; i < 4; ++i) {
            // Snap each ladder stage: at high resonance the stages self-
            // oscillate and their tails otherwise decay into denormals with
            // no FTZ guard. No-op above 1e-15.
            stage_[i] = snap_to_zero(
                stage_[i] + g_ * (saturated_prev - saturated_stage_[i]));
            saturated_stage_[i] = saturate(stage_[i]);
            saturated_prev = saturated_stage_[i];
        }

        return stage_[3];
    }

    void process(SampleType* buffer, int num_samples) {
        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() {
        for (auto& s : stage_) s = 0;
        for (auto& s : saturated_stage_) s = 0;
    }

private:
    /// Saturating non-linearity. The `float` path goes through
    /// `signal::ladder_tanh`, which is exact `std::tanh` by default and switches
    /// to the float-only Padé `FastMath::tanh` only when
    /// `PULP_SIGNAL_FAST_LADDER_TANH=1` (see fast_math.hpp for the fidelity
    /// rationale — the Padé form's +/-4 hard clamp alters the ladder's overdrive
    /// character, so exact saturation is the default). `LadderFilterT<double>`
    /// always uses libm: no double Padé exists and it is not on a hot path.
    static SampleType saturate(SampleType x) {
        if constexpr (std::is_same_v<SampleType, float>)
            return ladder_tanh(x);
        else
            return std::tanh(x);
    }

    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType cutoff_ = SampleType{1000.0f};
    SampleType resonance_ = SampleType{0.0f};
    SampleType g_ = 0;
    SampleType stage_[4] = {};
    SampleType saturated_stage_[4] = {};

    void update() {
        g_ = SampleType{1.0f} -
             std::exp(SampleType{-2.0f} * SampleType{3.14159265f} *
                      cutoff_ / sample_rate_);
    }
};

using LadderFilter = LadderFilterT<float>;
using LadderFilter64 = LadderFilterT<double>;

} // namespace pulp::signal
