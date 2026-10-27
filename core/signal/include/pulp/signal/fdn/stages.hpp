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
#include <pulp/signal/detail/simd_lanes.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/modulation.hpp>
#include <pulp/signal/fdn/sanitize.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal::fdn {

namespace lanes = signal::detail::lanes;

// The loop saturator's curve. See signal::lipschitz_tanh for why it is this
// one — 1-Lipschitz so it can never raise the loop gain, and bounded so a
// freeze-regime tail settles rather than merely decaying slower than it grows.
// The character delay uses the same curve for its dub feedback; fast_math.hpp
// holds the one definition.
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

    // In place, n <= kMaxBlockSamples. The span is pushed first; then each
    // sample's twelve taps (six per rail) are read back against that sample's
    // own post-push position, all twelve per pass: one 16-byte tap load each
    // (FracDelayT::taps_at) and the interpolation three taps-groups per
    // vector. Same delays, fractions, taps, curve, and summation order as
    // reading each tap with FracDelayT::read_hermite_at.
    void process_block(SampleType* l, SampleType* r, int n) {
        static_assert(kNumEnsembleLfos == 6, "the tap grouping below is for six LFOs");
        using lanes::D2;
        using lanes::F4;
        using lanes::I2;
        left_.push_block(l, n);
        right_.push_block(r, n);
        const SampleType* rails[2] = {left_.taps_at(0), right_.taps_at(0)};
        const I2 mask = lanes::splat_i(static_cast<std::int64_t>(left_.mask()));
        const std::size_t end = left_.write_index();
        const D2 base = lanes::splat(base_samples_);
        const D2 depth = lanes::splat(depth_samples_);
        const D2 lowest = lanes::splat(2.0);
        const D2 highest = lanes::splat(static_cast<double>(left_.max_delay()));
        const F4 half = lanes::splat(0.5f);
        const F4 two_and_half = lanes::splat(2.5f);
        const F4 two = lanes::splat(2.0f);
        const F4 one_and_half = lanes::splat(1.5f);
        const F4 one = lanes::splat(1.0f);
        constexpr auto dry = static_cast<SampleType>(1.0 - kEnsembleWetMix);
        constexpr auto wet = static_cast<SampleType>(kEnsembleWetMix / kNumEnsembleLfos);
        for (int k = 0; k < n; ++k) {
            lfo_.tick();
            // Taps 0-5 read the left rail at the sines, 6-11 the right rail at
            // the cosines: 90 degrees of phase between the rails.
            const I2 origin = lanes::splat_i(
                static_cast<std::int64_t>(end - static_cast<std::size_t>(n - 1 - k)) - 2);
            alignas(16) std::int64_t start[12];
            D2 f[6];
            for (int p = 0; p < 6; ++p) {
                const int i = (2 * p) % kNumEnsembleLfos;
                const D2 phase = p < 3 ? lanes::make(lfo_.sin(i), lfo_.sin(i + 1))
                                       : lanes::make(lfo_.cos(i), lfo_.cos(i + 1));
                const D2 d = lanes::clamp(base + depth * phase, lowest, highest);
                const I2 whole = lanes::to_int(d);
                f[p] = d - lanes::to_double(whole);
                const I2 at = (origin - whole) & mask;
                start[2 * p] = lanes::lane(at, 0);
                start[2 * p + 1] = lanes::lane(at, 1);
            }
            F4 out[3];
            for (int g = 0; g < 3; ++g) {
                F4 ym1 = lanes::load(rails[(4 * g) / 6] + start[4 * g]);
                F4 y0 = lanes::load(rails[(4 * g + 1) / 6] + start[4 * g + 1]);
                F4 y1 = lanes::load(rails[(4 * g + 2) / 6] + start[4 * g + 2]);
                F4 y2 = lanes::load(rails[(4 * g + 3) / 6] + start[4 * g + 3]);
                lanes::transpose(ym1, y0, y1, y2);
                const F4 t = one - lanes::narrow(f[2 * g], f[2 * g + 1]);
                const F4 c1 = half * (y1 - ym1);
                const F4 c2 = ym1 - two_and_half * y0 + two * y1 - half * y2;
                const F4 c3 = half * (y2 - ym1) + one_and_half * (y0 - y1);
                out[g] = ((c3 * t + c2) * t + c1) * t + y0;
            }
            SampleType sum_l = 0;
            SampleType sum_r = 0;
            for (int tap = 0; tap < kNumEnsembleLfos; ++tap)
                sum_l += lanes::lane(out[tap / 4], tap % 4);
            for (int tap = kNumEnsembleLfos; tap < 2 * kNumEnsembleLfos; ++tap)
                sum_r += lanes::lane(out[tap / 4], tap % 4);
            l[k] = l[k] * dry + sum_l * wet;
            r[k] = r[k] * dry + sum_r * wet;
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
