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
// BLOCK PROCESSING. Every stage between one line read and the matching write
// is either per line (read, diffusion) or per sample across lines (damping,
// flux, the matrix). The shortest loop is far longer than a control period, so
// the tank runs a whole period at a time: each line reads and diffuses its span
// along time, the cross-line stages then run one sample at a time over all
// sixteen lines as one vector, and the writes are committed last. Every sample
// is computed by the same expression it would be one at a time, so the block
// partition is unobservable — process() is simply a block of one.
//
// RT contract: prepare() allocates; configure/update/process/reset allocate
// nothing, so a live tank-rate change never touches the allocator.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/diffusion.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/detail/simd_lanes.hpp>
#include <pulp/signal/fdn/loop_eq.hpp>
#include <pulp/signal/fdn/modulation.hpp>
#include <pulp/signal/fdn/shimmer.hpp>
#include <pulp/signal/fdn/stages.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <type_traits>

namespace pulp::signal::fdn {

namespace lanes = signal::detail::lanes;

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

// The matrix is the last point where one corrupt line can spread into all
// sixteen. Guard it before input fan-out: non-finite state is killed locally,
// and a finite overflow is contained below the tank's wider emergency ceiling.
template <typename T>
inline T post_hadamard_guard(T value) {
    if (!std::isfinite(static_cast<double>(value))) return T{0};
    return snap_to_zero(
        std::clamp(value, static_cast<T>(-kPostHadamardCeil),
                   static_cast<T>(kPostHadamardCeil)));
}

inline double damping_state_step(double input, double coefficient,
                                 double& state) {
    state = snap_to_zero(input + (state - input) * coefficient);
    if (!std::isfinite(state)) state = 0.0;
    state = snap_to_zero(
        std::clamp(state, -static_cast<double>(kSanityCeil),
                   static_cast<double>(kSanityCeil)));
    return state;
}

template <typename SampleType = float>
class FdnTank {
    static_assert(std::is_same_v<SampleType, float>,
                  "the tank's block kernels are single precision");

  public:
    // Sizes every allocation for the worst case: the longest line at the
    // highest tank rate. configure() only re-derives lengths and coefficients.
    void prepare(double max_tank_rate) {
        const int max_line = static_cast<int>(kDelayMaxSeconds * kSizeScaleMax *
                                              max_tank_rate) +
                             kHermiteGuard;
        // The sixteen lines, and the sixteen cascades' stages, advance in
        // lockstep: stagger every ring so none shares low address bits with
        // another (FracDelayT::prepare).
        for (std::size_t i = 0; i < lines_.size(); ++i)
            lines_[i].prepare(max_line, i * kRingStaggerSamples);
        for (std::size_t i = 0; i < loop_diffusion_.size(); ++i)
            loop_diffusion_[i].prepare(kLoopDiffusionBaseMs, max_tank_rate, kLoopDiffusionStages,
                                       i * kLoopDiffusionStages * kRingStaggerSamples);
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
            lp_state_[i] = 0.0f;
            hp_state_[i] = 0.0f;
            current_delay_[i] = target_delay_[i];
            flutter_lfo_[i].reset(static_cast<double>(i) /
                                  static_cast<double>(kNumChannels));
        }
        // Reset snaps the size glide too. Recompute every length-dependent
        // coefficients now so direct tank users cannot observe target delays
        // paired with stale decay or damping state.
        recompute_derived_state(controls_, true);
        cached_ = controls_;
        heal_phase_ = 0;
    }

