#pragma once

/// @file resampler.hpp
/// Polyphase-FIR resampler with a Kaiser-window low-pass prototype.
///
/// `Resampler` implements arbitrary-ratio sample-rate conversion using
/// a polyphase decomposition of a windowed-sinc low-pass filter. The
/// prototype is designed once via the Kaiser-window method
/// (cutoff/transition/stopband knobs), oversampled at `L` phases, and
/// then sliced into `L` sub-filters. The streaming `process_sample`
/// path uses a delay-line + phase accumulator and linearly interpolates
/// between adjacent phases for arbitrary (irrational) ratios — this is
/// what lets a single design support 44.1↔48, 48↔96, 96↔192, and
/// near-irrational ratios like 48↔44099 with no per-ratio re-design.
///
/// Design lineage: the polyphase decomposition + Kaiser-window
/// prototype is the canonical textbook approach (Oppenheim & Schafer,
/// "Discrete-Time Signal Processing"; Crochiere & Rabiner,
/// "Multirate Digital Signal Processing"). Pulp's implementation is
/// written from first principles against those references — no
/// libsamplerate / SoX / SRC source code consulted.
///
/// The Kaiser-window helpers (`kaiser_beta_for_stopband`,
/// `kaiser_length_for_transition`, `kaiser_window`,
/// `bessel_i0`) are intentionally inlined here so this header stays
/// header-only and the test surface can exercise the math directly.
///
/// Streaming contract:
/// - `prepare(input_rate, output_rate, channels, max_block_size)`
///   allocates the delay line and polyphase tables. After prepare,
///   `process_sample()` / `process_block()` are allocation-free in the
///   audio thread.
/// - `set_ratio(input_rate, output_rate)` may be called mid-stream;
///   the phase accumulator is preserved so there's no click beyond
///   the documented transition window (one filter span).
/// - `reset()` zeroes the delay line + phase. Use at transport
///   re-start, not at every block.
///
/// Acceptance-test friendly invariants (verified in
/// `test/test_resampler.cpp`):
/// - DC unity gain across ratios.
/// - Pure sine in [0, output_nyquist) round-trips with low THD+N.
/// - Image / alias energy at the rejected sideband is below the
///   stopband attenuation set at design time.
/// - Allocation-free `process_block()` after `prepare()`.
/// - Streaming state survives buffer-size changes without click.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

// ────────────────────────────────────────────────────────────────────────────
// Kaiser-window math (header-inline so tests can hit it directly).
// ────────────────────────────────────────────────────────────────────────────

/// Modified Bessel function of the first kind, order 0 — series
/// expansion sufficient for `|x|` in the regime used by Kaiser-window
/// design (β rarely exceeds ~20 for any audio-grade stopband target).
inline double bessel_i0(double x) {
    // I0(x) = sum_{k=0..∞} ((x/2)^(2k) / (k!)^2)
    // Converges fast for moderate x; cap the series at a generous
    // iteration count and break early when the term falls below
    // machine epsilon × the running sum.
    const double half_x = 0.5 * x;
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= (half_x / static_cast<double>(k));
        const double t2 = term * term;
        sum += t2;
        if (t2 < 1e-20 * sum) break;
    }
    return sum;
}

/// Kaiser β from a desired stopband attenuation in dB. Standard
/// closed form from Oppenheim & Schafer §7.5.3.
inline double kaiser_beta_for_stopband(double stopband_db) {
    if (stopband_db > 50.0) {
        return 0.1102 * (stopband_db - 8.7);
    } else if (stopband_db >= 21.0) {
        return 0.5842 * std::pow(stopband_db - 21.0, 0.4)
             + 0.07886 * (stopband_db - 21.0);
    } else {
        return 0.0;
    }
}

/// Number of FIR taps needed for a given normalized transition band
/// `df_norm` (transition width / sample-rate of the filter) at a
/// given stopband attenuation. Rounded up to odd so the filter has a
/// well-defined center tap.
inline std::size_t kaiser_length_for_transition(double stopband_db,
                                                double df_norm) {
    if (df_norm <= 0.0) df_norm = 1e-6;
    const double n = (stopband_db - 7.95) / (14.36 * df_norm);
    std::size_t taps = static_cast<std::size_t>(std::ceil(n)) + 1;
    if ((taps & 1u) == 0u) ++taps;        // force odd → symmetric
    if (taps < 3u) taps = 3u;
    return taps;
}

