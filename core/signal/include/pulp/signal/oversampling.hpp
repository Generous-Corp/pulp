#pragma once

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/halfband_iir.hpp>
#include <pulp/signal/oversampling_fir.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>
#include <functional>
#include <utility>

namespace pulp::signal {

// Oversampling processor — runs a callback at 2x, 4x, 8x, or 16x sample rate
// with anti-aliasing filters on input and output.
//
// Three filter `Kind`s are available:
//
//   * `fir_biquad` — a Butterworth Biquad pair per 2x stage.
//     Lightweight and symmetric, but a single biquad cannot reject much near
//     the fold boundary: worst-case base-band alias rejection is only around
//     7 dB, and the passband is about -1.9 dB at 0.3 of the base rate. Those
//     figures hold at every factor — raising the factor buys headroom for the
//     callback's own harmonics, not a cleaner filter. Reach for one of the
//     kinds below when the anti-aliasing itself has to be good.
//   * `polyphase_iir` (default) — elliptic polyphase half-band IIR per 2x
//     stage (`design_halfband_iir`). Minimum phase, so no constant latency,
//     and the cheapest of the three: about a sixth of the biquad lane's cost
//     at x2 while rejecting 88 dB more. Its round trip delays the base band
//     by 2.66 samples near DC at x2 (3.49 at x4), rising toward the passband
//     edge, against the biquad lane's 0.97 (1.46). `Quality::standard`
//     keeps the base band flat (< 1e-8 dB droop) to 0.42 Fs and rejects
//     images and aliases from 0.58 Fs by at least 94 dB; `Quality::pristine`
//     is flat to 0.45 Fs and rejects from 0.55 Fs by at least 140 dB (the
//     float build sits on its own rounding floor a little above that). Upper
//     stages only guard the base band, so they use short designs.
//     `polyphase_stage_design()` returns each stage's design and
//     `group_delay_samples()` the round trip's frequency-dependent delay.
//   * `linear_phase_fir` — Kaiser-windowed FIR with a transition that ends at
//     the base-rate Nyquist. `Quality::standard` is a 96 dB design flat to
//     80% of Nyquist; `Quality::pristine` uses a 140 dB prototype flat to 90%.
//
// A half-band IIR's transition is symmetric about base Nyquist, so content
// in (0.5, 0.58) Fs (standard) folds onto (0.42, 0.5) Fs with only partial
// rejection — above the passband, but audible in principle. The linear-phase
// lane ends its transition at Nyquist instead and has no such region.
//
// Switch with `set_kind()`. All kinds share the same callback API so
// callers don't need to know which filter is active. Configure factor,
// sample rate, and kind outside the audio callback; `process()` and
// `process_block()` do not allocate after construction/configuration as
// long as the supplied callback is also allocation-free.
//
// For the polyphase IIR and linear-phase lanes, `process_block()` runs each
// filter stage over a whole chunk of samples and calls the callback for every
// oversampled sample of the chunk before any decimation. The callback sees
// exactly the samples, in exactly the order, that per-sample `process()`
// calls would hand it, and the output is identical — every stage is causal
// and processes its input in time order either way. The one exception: the
// polyphase stages flush state below ~1e-30 (float) to zero at the end of
// each chunk so silence decays to exact zero instead of a subnormal limit
// cycle, and per-sample calls flush every sample, so the two can differ by
// that much while a signal is dying away.
//
template <typename SampleType = float> class OversamplerT {
  public:
    enum class Factor { x2 = 2, x4 = 4, x8 = 8, x16 = 16 };

    /// Which anti-aliasing filter family the oversampler uses.
    enum class Kind {
        fir_biquad,       ///< Butterworth biquad lowpass on both sides; ~7 dB rejection.
        polyphase_iir,    ///< Elliptic half-band polyphase IIR (allpass network). Default.
        linear_phase_fir, ///< Linear-phase FIR with reported integer latency.
    };

    enum class Quality {
        standard, ///< FIR: 96 dB, flat to 0.40 Fs. IIR: 94 dB, flat to 0.42 Fs.
        pristine, ///< FIR: 140 dB, flat to 0.45 Fs. IIR: 140 dB, flat to 0.45 Fs.
    };

    struct Latency {
        int input_samples = 0;
        double exact_input_samples = 0.0;
        bool constant = false;
    };

    OversamplerT() {
        configure_filters();
    }

    /// RT contract: set_factor(), set_sample_rate(), set_kind(), and reset()
    /// are setup/control operations that mutate filter state and should run
    /// outside the audio callback. After configuration, process() and
    /// process_block() are allocation-free whenever the supplied callback is —
    /// including for a std::function callback, which is taken by reference and
    /// never copied per sample. A std::function still costs an indirect call, so
    /// hot paths should prefer the templated overload.
    void set_factor(Factor f) {
        factor_ = f;
        configure_filters();
        reset();
    }
    void set_sample_rate(SampleType sr) {
        sample_rate_ = sr;
        configure_filters();
        reset();
    }
    void set_kind(Kind kind) {
        kind_ = kind;
        configure_kind_filters();
        reset();
    }
    void set_quality(Quality quality) {
        quality_ = quality;
        configure_kind_filters();
        reset();
    }
    Kind kind() const {
        return kind_;
    }
    Quality quality() const {
        return quality_;
    }
    Factor factor() const {
        return factor_;
    }
    int factor_value() const {
        return static_cast<int>(factor_);
    }

    /// Linear-phase modes expose a constant delay suitable for host
    /// compensation. Minimum-phase IIR modes have frequency-dependent group
    /// delay, so `constant` is false and no exact scalar is claimed; see
    /// `group_delay_samples()`.
    Latency latency() const noexcept {
        if (kind_ != Kind::linear_phase_fir)
            return {};
        double exact = 0.0;
        const int stages = stage_count();
        for (int stage = 0; stage < stages; ++stage) {
            exact += static_cast<double>(fir_stages_[stage].taps() - 1u) /
                     static_cast<double>(1u << (stage + 1));
        }
        return {static_cast<int>(exact), exact, true};
    }

    int latency_samples() const noexcept {
        return latency().input_samples;
    }

    /// Group delay of the round trip (upsample, identity callback, downsample)
    /// in base-rate samples at `frequency` cycles per base sample. Constant and
    /// equal to `latency().exact_input_samples` for the linear-phase lane. The
    /// figure is the LTI delay of the filter cascade; the biquad lane rejects
    /// images so weakly that above ~0.2 Fs the decimator folds a tone's own
    /// image back onto it, and the measured phase there departs from it.
    double group_delay_samples(double frequency) const noexcept {
        if (kind_ == Kind::linear_phase_fir)
            return latency().exact_input_samples;
        double total = 0.0;
        const int stages = stage_count();
        for (int stage = 0; stage < stages; ++stage) {
            const double rate = static_cast<double>(1u << (stage + 1));
            const double f = frequency / rate;
            if (kind_ == Kind::polyphase_iir) {
                // The interpolator is H on the zero-stuffed input; the
                // decimator keeps H's odd-phase samples, one high-rate sample
                // ahead of H's own delay.
                const auto& design = iir_designs_[static_cast<std::size_t>(stage)];
                total += (2.0 * halfband_iir_group_delay<SampleType>(design, f) - 1.0) / rate;
            } else {
                // Both biquads of a stage see the full high-rate stream and the
                // decimator keeps even-phase samples.
                total += 2.0 *
                         biquad_group_delay(
                             biquad_up_stages_[static_cast<std::size_t>(stage)].coefficients(), f) /
                         rate;
            }
        }
        return total;
    }

    /// The elliptic half-band design stage `stage` uses in the polyphase_iir
    /// lane at the current quality. Stage 0 runs at twice the base rate, stage
    /// s at 2^(s+1) times it; frequencies in the design are relative to that
    /// stage's own rate.
    static HalfBandIirDesign polyphase_stage_design(Quality quality, int stage) noexcept {
        // Stage 0 fixes the base-band edge; stage s only has to keep the same
        // absolute band clean at 2^s times the rate, so its edge halves each
        // stage and a much shorter design reaches the same floor.
        const bool pristine = quality == Quality::pristine;
        constexpr std::array<std::size_t, kMaxStages> kStandardCounts = {6, 3, 2, 2};
        constexpr std::array<std::size_t, kMaxStages> kPristineCounts = {11, 5, 4, 3};
        const double base_edge = pristine ? 0.225 : 0.21;
        const auto index = static_cast<std::size_t>(std::clamp(stage, 0, int(kMaxStages) - 1));
        const double edge = base_edge / static_cast<double>(1u << index);
        return design_halfband_iir(pristine ? kPristineCounts[index] : kStandardCounts[index],
                                   edge);
    }

    // Process a single sample with oversampled callback.
    // The callback receives an upsampled sample and returns the processed
    // output. The same callback fires N times per `process()` call for factor
    // xN (the loop runs at the oversampled rate). Prefer this templated path
    // for realtime use; it does not need to type-erase callbacks.
    template <typename Callback> SampleType process(SampleType input, Callback&& callback) {
        SampleType output;
        process_chunk(&input, &output, 1, callback);
        return output;
    }

    // Convenience overload for callers that already hold a std::function. Taken by
    // const reference: a by-value parameter would copy the callable — and so heap
    // -allocate, for any payload too large for the small-object buffer — on every
    // sample, including once per sample underneath process_block(). Invoking a
    // std::function still costs an indirect call; hot paths want the templated
    // overload above.
    SampleType process(SampleType input, const std::function<SampleType(SampleType)>& callback) {
        SampleType output;
        process_chunk(&input, &output, 1, callback);
        return output;
    }

    // Process a contiguous block through the same callback. `input` and
    // `output` may alias for in-place processing.
    template <typename Callback>
    void process_block(const SampleType* input, SampleType* output, std::size_t num_samples,
                       Callback&& callback) {
        const std::size_t chunk = kScratchSamples / static_cast<std::size_t>(factor_value());
        for (std::size_t offset = 0; offset < num_samples; offset += chunk) {
            const std::size_t count = std::min(chunk, num_samples - offset);
            process_chunk(input + offset, output + offset, count, callback);
        }
    }

    void reset() {
        for (auto& stage : biquad_up_stages_)
            stage.reset();
        for (auto& stage : biquad_down_stages_)
            stage.reset();
        for (auto& stage : iir_stages_)
            stage.reset();
        for (auto& stage : fir_stages_)
            stage.reset();
    }

  private:
    static constexpr std::size_t kMaxStages = 4;
    // Oversampled samples per chunk; the base-rate chunk is this over the factor.
    static constexpr std::size_t kScratchSamples = 512;
    Factor factor_ = Factor::x2;
    Kind kind_ = Kind::polyphase_iir;
    Quality quality_ = Quality::standard;
    SampleType sample_rate_ = SampleType{44100.0f};

    // Ping-pong buffers for the rate-changing stages.
    std::array<SampleType, kScratchSamples> scratch_a_{};
    std::array<SampleType, kScratchSamples> scratch_b_{};

    static constexpr int factor_value(Factor factor) noexcept {
        return static_cast<int>(factor);
    }

    int stage_count() const noexcept {
        int count = 0;
        for (int factor = factor_value(); factor > 1; factor >>= 1)
            ++count;
        return count;
    }

    // `count` must not exceed kScratchSamples / factor. `input` is fully
    // consumed by the first stage before `output` is written, so they may
    // alias.
    template <typename Callback>
    void process_chunk(const SampleType* input, SampleType* output, std::size_t count,
                       Callback& callback) {
        if (kind_ == Kind::fir_biquad) {
            for (std::size_t i = 0; i < count; ++i)
                output[i] = process_biquad_sample(input[i], callback);
            return;
        }
        const int stages = stage_count();
        SampleType* buffers[2] = {scratch_a_.data(), scratch_b_.data()};
        const SampleType* source = input;
        std::size_t n = count;
        int next = 0;
        for (int stage = 0; stage < stages; ++stage) {
            upsample_stage(stage, source, buffers[next], n);
            source = buffers[next];
            n *= 2;
            next ^= 1;
        }
        SampleType* work = buffers[next ^ 1];
        for (std::size_t i = 0; i < n; ++i)
            work[i] = callback(work[i]);
        for (int stage = stages; stage-- > 0;) {
            n /= 2;
            SampleType* destination = stage == 0 ? output : buffers[next];
            downsample_stage(stage, work, destination, n);
            work = destination;
            next ^= 1;
        }
    }

    void upsample_stage(int stage, const SampleType* input, SampleType* output, std::size_t n) {
        const auto s = static_cast<std::size_t>(stage);
        if (kind_ == Kind::polyphase_iir) {
            iir_stages_[s].upsample(input, output, n);
            return;
        }
        fir_stages_[s].upsample(input, output, n);
    }

    void downsample_stage(int stage, const SampleType* input, SampleType* output, std::size_t n) {
        const auto s = static_cast<std::size_t>(stage);
        if (kind_ == Kind::polyphase_iir) {
            iir_stages_[s].downsample(input, output, n);
            return;
        }
        fir_stages_[s].downsample(input, output, n);
    }

    // The biquad lane stays sample by sample. Each biquad is one serial
    // recurrence, so running a stage over a whole chunk leaves the core waiting
    // on a single dependency chain; interleaving every stage's up and down
    // filters per input sample lets those independent chains overlap, and
    // measured about twice as fast.
    template <typename Callback>
    SampleType process_biquad_sample(SampleType input, Callback& callback) {
        constexpr auto kMax = static_cast<std::size_t>(Factor::x16);
        std::array<SampleType, kMax> values; // only [0, count) is ever read
        std::array<SampleType, kMax> expanded;
        values[0] = input;
        std::size_t count = 1;
        const int stages = stage_count();
        for (int stage = 0; stage < stages; ++stage) {
            auto& up = biquad_up_stages_[static_cast<std::size_t>(stage)];
            for (std::size_t i = 0; i < count; ++i) {
                expanded[2 * i] = up.process(SampleType{2} * values[i]);
                expanded[2 * i + 1] = up.process(SampleType{0});
            }
            count *= 2;
            std::copy_n(expanded.begin(), count, values.begin());
        }
        for (std::size_t i = 0; i < count; ++i)
            values[i] = callback(values[i]);
        for (int stage = stages; stage-- > 0;) {
            auto& down = biquad_down_stages_[static_cast<std::size_t>(stage)];
            count /= 2;
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = down.process(values[2 * i]);
                static_cast<void>(down.process(values[2 * i + 1]));
            }
        }
        return values[0];
    }

