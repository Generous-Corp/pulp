#pragma once

// Multirate FDN reverb — a 16-channel feedback delay network whose tank runs at
// a selectable internal sample rate.
//
// WHAT MAKES IT THIS REVERB
//  1. The tank has its OWN sample rate, chosen from eight pinned steps. Low
//     rates are darker and grittier by design; high rates are airier. The
//     control is a musical lo-fi <-> hi-fi axis, not a quality setting.
//  2. Stability is proven, not tuned. The loop gain is normalized each control
//     tick against a closed-form worst case over every stage that can add
//     energy, so the realized per-pass gain is provably below unity for ANY
//     parameter combination — and Bloom then lifts it toward (never to) unity
//     for near-infinite tails, with decay keeping final authority.
//  3. Everything that adds energy in the loop is normalized: the shimmer
//     injection is energy-weighted, the saturator is 1-Lipschitz, the flux
//     peak is absorptive-only, and loudness makeup happens outside the
//     recursion.
//  4. Motion is layered and decorrelated per channel — per-line sine plus
//     mean-reverting walk on the delays, a per-line wandering absorption peak,
//     optional diffusion flutter.
//
// Output is WET ONLY. Dry/wet belongs to whatever graph or host hosts this
// block, so the block never has to guess what "dry" means for its caller.
//
// TIERS. set_tier(fdn::Tier::classic) swaps the network for ClassicReverbT —
// parallel combs into series allpasses at the host rate — behind the same
// parameters, modes, and wet-only output. The parameters that describe
// machinery the classic tier does not have are kept but have no effect there;
// see classic_reverb.hpp. A tier change is a clean flush, like a tank-rate
// change: the newly active engine starts from silence at the next block.
//
// RT contract: prepare() allocates for the worst case (96 kHz tank at the
// maximum block size) for both tiers. Every setter, process_block(), reset(),
// a live tank-rate change, and a tier change allocate nothing.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/classic_reverb.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/diffusion.hpp>
#include <pulp/signal/fdn/filter_state.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/multirate.hpp>
#include <pulp/signal/fdn/stages.hpp>
#include <pulp/signal/fdn/tank.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <type_traits>

namespace pulp::signal {

template <typename SampleType = float>
class FdnReverbT {
  public:
    // The network and the classic tier run in single precision: their block
    // kernels are four-lane float. A double instantiation keeps the double
    // buffer interface and converts at the block boundary.
    using Engine = float;
    using Param = fdn::Param;
    using Mode = fdn::Mode;

    // ── Lifecycle ────────────────────────────────────────────────────────
    void prepare(double sample_rate, int max_block_size) {
        host_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        max_block_ = std::max(max_block_size, 1);
        bridge_.prepare(host_rate_, max_block_);

        // Room for a whole block to be pushed ahead of its reads.
        const int predelay_cap =
            static_cast<int>(0.001 *
                             fdn::kParamSpecs[static_cast<std::size_t>(Param::predelay)].max *
                             fdn::kMaxTankRate) +
            fdn::kHermiteGuard + fdn::kMaxBlockSamples;
        for (auto& line : predelay_) line.prepare(predelay_cap);
        for (auto& cascade : pre_diffusion_)
            cascade.prepare(fdn::kPreDiffusionBaseMs, fdn::kMaxTankRate,
                            fdn::kPreDiffusionStages);
        tank_.prepare(fdn::kMaxTankRate);
        ensemble_.prepare(fdn::kMaxTankRate);

        for (int p = 0; p < fdn::kNumParams; ++p) {
            target_[p] = fdn::kParamSpecs[static_cast<std::size_t>(p)].default_value;
            smoothed_[p] = target_[p];
        }
        apply_tank_rate(rate_index());
        classic_.prepare(host_rate_, max_block_);
        reset();
    }

    // Stamp a mode's defaults. A mode is a parameter table, never a code
    // branch: all five catalog reverbs run byte-identical DSP.
    void set_mode(Mode mode) {
        for (int p = 0; p < fdn::kNumParams; ++p) {
            const auto param = static_cast<Param>(p);
            if (param == Param::predelay || param == Param::width) continue;
            set_parameter(param, fdn::mode_default(mode, param));
        }
        output_lp_hz_ = fdn::mode_config(mode).output_lp_hz;
        design_output_lp();
        snap_parameters();
    }