/// Sample the symmetric Kaiser window of length `n` (odd recommended)
/// into `out`. `out` must be pre-sized to `n`.
inline void kaiser_window(std::vector<double>& out, double beta) {
    const std::size_t n = out.size();
    if (n == 0) return;
    const double denom = bessel_i0(beta);
    const double half = 0.5 * static_cast<double>(n - 1);
    for (std::size_t i = 0; i < n; ++i) {
        const double x = (static_cast<double>(i) - half) / half;  // x ∈ [-1, +1]
        const double arg = beta * std::sqrt(std::max(0.0, 1.0 - x * x));
        out[i] = bessel_i0(arg) / denom;
    }
}

// ────────────────────────────────────────────────────────────────────────────
// Windowed-sinc low-pass design.
// ────────────────────────────────────────────────────────────────────────────

/// Build a Kaiser-windowed-sinc low-pass FIR with `n_taps` coefficients
/// (odd recommended), normalized cutoff `fc` in cycles/sample (i.e.
/// fraction of the operating sample rate; 0.5 = Nyquist), and Kaiser
/// `beta`. Coefficients are normalized so DC gain is exactly 1.0.
inline std::vector<double> design_windowed_sinc(std::size_t n_taps,
                                                double fc,
                                                double beta) {
    std::vector<double> h(n_taps, 0.0);
    std::vector<double> win(n_taps, 0.0);
    kaiser_window(win, beta);
    const double half = 0.5 * static_cast<double>(n_taps - 1);
    constexpr double kPi = 3.14159265358979323846;
    const double two_fc = 2.0 * fc;
    for (std::size_t i = 0; i < n_taps; ++i) {
        const double m = static_cast<double>(i) - half;
        double sinc;
        if (std::abs(m) < 1e-12) {
            sinc = two_fc;
        } else {
            const double a = kPi * two_fc * m;
            sinc = std::sin(a) / (kPi * m);
        }
        h[i] = sinc * win[i];
    }
    // Normalize DC gain to 1.
    double sum = 0.0;
    for (double v : h) sum += v;
    if (std::abs(sum) > 1e-18) {
        const double inv = 1.0 / sum;
        for (double& v : h) v *= inv;
    }
    return h;
}

// ────────────────────────────────────────────────────────────────────────────
// Polyphase-FIR resampler with arbitrary ratio.
// ────────────────────────────────────────────────────────────────────────────

/// Quality knobs for the Kaiser-window prototype. Defaults are
/// audio-grade: 96 dB stopband, transition = 10 % of the lower
/// Nyquist, and the cutoff sits at 90 % of the lower Nyquist so the
/// passband is flat up to ~0.45 × (min Nyquist) on either side.
struct ResamplerQuality {
    double stopband_db = 96.0;          ///< minimum stopband attenuation
    double transition_fraction = 0.10;  ///< transition width / min Nyquist
    double cutoff_fraction = 0.90;      ///< cutoff / min Nyquist
    std::size_t phases = 64;            ///< polyphase oversample factor `L`
};

class Resampler {
public:
    Resampler() = default;

