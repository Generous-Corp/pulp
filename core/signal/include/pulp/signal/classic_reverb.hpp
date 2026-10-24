#pragma once

// Classic reverb tier — parallel lowpass-feedback combs into series allpasses,
// the Schroeder/Moorer topology, driven by the same parameter set as the
// multirate FDN reverb (fdn::Param).
//
// WHAT IT IS FOR
// The FDN engine is a sixteen-line network with a resampling tank, layered
// modulation, flux, shimmer and an output ensemble. This tier is the cheap,
// conventional reverb: eight combs per rail, four allpasses per rail, all at
// the host rate. It takes the controls that mean the same thing in both
// engines and ignores the ones that describe machinery it does not have:
//
//   decay      T60 in seconds. Every comb's feedback gain follows the
//              absorptive law g = 10^(-3 D / (T60 fs)), so each comb reaches
//              -60 dB at the same time whatever its length.
//   size       scales every comb and allpass length, as in the FDN. Lengths
//              are set when the parameters are snapped (snap_parameters(),
//              reset(), set_mode()): integer comb lengths cannot glide, so a
//              live size change waits for the next snap rather than jumping.
//   predelay   milliseconds before the combs.
//   damp_hi    the in-loop one-pole lowpass, on the FDN's cutoff curve, with
//              its loss scaled per comb by length so the frequency-dependent
//              decay agrees across combs.
//   diffusion  the allpass coefficient (diffusion x 0.7).
//   width      mid/side width of the wet output.
//   damp_lo, mod, shimmer, drive, bloom, tank_rate
//              accepted and stored, no effect: this tier has no in-loop
//              highpass, modulation, pitch shifting, saturation, gain
//              lifting, or separate tank rate.
//
// Continuous controls are smoothed at the 32-sample control rate, and the
// comb gains, damping and width then move to each tick's values in
// per-sample steps, so automation does not zipper. The allpass coefficient
// and predelay change at the tick.
//
// Output is WET ONLY, like the FDN engine. Latency is zero.
//
// BLOCK PROCESSING
// Every comb is longer than a control period, so a period runs at once: the
// combs read spans written before the period began and write theirs after, and
// the allpasses run along time on the same principle. Recursive state is
// snapped on a fixed sample grid, so the result does not depend on how the
// host divides its blocks.
//
// RT contract: prepare() allocates. Setters, process_block(), and reset()
// allocate nothing.

#include <pulp/signal/denormal.hpp>
#include <pulp/signal/detail/simd_lanes.hpp>
#include <pulp/signal/fdn/config.hpp>
#include <pulp/signal/fdn/diffusion.hpp>
#include <pulp/signal/fdn/frac_delay.hpp>
#include <pulp/signal/fdn/sanitize.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace pulp::signal {

namespace classic_reverb {

inline constexpr int kCombsPerRail = 8;
inline constexpr int kCombs = 2 * kCombsPerRail;
inline constexpr int kAllpassStages = 4;

// Comb lengths at size 0.5, in milliseconds: spread over about an octave's
// worth of modal spacing between 25 and 37 ms, so the comb peaks interleave.
inline constexpr std::array<double, kCombsPerRail> kCombMs = {25.6, 27.1, 28.9, 30.4,
                                                              32.0, 33.3, 34.9, 36.6};
// Added (never multiplied) to the rounded lengths so no two combs coincide.
inline constexpr std::array<int, kCombsPerRail> kCombPrimes = {2, 3, 5, 7, 11, 13, 17, 19};
// The right rail runs every comb and allpass this much longer, which is what
// decorrelates the two outputs.
inline constexpr double kRailSpreadMs = 0.48;
inline constexpr double kAllpassBaseMs = 12.0;
// Per-rail input gain into the comb banks, and the wet output gain: chosen so the
// tier's wet level at its defaults sits within a decibel of the FDN engine's.
inline constexpr double kInputGain = 0.05;
inline constexpr double kOutputGain = 0.7;

} // namespace classic_reverb

