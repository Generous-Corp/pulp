#pragma once

// The tank: sixteen delay lines cross-coupled through a Hadamard matrix, with
// the whole in-loop stack (diffusion, damping, EQ, flux, shimmer, saturation)
// between the read and the write.
//
// Processing order inside one pass, per channel, is load-bearing and matches
// the engine's block diagram:
//
//   read (modulated) -> in-loop diffusion -> damping -> EQ -> flux
//     -> [OUTPUT TAP] -> + shimmer -> saturation -> x decay gain -> Hadamard
//
// The tap sits BEFORE the shimmer injection and the saturator so the dry tail
// the listener hears is the tail the decay law describes; what is fed back is
// the tail plus everything that must be energy-accounted for.
//
// RT contract: prepare() allocates; configure/update/process/reset allocate
// nothing, so a live tank-rate change never touches the allocator.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/diffusion.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/loop_eq.hpp>
#include <pulp/signal/fdn/modulation.hpp>
#include <pulp/signal/fdn/shimmer.hpp>
#include <pulp/signal/fdn/stages.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal::fdn {

// Everything the tank re-derives at the control rate. The engine owns the
// smoothing; the tank owns what the smoothed values mean.
struct TankControls {
    double decay = 2.5;
    double size = 0.5;
    double damp_hi = 0.4;
    double damp_lo = 0.1;
    double diffusion = 0.7;
    double mod = 0.35;
    double shimmer = 0.0;
    double drive = 0.0;
    double bloom = 0.0;
    bool flutter = false;
};

// How much authority Bloom has at a given decay. It approaches 1 only at long
// decays: at short decays Bloom fattens the tail slightly, at long decays it
// lifts the loop to the ceiling and the tank sits just under unity — the
// freeze regime. Decay always keeps final authority, which is what makes
// "bloom 1 with a 0.5 s decay still decays" true by construction.
inline double bloom_authority(double decay_seconds) {
    if (decay_seconds <= kBloomAuthorityMinSeconds) return 0.0;
    if (decay_seconds >= kBloomAuthorityMaxSeconds) return 1.0;
    const double t = std::log(decay_seconds / kBloomAuthorityMinSeconds) /
                     std::log(kBloomAuthorityMaxSeconds / kBloomAuthorityMinSeconds);
    return t * t * (3.0 - 2.0 * t);
}

// In-place 16-point Hadamard butterfly, O(N log N). Hadamard maximizes
// inter-line scattering per pass at trivial cost, and being unitary it is
// energy-preserving — which is the foundation every stability claim here rests
// on. No matrix morphing: the motion comes from delay modulation and flux.
template <typename T>
inline void hadamard16(std::array<T, kNumChannels>& v) {
    for (int stride = 1; stride < kNumChannels; stride *= 2) {
        for (int i = 0; i < kNumChannels; i += 2 * stride) {
            for (int j = i; j < i + stride; ++j) {
                const T a = v[static_cast<std::size_t>(j)];
                const T b = v[static_cast<std::size_t>(j + stride)];
                v[static_cast<std::size_t>(j)] = a + b;
                v[static_cast<std::size_t>(j + stride)] = a - b;
            }
        }
    }
    for (auto& x : v) x *= T{0.25};  // 1/sqrt(16)
}

template <typename SampleType = float>
class FdnTank {
public:
    // Sizes every allocation for the worst case: the longest line at the
    // highest tank rate. configure() only re-derives lengths and coefficients.
    void prepare(double max_tank_rate) {
        const int max_line = static_cast<int>(kDelayMaxSeconds * kSizeScaleMax *
                                              max_tank_rate) +
                             kHermiteGuard;
        for (auto& line : lines_) line.prepare(max_line);
        for (auto& cascade : loop_diffusion_)
            cascade.prepare(kLoopDiffusionBaseMs, max_tank_rate, kLoopDiffusionStages);
        shimmer_.prepare(max_tank_rate);
        for (int i = 0; i < kNumChannels; ++i) {
            flutter_lfo_[i].reset(static_cast<double>(i) /
                                  static_cast<double>(kNumChannels));
        }
    }