    /// Configure the conversion. `input_rate` and `output_rate` may be
    /// any positive values; the polyphase table is designed once at
    /// `prepare()`, and `set_ratio()` may be called later to nudge the
    /// streaming ratio without re-designing the filter.
    ///
    /// `max_block_size` is used to pre-size the per-channel scratch
    /// vector used by `process_block()` so the audio thread does not
    /// allocate. Pass the worst-case host block size; an input block of
    /// that many samples may produce up to `ceil(max_block * ratio) + 1`
    /// output samples.
    void prepare(double input_rate,
                 double output_rate,
                 std::size_t channels,
                 std::size_t max_block_size,
                 ResamplerQuality quality = {}) {
        input_rate_ = input_rate;
        output_rate_ = output_rate;
        channels_ = (channels == 0) ? 1u : channels;
        quality_ = quality;
        if (quality_.phases < 2u) quality_.phases = 2u;

        // Design the prototype LP at the polyphase sample rate
        // (= L × input_rate). When each phase is applied to the input
        // delay line, the effective filter response — in input-rate-
        // normalized terms — is the prototype's response scaled by L
        // along the frequency axis (phase p contains every Lth tap of
        // the prototype). To reject everything above min(in_nyq, out_nyq),
        // the cutoff must sit below min(in_nyq, out_nyq) in continuous Hz
        // terms; we then normalize to the polyphase rate.
        const double min_rate = std::min(input_rate_, output_rate_);
        const double min_nyq = 0.5 * min_rate;
        const double cutoff_hz = quality_.cutoff_fraction * min_nyq;
        const double transition_hz = quality_.transition_fraction * min_nyq;

        // Polyphase rate is L × input_rate so the L-fold spacing of
        // each phase places the effective cutoff at `fc_norm * L` in
        // input-rate-normalized units, which equals `cutoff_hz / input_rate`.
        const double poly_rate = static_cast<double>(quality_.phases) * input_rate_;
        const double fc_norm = cutoff_hz / poly_rate;
        const double df_norm = transition_hz / poly_rate;

        const double beta = kaiser_beta_for_stopband(quality_.stopband_db);
        std::size_t n_taps = kaiser_length_for_transition(quality_.stopband_db, df_norm);

        // Force n_taps to be `L * taps_per_phase` so the polyphase
        // decomposition is rectangular.
        const std::size_t L = quality_.phases;
        const std::size_t taps_per_phase = (n_taps + L - 1u) / L;
        n_taps = taps_per_phase * L;

        prototype_ = design_windowed_sinc(n_taps, fc_norm, beta);

        // Scale by L so each phase has unit DC gain after polyphase split.
        for (auto& v : prototype_) v *= static_cast<double>(L);

        // Slice into L phases of `taps_per_phase` each. Phase p contains
        // prototype_[p, p+L, p+2L, ...].
        phases_.assign(L, std::vector<float>(taps_per_phase, 0.0f));
        for (std::size_t p = 0; p < L; ++p) {
            for (std::size_t k = 0; k < taps_per_phase; ++k) {
                phases_[p][k] = static_cast<float>(prototype_[k * L + p]);
            }
        }
        taps_per_phase_ = taps_per_phase;

        // Pre-size delay lines so we never allocate in the hot path.
        delays_.assign(channels_, std::vector<float>(taps_per_phase, 0.0f));
        write_pos_.assign(channels_, 0u);

        // Output scratch — worst-case output count for a max-size input block.
        const double ratio = output_rate_ / input_rate_;
        const std::size_t worst_out =
            static_cast<std::size_t>(std::ceil(static_cast<double>(max_block_size) * ratio)) + 8u;
        scratch_out_.assign(channels_, std::vector<float>(worst_out, 0.0f));

        // Step (input samples per output sample) in the input domain.
        step_ = input_rate_ / output_rate_;
        phase_acc_ = 0.0;
    }

    /// Update the streaming ratio without rebuilding the filter.
    /// Preserves the delay line + phase accumulator so audio continues
    /// without a click beyond a one-filter-span transition window.
    void set_ratio(double input_rate, double output_rate) {
        if (input_rate <= 0.0 || output_rate <= 0.0) return;
        input_rate_ = input_rate;
        output_rate_ = output_rate;
        step_ = input_rate_ / output_rate_;
    }

    /// Zero the delay lines + phase accumulator. Not RT-safe to call
    /// concurrently with `process_*` — call from the audio thread
    /// between blocks, or while bypassed.
    void reset() {
        for (auto& d : delays_) std::fill(d.begin(), d.end(), 0.0f);
        std::fill(write_pos_.begin(), write_pos_.end(), 0u);
        phase_acc_ = 0.0;
    }