    void set_parameter(Param p, double value) {
        const auto index = static_cast<std::size_t>(p);
        const fdn::ParamSpec& spec = fdn::kParamSpecs[index];
        double v = std::clamp(value, spec.min, spec.max);
        if (spec.stepped) v = std::floor(v + 0.5);
        target_[index] = v;
        classic_.set_parameter(p, v);
    }

    double parameter(Param p) const { return target_[static_cast<std::size_t>(p)]; }

    // Skip the smoothing ramps — for a preset load or a fresh render, where a
    // 5 ms glide from the previous patch is an artifact, not a feature.
    void snap_parameters() {
        for (int p = 0; p < fdn::kNumParams; ++p) smoothed_[p] = target_[p];
        const bool rate_changed = apply_tank_rate(rate_index());
        push_controls();
        // apply_tank_rate() must configure before controls can be derived at
        // the new rate. Clear once more after that derivation so a snapped rate
        // change starts from the same stochastic/control phase as a cold reset.
        if (rate_changed) reset_rate_domain_state();
        classic_.snap_parameters();
    }

    // Authored EQ. Not a baked param: the in-loop EQ is a voicing decision
    // stamped into a preset, not a knob a host automates — and an automatable
    // in-loop boost would force the stability normalization to trade broadband
    // decay for it on every block.
    void set_eq_band(int index, const fdn::EqBand& band) {
        tank_.eq().set_band(index, band);
        tank_.eq().configure(bridge_.tank_rate());
    }

    void set_active_channels(int n) { tank_.set_active_channels(n); }

    // Flux depth, in dB of in-loop absorption. Voicing, like the EQ: a
    // host-automatable version would let a knob turn change the realized decay.
    void set_flux_depth_db(double db) { tank_.set_flux_depth_db(db); }
    void set_flutter(bool on) { controls_.flutter = on; }

    // Select the engine. Takes effect at the start of the next process_block()
    // call, which flushes the engine it switches to.
    void set_tier(fdn::Tier tier) {
        requested_tier_ = tier;
    }
    fdn::Tier tier() const {
        return requested_tier_;
    }

    void reset() {
        snap_parameters();
        reset_rate_domain_state();
        classic_.reset();
        active_tier_ = requested_tier_;
    }

    // Zero. The engine introduces no block-scale buffering; what it does have
    // is the resampler's 4-point interpolation window, which is a fraction of a
    // millisecond, ratio-dependent, and not bufferable delay. See
    // interpolation_skew_samples() for the measured bound.
    int latency_samples() const { return 0; }

    double interpolation_skew_samples() const {
        return bridge_.interpolation_skew_samples();
    }

    double tank_rate() const { return bridge_.tank_rate(); }

    // Ducker telemetry: the input gain the transient ducker applied to the most
    // recent tank sample, in (0, 1]. Read-only observability for the acceptance
    // suite and the audio harness — the ducker is otherwise invisible, and
    // "the input dipped on that onset" is not inferable from the wet output
    // alone once the tail has smeared it.
    double duck_gain() const { return duck_gain_; }
    const fdn::FdnTank<Engine>& tank() const {
        return tank_;
    }

    // ── Audio ────────────────────────────────────────────────────────────
    // Wet only. `n` may be anything up to the prepared maximum block size.
    void process_block(const SampleType* in_left, const SampleType* in_right,
                       SampleType* out_left, SampleType* out_right, int n) {
        if constexpr (std::is_same_v<SampleType, Engine>) {
            process_engine(in_left, in_right, out_left, out_right, n);
        } else {
            Engine l[fdn::kMaxBlockSamples];
            Engine r[fdn::kMaxBlockSamples];
            for (int offset = 0; offset < n; offset += fdn::kMaxBlockSamples) {
                const int m = std::min(n - offset, fdn::kMaxBlockSamples);
                for (int i = 0; i < m; ++i) {
                    l[i] = static_cast<Engine>(in_left[offset + i]);
                    r[i] = static_cast<Engine>(in_right[offset + i]);
                }
                process_engine(l, r, l, r, m);
                for (int i = 0; i < m; ++i) {
                    out_left[offset + i] = static_cast<SampleType>(l[i]);
                    out_right[offset + i] = static_cast<SampleType>(r[i]);
                }
            }
        }
    }

