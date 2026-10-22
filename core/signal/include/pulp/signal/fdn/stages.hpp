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
#include <pulp/signal/fast_math.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/modulation.hpp>
#include <pulp/signal/fdn/sanitize.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal::fdn {

// The loop saturator's curve. See signal::lipschitz_tanh for why it is this
// one — 1-Lipschitz so it can never raise the loop gain, and bounded so a
// freeze-regime tail settles rather than merely decaying slower than it grows.
// The character delay derived the same curve for its dub feedback; it lives in
// fast_math.hpp now so there is one definition to reason about.
using signal::lipschitz_tanh;

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
        magnitude = finite_or_zero(magnitude);
        fast_ = snap_to_zero(fast_ + (magnitude - fast_) * fast_coeff_);
        slow_ = snap_to_zero(slow_ + (magnitude - slow_) * slow_coeff_);
        fast_ = finite_or_zero(fast_);
        slow_ = finite_or_zero(slow_);
        return gain_for(fast_, slow_);
    }

    // In place over a stereo block: each sample's gain from the mean of the
    // two rails' magnitudes, applied to both. Returns the last gain. The
    // block's input is finite (the engine sanitizes at its entry) and the
    // envelopes follow non-negative magnitudes, so the recursion runs bare;
    // the owner calls heal() on a fixed sample grid.
    template <typename SampleType> double process_block(SampleType* l, SampleType* r, int n) {
        double fast = fast_;
        double slow = slow_;
        double gain = 1.0;
        for (int k = 0; k < n; ++k) {
            const double magnitude =
                0.5 * (std::abs(static_cast<double>(l[k])) + std::abs(static_cast<double>(r[k])));
            fast += (magnitude - fast) * fast_coeff_;
            slow += (magnitude - slow) * slow_coeff_;
            gain = gain_for(fast, slow);
            l[k] = static_cast<SampleType>(static_cast<double>(l[k]) * gain);
            r[k] = static_cast<SampleType>(static_cast<double>(r[k]) * gain);
        }
        fast_ = fast;
        slow_ = slow;
        return gain;
    }

    // Snap and sanitize the envelopes, as process() does every sample.
    void heal() {
        fast_ = finite_or_zero(snap_to_zero(fast_));
        slow_ = finite_or_zero(snap_to_zero(slow_));
    }

    double fast_envelope() const { return fast_; }
    double slow_envelope() const { return slow_; }

  private:
    static double gain_for(double fast, double slow) {
        const double ratio = (fast - slow) / std::max(slow, 1e-9);
        const double t = std::clamp(ratio / kDuckKnee, 0.0, 1.0);
        const double smooth = t * t * (3.0 - 2.0 * t);
        return 1.0 - kDuckAmount * smooth;
    }

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
//
// The LFOs are a lockstep quadrature bank: the right rail's cosine tap is the
// same oscillator's quadrature output, so a sample costs six rotations rather
// than six sines and six cosines. process_block() pushes a whole span first
// and then reads it back, which is exact because the shortest tap delay
// (base minus depth) is longer than kMaxBlockSamples at every tank rate.
template <typename SampleType = float>
class EnsembleChorus {
public:
    void prepare(double max_sample_rate) {
        const int cap =
            static_cast<int>((kEnsembleBaseMs + kEnsembleDepthMs * 2.0) * 0.001 * max_sample_rate) +
            kHermiteGuard + kMaxBlockSamples;
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
            lfo_.set_rate(i, hz, sample_rate);
        }
    }

    void reset() {
        left_.reset();
        right_.reset();
        lfo_.restart();
        for (int i = 0; i < kNumEnsembleLfos; ++i)
            lfo_.reset(i, static_cast<double>(i) / static_cast<double>(kNumEnsembleLfos));
    }

    void process(SampleType& l, SampleType& r) {
        process_block(&l, &r, 1);
    }

    // In place, n <= kMaxBlockSamples.
    void process_block(SampleType* l, SampleType* r, int n) {
        left_.push_block(l, n);
        right_.push_block(r, n);
        // Each tap's delays for the whole span, read back against each
        // sample's own post-push position (n - 1 behind the span's end).
        double sin_delay[kNumEnsembleLfos][kMaxBlockSamples];
        double cos_delay[kNumEnsembleLfos][kMaxBlockSamples];
        const double base = base_samples_;
        for (int k = 0; k < n; ++k) {
            lfo_.tick();
            for (int i = 0; i < kNumEnsembleLfos; ++i) {
                sin_delay[i][k] = base + depth_samples_ * lfo_.sin(i);
                // 90 degrees of phase between the rails: sin -> cos.
                cos_delay[i][k] = base + depth_samples_ * lfo_.cos(i);
            }
        }
        SampleType sum_l[kMaxBlockSamples] = {};
        SampleType sum_r[kMaxBlockSamples] = {};
        SampleType tap[kMaxBlockSamples];
        for (int i = 0; i < kNumEnsembleLfos; ++i) {
            left_.read_hermite_span(sin_delay[i], tap, n, n - 1);
            for (int k = 0; k < n; ++k)
                sum_l[k] += tap[k];
            right_.read_hermite_span(cos_delay[i], tap, n, n - 1);
            for (int k = 0; k < n; ++k)
                sum_r[k] += tap[k];
        }
        constexpr auto dry = static_cast<SampleType>(1.0 - kEnsembleWetMix);
        constexpr auto wet = static_cast<SampleType>(kEnsembleWetMix / kNumEnsembleLfos);
        for (int k = 0; k < n; ++k) {
            l[k] = l[k] * dry + sum_l[k] * wet;
            r[k] = r[k] * dry + sum_r[k] * wet;
        }
    }

private:
    FracDelayT<SampleType> left_;
    FracDelayT<SampleType> right_;
    QuadratureBank<kNumEnsembleLfos> lfo_{};
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

// tanh in single precision as a [9/8] odd rational, fitted to tanh on
// |x| <= 9 with the slope pinned to 1 at the origin: relative error below
// 4e-7 everywhere in float, and the result is clamped to [-1, 1] so the curve
// stays bounded. Branch-free, so a block of it vectorizes.
inline float tanh_rational(float x) {
    const float c = std::clamp(x, -9.0f, 9.0f);
    const float s = c * c;
    const float p =
        (((1.3810137e-08f * s + 2.0946818e-05f) * s + 3.5165233e-03f) * s + 1.3398798e-01f) * s +
        1.0f;
    const float q =
        (((7.9656114e-07f * s + 3.3195784e-04f) * s + 2.5957071e-02f) * s + 4.6732125e-01f) * s +
        1.0f;
    return std::clamp(c * p / q, -1.0f, 1.0f);
}

inline float soft_limit(float x) {
    constexpr auto headroom = static_cast<float>(kWetLimiterHeadroom);
    return headroom * tanh_rational(x / headroom);
}

}  // namespace pulp::signal::fdn
