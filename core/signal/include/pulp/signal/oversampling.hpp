#pragma once

#include <pulp/signal/biquad.hpp>
#include <pulp/signal/halfband_iir.hpp>

#include <array>
#include <functional>
#include <vector>

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
// callers don't need to know which filter is active.
//
// macOS plan §2.2 — polyphase IIR integration into Oversampler.
class Oversampler {
public:
    enum class Factor { x2 = 2, x4 = 4 };

    /// Which anti-aliasing filter family the oversampler uses.
    enum class Kind {
        fir_biquad,    ///< Original biquad lowpass on both sides.
        polyphase_iir, ///< Half-band polyphase IIR (allpass-network).
    };

    void set_factor(Factor f) { factor_ = f; reset(); }
    void set_sample_rate(float sr) { sample_rate_ = sr; configure_filters(); }
    void set_kind(Kind k) { kind_ = k; reset(); }
    Kind kind() const { return kind_; }

    // Process a single sample with oversampled callback.
    // The callback receives an upsampled sample and returns the processed
    // output. The same callback fires `2*N` times per `process()` call for
    // factor xN (the loop runs at the oversampled rate).
    float process(float input, std::function<float(float)> callback) {
        if (kind_ == Kind::polyphase_iir) {
            return process_polyphase_iir(input, callback);
        }
        return process_fir_biquad(input, callback);
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
    float sample_rate_ = 44100.0f;

    // ── fir_biquad lane ────────────────────────────────────────────────
    Biquad aa_up_, aa_down_;

    void configure_filters() {
        float os_rate = sample_rate_ * static_cast<float>(factor_);
        float cutoff = sample_rate_ * 0.4f; // Below Nyquist of original rate
        aa_up_.set_coefficients(Biquad::Type::lowpass, cutoff, 0.707f, os_rate);
        aa_down_.set_coefficients(Biquad::Type::lowpass, cutoff, 0.707f, os_rate);
    }

    float process_fir_biquad(float input, std::function<float(float)>& callback) {
        int n = static_cast<int>(factor_);
        // Upsample: insert zeros.
        std::vector<float> upsampled(n, 0.0f);
        upsampled[0] = input * n; // scale-up to preserve energy
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
    HalfBandUpsampler2x   hb_up_stage1_;
    HalfBandDownsampler2x hb_down_stage1_;
    HalfBandUpsampler2x   hb_up_stage2_;
    HalfBandDownsampler2x hb_down_stage2_;

    float process_polyphase_iir(float input, std::function<float(float)>& callback) {
        if (factor_ == Factor::x2) {
            // 1 in → 2 oversampled samples → 2 callback hits → 1 out.
            float u_lo = 0.f, u_hi = 0.f;
            hb_up_stage1_.process(input, u_lo, u_hi);
            const float p_lo = callback(u_lo);
            const float p_hi = callback(u_hi);
            return hb_down_stage1_.process(p_lo, p_hi);
        }
        // x4: cascade. Stage 1 produces (lo, hi) at 2Fs. Stage 2 expands
        // each of those to (lo, hi) at 4Fs, the callback runs 4×, and
        // the decimation mirror-images the structure.
        float s1_lo = 0.f, s1_hi = 0.f;
        hb_up_stage1_.process(input, s1_lo, s1_hi);
        float a_lo = 0.f, a_hi = 0.f, b_lo = 0.f, b_hi = 0.f;
        hb_up_stage2_.process(s1_lo, a_lo, a_hi);
        hb_up_stage2_.process(s1_hi, b_lo, b_hi);
        const float pa_lo = callback(a_lo);
        const float pa_hi = callback(a_hi);
        const float pb_lo = callback(b_lo);
        const float pb_hi = callback(b_hi);
        const float d_a = hb_down_stage2_.process(pa_lo, pa_hi);
        const float d_b = hb_down_stage2_.process(pb_lo, pb_hi);
        return hb_down_stage1_.process(d_a, d_b);
    }
};

} // namespace pulp::signal
