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
// RT contract: prepare() allocates for the worst case (96 kHz tank at the
// maximum block size). Every setter, process_block(), reset(), and a live
// tank-rate change allocate nothing.

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/denormal.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/diffusion.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/multirate.hpp>
#include <pulp/signal/fdn/stages.hpp>
#include <pulp/signal/fdn/tank.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal {

template <typename SampleType = float>
class FdnReverbT {
public:
    using Param = fdn::Param;
    using Mode = fdn::Mode;

    // ── Lifecycle ────────────────────────────────────────────────────────
    void prepare(double sample_rate, int max_block_size) {
        host_rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        max_block_ = std::max(max_block_size, 1);
        bridge_.prepare(host_rate_, max_block_);

        const int predelay_cap =
            static_cast<int>(0.001 * fdn::kParamSpecs[static_cast<std::size_t>(Param::predelay)]
                                         .max * fdn::kMaxTankRate) +
            fdn::kHermiteGuard;
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
    }

    double parameter(Param p) const { return target_[static_cast<std::size_t>(p)]; }

    // Skip the smoothing ramps — for a preset load or a fresh render, where a
    // 5 ms glide from the previous patch is an artifact, not a feature.
    void snap_parameters() {
        for (int p = 0; p < fdn::kNumParams; ++p) smoothed_[p] = target_[p];
        apply_tank_rate(rate_index());
        push_controls();
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

    void reset() {
        bridge_.reset();
        for (auto& line : predelay_) line.reset();
        for (auto& cascade : pre_diffusion_) cascade.reset();
        tank_.reset();
        ensemble_.reset();
        ducker_.reset();
        for (auto& f : input_lp_) f.reset();
        for (auto& f : output_lp_) f.reset();
        control_countdown_ = 0;
        snap_parameters();
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
    const fdn::FdnTank<SampleType>& tank() const { return tank_; }

    // ── Audio ────────────────────────────────────────────────────────────
    // Wet only. `n` may be anything up to the prepared maximum block size.
    void process_block(const SampleType* in_left, const SampleType* in_right,
                       SampleType* out_left, SampleType* out_right, int n) {
        if (n <= 0) return;
        int offset = 0;
        while (offset < n) {
            const int chunk = std::min(n - offset, max_block_);
            process_chunk(in_left + offset, in_right + offset, out_left + offset,
                          out_right + offset, chunk);
            offset += chunk;
        }
    }

private:
    // tank_rate is stepped and never smoothed, so the requested index is read
    // straight off the target rather than off the smoothing state.
    int rate_index() const {
        const auto raw = static_cast<int>(
            target_[static_cast<std::size_t>(Param::tank_rate)] + 0.5);
        return std::clamp(raw, 0, fdn::kNumTankRates - 1);
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
    void apply_tank_rate(int index) {
        if (index == active_rate_index_) return;
        active_rate_index_ = index;
        const double rate = fdn::kTankRates[static_cast<std::size_t>(index)];
        bridge_.configure(rate);
        tank_.configure(rate);
        ducker_.configure(rate);
        ensemble_.configure(rate);
        for (int c = 0; c < kNumRails; ++c) {
            pre_diffusion_[c].configure(fdn::kPreDiffusionBaseMs, rate,
                                        c == 0 ? 3 : 7);
            pre_diffusion_[c].reset();
            predelay_[c].reset();
            input_lp_[c].reset();
        }
        design_input_lp(rate);
        makeup_gain_ = std::pow(
            10.0, fdn::kRateMakeupDb[static_cast<std::size_t>(index)] / 20.0);
        tank_.reset();
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
        const auto c = designer.coefficients();
        for (auto& f : input_lp_) f.set_coefficients(c);
    }

    // The per-mode wet lowpass runs at the HOST rate, after reconstruction: it
    // shapes the wet return, and doing it downstream of the resampler keeps it
    // independent of the tank-rate texture.
    void design_output_lp() {
        BiquadT<double> designer;
        designer.set_coefficients(BiquadT<double>::Type::lowpass,
                                  std::min(output_lp_hz_, 0.45 * host_rate_),
                                  0.7071067811865476, host_rate_);
        const auto c = designer.coefficients();
        for (auto& f : output_lp_) f.set_coefficients(c);
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
        pre_g_ = static_cast<SampleType>(
            smoothed_[static_cast<std::size_t>(Param::diffusion)] *
            fdn::kPreDiffusionG);
        width_ = smoothed_[static_cast<std::size_t>(Param::width)];
    }

    void tick_controls() {
        const double coeff =
            1.0 - std::exp(-static_cast<double>(fdn::kControlRateSamples) /
                           std::max(1.0, fdn::kSmoothingMs * 0.001 * bridge_.tank_rate()));
        for (int p = 0; p < fdn::kNumParams; ++p) {
            if (fdn::kParamSpecs[static_cast<std::size_t>(p)].stepped)
                smoothed_[p] = target_[p];
            else
                smoothed_[p] += (target_[p] - smoothed_[p]) * coeff;
        }
        push_controls();
    }

    void process_chunk(const SampleType* in_left, const SampleType* in_right,
                       SampleType* out_left, SampleType* out_right, int n) {
        // Before either resampler leg runs — see the note on apply_tank_rate().
        apply_tank_rate(rate_index());
        const int tank_n = bridge_.host_to_tank(in_left, in_right, n);
        SampleType* tl = bridge_.tank_input(0);
        SampleType* tr = bridge_.tank_input(1);
        SampleType* ol = bridge_.tank_output(0);
        SampleType* or_ = bridge_.tank_output(1);

        for (int i = 0; i < tank_n; ++i) {
            if (control_countdown_ <= 0) {
                tick_controls();
                control_countdown_ = fdn::kControlRateSamples;
            }
            --control_countdown_;

            // Predelay, then the transient ducker, then the low-rate input
            // lowpass — all inside the tank domain.
            predelay_[0].push(tl[i]);
            predelay_[1].push(tr[i]);
            SampleType l = predelay_[0].read(predelay_samples_);
            SampleType r = predelay_[1].read(predelay_samples_);

            const double duck = ducker_.process(
                0.5 * (std::abs(static_cast<double>(l)) + std::abs(static_cast<double>(r))));
            duck_gain_ = duck;
            l = static_cast<SampleType>(static_cast<double>(l) * duck);
            r = static_cast<SampleType>(static_cast<double>(r) * duck);

            if (input_lp_active_) {
                l = static_cast<SampleType>(input_lp_[0].process(static_cast<double>(l)));
                r = static_cast<SampleType>(input_lp_[1].process(static_cast<double>(r)));
            }

            // Pre-diffusion with the coefficient's sign alternated between the
            // rails, so left and right decorrelate from the first reflection.
            l = pre_diffusion_[0].process(l, pre_g_);
            r = pre_diffusion_[1].process(r, static_cast<SampleType>(-pre_g_));

            SampleType wet_l = 0;
            SampleType wet_r = 0;
            tank_.process(l, r, wet_l, wet_r);

            double dl = static_cast<double>(wet_l);
            double dr = static_cast<double>(wet_r);
            fdn::apply_width(dl, dr, width_);
            SampleType el = static_cast<SampleType>(dl);
            SampleType er = static_cast<SampleType>(dr);
            ensemble_.process(el, er);

            ol[i] = static_cast<SampleType>(fdn::soft_limit(static_cast<double>(el)));
            or_[i] = static_cast<SampleType>(fdn::soft_limit(static_cast<double>(er)));
        }

        bridge_.tank_to_host(tank_n, out_left, out_right, n);

        // Host-rate tail: the per-mode wet lowpass, then the rate-step makeup
        // gain — the last multiply before the output buffer.
        for (int i = 0; i < n; ++i) {
            out_left[i] = static_cast<SampleType>(
                output_lp_[0].process(static_cast<double>(out_left[i])) * makeup_gain_);
            out_right[i] = static_cast<SampleType>(
                output_lp_[1].process(static_cast<double>(out_right[i])) * makeup_gain_);
        }
    }

    static constexpr int kNumRails = 2;

    fdn::MultirateBridge<SampleType> bridge_{};
    std::array<fdn::FracDelayT<SampleType>, kNumRails> predelay_{};
    std::array<fdn::AllpassCascade<SampleType>, kNumRails> pre_diffusion_{};
    fdn::FdnTank<SampleType> tank_{};
    fdn::TransientDucker ducker_{};
    fdn::EnsembleChorus<SampleType> ensemble_{};
    std::array<BiquadT<double>, kNumRails> input_lp_{};
    std::array<BiquadT<double>, kNumRails> output_lp_{};

    std::array<double, fdn::kNumParams> target_{};
    std::array<double, fdn::kNumParams> smoothed_{};
    fdn::TankControls controls_{};

    double host_rate_ = 48000.0;
    double predelay_samples_ = 2.0;
    double width_ = 1.0;
    double makeup_gain_ = 1.0;
    double duck_gain_ = 1.0;
    double output_lp_hz_ = 14000.0;
    SampleType pre_g_ = 0;
    int max_block_ = 512;
    int control_countdown_ = 0;
    int active_rate_index_ = -1;
    bool input_lp_active_ = false;
};

using FdnReverb = FdnReverbT<float>;

}  // namespace pulp::signal
