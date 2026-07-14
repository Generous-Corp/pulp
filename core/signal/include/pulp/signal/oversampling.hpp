#pragma once

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/elliptic_halfband_iir.hpp>
#include <pulp/signal/halfband_iir.hpp>
#include <pulp/signal/oversampling_fir.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <utility>

namespace pulp::signal {

// Oversampling processor — runs a callback at 2x, 4x, 8x, or 16x sample rate
// with anti-aliasing filters on input and output.
//
// Four filter `Kind`s are available:
//
//   * `fir_biquad` (default) — a Butterworth Biquad pair per 2x stage.
//     Lightweight and symmetric, but a single biquad cannot reject much near
//     the fold boundary: worst-case base-band alias rejection is only around
//     7 dB, and the passband is about -1.9 dB at 0.3 of the base rate. Those
//     figures hold at every factor — raising the factor buys headroom for the
//     callback's own harmonics, not a cleaner filter. Reach for one of the
//     kinds below when the anti-aliasing itself has to be good.
//   * `polyphase_iir` — half-band polyphase IIR (Vaidyanathan /
//     Regalia-Mitra) using `HalfBandUpsampler2x` /
//     `HalfBandDownsampler2x`. Sample-rate-invariant, < 0.001 dB
//     passband ripple, single-multiply-per-section inner loop. x4
//     cascades two half-band stages.
//   * `linear_phase_fir` — Kaiser-windowed FIR with a transition that ends at
//     the base-rate Nyquist. `Quality::standard` is a 96 dB design flat to
//     80% of Nyquist; `Quality::pristine` uses a 140 dB prototype flat to 90%.
//   * `elliptic_polyphase_iir` — half-band polyphase IIR using the
//     Valenzuela-Constantinides elliptic design (`EllipticHalfBandUpsampler2x`
//     / `EllipticHalfBandDownsampler2x`) instead of `polyphase_iir`'s fixed
//     six-section design. Same allpass-network shape and the same "no
//     constant latency" contract, but a different, design-equation-driven
//     section count per stage: the design equations solve directly for a
//     requested transition width and stopband floor, so pick this kind when
//     a specific latency/response operating point matters more than
//     matching `polyphase_iir`'s fixed coefficient table. `Quality` selects
//     the per-stage transition-width/stopband schedule, same as
//     `linear_phase_fir`.
//
// Switch with `set_kind()`. All kinds share the same callback API so
// callers don't need to know which filter is active. Configure factor,
// sample rate, and kind outside the audio callback; `process()` and
// `process_block()` do not allocate after construction/configuration as
// long as the supplied callback is also allocation-free.
//
template <typename SampleType = float> class OversamplerT {
  public:
    enum class Factor { x2 = 2, x4 = 4, x8 = 8, x16 = 16 };

    /// Which anti-aliasing filter family the oversampler uses.
    enum class Kind {
        fir_biquad,             ///< Original biquad lowpass on both sides.
        polyphase_iir,          ///< Half-band polyphase IIR (allpass-network).
        linear_phase_fir,       ///< Linear-phase FIR with reported integer latency.
        elliptic_polyphase_iir, ///< Half-band polyphase IIR, elliptic (Valenzuela-Constantinides) design.
    };

    enum class Quality {
        standard, ///< 96 dB stopband, passband edge at 80% of Nyquist.
        pristine, ///< 140 dB prototype, passband edge at 90% of Nyquist.
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
        if (kind_ == Kind::linear_phase_fir)
            configure_linear_phase_filters();
        else if (kind_ == Kind::elliptic_polyphase_iir)
            configure_elliptic_filters();
        reset();
    }
    void set_quality(Quality quality) {
        quality_ = quality;
        if (kind_ == Kind::linear_phase_fir)
            configure_linear_phase_filters();
        else if (kind_ == Kind::elliptic_polyphase_iir)
            configure_elliptic_filters();
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
    /// delay, so `constant` is false and no exact scalar is claimed.
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

    // Process a single sample with oversampled callback.
    // The callback receives an upsampled sample and returns the processed
    // output. The same callback fires N times per `process()` call for factor
    // xN (the loop runs at the oversampled rate). Prefer this templated path
    // for realtime use; it does not need to type-erase callbacks.
    template <typename Callback> SampleType process(SampleType input, Callback&& callback) {
        return process_with_callback(input, std::forward<Callback>(callback));
    }

    // Convenience overload for callers that already hold a std::function. Taken by
    // const reference: a by-value parameter would copy the callable — and so heap
    // -allocate, for any payload too large for the small-object buffer — on every
    // sample, including once per sample underneath process_block(). Invoking a
    // std::function still costs an indirect call; hot paths want the templated
    // overload above.
    SampleType process(SampleType input, const std::function<SampleType(SampleType)>& callback) {
        return process_with_callback(input, callback);
    }

    // Process a contiguous block through the same callback. `input` and
    // `output` may alias for in-place processing.
    template <typename Callback>
    void process_block(const SampleType* input, SampleType* output, std::size_t num_samples,
                       Callback&& callback) {
        // Dispatch straight to the implementation instead of re-running overload
        // resolution once per sample.
        for (std::size_t i = 0; i < num_samples; ++i)
            output[i] = process_with_callback(input[i], callback);
    }

    void reset() {
        for (auto& stage : biquad_up_stages_)
            stage.reset();
        for (auto& stage : biquad_down_stages_)
            stage.reset();
        for (auto& stage : hb_up_stages_)
            stage.reset();
        for (auto& stage : hb_down_stages_)
            stage.reset();
        for (auto& stage : elliptic_up_stages_)
            stage.reset();
        for (auto& stage : elliptic_down_stages_)
            stage.reset();
        for (auto& stage : fir_stages_)
            stage.reset();
    }

  private:
    static constexpr std::size_t kMaxStages = 4;
    Factor factor_ = Factor::x2;
    Kind kind_ = Kind::fir_biquad;
    Quality quality_ = Quality::standard;
    SampleType sample_rate_ = SampleType{44100.0f};

    static constexpr int factor_value(Factor factor) noexcept {
        return static_cast<int>(factor);
    }

    int stage_count() const noexcept {
        int count = 0;
        for (int factor = factor_value(); factor > 1; factor >>= 1)
            ++count;
        return count;
    }

    template <typename Callback>
    SampleType process_with_callback(SampleType input, Callback&& callback) {
        if (kind_ == Kind::linear_phase_fir) {
            return process_linear_phase_fir(input, callback);
        }
        if (kind_ == Kind::polyphase_iir) {
            return process_polyphase_iir(input, callback);
        }
        if (kind_ == Kind::elliptic_polyphase_iir) {
            return process_elliptic_polyphase_iir(input, callback);
        }
        return process_fir_biquad(input, callback);
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
        if (kind_ == Kind::linear_phase_fir)
            configure_linear_phase_filters();
        else if (kind_ == Kind::elliptic_polyphase_iir)
            configure_elliptic_filters();
    }

    // Per-stage transition-width / stopband-floor schedule for the elliptic
    // design: stage 0 gets half the transition width of later stages (its
    // Nyquist has the least headroom), and each stage's stopband floor
    // shallows by a fixed step off a deep stage-0 floor — a linear
    // `floor_start + step * stage_index` progression. `Quality::pristine`
    // narrows the transition width and deepens the stage-0 floor relative to
    // `Quality::standard`, with a larger per-stage step.
    void configure_elliptic_filters() {
        const bool max_quality = quality_ == Quality::pristine;
        const double tw_up_base = max_quality ? 0.10 : 0.12;
        const double tw_down_base = max_quality ? 0.12 : 0.15;
        const double gain_start_up = max_quality ? -90.0 : -70.0;
        const double gain_start_down = max_quality ? -75.0 : -60.0;
        const double gain_step = max_quality ? 10.0 : 8.0;
        for (std::size_t stage = 0; stage < kMaxStages; ++stage) {
            const double half_first = stage == 0 ? 0.5 : 1.0;
            const double stage_index = static_cast<double>(stage);
            elliptic_up_stages_[stage].configure(tw_up_base * half_first,
                                                 gain_start_up + gain_step * stage_index);
            elliptic_down_stages_[stage].configure(tw_down_base * half_first,
                                                   gain_start_down + gain_step * stage_index);
        }
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

    template <typename Callback>
    SampleType process_fir_biquad(SampleType input, Callback& callback) {
        std::array<SampleType, static_cast<std::size_t>(Factor::x16)> values{};
        std::array<SampleType, static_cast<std::size_t>(Factor::x16)> expanded{};
        values[0] = input;
        std::size_t count = 1;
        const int stages = stage_count();
        for (int stage = 0; stage < stages; ++stage) {
            for (std::size_t i = 0; i < count; ++i) {
                expanded[2 * i] = biquad_up_stages_[stage].process(SampleType{2} * values[i]);
                expanded[2 * i + 1] = biquad_up_stages_[stage].process(SampleType{0});
            }
            count *= 2;
            std::copy_n(expanded.begin(), count, values.begin());
        }
        for (std::size_t i = 0; i < count; ++i)
            values[i] = callback(values[i]);
        for (int stage = stages; stage-- > 0;) {
            count /= 2;
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = biquad_down_stages_[stage].process(values[2 * i]);
                static_cast<void>(biquad_down_stages_[stage].process(values[2 * i + 1]));
            }
        }
        return values[0];
    }

    // ── polyphase_iir lane ─────────────────────────────────────────────
    std::array<HalfBandUpsampler2xT<SampleType>, kMaxStages> hb_up_stages_;
    std::array<HalfBandDownsampler2xT<SampleType>, kMaxStages> hb_down_stages_;
    // ── elliptic_polyphase_iir lane ────────────────────────────────────
    std::array<EllipticHalfBandUpsampler2xT<SampleType>, kMaxStages> elliptic_up_stages_;
    std::array<EllipticHalfBandDownsampler2xT<SampleType>, kMaxStages> elliptic_down_stages_;
    std::array<detail::LinearPhaseOversamplingStage2x<SampleType>, kMaxStages> fir_stages_;

    template <typename Callback>
    SampleType process_polyphase_iir(SampleType input, Callback& callback) {
        std::array<SampleType, static_cast<std::size_t>(Factor::x16)> values{};
        std::array<SampleType, static_cast<std::size_t>(Factor::x16)> expanded{};
        values[0] = input;
        std::size_t count = 1;
        const int stages = stage_count();
        for (int stage = 0; stage < stages; ++stage) {
            for (std::size_t i = 0; i < count; ++i) {
                hb_up_stages_[stage].process(values[i], expanded[2 * i], expanded[2 * i + 1]);
            }
            count *= 2;
            std::copy_n(expanded.begin(), count, values.begin());
        }
        for (std::size_t i = 0; i < count; ++i)
            values[i] = callback(values[i]);
        for (int stage = stages; stage-- > 0;) {
            count /= 2;
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = hb_down_stages_[stage].process(values[2 * i], values[2 * i + 1]);
            }
        }
        return values[0];
    }

    template <typename Callback>
    SampleType process_elliptic_polyphase_iir(SampleType input, Callback& callback) {
        std::array<SampleType, static_cast<std::size_t>(Factor::x16)> values{};
        std::array<SampleType, static_cast<std::size_t>(Factor::x16)> expanded{};
        values[0] = input;
        std::size_t count = 1;
        const int stages = stage_count();
        for (int stage = 0; stage < stages; ++stage) {
            for (std::size_t i = 0; i < count; ++i) {
                elliptic_up_stages_[stage].process(values[i], expanded[2 * i], expanded[2 * i + 1]);
            }
            count *= 2;
            std::copy_n(expanded.begin(), count, values.begin());
        }
        for (std::size_t i = 0; i < count; ++i)
            values[i] = callback(values[i]);
        for (int stage = stages; stage-- > 0;) {
            count /= 2;
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = elliptic_down_stages_[stage].process(values[2 * i], values[2 * i + 1]);
            }
        }
        return values[0];
    }

    template <typename Callback>
    SampleType process_linear_phase_fir(SampleType input, Callback& callback) {
        std::array<SampleType, static_cast<std::size_t>(Factor::x16)> values{};
        std::array<SampleType, static_cast<std::size_t>(Factor::x16)> expanded{};
        values[0] = input;
        std::size_t count = 1;
        const int stages = stage_count();
        for (int stage = 0; stage < stages; ++stage) {
            for (std::size_t i = 0; i < count; ++i) {
                fir_stages_[stage].upsample(values[i], expanded[2 * i], expanded[2 * i + 1]);
            }
            count *= 2;
            std::copy_n(expanded.begin(), count, values.begin());
        }
        for (std::size_t i = 0; i < count; ++i)
            values[i] = callback(values[i]);
        for (int stage = stages; stage-- > 0;) {
            count /= 2;
            for (std::size_t i = 0; i < count; ++i) {
                values[i] = fir_stages_[stage].downsample(values[2 * i], values[2 * i + 1]);
            }
        }
        return values[0];
    }
};

using Oversampler = OversamplerT<float>;
using Oversampler64 = OversamplerT<double>;

} // namespace pulp::signal
