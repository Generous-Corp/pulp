#pragma once

#include <pulp/signal/biquad.hpp>
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
// Three filter `Kind`s are available:
//
//   * `fir_biquad` (default) — the source-compatible single Biquad pair for
//     x2/x4, with staged pairs for x8/x16. Lightweight and symmetric, but
//     transition-band selectivity is limited and the filters' cutoffs scale
//     with the labelled sample rate.
//   * `polyphase_iir` — half-band polyphase IIR (Vaidyanathan /
//     Regalia-Mitra) using `HalfBandUpsampler2x` /
//     `HalfBandDownsampler2x`. Sample-rate-invariant, < 0.001 dB
//     passband ripple, single-multiply-per-section inner loop. x4
//     cascades two half-band stages.
//   * `linear_phase_fir` — Kaiser-windowed FIR with a transition that ends at
//     the base-rate Nyquist. `Quality::standard` is a 96 dB design flat to
//     80% of Nyquist; `Quality::pristine` uses a 140 dB prototype flat to 90%.
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
        fir_biquad,       ///< Original biquad lowpass on both sides.
        polyphase_iir,    ///< Half-band polyphase IIR (allpass-network).
        linear_phase_fir, ///< Linear-phase FIR with reported integer latency.
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
    /// outside the audio callback. After configuration, the templated
    /// process() and process_block() paths are allocation-free when the
    /// supplied callback is also allocation-free. The std::function overload is
    /// source-compatible but does not make heap-backed callbacks RT-safe.
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
        reset();
    }
    void set_quality(Quality quality) {
        quality_ = quality;
        if (kind_ == Kind::linear_phase_fir)
            configure_linear_phase_filters();
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

    // Source-compatible convenience overload for callers that already hold a
    // std::function. Capturing or heap-backed std::function payloads are not a
    // realtime-safety guarantee; hot paths should use the templated overload.
    SampleType process(SampleType input, std::function<SampleType(SampleType)> callback) {
        return process_with_callback(input, callback);
    }

    // Process a contiguous block through the same callback. `input` and
    // `output` may alias for in-place processing.
    template <typename Callback>
    void process_block(const SampleType* input, SampleType* output, std::size_t num_samples,
                       Callback&& callback) {
        for (std::size_t i = 0; i < num_samples; ++i)
            output[i] = process(input[i], callback);
    }

    void reset() {
        aa_up_.reset();
        aa_down_.reset();
        for (auto& stage : biquad_up_stages_)
            stage.reset();
        for (auto& stage : biquad_down_stages_)
            stage.reset();
        for (auto& stage : hb_up_stages_)
            stage.reset();
        for (auto& stage : hb_down_stages_)
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
        return process_fir_biquad(input, callback);
    }

    // ── fir_biquad lane ────────────────────────────────────────────────
    BiquadT<SampleType> aa_up_;
    BiquadT<SampleType> aa_down_;
    std::array<BiquadT<SampleType>, kMaxStages> biquad_up_stages_;
    std::array<BiquadT<SampleType>, kMaxStages> biquad_down_stages_;

    void configure_filters() {
        SampleType cutoff = sample_rate_ * SampleType{0.4f}; // Below Nyquist of original rate
        const SampleType os_rate = sample_rate_ * static_cast<SampleType>(factor_value(factor_));
        aa_up_.set_coefficients(BiquadT<SampleType>::Type::lowpass, cutoff, SampleType{0.707f},
                                os_rate);
        aa_down_.set_coefficients(BiquadT<SampleType>::Type::lowpass, cutoff, SampleType{0.707f},
                                  os_rate);
        for (std::size_t stage = 0; stage < kMaxStages; ++stage) {
            const SampleType stage_rate = sample_rate_ * static_cast<SampleType>(1u << (stage + 1));
            biquad_up_stages_[stage].set_coefficients(BiquadT<SampleType>::Type::lowpass, cutoff,
                                                      SampleType{0.707f}, stage_rate);
            biquad_down_stages_[stage].set_coefficients(BiquadT<SampleType>::Type::lowpass, cutoff,
                                                        SampleType{0.707f}, stage_rate);
        }
        if (kind_ == Kind::linear_phase_fir)
            configure_linear_phase_filters();
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
        if (factor_ == Factor::x2 || factor_ == Factor::x4) {
            const int factor = factor_value(factor_);
            std::array<SampleType, static_cast<std::size_t>(Factor::x4)> values{};
            values[0] = input * static_cast<SampleType>(factor);
            for (int i = 0; i < factor; ++i)
                values[i] = aa_up_.process(values[i]);
            for (int i = 0; i < factor; ++i)
                values[i] = callback(values[i]);
            for (int i = 0; i < factor; ++i)
                values[i] = aa_down_.process(values[i]);
            return values[0];
        }

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