    static double biquad_group_delay(const BiquadCoefficientsT<SampleType>& c,
                                     double frequency) noexcept {
        // tau(B/A) = tau(B) - tau(A), tau(P) = Re(sum k p_k z^-k / sum p_k z^-k).
        constexpr double kPi = 3.14159265358979323846;
        const double w = 2.0 * kPi * frequency;
        const std::complex<double> z1(std::cos(w), -std::sin(w));
        const std::complex<double> z2 = z1 * z1;
        const auto delay = [&](double p0, double p1, double p2) {
            return std::real((p1 * z1 + 2.0 * p2 * z2) / (p0 + p1 * z1 + p2 * z2));
        };
        return delay(c.b0, c.b1, c.b2) - delay(1.0, c.a1, c.a2);
    }

    void configure_kind_filters() {
        if (kind_ == Kind::linear_phase_fir)
            configure_linear_phase_filters();
        else if (kind_ == Kind::polyphase_iir)
            configure_polyphase_filters();
    }

    // ── fir_biquad lane ────────────────────────────────────────────────
    std::array<BiquadT<SampleType>, kMaxStages> biquad_up_stages_;
    std::array<BiquadT<SampleType>, kMaxStages> biquad_down_stages_;

    void configure_filters() {
        // A 2x stage decimates to 2^stage * Fs, so what it has to keep clean is that
        // rate's Nyquist — half of it — not the base rate's. Putting each stage's
        // cutoff at 0.8 of its own fold boundary hands every stage the same
        // normalized filter, which is what stage 0's 0.4 * Fs already was. Reusing
        // one absolute 0.4 * Fs cutoff at every stage instead would make each added
        // stage lowpass the base band again, dragging the passband down with it.
        const SampleType base_cutoff = sample_rate_ * SampleType{0.4f};
        for (std::size_t stage = 0; stage < kMaxStages; ++stage) {
            const SampleType stage_rate = sample_rate_ * static_cast<SampleType>(1u << (stage + 1));
            const SampleType stage_cutoff = base_cutoff * static_cast<SampleType>(1u << stage);
            biquad_up_stages_[stage].set_coefficients(BiquadT<SampleType>::Type::lowpass,
                                                      stage_cutoff, SampleType{0.707f}, stage_rate);
            biquad_down_stages_[stage].set_coefficients(BiquadT<SampleType>::Type::lowpass,
                                                        stage_cutoff, SampleType{0.707f},
                                                        stage_rate);
        }
        configure_kind_filters();
    }