    void set_active_channels(int n) {
        active_n_ = std::clamp(n, 2, kNumChannels);
        // Unused lines are zeroed before the matrix; the taps and the input
        // fan-out renormalize so the wet level is independent of the count.
        tap_scale_ = static_cast<SampleType>(
            1.0 / std::sqrt(static_cast<double>(active_n_)));
        in_gain_ = static_cast<SampleType>(
            1.0 / std::sqrt(static_cast<double>(active_n_) * 0.5));
        for (int j = 0; j < kNumChannels / 4; ++j)
            active_[j] = lanes::make(
                4 * j < active_n_ ? 1.0f : 0.0f, 4 * j + 1 < active_n_ ? 1.0f : 0.0f,
                4 * j + 2 < active_n_ ? 1.0f : 0.0f, 4 * j + 3 < active_n_ ? 1.0f : 0.0f);
        derive_safe_block();
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
    // Structural telemetry for the acceptance suite: the public reverb drive
    // must reach the in-loop saturator independently of output makeup/limiting.
    double loop_drive() const { return drive_; }
    double worst_case_boost() const { return worst_case_boost_; }
    // Structural telemetry for the acceptance suite: delay construction is a
    // normative equation, so expose its realized sample count rather than
    // asking a spectral test to infer rounding from a dense 16-line response.
    double target_delay_samples(int channel) const {
        return target_delay_[static_cast<std::size_t>(
            std::clamp(channel, 0, kNumChannels - 1))];
    }

    // One control-rate tick: re-derive lengths, decay gains, damping, and the
    // stability normalization, then advance the walk and flux sources.
    void update_controls(const TankControls& c) {
        controls_ = c;
        if (c.size != cached_.size) derive_lengths(c.size);
        for (int i = 0; i < kNumChannels; ++i) {
            current_delay_[i] += (target_delay_[i] - current_delay_[i]) * (1.0 - glide_);
        }
        recompute_derived_state(c, false);

        modulation_.step_walks();
        flux_.tick_control(kControlRateSamples);
        cached_ = c;
    }

    // One tank sample. `in_l` / `in_r` are already pre-diffused and ducked;
    // `out_l` / `out_r` are the wet taps, before width and the output stage.
    void process(SampleType in_l, SampleType in_r, SampleType& out_l, SampleType& out_r) {
        process_block(&in_l, &in_r, &out_l, &out_r, 1);
    }

    // A block of tank samples with the controls held, as between two
    // update_controls() calls. Any length; it is split internally so no read
    // ever reaches a sample written inside the same piece.
    void process_block(const SampleType* in_l, const SampleType* in_r, SampleType* out_l,
                       SampleType* out_r, int n) {
        int offset = 0;
        while (offset < n) {
            const int chunk =
                std::min({n - offset, safe_block_, kHealIntervalSamples - heal_phase_});
            process_chunk(in_l + offset, in_r + offset, out_l + offset, out_r + offset, chunk);
            offset += chunk;
            heal_phase_ += chunk;
            if (heal_phase_ == kHealIntervalSamples) {
                heal();
                heal_phase_ = 0;
            }
        }
    }

  private:
    using Lane = std::array<SampleType, kNumChannels>;

    // One piece of at most safe_block_ samples. See the file note.
    void process_chunk(const SampleType* in_l, const SampleType* in_r, SampleType* out_l,
                       SampleType* out_r, int n) {
        // Sample-major work area: x[k] holds every line's value at sample k.
        alignas(64) std::array<Lane, kMaxBlockSamples> x;
        // Line-major read positions: delay[i] is line i's span.
        alignas(64) double delay[kNumChannels][kMaxBlockSamples];

        // Modulated read positions, all lines at once per sample.
        for (int k = 0; k < n; ++k) {
            double m[kNumChannels];
            modulation_.tick(m);
            for (std::size_t i = 0; i < static_cast<std::size_t>(kNumChannels); ++i)
                delay[i][k] = current_delay_[i] * (1.0 + mod_depth_ * m[i]);
        }

        // Per line, along time: modulated read, then the in-loop diffusion.
        // Its sign alternates by channel parity so neighbouring lines never
        // diffuse alike.
        alignas(64) std::array<SampleType, kMaxBlockSamples> span;
        alignas(64) std::array<double, kMaxBlockSamples> lfo;
        for (int i = 0; i < active_n_; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            lines_[ui].read_hermite_span(delay[ui], span.data(), n);
            const SampleType g = (i & 1) ? -loop_g_ : loop_g_;
            if (controls_.flutter) {
                for (int k = 0; k < n; ++k)
                    lfo[static_cast<std::size_t>(k)] = flutter_lfo_[ui].tick();
                loop_diffusion_[ui].process_block_fluttered(span.data(), n, g, lfo.data(),
                                                            kFlutterDepth);
            } else {
                loop_diffusion_[ui].process_block(span.data(), n, g);
            }
            for (int k = 0; k < n; ++k)
                x[static_cast<std::size_t>(k)][ui] = span[static_cast<std::size_t>(k)];
        }
        for (int k = 0; k < n; ++k)
            for (int i = active_n_; i < kNumChannels; ++i)
                x[static_cast<std::size_t>(k)][static_cast<std::size_t>(i)] = SampleType{0};

        // Across lines, one sample at a time, as four 4-lane vectors.
        using lanes::F4;
        constexpr int kVectors = kNumChannels / 4;
        F4 lp_state[kVectors];
        F4 hp_state[kVectors];
        for (int j = 0; j < kVectors; ++j) {
            lp_state[j] = lanes::load(&lp_state_[4 * static_cast<std::size_t>(j)]);
            hp_state[j] = lanes::load(&hp_state_[4 * static_cast<std::size_t>(j)]);
        }
        const F4 ceiling = lanes::splat(static_cast<float>(kSanityCeil));
        const F4 floor = lanes::splat(-static_cast<float>(kSanityCeil));
        const bool shimmer = shimmer_weight_ > 0.0;
        const bool eq = eq_.active_bands() > 0;
        typename FluxBank<SampleType>::Lanes flux;
        flux_.load_lanes(flux);
        // Line-major staging for the writes, committed after the loop.
        alignas(64) float written[kNumChannels][kMaxBlockSamples];
        for (int k = 0; k < n; ++k) {
            float* row = x[static_cast<std::size_t>(k)].data();
            F4 v[kVectors];
            for (int j = 0; j < kVectors; ++j) {
                // Damping: one-pole LP then one-pole HP, both scaled per line
                // so the frequency-dependent decay is consistent across
                // lengths. The input is clamped to the tank's sanity ceiling,
                // and each one-pole is a convex mix of its input and its own
                // previous state, so both states stay inside that ceiling with
                // no clamp on the recursion; heal() snaps and sanitizes them
                // once per chunk.
                const F4 in = lanes::clamp(lanes::load(row + 4 * j), floor, ceiling);
                const F4 lp = in + (lp_state[j] - in) * lp_coeff_[j];
                lp_state[j] = lp;
                const F4 y = lanes::select(lp_on_[j], lp, in);
                const F4 hp = y + (hp_state[j] - y) * hp_coeff_[j];
                hp_state[j] = hp;
                v[j] = lanes::select(hp_on_[j], y - hp, y);
            }
            if (eq) {
                for (int j = 0; j < kVectors; ++j)
                    lanes::store(row + 4 * j, v[j]);
                for (int i = 0; i < active_n_; ++i)
                    row[i] = eq_.process(i, row[i]);
                for (int j = 0; j < kVectors; ++j)
                    v[j] = lanes::load(row + 4 * j);
            }
            FluxBank<SampleType>::process_lanes(flux, v);
            for (int j = 0; j < kVectors; ++j)
                v[j] *= active_[j];

            // Output tap: even channels feed left, odd feed right.
            const F4 taps = (v[0] + v[1]) + (v[2] + v[3]);
            out_l[k] = (lanes::lane(taps, 0) + lanes::lane(taps, 2)) * tap_scale_;
            out_r[k] = (lanes::lane(taps, 1) + lanes::lane(taps, 3)) * tap_scale_;

            // Shimmer injects into the FEEDBACK, which is what makes it bloom.
            if (shimmer) {
                for (int j = 0; j < kVectors; ++j)
                    lanes::store(row + 4 * j, v[j]);
                for (int i = 0; i < active_n_; ++i)
                    row[i] += static_cast<SampleType>(
                        shimmer_weight_ * static_cast<double>(shimmer_.process(i, row[i])));
                for (int j = 0; j < kVectors; ++j)
                    v[j] = lanes::load(row + 4 * j);
            }

            // 1-Lipschitz crossfade: both terms have slope <= 1 everywhere, so
            // the saturator can never raise the loop gain at any drive.
            if (drive_ > 0.0) {
                const F4 drive = lanes::splat(static_cast<float>(drive_));
                for (int j = 0; j < kVectors; ++j)
                    v[j] += drive * (lipschitz_tanh_lanes(v[j]) - v[j]);
            }

            // Per-line decay gain (with the matrix's 1/4 normalization folded
            // in), then the 16-point Hadamard: two butterfly stages across the
            // vectors, two inside each.
            for (int j = 0; j < kVectors; ++j)
                v[j] *= matrix_gain_[j];
            const F4 a0 = v[0] + v[1];
            const F4 a1 = v[0] - v[1];
            const F4 a2 = v[2] + v[3];
            const F4 a3 = v[2] - v[3];
            v[0] = lanes::hadamard4(a0 + a2);
            v[1] = lanes::hadamard4(a1 + a3);
            v[2] = lanes::hadamard4(a0 - a2);
            v[3] = lanes::hadamard4(a1 - a3);

            // Input fan-out: left feeds the even lines, right the odd, so
            // stereo separation is built into the topology rather than bolted
            // on after. The matrix output is clamped to its narrower ceiling
            // before the fan-out, and the line input is fully sanitized after
            // it (non-finite to zero, clamped, snapped).
            const F4 feed = lanes::make(in_gain_ * in_l[k], in_gain_ * in_r[k], in_gain_ * in_l[k],
                                        in_gain_ * in_r[k]);
            const F4 matrix_ceiling = lanes::splat(static_cast<float>(kPostHadamardCeil));
            for (int j = 0; j < kVectors; ++j) {
                const F4 w =
                    clean(lanes::clamp(v[j], -matrix_ceiling, matrix_ceiling) + feed, kSanityCeil);
                for (int lane = 0; lane < 4; ++lane)
                    written[4 * j + lane][k] = lanes::lane(w, lane);
            }
        }
        flux_.store_lanes(flux);
        for (int j = 0; j < kVectors; ++j) {
            lanes::store(&lp_state_[4 * static_cast<std::size_t>(j)], lp_state[j]);
            lanes::store(&hp_state_[4 * static_cast<std::size_t>(j)], hp_state[j]);
        }

        for (int i = 0; i < active_n_; ++i)
            lines_[static_cast<std::size_t>(i)].push_block(written[i], n);
    }

    // Every kHealIntervalSamples: snap decayed recursive state to zero (the
    // interval is far too short for a state at the snap threshold to reach the
    // denormal range), and zero any state that went non-finite, so one corrupt
    // value can never persist in a recursion.
    void heal() {
        for (std::size_t i = 0; i < static_cast<std::size_t>(kNumChannels); ++i) {
            lp_state_[i] = std::isfinite(lp_state_[i]) ? snap_to_zero(lp_state_[i]) : 0.0f;
            hp_state_[i] = std::isfinite(hp_state_[i]) ? snap_to_zero(hp_state_[i]) : 0.0f;
        }
        flux_.heal();
    }

    // post_hadamard_guard / sanitize on four lanes: non-finite to zero, finite
    // values clamped to +/-ceiling and snapped.
    static lanes::F4 clean(lanes::F4 x, double ceiling) {
        const auto c = lanes::splat(static_cast<float>(ceiling));
        const lanes::F4 bounded = lanes::clamp(x, -c, c);
        const lanes::F4 magnitude = lanes::abs(x);
        const lanes::M4 keep = lanes::both(
            lanes::less_equal(magnitude, lanes::splat(std::numeric_limits<float>::max())),
            lanes::less_equal(lanes::splat(snap_threshold<float>()), magnitude));
        return lanes::select(keep, bounded, lanes::splat(0.0f));
    }

    static lanes::F4 lipschitz_tanh_lanes(lanes::F4 v) {
        const lanes::F4 x = lanes::clamp(v, lanes::splat(-3.0f), lanes::splat(3.0f));
        const lanes::F4 x2 = x * x;
        return x * (lanes::splat(27.0f) + x2) / (lanes::splat(27.0f) + lanes::splat(9.0f) * x2);
    }

    // The longest piece process_chunk() may take: every line read and every
    // diffusion stage must reach only samples written before the piece began.
    void derive_safe_block() {
        int bound = kMaxBlockSamples;
        for (int i = 0; i < active_n_; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            // |modulation| <= 1, and the read is clamped at a delay of 2. The
            // Hermite window's newest tap is one sample newer than the floor.
            const double shortest = std::max(2.0, current_delay_[ui] * (1.0 - mod_depth_));
            bound = std::min(bound, static_cast<int>(shortest) - 1);
            int stage = loop_diffusion_[ui].min_delay();
            if (controls_.flutter)
                stage = static_cast<int>(static_cast<double>(stage) * (1.0 - kFlutterDepth)) - 1;
            bound = std::min(bound, stage);
        }
        safe_block_ = std::max(bound, 1);
    }

    void recompute_derived_state(const TankControls& c, bool force_damping) {
        for (int i = 0; i < kNumChannels; ++i) {
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
        // Once the controls and the size glide settle, every input below is
        // unchanged from tick to tick; the per-line pow() is then skipped.
        if (!force_damping && ceiling == gain_key_.ceiling && authority == gain_key_.authority &&
            t60 == gain_key_.t60 && tank_rate_ == gain_key_.tank_rate &&
            loop_delay_ == gain_key_.loop_delay) {
            finish_derived_state(c, force_damping);
            return;
        }
        gain_key_ = {ceiling, authority, t60, tank_rate_, loop_delay_};
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
        for (int j = 0; j < kNumChannels / 4; ++j) {
            const auto u = static_cast<std::size_t>(4 * j);
            // The matrix's 1/sqrt(16) normalization rides on the decay gain.
            matrix_gain_[j] = lanes::make(
                static_cast<float>(gain_[u] * 0.25), static_cast<float>(gain_[u + 1] * 0.25),
                static_cast<float>(gain_[u + 2] * 0.25), static_cast<float>(gain_[u + 3] * 0.25));
        }

        finish_derived_state(c, force_damping);
    }

    void finish_derived_state(const TankControls& c, bool force_damping) {
        if (force_damping || c.damp_hi != cached_.damp_hi ||
            c.damp_lo != cached_.damp_lo || c.size != cached_.size)
            derive_damping(c.damp_hi, c.damp_lo);

        mod_depth_ = c.mod * kModMaxFraction * mod_depth_shrink(c.decay);
        loop_g_ = static_cast<SampleType>(c.diffusion * kLoopDiffusionG);
        drive_ = c.drive;
        derive_safe_block();
    }

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
                std::round(base * tank_rate_) +
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
        for (int j = 0; j < kNumChannels / 4; ++j) {
            const auto u = static_cast<std::size_t>(4 * j);
            lp_coeff_[j] =
                lanes::make(static_cast<float>(lp_a_[u]), static_cast<float>(lp_a_[u + 1]),
                            static_cast<float>(lp_a_[u + 2]), static_cast<float>(lp_a_[u + 3]));
            hp_coeff_[j] =
                lanes::make(static_cast<float>(hp_a_[u]), static_cast<float>(hp_a_[u + 1]),
                            static_cast<float>(hp_a_[u + 2]), static_cast<float>(hp_a_[u + 3]));
            lp_on_[j] = lanes::mask_of(lp_a_[u] > 0.0, lp_a_[u + 1] > 0.0, lp_a_[u + 2] > 0.0,
                                       lp_a_[u + 3] > 0.0);
            hp_on_[j] = lanes::mask_of(hp_a_[u] > 0.0, hp_a_[u + 1] > 0.0, hp_a_[u + 2] > 0.0,
                                       hp_a_[u + 3] > 0.0);
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
    std::array<float, kNumChannels> lp_state_{};
    std::array<float, kNumChannels> hp_state_{};
    // The cross-line stage's per-line constants, as four 4-lane vectors.
    lanes::F4 matrix_gain_[kNumChannels / 4] = {};
    lanes::F4 lp_coeff_[kNumChannels / 4] = {};
    lanes::F4 hp_coeff_[kNumChannels / 4] = {};
    lanes::M4 lp_on_[kNumChannels / 4] = {};
    lanes::M4 hp_on_[kNumChannels / 4] = {};
    lanes::F4 active_[kNumChannels / 4] = {lanes::splat(1.0f), lanes::splat(1.0f),
                                           lanes::splat(1.0f), lanes::splat(1.0f)};

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
    int safe_block_ = 1;
    int heal_phase_ = 0;

    // The inputs the per-line decay gains were last derived from.
    struct GainKey {
        double ceiling = -1.0;
        double authority = -1.0;
        double t60 = -1.0;
        double tank_rate = -1.0;
        std::array<double, kNumChannels> loop_delay{};
    } gain_key_{};
};

}  // namespace pulp::signal::fdn