    void configure(double tank_rate) {
        tank_rate_ = tank_rate;
        modulation_.configure(tank_rate);
        eq_.configure(tank_rate);
        flux_.configure(tank_rate, flux_depth_db_);
        shimmer_.configure(tank_rate);
        for (int i = 0; i < kNumChannels; ++i) {
            // Stagger each channel's in-loop cascade by its own prime so no two
            // channels share a diffusion stage length.
            loop_diffusion_[i].configure(kLoopDiffusionBaseMs, tank_rate,
                                         kChannelPrimes[static_cast<std::size_t>(i)]);
            const double frac = (kNumChannels > 1)
                                    ? static_cast<double>(i) /
                                          static_cast<double>(kNumChannels - 1)
                                    : 0.0;
            flutter_lfo_[i].set_rate(
                kFlutterRateMinHz + frac * (kFlutterRateMaxHz - kFlutterRateMinHz),
                tank_rate);
        }
        glide_ = std::exp(-static_cast<double>(kControlRateSamples) /
                          std::max(1.0, kSmoothingMs * 0.001 * tank_rate));
        derive_lengths(controls_.size);
        for (int i = 0; i < kNumChannels; ++i) current_delay_[i] = target_delay_[i];
        // Sentinels no legal parameter can take, so the next tick re-derives
        // every rate-dependent coefficient rather than trusting a stale cache.
        cached_.size = -1.0;
        cached_.damp_hi = -1.0;
        cached_.damp_lo = -1.0;
        update_controls(controls_);
    }

    void reset() {
        for (auto& line : lines_) line.reset();
        for (auto& cascade : loop_diffusion_) cascade.reset();
        modulation_.reset();
        eq_.reset();
        flux_.reset();
        shimmer_.reset();
        for (int i = 0; i < kNumChannels; ++i) {
            lp_state_[i] = 0.0;
            hp_state_[i] = 0.0;
            current_delay_[i] = target_delay_[i];
            flutter_lfo_[i].reset(static_cast<double>(i) /
                                  static_cast<double>(kNumChannels));
        }
    }

    void set_active_channels(int n) {
        active_n_ = std::clamp(n, 2, kNumChannels);
        // Unused lines are zeroed before the matrix; the taps and the input
        // fan-out renormalize so the wet level is independent of the count.
        tap_scale_ = static_cast<SampleType>(
            1.0 / std::sqrt(static_cast<double>(active_n_)));
        in_gain_ = static_cast<SampleType>(
            1.0 / std::sqrt(static_cast<double>(active_n_) * 0.5));
    }
    int active_channels() const { return active_n_; }

    LoopEq<SampleType>& eq() { return eq_; }
    const LoopEq<SampleType>& eq() const { return eq_; }

    // Flux depth, in dB of absorption. Voicing, not a host-automatable knob.
    void set_flux_depth_db(double db) {
        flux_depth_db_ = std::clamp(db, 0.0, kFluxMaxDb);
        flux_.configure(tank_rate_, flux_depth_db_);
    }

    // The realized per-pass gain of one line, and the worst-case boost it was
    // normalized against. Exposed so the acceptance suite can assert the
    // stability bound DIRECTLY — gain x boost <= kGainCeil — rather than only
    // observing that the tail happened to decay on the vectors it tried.
    double applied_gain(int channel) const {
        return gain_[static_cast<std::size_t>(channel)];
    }
    double worst_case_boost() const { return worst_case_boost_; }

    // One control-rate tick: re-derive lengths, decay gains, damping, and the
    // stability normalization, then advance the walk and flux sources.
    void update_controls(const TankControls& c) {
        controls_ = c;
        if (c.size != cached_.size) derive_lengths(c.size);
        for (int i = 0; i < kNumChannels; ++i) {
            current_delay_[i] += (target_delay_[i] - current_delay_[i]) * (1.0 - glide_);
            loop_delay_[i] =
                current_delay_[i] + loop_diffusion_[static_cast<std::size_t>(i)].total_delay();
        }

        // Worst-case per-pass boost, from every stage that can add energy.
        // Flux is absorptive-only and contributes exactly 1; the shimmer term
        // is the injection weight, which is already 1/sqrt(active_n) scaled.
        shimmer_weight_ = ShimmerBank<SampleType>::injection_weight(c.shimmer, active_n_);
        worst_case_boost_ =
            eq_.worst_case_boost() * flux_.worst_case_boost() * (1.0 + shimmer_weight_);

        const double ceiling = kGainCeil / worst_case_boost_;
        const double authority = c.bloom * bloom_authority(c.decay);
        const double t60 = std::max(c.decay, 0.01);
        for (int i = 0; i < kNumChannels; ++i) {
            // Jot's absorptive law: every line reaches -60 dB at the same time
            // regardless of its length. The length that matters is the whole
            // LOOP — the delay line plus the in-loop diffusion cascade, which is
            // lossless but very much not delay-free. Using the bare line length
            // here computes the gain for a loop that does not exist and stretches
            // the realized decay by the cascade's share of the round trip.
            const double jot =
                std::pow(10.0, -3.0 * loop_delay_[i] / (t60 * tank_rate_));
            double g = std::min(jot, ceiling);
            g += (ceiling - g) * authority;
            gain_[i] = g;
        }

        if (c.damp_hi != cached_.damp_hi || c.damp_lo != cached_.damp_lo ||
            c.size != cached_.size)
            derive_damping(c.damp_hi, c.damp_lo);

        mod_depth_ = c.mod * kModMaxFraction * mod_depth_shrink(c.decay);
        loop_g_ = static_cast<SampleType>(c.diffusion * kLoopDiffusionG);
        drive_ = c.drive;

        modulation_.step_walks();
        flux_.tick_control(kControlRateSamples);
        cached_ = c;
    }

