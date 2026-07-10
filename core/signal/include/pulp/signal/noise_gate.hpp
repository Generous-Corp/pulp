#pragma once

#include <pulp/signal/denormal.hpp>

#include <cmath>
#include <algorithm>

namespace pulp::signal {

// Noise gate / expander
// Attenuates signal below threshold with configurable attack/release.
//
// RT contract: setters, process paths, and reset are scalar-only and allocate
// no memory.
template <typename SampleType = float>
class NoiseGateT {
public:
    struct Params {
        SampleType threshold_db = SampleType{-40.0f};
        SampleType ratio = SampleType{10.0f};          // Expansion ratio (10:1 = gate)
        SampleType attack_ms = SampleType{0.5f};
        SampleType release_ms = SampleType{50.0f};
        SampleType range_db = SampleType{-80.0f};       // Max attenuation
    };

    NoiseGateT() { recompute_coeffs(); }  // coeffs match the default params up front

    void set_params(const Params& p) {
        params_.threshold_db =
            std::isfinite(p.threshold_db) ? p.threshold_db : SampleType{-40.0f};
        params_.ratio = std::isfinite(p.ratio)
            ? std::max(SampleType{1.0f}, p.ratio)
            : SampleType{1.0f};
        params_.attack_ms =
            std::isfinite(p.attack_ms)
                ? std::max(SampleType{0.0f}, p.attack_ms)
                : SampleType{0.0f};
        params_.release_ms =
            std::isfinite(p.release_ms)
                ? std::max(SampleType{0.0f}, p.release_ms)
                : SampleType{0.0f};
        params_.range_db =
            std::isfinite(p.range_db)
                ? std::min(SampleType{0.0f}, p.range_db)
                : SampleType{-80.0f};
        recompute_coeffs();
    }

    void set_sample_rate(SampleType sr) {
        sample_rate_ = (std::isfinite(sr) && sr > SampleType{0.0f})
            ? sr
            : SampleType{44100.0f};
        recompute_coeffs();
    }

    SampleType process(SampleType input) {
        SampleType abs_in = std::abs(input);
        if (abs_in < SampleType{1e-10f}) abs_in = SampleType{1e-10f};
        SampleType input_db = SampleType{20.0f} * std::log10(abs_in);

        SampleType gain_db = SampleType{0.0f};
        if (input_db < params_.threshold_db) {
            // Expansion: apply ratio below threshold
            SampleType below = params_.threshold_db - input_db;
            gain_db = -below * (params_.ratio - SampleType{1.0f});
            gain_db = std::max(gain_db, params_.range_db);
        }

        // Envelope follower — coefficients depend only on the attack/release times
        // and the sample rate, so they are cached in the setters (bit-identical to
        // recomputing the two exp() per sample, minus the per-sample transcendentals).
        SampleType coeff = (gain_db < envelope_db_) ? attack_coeff_ : release_coeff_;
        envelope_db_ += coeff * (gain_db - envelope_db_);

        // Clamp to prevent overflow
        envelope_db_ = std::max(envelope_db_, params_.range_db);
        // Snap the recursive envelope: a fully-open gate settles toward 0 dB and
        // otherwise stalls in the denormal range with no FTZ guard. No-op above
        // 1e-15.
        envelope_db_ = snap_to_zero(envelope_db_);

        SampleType gain_linear =
            std::pow(SampleType{10.0f}, envelope_db_ / SampleType{20.0f});
        return input * gain_linear;
    }

    void process(SampleType* buffer, int num_samples) {
        if (buffer == nullptr || num_samples <= 0)
            return;

        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() { envelope_db_ = SampleType{0.0f}; }

private:
    // Envelope attack/release one-pole coefficients, recomputed only when the
    // params or sample rate change (never per sample). Expression is identical to
    // the former inline computation, so results are bit-for-bit unchanged.
    void recompute_coeffs() {
        attack_coeff_ = params_.attack_ms > SampleType{0.0f}
            ? SampleType{1.0f} -
                  std::exp(SampleType{-1.0f} /
                           (params_.attack_ms * SampleType{0.001f} * sample_rate_))
            : SampleType{1.0f};
        release_coeff_ = params_.release_ms > SampleType{0.0f}
            ? SampleType{1.0f} -
                  std::exp(SampleType{-1.0f} /
                           (params_.release_ms * SampleType{0.001f} * sample_rate_))
            : SampleType{1.0f};
    }

    Params params_;
    SampleType sample_rate_ = SampleType{44100.0f};
    SampleType envelope_db_ = SampleType{0.0f};
    SampleType attack_coeff_ = SampleType{1.0f};
    SampleType release_coeff_ = SampleType{1.0f};
};

using NoiseGate = NoiseGateT<float>;
using NoiseGate64 = NoiseGateT<double>;

} // namespace pulp::signal