    /// Block API. Reads `in_count` samples from `input[c]` per channel
    /// and writes up to `out_capacity` resampled samples to `output[c]`.
    /// Returns the actual number of output samples produced.
    ///
    /// Allocation-free after `prepare()`.
    std::size_t process_block(const float* const* input,
                              std::size_t in_count,
                              float* const* output,
                              std::size_t out_capacity) {
        if (channels_ == 0 || taps_per_phase_ == 0 || phases_.empty()) return 0;
        const std::size_t L = phases_.size();
        const double L_d = static_cast<double>(L);

        std::size_t out_n = 0;
        std::size_t input_consumed = 0;

        // Process: for each output sample we need, check whether the
        // phase accumulator has crossed an integer (meaning we must
        // push the next input sample into the delay line) and then
        // evaluate the polyphase filter at the fractional position.
        while (out_n < out_capacity) {
            // Push input samples until the accumulator is below 1.0.
            while (phase_acc_ >= 1.0) {
                if (input_consumed >= in_count) {
                    // Ran out of input — return what we have.
                    return out_n;
                }
                for (std::size_t c = 0; c < channels_; ++c) {
                    auto& d = delays_[c];
                    d[write_pos_[c]] = input[c][input_consumed];
                    write_pos_[c] = (write_pos_[c] + 1u) % taps_per_phase_;
                }
                ++input_consumed;
                phase_acc_ -= 1.0;
            }

            // We need at least one input sample in the line. On the
            // very first call, the delay is zeroed which still yields
            // a valid (silence) output until input arrives.
            const double phase_pos = phase_acc_ * L_d;     // in [0, L)
            std::size_t p0 = static_cast<std::size_t>(phase_pos);
            if (p0 >= L) p0 = L - 1u;
            std::size_t p1 = p0 + 1u;
            double frac = phase_pos - static_cast<double>(p0);
            if (p1 >= L) { p1 = p0; frac = 0.0; }

            for (std::size_t c = 0; c < channels_; ++c) {
                const auto& d = delays_[c];
                const std::size_t wp = write_pos_[c];
                const auto& ph0 = phases_[p0];
                const auto& ph1 = phases_[p1];

                // Convolve: y = sum_k h[k] * x[wp - 1 - k] (most recent first).
                double acc0 = 0.0, acc1 = 0.0;
                std::size_t idx = (wp == 0 ? taps_per_phase_ - 1u : wp - 1u);
                for (std::size_t k = 0; k < taps_per_phase_; ++k) {
                    const float xs = d[idx];
                    acc0 += static_cast<double>(xs) * static_cast<double>(ph0[k]);
                    acc1 += static_cast<double>(xs) * static_cast<double>(ph1[k]);
                    idx = (idx == 0 ? taps_per_phase_ - 1u : idx - 1u);
                }
                const double y = acc0 + frac * (acc1 - acc0);
                output[c][out_n] = static_cast<float>(y);
            }

            ++out_n;
            phase_acc_ += step_;
        }
        return out_n;
    }

    /// Mono convenience wrapper around `process_block`.
    std::size_t process_block_mono(const float* input,
                                   std::size_t in_count,
                                   float* output,
                                   std::size_t out_capacity) {
        const float* in_ptrs[1] = { input };
        float* out_ptrs[1] = { output };
        return process_block(in_ptrs, in_count, out_ptrs, out_capacity);
    }

    /// Worst-case output sample count for a given input count.
    /// Includes a small margin for phase-accumulator state carry-over
    /// across blocks so callers can safely size output buffers from
    /// this value.
    std::size_t max_output_for(std::size_t in_count) const {
        const double ratio = output_rate_ / input_rate_;
        return static_cast<std::size_t>(std::ceil(static_cast<double>(in_count) * ratio)) + 8u;
    }

    /// Per-phase filter length. Useful for tests verifying the
    /// polyphase decomposition was rectangular.
    std::size_t taps_per_phase() const { return taps_per_phase_; }

    /// Number of polyphase branches (`L`).
    std::size_t phases() const { return phases_.size(); }

    /// Total prototype length (= phases * taps_per_phase). Useful for
    /// reasoning about latency: group delay is roughly (N-1)/2 samples
    /// at the polyphase rate, i.e. (taps_per_phase - 1) / 2 at the
    /// input rate.
    std::size_t prototype_length() const { return prototype_.size(); }

    double input_rate() const { return input_rate_; }
    double output_rate() const { return output_rate_; }
    std::size_t channels() const { return channels_; }

private:
    double input_rate_ = 0.0;
    double output_rate_ = 0.0;
    std::size_t channels_ = 0;
    ResamplerQuality quality_{};

    // Prototype LP (length L * taps_per_phase). Kept around so tests
    // can introspect it. Phases are the runtime representation.
    std::vector<double> prototype_;
    std::vector<std::vector<float>> phases_;
    std::size_t taps_per_phase_ = 0;

    // Per-channel circular delay line + write head.
    std::vector<std::vector<float>> delays_;
    std::vector<std::size_t> write_pos_;

    // Per-channel scratch — not used directly by process_block but
    // kept available so future block-process variants can drain into
    // it without allocating.
    std::vector<std::vector<float>> scratch_out_;

    // Phase accumulator in input samples per output sample.
    double step_ = 1.0;
    double phase_acc_ = 0.0;
};

} // namespace pulp::signal