  private:
    void process_engine(const Engine* in_left, const Engine* in_right, Engine* out_left,
                        Engine* out_right, int n) {
        if (n <= 0) return;
        if (requested_tier_ != active_tier_) {
            active_tier_ = requested_tier_;
            if (active_tier_ == fdn::Tier::classic)
                classic_.reset();
            else
                reset_rate_domain_state();
        }
        if (active_tier_ == fdn::Tier::classic) {
            classic_.process_block(in_left, in_right, out_left, out_right, n);
            return;
        }
        int offset = 0;
        while (offset < n) {
            const int chunk = std::min(n - offset, max_block_);
            process_chunk(in_left + offset, in_right + offset, out_left + offset,
                          out_right + offset, chunk);
            offset += chunk;
        }
    }

    // tank_rate is stepped and never smoothed, so the requested index is read
    // straight off the target rather than off the smoothing state.
    int rate_index() const {
        const auto raw = static_cast<int>(
            target_[static_cast<std::size_t>(Param::tank_rate)] + 0.5);
        return std::clamp(raw, 0, fdn::kNumTankRates - 1);
    }

    void reset_rate_domain_state() {
        bridge_.reset();
        for (auto& line : predelay_) line.reset();
        for (auto& cascade : pre_diffusion_) cascade.reset();
        tank_.reset();
        ensemble_.reset();
        ducker_.reset();
        input_lp_.reset();
        output_lp_.reset();
        duck_gain_ = 1.0;
        control_countdown_ = 0;
    }

    // A live tank-rate change is a hard, clean flush — never a crossfade. The
    // running tail is dropped rather than reinterpreted at the new rate, so no
    // pitch-shifted tail artifact is possible, and every resampler phase and
    // filter state resets with it. tank_rate is a setup/texture choice, not a
    // performance macro: do not macro-automate it.
    //
    // It must only ever be applied at a CHUNK BOUNDARY. The two resampler legs
    // agree on how many tank samples a host chunk is worth; changing the ratio
    // between them — after the input leg has produced its samples but before
    // the output leg consumes them — leaves the output leg permanently reading
    // past the end of the tank stream, and a switch UP in rate then silences the
    // wet output for good rather than flushing it. process_chunk() applies any
    // pending change before either leg runs, which is what keeps the two in
    // step.
    bool apply_tank_rate(int index) {
        if (index == active_rate_index_) return false;
        active_rate_index_ = index;
        const double rate = fdn::kTankRates[static_cast<std::size_t>(index)];
        bridge_.configure(rate);
        tank_.configure(rate);
        ducker_.configure(rate);
        ensemble_.configure(rate);
        for (int c = 0; c < kNumRails; ++c) {
            pre_diffusion_[c].configure(fdn::kPreDiffusionBaseMs, rate,
                                        c == 0 ? 3 : 7);
        }
        design_input_lp(rate);
        smoothing_coeff_ = 1.0 - std::exp(-static_cast<double>(fdn::kControlRateSamples) /
                                          std::max(1.0, fdn::kSmoothingMs * 0.001 * rate));
        makeup_gain_ = std::pow(
            10.0, fdn::kRateMakeupDb[static_cast<std::size_t>(index)] / 20.0);
        reset_rate_domain_state();
        return true;
    }

    // Below an 18 kHz tank bandwidth an extra 2-pole lowpass engages INSIDE the
    // tank domain, ahead of the network. This is embracing the darkening, not
    // fighting it: it is the vintage-hardware character of the low rates.
    void design_input_lp(double tank_rate) {
        input_lp_active_ = (fdn::kTankInputLpFraction * tank_rate) <
                           fdn::kTankInputLpThresholdHz;
        if (!input_lp_active_) return;
        BiquadT<double> designer;
        designer.set_coefficients(BiquadT<double>::Type::lowpass,
                                  fdn::kTankInputLpFraction * tank_rate,
                                  0.7071067811865476, tank_rate);
        input_lp_coefficients_ = fdn::StereoSection::from(designer.coefficients());
    }

    // The per-mode wet lowpass runs at the HOST rate, after reconstruction: it
    // shapes the wet return, and doing it downstream of the resampler keeps it
    // independent of the tank-rate texture.
    void design_output_lp() {
        BiquadT<double> designer;
        designer.set_coefficients(BiquadT<double>::Type::lowpass,
                                  std::min(output_lp_hz_, 0.45 * host_rate_),
                                  0.7071067811865476, host_rate_);
        output_lp_coefficients_ = fdn::StereoSection::from(designer.coefficients());
    }