    // One tank sample. `in_l` / `in_r` are already pre-diffused and ducked;
    // `out_l` / `out_r` are the wet taps, before width and the output stage.
    void process(SampleType in_l, SampleType in_r, SampleType& out_l, SampleType& out_r) {
        std::array<SampleType, kNumChannels> v{};
        SampleType tap_l = 0;
        SampleType tap_r = 0;

        for (int i = 0; i < active_n_; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            // Modulated read.
            const double d = current_delay_[ui] * (1.0 + mod_depth_ * modulation_.tick(i));
            SampleType s = lines_[ui].read(d);

            // In-loop diffusion — a lower coefficient than the input cascade,
            // because this one is re-applied on every pass. Its sign alternates
            // by channel parity so neighbouring lines never diffuse alike.
            const SampleType g = (i & 1) ? -loop_g_ : loop_g_;
            s = controls_.flutter
                    ? loop_diffusion_[ui].process_fluttered(s, g, flutter_lfo_[ui].tick(),
                                                            kFlutterDepth)
                    : loop_diffusion_[ui].process(s, g);

            // Damping: one-pole LP then one-pole HP, both scaled per line so
            // the frequency-dependent decay is consistent across lengths.
            double x = static_cast<double>(s);
            if (lp_a_[ui] > 0.0) {
                lp_state_[ui] = snap_to_zero(x + (lp_state_[ui] - x) * lp_a_[ui]);
                x = lp_state_[ui];
            }
            if (hp_a_[ui] > 0.0) {
                hp_state_[ui] = snap_to_zero(x + (hp_state_[ui] - x) * hp_a_[ui]);
                x -= hp_state_[ui];
            }
            s = static_cast<SampleType>(x);

            s = eq_.process(i, s);
            s = flux_.process(i, s);

            // Output tap: even channels feed left, odd feed right.
            if (i & 1)
                tap_r += s;
            else
                tap_l += s;

            // Shimmer injects into the FEEDBACK, which is what makes it bloom.
            if (shimmer_weight_ > 0.0)
                s += static_cast<SampleType>(shimmer_weight_ *
                                             static_cast<double>(shimmer_.process(i, s)));

            // 1-Lipschitz crossfade: both terms have slope <= 1 everywhere, so
            // the saturator can never raise the loop gain at any drive.
            if (drive_ > 0.0) {
                const double xs = static_cast<double>(s);
                s = static_cast<SampleType>(xs + drive_ * (fast_tanh(xs) - xs));
            }

            v[ui] = static_cast<SampleType>(static_cast<double>(s) * gain_[ui]);
        }
        for (int i = active_n_; i < kNumChannels; ++i) v[static_cast<std::size_t>(i)] = 0;

        hadamard16(v);

        out_l = tap_l * tap_scale_;
        out_r = tap_r * tap_scale_;

        // Input fan-out: left feeds the even lines, right the odd, so stereo
        // separation is built into the topology rather than bolted on after.
        for (int i = 0; i < active_n_; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            const SampleType w = sanitize(v[ui]) + in_gain_ * ((i & 1) ? in_r : in_l);
            lines_[ui].push(sanitize(w));
        }
    }

private:
    // Defense in depth. One guard at the output is not enough in a 16-line
    // recursive structure: a single corrupted channel is inaudible at the sum
    // until the matrix has already spread it into every other line.
    static SampleType sanitize(SampleType x) {
        if (!std::isfinite(static_cast<double>(x))) return SampleType{0};
        // Denormals matter more here than anywhere else in the engine: a 60 s
        // tail spends its last seconds in exactly the range where a
        // denormal-stalling CPU pays for every one of sixteen recirculating
        // lines at once.
        return snap_to_zero(std::clamp(x, static_cast<SampleType>(-kSanityCeil),
                                       static_cast<SampleType>(kSanityCeil)));
    }