template <typename SampleType = float> class ClassicReverbT {
    static_assert(std::is_same_v<SampleType, float>,
                  "the classic reverb's block kernels are single precision");

  public:
    using Param = fdn::Param;
    using Mode = fdn::Mode;

    void prepare(double sample_rate, int max_block_size) {
        rate_ = sample_rate > 0.0 ? sample_rate : 48000.0;
        (void)max_block_size;
        const double longest_ms =
            classic_reverb::kCombMs.back() * fdn::kSizeScaleMax + classic_reverb::kRailSpreadMs;
        const int comb_cap = static_cast<int>(longest_ms * 0.001 * rate_) +
                             classic_reverb::kCombPrimes.back() + fdn::kMaxBlockSamples + 4;
        std::size_t size = 8;
        while (size < static_cast<std::size_t>(comb_cap))
            size <<= 1;
        // All sixteen combs share one write position, so they share one
        // interleaved ring: row p holds every comb's sample p, a sample's
        // writes are four vector stores, and kMaxBlockSamples rows past the
        // end mirror the first rows so no span ever wraps.
        storage_.assign((size + static_cast<std::size_t>(fdn::kMaxBlockSamples)) * kCombs,
                        SampleType{0});
        comb_mask_ = size - 1;
        for (auto& cascade : allpass_)
            cascade.prepare(classic_reverb::kAllpassBaseMs * fdn::kSizeScaleMax +
                                classic_reverb::kRailSpreadMs,
                            rate_, classic_reverb::kAllpassStages);
        const int predelay_cap =
            static_cast<int>(
                0.001 * fdn::kParamSpecs[static_cast<std::size_t>(Param::predelay)].max * rate_) +
            fdn::kHermiteGuard + fdn::kMaxBlockSamples;
        for (auto& line : predelay_)
            line.prepare(predelay_cap);
        smoothing_coeff_ = 1.0 - std::exp(-static_cast<double>(fdn::kControlRateSamples) /
                                          std::max(1.0, fdn::kSmoothingMs * 0.001 * rate_));
        for (int p = 0; p < fdn::kNumParams; ++p) {
            target_[static_cast<std::size_t>(p)] =
                fdn::kParamSpecs[static_cast<std::size_t>(p)].default_value;
        }
        reset();
    }

    // Stamp a mode's defaults, as FdnReverbT::set_mode does.
    void set_mode(Mode mode) {
        for (int p = 0; p < fdn::kNumParams; ++p) {
            const auto param = static_cast<Param>(p);
            if (param == Param::predelay || param == Param::width)
                continue;
            set_parameter(param, fdn::mode_default(mode, param));
        }
        snap_parameters();
    }

    void set_parameter(Param p, double value) {
        const fdn::ParamSpec& spec = fdn::kParamSpecs[static_cast<std::size_t>(p)];
        double v = std::clamp(value, spec.min, spec.max);
        if (spec.stepped)
            v = std::floor(v + 0.5);
        target_[static_cast<std::size_t>(p)] = v;
        settling_ = true;
    }

    double parameter(Param p) const {
        return target_[static_cast<std::size_t>(p)];
    }

    // Skip the smoothing ramps, and re-derive the lengths from size.
    void snap_parameters() {
        smoothed_ = target_;
        smoothing_stages_.fill(target_);
        settling_ = false;
        derive_lengths();
        derive();
        ramping_ = false;
    }

    void reset() {
        snap_parameters();
        std::fill(storage_.begin(), storage_.end(), SampleType{0});
        write_ = 0;
        lp_.fill(SampleType{0});
        for (auto& cascade : allpass_)
            cascade.reset();
        for (auto& line : predelay_)
            line.reset();
        control_countdown_ = 0;
    }

    int latency_samples() const {
        return 0;
    }

    // The realized comb lengths and feedback gains, for structural tests.
    int comb_length(int comb) const {
        return length_[static_cast<std::size_t>(comb)];
    }
    double comb_gain(int comb) const {
        return gain_[static_cast<std::size_t>(comb)];
    }

    // Wet only. Any n; the input may alias neither output.
    void process_block(const SampleType* in_l, const SampleType* in_r, SampleType* out_l,
                       SampleType* out_r, int n) {
        int done = 0;
        while (done < n) {
            if (control_countdown_ <= 0) {
                tick_controls();
                control_countdown_ = fdn::kControlRateSamples;
            }
            const int piece = std::min(n - done, control_countdown_);
            process_piece(in_l + done, in_r + done, out_l + done, out_r + done, piece,
                          fdn::kControlRateSamples - control_countdown_);
            control_countdown_ -= piece;
            if (control_countdown_ == 0)
                heal();
            done += piece;
        }
    }

  private:
    static constexpr int kCombs = classic_reverb::kCombs;

    // Continuous controls pass through three one-pole stages. A host that
    // moves a target once per block steps it at the block rate, and each
    // stage takes those steps down by about 20 dB more before they reach the
    // comb gains; three bring a block-rate sweep's sidebands to the floor of
    // a measurement that sees -116 dB, for about 10 ms more settling.
    void tick_controls() {
        // Once a tick moves nothing, every later tick is the same no-op until
        // a target changes.
        if (!settling_) {
            ramping_ = false;
            return;
        }
        bool changed = false;
        bool moving = false;
        for (std::size_t p = 0; p < smoothed_.size(); ++p) {
            const double before = smoothed_[p];
            if (fdn::kParamSpecs[p].stepped) {
                for (auto& stage : smoothing_stages_)
                    stage[p] = target_[p];
                smoothed_[p] = target_[p];
            } else {
                double in = target_[p];
                for (auto& stage : smoothing_stages_) {
                    const double was = stage[p];
                    stage[p] += (in - stage[p]) * smoothing_coeff_;
                    moving = moving || stage[p] != was;
                    in = stage[p];
                }
                smoothed_[p] += (in - smoothed_[p]) * smoothing_coeff_;
            }
            changed = changed || smoothed_[p] != before;
        }
        settling_ = changed || moving;
        ramping_ = changed;
        if (!changed)
            return;
        // The comb gains, the damping and the width move to their new values
        // over the control period in equal per-sample steps, starting from
        // where the last period ended, so a swept control changes the sound
        // smoothly rather than in 32-sample stairs.
        const auto from_gain = gain_f_;
        const auto from_damp = damp_f_;
        const SampleType from_width = width_;
        derive();
        constexpr auto inv = static_cast<SampleType>(1.0 / fdn::kControlRateSamples);
        for (std::size_t i = 0; i < static_cast<std::size_t>(kCombs); ++i) {
            gain_from_[i] = from_gain[i];
            gain_step_[i] = (gain_f_[i] - from_gain[i]) * inv;
            damp_from_[i] = from_damp[i];
            damp_step_[i] = (damp_f_[i] - from_damp[i]) * inv;
        }
        width_from_ = from_width;
        width_step_ = (width_ - from_width) * inv;
    }

    double smoothed(Param p) const {
        return smoothed_[static_cast<std::size_t>(p)];
    }

    // Comb and allpass lengths from size.
    void derive_lengths() {
        const double scale = fdn::kSizeScaleMin + std::clamp(smoothed(Param::size), 0.0, 1.0) *
                                                      (fdn::kSizeScaleMax - fdn::kSizeScaleMin);
        const double spread = classic_reverb::kRailSpreadMs * 0.001 * rate_;
        const int longest = static_cast<int>(comb_mask_) + 1 - fdn::kMaxBlockSamples - 4;
        for (int rail = 0; rail < 2; ++rail) {
            for (int c = 0; c < classic_reverb::kCombsPerRail; ++c) {
                const auto i = static_cast<std::size_t>(rail * classic_reverb::kCombsPerRail + c);
                const double base =
                    classic_reverb::kCombMs[static_cast<std::size_t>(c)] * 0.001 * rate_ * scale;
                const int length = static_cast<int>(std::lround(base + rail * spread)) +
                                   classic_reverb::kCombPrimes[static_cast<std::size_t>(c)];
                length_[i] = std::clamp(length, fdn::kMaxBlockSamples, longest);
            }
        }
        for (int rail = 0; rail < 2; ++rail) {
            allpass_[static_cast<std::size_t>(rail)].configure(
                classic_reverb::kAllpassBaseMs * scale, rate_,
                rail == 0 ? 0 : static_cast<int>(std::lround(spread)));
        }
    }

    // Gains, damping, allpass coefficient, predelay and width from the
    // smoothed controls and the current lengths.
    void derive() {
        // The absorptive law on every comb, and damping whose loss scales with
        // length (the FDN's rule): a longer comb filters proportionally harder.
        const double t60 = std::max(smoothed(Param::decay), 0.01);
        const double lp_hz = fdn::kDampHiMinHz *
                             std::pow(fdn::kDampHiMaxHz / fdn::kDampHiMinHz,
                                      std::pow(1.0 - std::clamp(smoothed(Param::damp_hi), 0.0, 1.0),
                                               fdn::kDampCurve));
        const double pole = std::exp(-6.283185307179586 * lp_hz / rate_);
        double median = 0.0;
        for (int i = 0; i < kCombs; ++i)
            median += static_cast<double>(length_[static_cast<std::size_t>(i)]);
        median /= kCombs;
        for (std::size_t i = 0; i < static_cast<std::size_t>(kCombs); ++i) {
            const double length = static_cast<double>(length_[i]);
            gain_[i] = std::pow(10.0, -3.0 * length / (t60 * rate_));
            const double ratio = std::clamp(length / median, 0.05, 20.0);
            const double damp = 1.0 - std::pow(1.0 - pole, ratio);
            gain_f_[i] = static_cast<SampleType>(gain_[i]);
            damp_f_[i] = static_cast<SampleType>(damp);
        }
        allpass_g_ = static_cast<SampleType>(smoothed(Param::diffusion) * fdn::kPreDiffusionG);
        predelay_samples_ = std::max(2.0, smoothed(Param::predelay) * 0.001 * rate_);
        width_ = static_cast<SampleType>(smoothed(Param::width));
    }

    // `phase` is the piece's first sample's position within its control period.
    void process_piece(const SampleType* in_l, const SampleType* in_r, SampleType* out_l,
                       SampleType* out_r, int n, int phase) {
        // Each rail through its own predelay, pushed as a span and read back
        // against each sample's own post-push position. The left input feeds
        // the left comb bank and the right the right one, so the tier is true
        // stereo like the network: an anti-phase input is not cancelled.
        SampleType feed[2][fdn::kMaxBlockSamples];
        constexpr auto input_gain = static_cast<SampleType>(classic_reverb::kInputGain);
        for (int k = 0; k < n; ++k) {
            feed[0][k] = input_gain * fdn::finite_or_zero(in_l[k]);
            feed[1][k] = input_gain * fdn::finite_or_zero(in_r[k]);
        }
        for (int rail = 0; rail < 2; ++rail) {
            predelay_[static_cast<std::size_t>(rail)].push_block(feed[rail], n);
            predelay_[static_cast<std::size_t>(rail)].read_hermite_span(predelay_samples_,
                                                                        feed[rail], n, n - 1);
        }

        // The comb bank.
        SampleType left[fdn::kMaxBlockSamples];
        SampleType right[fdn::kMaxBlockSamples];
        if (ramping_)
            run_combs<true>(feed[0], feed[1], left, right, n, phase);
        else
            run_combs<false>(feed[0], feed[1], left, right, n, phase);
        commit_combs(n);

        allpass_[0].process_block(left, n, allpass_g_);
        allpass_[1].process_block(right, n, allpass_g_);

        constexpr auto output_gain = static_cast<SampleType>(classic_reverb::kOutputGain);
        const auto mix = [&](int k, SampleType width) {
            const SampleType mid = SampleType{0.5} * (left[k] + right[k]);
            const SampleType side = SampleType{0.5} * width * (left[k] - right[k]);
            out_l[k] = output_gain * (mid + side);
            out_r[k] = output_gain * (mid - side);
        };
        if (ramping_) {
            for (int k = 0; k < n; ++k)
                mix(k, width_from_ + width_step_ * static_cast<SampleType>(phase + k + 1));
        } else {
            for (int k = 0; k < n; ++k)
                mix(k, width_);
        }
    }

    std::size_t read_index(std::size_t comb) const {
        return (write_ - static_cast<std::size_t>(length_[comb])) & comb_mask_;
    }

    // Every comb outputs its delayed sample and stores the input plus its
    // lowpassed delayed sample times its gain. Sixteen combs are four 4-lane
    // vectors (combs 0-7 the left rail, 8-15 the right); each reads its own
    // delayed row, all write the same row. While Ramp, each sample's gain and
    // damping are the ramp's value at that sample's place in the period.
    template <bool Ramp>
    void run_combs(const SampleType* in_l, const SampleType* in_r, SampleType* left,
                   SampleType* right, int n, int phase) {
        namespace lanes = detail::lanes;
        using lanes::F4;
        constexpr std::size_t kRow = kCombs;
        const SampleType* read[kCombs];
        for (std::size_t i = 0; i < static_cast<std::size_t>(kCombs); ++i)
            read[i] = storage_.data() + read_index(i) * kRow + i;
        SampleType* write = storage_.data() + write_ * kRow;
        F4 lp[4], damp[4], gain[4];
        for (int j = 0; j < 4; ++j) {
            lp[j] = lanes::load(&lp_[4 * static_cast<std::size_t>(j)]);
            damp[j] = lanes::load(&damp_f_[4 * static_cast<std::size_t>(j)]);
            gain[j] = lanes::load(&gain_f_[4 * static_cast<std::size_t>(j)]);
        }
        for (int k = 0; k < n; ++k) {
            const std::size_t at = static_cast<std::size_t>(k) * kRow;
            F4 delayed[4];
            for (int j = 0; j < 4; ++j)
                delayed[j] = lanes::make(read[4 * j][at], read[4 * j + 1][at], read[4 * j + 2][at],
                                         read[4 * j + 3][at]);
            const F4 feed[2] = {lanes::splat(in_l[k]), lanes::splat(in_r[k])};
            if constexpr (Ramp) {
                const F4 step = lanes::splat(static_cast<SampleType>(phase + k + 1));
                for (int j = 0; j < 4; ++j) {
                    const auto u = 4 * static_cast<std::size_t>(j);
                    gain[j] = lanes::load(&gain_from_[u]) + lanes::load(&gain_step_[u]) * step;
                    damp[j] = lanes::load(&damp_from_[u]) + lanes::load(&damp_step_[u]) * step;
                }
            }
            for (int j = 0; j < 4; ++j) {
                lp[j] = delayed[j] + (lp[j] - delayed[j]) * damp[j];
                lanes::store(write + at + 4 * static_cast<std::size_t>(j),
                             feed[j / 2] + gain[j] * lp[j]);
            }
            const F4 l = delayed[0] + delayed[1];
            const F4 r = delayed[2] + delayed[3];
            left[k] =
                (lanes::lane(l, 0) + lanes::lane(l, 1)) + (lanes::lane(l, 2) + lanes::lane(l, 3));
            right[k] =
                (lanes::lane(r, 0) + lanes::lane(r, 1)) + (lanes::lane(r, 2) + lanes::lane(r, 3));
        }
        for (int j = 0; j < 4; ++j)
            lanes::store(&lp_[4 * static_cast<std::size_t>(j)], lp[j]);
    }

    // Advance past n rows written from write_: move rows written past the
    // ring's end to its start, then refresh the mirror if the first rows
    // changed.
    void commit_combs(int n) {
        constexpr std::size_t kRow = kCombs;
        const std::size_t size = comb_mask_ + 1;
        const std::size_t end = write_ + static_cast<std::size_t>(n);
        SampleType* rows = storage_.data();
        if (end > size)
            std::copy(rows + size * kRow, rows + end * kRow, rows);
        const auto mirror = static_cast<std::size_t>(fdn::kMaxBlockSamples);
        if (write_ < mirror || end > size)
            std::copy(rows, rows + mirror * kRow, rows + size * kRow);
        write_ = end & comb_mask_;
    }

    // On the control grid: snap decayed loop filters and clear any that went
    // non-finite. The comb input is finite and every gain is below one, so a
    // snapped filter stops a silent tail from recirculating forever.
    void heal() {
        for (auto& v : lp_)
            v = std::isfinite(v) ? snap_to_zero(v) : SampleType{0};
    }

    std::vector<SampleType> storage_{};
    std::array<int, kCombs> length_{};
    std::array<double, kCombs> gain_{};
    std::array<SampleType, kCombs> gain_f_{};
    std::array<SampleType, kCombs> damp_f_{};
    std::array<SampleType, kCombs> lp_{};
    std::array<fdn::AllpassCascade<SampleType>, 2> allpass_{};
    std::array<fdn::FracDelayT<SampleType>, 2> predelay_{};
    std::array<double, fdn::kNumParams> target_{};
    std::array<std::array<double, fdn::kNumParams>, 2> smoothing_stages_{};
    std::array<double, fdn::kNumParams> smoothed_{};
    std::size_t comb_mask_ = 0;
    std::size_t write_ = 0;
    double rate_ = 48000.0;
    double smoothing_coeff_ = 1.0;
    double predelay_samples_ = 2.0;
    SampleType allpass_g_ = 0;
    SampleType width_ = 1;
    // The per-sample ramp toward the values derive() last produced.
    std::array<SampleType, kCombs> gain_from_{};
    std::array<SampleType, kCombs> gain_step_{};
    std::array<SampleType, kCombs> damp_from_{};
    std::array<SampleType, kCombs> damp_step_{};
    SampleType width_from_ = 1;
    SampleType width_step_ = 0;
    bool ramping_ = false;
    bool settling_ = true;
    int control_countdown_ = 0;
};

using ClassicReverb = ClassicReverbT<float>;

} // namespace pulp::signal