    void push_controls() {
        controls_.decay = smoothed_[static_cast<std::size_t>(Param::decay)];
        controls_.size = smoothed_[static_cast<std::size_t>(Param::size)];
        controls_.damp_hi = smoothed_[static_cast<std::size_t>(Param::damp_hi)];
        controls_.damp_lo = smoothed_[static_cast<std::size_t>(Param::damp_lo)];
        controls_.diffusion = smoothed_[static_cast<std::size_t>(Param::diffusion)];
        controls_.mod = smoothed_[static_cast<std::size_t>(Param::mod)];
        controls_.shimmer = smoothed_[static_cast<std::size_t>(Param::shimmer)];
        controls_.drive = smoothed_[static_cast<std::size_t>(Param::drive)];
        controls_.bloom = smoothed_[static_cast<std::size_t>(Param::bloom)];
        tank_.update_controls(controls_);

        const double tank_rate = bridge_.tank_rate();
        predelay_samples_ =
            std::max(2.0, smoothed_[static_cast<std::size_t>(Param::predelay)] *
                              0.001 * tank_rate);
        pre_g_ = static_cast<Engine>(smoothed_[static_cast<std::size_t>(Param::diffusion)] *
                                     fdn::kPreDiffusionG);
        width_ = smoothed_[static_cast<std::size_t>(Param::width)];
        const double drive = smoothed_[static_cast<std::size_t>(Param::drive)];
        if (drive != makeup_drive_) {
            makeup_drive_ = drive;
            drive_makeup_gain_ = std::pow(10.0, drive * fdn::kDriveMakeupDb / 20.0);
        }
    }

    void tick_controls() {
        const double coeff = smoothing_coeff_;
        for (int p = 0; p < fdn::kNumParams; ++p) {
            if (fdn::kParamSpecs[static_cast<std::size_t>(p)].stepped)
                smoothed_[p] = target_[p];
            else
                smoothed_[p] += (target_[p] - smoothed_[p]) * coeff;
        }
        push_controls();
    }

    void process_chunk(const Engine* in_left, const Engine* in_right, Engine* out_left,
                       Engine* out_right, int n) {
        // Before either resampler leg runs — see the note on apply_tank_rate().
        apply_tank_rate(rate_index());
        const int tank_n = bridge_.host_to_tank(in_left, in_right, n);
        Engine* tl = bridge_.tank_input(0);
        Engine* tr = bridge_.tank_input(1);
        Engine* ol = bridge_.tank_output(0);
        Engine* or_ = bridge_.tank_output(1);

        // Tank-rate work runs in segments that never cross a control tick, so
        // every stage inside a segment sees constant controls. The ticks fall
        // on the same tank samples however the host divides its blocks.
        int done = 0;
        while (done < tank_n) {
            if (control_countdown_ <= 0) {
                tick_controls();
                control_countdown_ = fdn::kControlRateSamples;
            }
            const int segment = std::min(tank_n - done, control_countdown_);
            control_countdown_ -= segment;
            process_segment(tl + done, tr + done, ol + done, or_ + done, segment);
            done += segment;
            // The ducker's envelopes are cleaned on the control grid, which
            // falls on the same tank samples however the host splits blocks.
            if (control_countdown_ == 0)
                ducker_.heal();
        }

        bridge_.tank_to_host(tank_n, out_left, out_right, n);

        // Host-rate tail: the per-mode wet lowpass, then the rate-step makeup
        // gain — the last multiply before the output buffer.
        const auto makeup = fdn::lanes::splat(makeup_gain_);
        const auto output_lp = fdn::StereoSectionLanes::from(output_lp_coefficients_);
        auto output_state = output_lp_;
        for (int i = 0; i < n; ++i) {
            const auto y = output_state.process(
                &output_lp, fdn::lanes::make(static_cast<double>(out_left[i]),
                                             static_cast<double>(out_right[i])));
            const auto scaled = y * makeup;
            out_left[i] = static_cast<Engine>(fdn::lanes::lane(scaled, 0));
            out_right[i] = static_cast<Engine>(fdn::lanes::lane(scaled, 1));
        }
        output_lp_ = output_state;
    }

