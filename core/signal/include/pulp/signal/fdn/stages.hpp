#pragma once

// The stages that bracket the tank: the transient ducker on the way in, and
// the width / ensemble / limiter chain on the way out.
//
// Both live INSIDE the tank-rate domain (see the engine's block diagram). That
// is deliberate for the output chain: at a 20 kHz tank the ensemble's own
// bandwidth is part of the low-rate texture, and moving it to the host rate
// would sand off exactly the character the tank rate exists to provide.
//
// RT contract: prepare() allocates; process/reset allocate nothing.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/modulation.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal::fdn {

// Padé tanh. Its derivative is (x^2-9)^2 / (9(3+x^2)^2), which is 1 at the
// origin and strictly below 1 everywhere else — so it is 1-Lipschitz and adds
// no energy at any drive setting. That property is why the loop saturator is a
// crossfade onto THIS curve rather than tanh(k*x): a k above 1 raises the
// small-signal gain, which silently raises the loop gain with it.
inline double fast_tanh(double x) {
    const double x2 = x * x;
    return x * (27.0 + x2) / (27.0 + 9.0 * x2);
}

// Transient ducker on the INPUT. Attacks excite the reverb less, sustains
// excite it fully, which is what keeps a groove feeling dry under a big tail.
// The ratio is normalized by the slow envelope, so the duck responds to the
// SHAPE of the input, not its level.
class TransientDucker {
public:
    void configure(double sample_rate) {
        fast_coeff_ = coeff_for(kDuckFastMs, sample_rate);
        slow_coeff_ = coeff_for(kDuckSlowMs, sample_rate);
    }

    void reset() {
        fast_ = 0.0;
        slow_ = 0.0;
    }

    // Returns the input gain to apply, in (0, 1].
    double process(double magnitude) {
        fast_ = snap_to_zero(fast_ + (magnitude - fast_) * fast_coeff_);
        slow_ = snap_to_zero(slow_ + (magnitude - slow_) * slow_coeff_);
        const double ratio = (fast_ - slow_) / std::max(slow_, 1e-9);
        if (ratio <= 0.0) return 1.0;
        const double t = std::min(ratio / kDuckKnee, 1.0);
        const double smooth = t * t * (3.0 - 2.0 * t);
        return 1.0 - kDuckAmount * smooth;
    }

    double fast_envelope() const { return fast_; }
    double slow_envelope() const { return slow_; }

private:
    static double coeff_for(double ms, double sample_rate) {
        const double n = std::max(1.0, ms * 0.001 * sample_rate);
        return 1.0 - std::exp(-1.0 / n);
    }

    double fast_coeff_ = 0.0;
    double slow_coeff_ = 0.0;
    double fast_ = 0.0;
    double slow_ = 0.0;
};

// Six-LFO stereo ensemble. The L and R taps of each LFO sit 90 degrees apart,
// which is what turns six modulated delays into a wide, soft gloss instead of
// six audible chorus voices.
template <typename SampleType = float>
class EnsembleChorus {
public:
    void prepare(double max_sample_rate) {
        const int cap = static_cast<int>((kEnsembleBaseMs + kEnsembleDepthMs * 2.0) *
                                         0.001 * max_sample_rate) +
                        kHermiteGuard;
        left_.prepare(cap);
        right_.prepare(cap);
    }

    void configure(double sample_rate) {
        base_samples_ = kEnsembleBaseMs * 0.001 * sample_rate;
        depth_samples_ = kEnsembleDepthMs * 0.001 * sample_rate;
        for (int i = 0; i < kNumEnsembleLfos; ++i) {
            const double t = static_cast<double>(i) /
                             static_cast<double>(kNumEnsembleLfos - 1);
            const double hz =
                kEnsembleRateMinHz + t * (kEnsembleRateMaxHz - kEnsembleRateMinHz);
            lfo_[i].set_rate(hz, sample_rate);
        }
    }

    void reset() {
        left_.reset();
        right_.reset();
        for (int i = 0; i < kNumEnsembleLfos; ++i)
            lfo_[i].reset(static_cast<double>(i) /
                          static_cast<double>(kNumEnsembleLfos));
    }

    void process(SampleType& l, SampleType& r) {
        left_.push(l);
        right_.push(r);
        double sum_l = 0.0;
        double sum_r = 0.0;
        for (int i = 0; i < kNumEnsembleLfos; ++i) {
            const double s = lfo_[i].tick();
            // 90 degrees of phase between the rails: sin -> cos.
            const double c = std::cos(lfo_[i].phase() * 6.283185307179586);
            sum_l += static_cast<double>(left_.read(base_samples_ + depth_samples_ * s));
            sum_r += static_cast<double>(right_.read(base_samples_ + depth_samples_ * c));
        }
        const double norm = 1.0 / static_cast<double>(kNumEnsembleLfos);
        l = static_cast<SampleType>(static_cast<double>(l) * (1.0 - kEnsembleWetMix) +
                                    sum_l * norm * kEnsembleWetMix);
        r = static_cast<SampleType>(static_cast<double>(r) * (1.0 - kEnsembleWetMix) +
                                    sum_r * norm * kEnsembleWetMix);
    }

private:
    FracDelayT<SampleType> left_;
    FracDelayT<SampleType> right_;
    std::array<PhaseOsc, kNumEnsembleLfos> lfo_{};
    double base_samples_ = 0.0;
    double depth_samples_ = 0.0;
};

// Mid/side width. width 0 collapses to mono, 1 leaves the tank's natural
// stereo alone, and the tank's own even/odd fan-out already carries most of
// the separation — this only adjusts it.
inline void apply_width(double& l, double& r, double width) {
    const double mid = 0.5 * (l + r);
    const double side = 0.5 * (l - r) * width;
    l = mid + side;
    r = mid - side;
}

// Last-resort wet limiter. At normal levels the curve is within a fraction of a
// dB of unity; it exists so a pathological parameter combination clips softly
// instead of squaring off. This one uses the real tanh rather than the Padé
// approximation: it sits OUTSIDE the recursion, where the cost of one
// transcendental per sample is irrelevant and an actually-bounded output
// matters more than the 1-Lipschitz property the loop needs.
inline double soft_limit(double x) {
    return kWetLimiterHeadroom * std::tanh(x / kWetLimiterHeadroom);
}

}  // namespace pulp::signal::fdn
