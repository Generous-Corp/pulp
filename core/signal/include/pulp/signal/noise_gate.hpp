#pragma once

#include <cmath>
#include <algorithm>

namespace pulp::signal {

// Noise gate / expander
// Attenuates signal below threshold with configurable attack/release.
//
// RT contract: setters, process paths, and reset are scalar-only and allocate
// no memory.
class NoiseGate {
public:
    struct Params {
        float threshold_db = -40.0f;
        float ratio = 10.0f;          // Expansion ratio (10:1 = gate)
        float attack_ms = 0.5f;
        float release_ms = 50.0f;
        float range_db = -80.0f;       // Max attenuation
    };

    NoiseGate() { recompute_coeffs(); }  // coeffs match the default params up front

    void set_params(const Params& p) {
        params_.threshold_db =
            std::isfinite(p.threshold_db) ? p.threshold_db : -40.0f;
        params_.ratio = std::isfinite(p.ratio) ? std::max(1.0f, p.ratio) : 1.0f;
        params_.attack_ms =
            std::isfinite(p.attack_ms) ? std::max(0.0f, p.attack_ms) : 0.0f;
        params_.release_ms =
            std::isfinite(p.release_ms) ? std::max(0.0f, p.release_ms) : 0.0f;
        params_.range_db =
            std::isfinite(p.range_db) ? std::min(0.0f, p.range_db) : -80.0f;
        recompute_coeffs();
    }

    void set_sample_rate(float sr) {
        sample_rate_ = (std::isfinite(sr) && sr > 0.0f) ? sr : 44100.0f;
        recompute_coeffs();
    }

    float process(float input) {
        float abs_in = std::abs(input);
        if (abs_in < 1e-10f) abs_in = 1e-10f;
        float input_db = 20.0f * std::log10(abs_in);

        float gain_db = 0.0f;
        if (input_db < params_.threshold_db) {
            // Expansion: apply ratio below threshold
            float below = params_.threshold_db - input_db;
            gain_db = -below * (params_.ratio - 1.0f);
            gain_db = std::max(gain_db, params_.range_db);
        }

        // Envelope follower — coefficients depend only on the attack/release times
        // and the sample rate, so they are cached in the setters (bit-identical to
        // recomputing the two exp() per sample, minus the per-sample transcendentals).
        float coeff = (gain_db < envelope_db_) ? attack_coeff_ : release_coeff_;
        envelope_db_ += coeff * (gain_db - envelope_db_);

        // Clamp to prevent overflow
        envelope_db_ = std::max(envelope_db_, params_.range_db);

        float gain_linear = std::pow(10.0f, envelope_db_ / 20.0f);
        return input * gain_linear;
    }

    void process(float* buffer, int num_samples) {
        if (buffer == nullptr || num_samples <= 0)
            return;

        for (int i = 0; i < num_samples; ++i)
            buffer[i] = process(buffer[i]);
    }

    void reset() { envelope_db_ = 0.0f; }

private:
    // Envelope attack/release one-pole coefficients, recomputed only when the
    // params or sample rate change (never per sample). Expression is identical to
    // the former inline computation, so results are bit-for-bit unchanged.
    void recompute_coeffs() {
        attack_coeff_ = params_.attack_ms > 0.0f
            ? 1.0f - std::exp(-1.0f / (params_.attack_ms * 0.001f * sample_rate_))
            : 1.0f;
        release_coeff_ = params_.release_ms > 0.0f
            ? 1.0f - std::exp(-1.0f / (params_.release_ms * 0.001f * sample_rate_))
            : 1.0f;
    }

    Params params_;
    float sample_rate_ = 44100.0f;
    float envelope_db_ = 0.0f;
    float attack_coeff_ = 1.0f;
    float release_coeff_ = 1.0f;
};

} // namespace pulp::signal
