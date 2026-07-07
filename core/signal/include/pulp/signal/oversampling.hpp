#pragma once

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/halfband_iir.hpp>

#include <array>
#include <cstddef>
#include <functional>
#include <utility>

namespace pulp::signal {

// Oversampling processor — runs a callback at 2x or 4x sample rate
// with anti-aliasing filters on input and output.
//
// Two filter `Kind`s are available:
//
//   * `fir_biquad` (default, original behaviour) — a single biquad on the
//     upsample path and another on the downsample path. Lightweight and
//     symmetric, but transition-band selectivity is limited and the
//     filter's cutoff scales with the labelled sample rate.
//   * `polyphase_iir` — half-band polyphase IIR (Vaidyanathan /
//     Regalia-Mitra) using `HalfBandUpsampler2x` /
//     `HalfBandDownsampler2x`. Sample-rate-invariant, < 0.001 dB
//     passband ripple, single-multiply-per-section inner loop. x4
//     cascades two half-band stages.
//
// Switch with `set_kind()`. Both kinds share the same callback API so
// callers don't need to know which filter is active. Configure factor,
// sample rate, and kind outside the audio callback; `process()` and
// `process_block()` do not allocate after construction/configuration as
// long as the supplied callback is also allocation-free.
//
template <typename SampleType = float>
class OversamplerT {
public:
    enum class Factor { x2 = 2, x4 = 4 };

    /// Which anti-aliasing filter family the oversampler uses.
    enum class Kind {
        fir_biquad,    ///< Original biquad lowpass on both sides.
        polyphase_iir, ///< Half-band polyphase IIR (allpass-network).
    };

    OversamplerT() { configure_filters(); }

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
    void set_kind(Kind k) { kind_ = k; reset(); }
    Kind kind() const { return kind_; }
    Factor factor() const { return factor_; }
    int factor_value() const { return static_cast<int>(factor_); }

    // Process a single sample with oversampled callback.
    // The callback receives an upsampled sample and returns the processed
    // output. The same callback fires N times per `process()` call for factor
    // xN (the loop runs at the oversampled rate). Prefer this templated path
    // for realtime use; it does not need to type-erase callbacks.
    template <typename Callback>
    SampleType process(SampleType input, Callback&& callback) {
        return process_with_callback(input, std::forward<Callback>(callback));
    }

    // Source-compatible convenience overload for callers that already hold a
    // std::function. Capturing or heap-backed std::function payloads are not a
    // realtime-safety guarantee; hot paths should use the templated overload.
    SampleType process(SampleType input,
                       std::function<SampleType(SampleType)> callback) {
        return process_with_callback(input, callback);
    }

    // Process a contiguous block through the same callback. `input` and
    // `output` may alias for in-place processing.
    template <typename Callback>
    void process_block(const SampleType* input,
                       SampleType* output,
                       std::size_t num_samples,
                       Callback&& callback) {
        for (std::size_t i = 0; i < num_samples; ++i)
            output[i] = process(input[i], callback);
    }

    void reset() {
        aa_up_.reset();
        aa_down_.reset();
        hb_up_stage1_.reset();
        hb_down_stage1_.reset();
        hb_up_stage2_.reset();
        hb_down_stage2_.reset();
    }

private:
    Factor factor_ = Factor::x2;
    Kind kind_ = Kind::fir_biquad;
    SampleType sample_rate_ = SampleType{44100.0f};

    static constexpr int factor_value(Factor factor) noexcept {
        return factor == Factor::x4 ? 4 : 2;
    }

    template <typename Callback>
    SampleType process_with_callback(SampleType input, Callback&& callback) {
        if (kind_ == Kind::polyphase_iir) {
            return process_polyphase_iir(input, callback);
        }
        return process_fir_biquad(input, callback);
    }

    // ── fir_biquad lane ────────────────────────────────────────────────
    BiquadT<SampleType> aa_up_, aa_down_;

    void configure_filters() {
        SampleType os_rate =
            sample_rate_ * static_cast<SampleType>(factor_value(factor_));
        SampleType cutoff = sample_rate_ * SampleType{0.4f}; // Below Nyquist of original rate
        aa_up_.set_coefficients(BiquadT<SampleType>::Type::lowpass, cutoff,
                                SampleType{0.707f}, os_rate);
        aa_down_.set_coefficients(BiquadT<SampleType>::Type::lowpass, cutoff,
                                  SampleType{0.707f}, os_rate);
    }

    template <typename Callback>
    SampleType process_fir_biquad(SampleType input, Callback& callback) {
        const int n = factor_value(factor_);
        // Upsample: insert zeros.
        std::array<SampleType, static_cast<std::size_t>(Factor::x4)> upsampled{};
        upsampled[0] = input * static_cast<SampleType>(n); // scale-up to preserve energy
        for (int i = 0; i < n; ++i)
            upsampled[i] = aa_up_.process(upsampled[i]);
        for (int i = 0; i < n; ++i)
            upsampled[i] = callback(upsampled[i]);
        for (int i = 0; i < n; ++i)
            upsampled[i] = aa_down_.process(upsampled[i]);
        return upsampled[0];
    }

    // ── polyphase_iir lane ─────────────────────────────────────────────
    // x2 uses one stage; x4 cascades two stages (Fs → 2Fs → 4Fs and back).
    HalfBandUpsampler2xT<SampleType>   hb_up_stage1_;
    HalfBandDownsampler2xT<SampleType> hb_down_stage1_;
    HalfBandUpsampler2xT<SampleType>   hb_up_stage2_;
    HalfBandDownsampler2xT<SampleType> hb_down_stage2_;

    template <typename Callback>
    SampleType process_polyphase_iir(SampleType input, Callback& callback) {
        if (factor_ == Factor::x2) {
            // 1 in → 2 oversampled samples → 2 callback hits → 1 out.
            SampleType u_lo = 0.f, u_hi = 0.f;
            hb_up_stage1_.process(input, u_lo, u_hi);
            const SampleType p_lo = callback(u_lo);
            const SampleType p_hi = callback(u_hi);
            return hb_down_stage1_.process(p_lo, p_hi);
        }
        // x4: cascade. Stage 1 produces (lo, hi) at 2Fs. Stage 2 expands
        // each of those to (lo, hi) at 4Fs, the callback runs 4×, and
        // the decimation mirror-images the structure.
        SampleType s1_lo = 0.f, s1_hi = 0.f;
        hb_up_stage1_.process(input, s1_lo, s1_hi);
        SampleType a_lo = 0.f, a_hi = 0.f, b_lo = 0.f, b_hi = 0.f;
        hb_up_stage2_.process(s1_lo, a_lo, a_hi);
        hb_up_stage2_.process(s1_hi, b_lo, b_hi);
        const SampleType pa_lo = callback(a_lo);
        const SampleType pa_hi = callback(a_hi);
        const SampleType pb_lo = callback(b_lo);
        const SampleType pb_hi = callback(b_hi);
        const SampleType d_a = hb_down_stage2_.process(pa_lo, pa_hi);
        const SampleType d_b = hb_down_stage2_.process(pb_lo, pb_hi);
        return hb_down_stage1_.process(d_a, d_b);
    }
};

using Oversampler = OversamplerT<float>;
using Oversampler64 = OversamplerT<double>;

} // namespace pulp::signal