    // One segment of at most a control period, all in the tank domain.
    void process_segment(const Engine* tl, const Engine* tr, Engine* ol, Engine* or_, int n) {
        Engine l[fdn::kMaxBlockSamples];
        Engine r[fdn::kMaxBlockSamples];

        // Predelay, then the transient ducker, then the low-rate input
        // lowpass. The predelay pushes the whole segment first and reads each
        // sample back relative to its own push.
        // The whole segment is pushed first; each sample is read back against
        // its own post-push position, n - 1 behind the segment's end.
        predelay_[0].push_block(tl, n);
        predelay_[1].push_block(tr, n);
        predelay_[0].read_hermite_span(predelay_samples_, l, n, n - 1);
        predelay_[1].read_hermite_span(predelay_samples_, r, n, n - 1);
        duck_gain_ = ducker_.process_block(l, r, n);
        if (input_lp_active_) {
            const auto input_lp = fdn::StereoSectionLanes::from(input_lp_coefficients_);
            auto input_state = input_lp_;
            for (int k = 0; k < n; ++k) {
                const auto y =
                    input_state.process(&input_lp, fdn::lanes::make(static_cast<double>(l[k]),
                                                                    static_cast<double>(r[k])));
                l[k] = static_cast<Engine>(fdn::lanes::lane(y, 0));
                r[k] = static_cast<Engine>(fdn::lanes::lane(y, 1));
            }
            input_lp_ = input_state;
        }

        // Pre-diffusion with the coefficient's sign alternated between the
        // rails, so left and right decorrelate from the first reflection.
        pre_diffusion_[0].process_block(l, n, pre_g_);
        pre_diffusion_[1].process_block(r, n, static_cast<Engine>(-pre_g_));

        Engine wet_l[fdn::kMaxBlockSamples];
        Engine wet_r[fdn::kMaxBlockSamples];
        tank_.process_block(l, r, wet_l, wet_r, n);

        const auto makeup = static_cast<Engine>(drive_makeup_gain_);
        const auto width = static_cast<Engine>(width_);
        for (int k = 0; k < n; ++k) {
            const Engine dl = wet_l[k] * makeup;
            const Engine dr = wet_r[k] * makeup;
            const Engine mid = Engine{0.5} * (dl + dr);
            const Engine side = Engine{0.5} * (dl - dr) * width;
            wet_l[k] = mid + side;
            wet_r[k] = mid - side;
        }
        ensemble_.process_block(wet_l, wet_r, n);
        for (int k = 0; k < n; ++k) {
            ol[k] = fdn::soft_limit(wet_l[k]);
            or_[k] = fdn::soft_limit(wet_r[k]);
        }
    }

    static constexpr int kNumRails = 2;

    fdn::MultirateBridge<Engine> bridge_{};
    std::array<fdn::FracDelayT<Engine>, kNumRails> predelay_{};
    std::array<fdn::AllpassCascade<Engine>, kNumRails> pre_diffusion_{};
    fdn::FdnTank<Engine> tank_{};
    fdn::TransientDucker ducker_{};
    fdn::EnsembleChorus<Engine> ensemble_{};
    // Both rails share each design; the state runs them as one two-lane
    // vector, snapped and sanitized once per block.
    fdn::StereoBiquadState<1> input_lp_{};
    fdn::StereoBiquadState<1> output_lp_{};
    fdn::StereoSection input_lp_coefficients_{};
    fdn::StereoSection output_lp_coefficients_{};

    std::array<double, fdn::kNumParams> target_{};
    std::array<double, fdn::kNumParams> smoothed_{};
    fdn::TankControls controls_{};

    double host_rate_ = 48000.0;
    double predelay_samples_ = 2.0;
    double width_ = 1.0;
    double makeup_gain_ = 1.0;
    double drive_makeup_gain_ = 1.0;
    double makeup_drive_ = 0.0;
    double smoothing_coeff_ = 1.0;
    double duck_gain_ = 1.0;
    double output_lp_hz_ = 14000.0;
    Engine pre_g_ = 0;
    int max_block_ = 512;
    int control_countdown_ = 0;
    int active_rate_index_ = -1;
    ClassicReverbT<Engine> classic_{};
    fdn::Tier requested_tier_ = fdn::Tier::network;
    fdn::Tier active_tier_ = fdn::Tier::network;
    bool input_lp_active_ = false;
};

using FdnReverb = FdnReverbT<float>;

}  // namespace pulp::signal