    void configure_linear_phase_filters() {
        const bool pristine = quality_ == Quality::pristine;
        fir_stages_[0].configure(pristine ? 0.45 : 0.40, pristine ? 140.0 : 96.0,
                                 pristine ? 385u : 129u);
        // Upper stages only keep distant images out of the next lower stage.
        // Standard filters there preserve the base-band guarantee without
        // multiplying the steepest filter's cost at high rates. These edges
        // shrink as fractions because each stage's input rate doubles: 0.20 of
        // 2Fs and 0.10 of 4Fs both preserve the same absolute 0.40Fs band.
        if (pristine) {
            fir_stages_[1].configure(0.225, 140.0, 69u);
            fir_stages_[2].configure(0.1125, 140.0, 57u);
            fir_stages_[3].configure(0.05625, 140.0, 49u);
        } else {
            fir_stages_[1].configure(0.20, 96.0, 49u);
            fir_stages_[2].configure(0.10, 96.0, 33u);
            fir_stages_[3].configure(0.05, 96.0, 33u);
        }
    }

    // ── polyphase_iir lane ─────────────────────────────────────────────
    std::array<HalfBandIirDesign, kMaxStages> iir_designs_{};
    std::array<detail::HalfBandIirStage<SampleType>, kMaxStages> iir_stages_;

    void configure_polyphase_filters() {
        for (std::size_t stage = 0; stage < kMaxStages; ++stage) {
            iir_designs_[stage] = polyphase_stage_design(quality_, static_cast<int>(stage));
            iir_stages_[stage].configure(iir_designs_[stage]);
        }
    }

    // ── linear_phase_fir lane ──────────────────────────────────────────
    std::array<detail::LinearPhaseOversamplingBlockStage2x<SampleType>, kMaxStages> fir_stages_;
};

using Oversampler = OversamplerT<float>;
using Oversampler64 = OversamplerT<double>;

} // namespace pulp::signal