    void derive_lengths(double size) {
        const double scale =
            kSizeScaleMin + std::clamp(size, 0.0, 1.0) * (kSizeScaleMax - kSizeScaleMin);
        const double dmin = kDelayMinSeconds * scale;
        const double dmax = kDelayMaxSeconds * scale;
        for (int i = 0; i < kNumChannels; ++i) {
            const double frac = static_cast<double>(i) /
                                static_cast<double>(kNumChannels - 1);
            const double base = dmin + (dmax - dmin) * std::pow(frac, kDelayExponent);
            // ADDITIVE prime offsets, not multiplicative co-prime ratios:
            // multiplying near-equal base lengths diverges wildly when the
            // requested range is narrow (a small room), while adding distinct
            // primes guarantees pairwise-distinct lengths with a bounded
            // distortion of the intended distribution.
            target_delay_[i] =
                std::floor(base * tank_rate_) +
                static_cast<double>(kChannelPrimes[static_cast<std::size_t>(i)]);
            const double cap = static_cast<double>(lines_[static_cast<std::size_t>(i)]
                                                       .max_delay());
            target_delay_[i] = std::clamp(target_delay_[i], 4.0, cap);
        }
        median_delay_ = target_delay_[kNumChannels / 2] +
                        loop_diffusion_[kNumChannels / 2].total_delay();
    }

    void derive_damping(double damp_hi, double damp_lo) {
        const double lp_hz =
            kDampHiMinHz * std::pow(kDampHiMaxHz / kDampHiMinHz,
                                    std::pow(1.0 - std::clamp(damp_hi, 0.0, 1.0), kDampCurve));
        const double hp_hz =
            kDampLoMinHz * std::pow(kDampLoMaxHz / kDampLoMinHz,
                                    std::pow(std::clamp(damp_lo, 0.0, 1.0), kDampCurve));
        const double lp_med = std::exp(-6.283185307179586 * lp_hz / tank_rate_);
        for (int i = 0; i < kNumChannels; ++i) {
            const double ratio =
                std::clamp(loop_delay_[i] / std::max(median_delay_, 1.0), 0.05, 20.0);
            // Lowpass: the Jot-proportional exponent applies to the filter's
            // LOSS, not to the pole. A one-pole lowpass passes (1 - a) in its
            // stopband, so (1 - a) is what gets raised to D_i / D_med, and a
            // longer line ends up filtering proportionally harder.
            lp_a_[i] = lp_med <= 0.0
                           ? 0.0
                           : lerp_uniform(lp_med, 1.0 - std::pow(1.0 - lp_med, ratio));
            // Highpass: no exact analogue exists. A one-pole highpass has no
            // flat stopband — its attenuation is 20*log10(f / fc), which
            // depends on frequency — so raising a gain to D_i / D_med is not
            // defined for it. Scaling the CUTOFF by the length ratio gives the
            // same monotone behaviour (longer lines filter harder) with a
            // bounded, documented departure from exact proportionality.
            const double hp_hz_i = hp_hz * ratio;
            hp_a_[i] = lerp_uniform(
                std::exp(-6.283185307179586 * hp_hz / tank_rate_),
                std::exp(-6.283185307179586 * hp_hz_i / tank_rate_));
        }
    }

    // Blend the uniform coefficient against the length-proportional one.
    // kDampingScale 1 is textbook-correct; 0 is uniform, which is "wrong" but
    // sometimes sounds better on small bright rooms — the choice is data.
    static double lerp_uniform(double uniform, double proportional) {
        return uniform + (proportional - uniform) * kDampingScale;
    }

    std::array<FracDelayT<SampleType>, kNumChannels> lines_{};
    std::array<AllpassCascade<SampleType>, kNumChannels> loop_diffusion_{};
    std::array<PhaseOsc, kNumChannels> flutter_lfo_{};
    DelayModulationBank modulation_{};
    LoopEq<SampleType> eq_{};
    FluxBank<SampleType> flux_{};
    ShimmerBank<SampleType> shimmer_{};

    std::array<double, kNumChannels> target_delay_{};
    std::array<double, kNumChannels> current_delay_{};
    std::array<double, kNumChannels> loop_delay_{};
    std::array<double, kNumChannels> gain_{};
    std::array<double, kNumChannels> lp_a_{};
    std::array<double, kNumChannels> hp_a_{};
    std::array<double, kNumChannels> lp_state_{};
    std::array<double, kNumChannels> hp_state_{};

    TankControls controls_{};
    TankControls cached_{};
    double tank_rate_ = 20000.0;
    double flux_depth_db_ = kFluxDefaultDb;
    double median_delay_ = 1.0;
    double glide_ = 0.0;
    double mod_depth_ = 0.0;
    double drive_ = 0.0;
    double shimmer_weight_ = 0.0;
    double worst_case_boost_ = 1.0;
    SampleType loop_g_ = 0;
    SampleType tap_scale_ = static_cast<SampleType>(0.25);
    SampleType in_gain_ = static_cast<SampleType>(0.35355339059327373);
    int active_n_ = kNumChannels;
};

}  // namespace pulp::signal::fdn
