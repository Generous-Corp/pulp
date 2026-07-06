#pragma once

/// @file phase_vocoder.hpp
/// Offline phase-vocoder time-stretch and pitch-shift.
///
/// A standard STFT phase-vocoder: analyze overlapping windowed frames, track the
/// instantaneous frequency per bin by unwrapping the phase advance, re-synthesize
/// at a different hop with accumulated phase, and overlap-add. Time-stretching
/// changes duration while preserving pitch; pitch-shifting is a time-stretch
/// followed by resampling, preserving duration. This gives transient-aware
/// stretch/shift without the pitch artifacts of naive resampling — a reusable
/// DSP primitive built only on Pulp's existing FFT and window functions.
///
/// These are offline operations (they allocate and process a whole buffer), so
/// configure()/time_stretch()/pitch_shift() are control-thread work, not
/// real-time-audio-safe. Input/output are mono sample buffers. Header-only to
/// match the header-only pulp::signal umbrella.

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

#include <pulp/signal/fft.hpp>
#include <pulp/signal/windowing.hpp>

namespace pulp::signal {

struct PhaseVocoderConfig {
    int fft_size = 2048;       ///< Analysis/synthesis frame size (power of two).
    int synthesis_hop = 0;     ///< Output hop; 0 selects fft_size/4 (75% overlap).
    WindowFunction::Type window = WindowFunction::Type::hann;
};

template <typename SampleType = float>
class PhaseVocoderT {
public:
    PhaseVocoderT() { configure({}); }
    explicit PhaseVocoderT(const PhaseVocoderConfig& config) { configure(config); }

    void configure(const PhaseVocoderConfig& config) {
        config_ = config;
        // Fft requires a power-of-two size; round the requested size to the
        // nearest power of two (down on a tie) so a non-pow2 request can't
        // silently transform only part of each frame.
        fft_size_ = round_to_pow2(config.fft_size > 1 ? config.fft_size : 2048);
        synthesis_hop_ =
            config.synthesis_hop > 0 ? config.synthesis_hop : fft_size_ / 4;
        if (synthesis_hop_ < 1) synthesis_hop_ = 1;
        window_ = WindowFunction::generate<SampleType>(
            fft_size_, config.window, SampleType{0});
    }

    const PhaseVocoderConfig& config() const noexcept { return config_; }

    /// Time-stretch @p input by @p factor (> 0). Output is exactly
    /// round(input.size() * factor) frames long and keeps the same pitch.
    /// factor == 1 is ~identity. @p factor is clamped to a sane maximum
    /// (kMaxStretch) so an absurd value can't request a multi-gigabyte buffer.
    std::vector<SampleType> time_stretch(const std::vector<SampleType>& input,
                                         double factor) const {
        const int N = fft_size_;
        if (input.empty() || factor <= 0.0 || N <= 1) return input;
        // Guard against an absurd factor allocating gigabytes — a 100x stretch is
        // already far past any musical use. Clamp rather than throw bad_alloc.
        factor = std::min(factor, kMaxStretch());

        const std::size_t n_in = input.size();
        // Exact length contract: every path returns round(n_in * factor) frames.
        const std::size_t target = static_cast<std::size_t>(
            std::lround(static_cast<double>(n_in) * factor));

        const int Hs = synthesis_hop_;
        int Ha = static_cast<int>(std::lround(static_cast<double>(Hs) / factor));
        Ha = std::max(1, Ha);

        if (n_in < static_cast<std::size_t>(N)) {
            // Too short to form even one analysis frame — no phase-vocoder pitch
            // preservation is possible below one FFT frame. Honor the length
            // contract by linear-resampling to the target (resample_linear
            // returns an empty buffer when target == 0).
            return resample_linear(input, target);
        }

        const int num_frames =
            static_cast<int>((n_in - static_cast<std::size_t>(N)) / Ha) + 1;
        const std::size_t out_len =
            static_cast<std::size_t>(num_frames - 1) * Hs + N;

        std::vector<SampleType> out(out_len, SampleType{0});
        std::vector<SampleType> norm(out_len, SampleType{0});

        FftT<SampleType> fft(N);
        std::vector<std::complex<SampleType>> buf(N);
        std::vector<double> prev_phase(N, 0.0);
        std::vector<double> sum_phase(N, 0.0);

        for (int frame = 0; frame < num_frames; ++frame) {
            const std::size_t in_pos = static_cast<std::size_t>(frame) * Ha;
            const std::size_t out_pos = static_cast<std::size_t>(frame) * Hs;

            for (int k = 0; k < N; ++k)
                buf[k] = std::complex<SampleType>(input[in_pos + k] * window_[k],
                                                  SampleType{0});

            fft.forward(buf.data());

            for (int k = 0; k < N; ++k) {
                const double mag = std::abs(buf[k]);
                const double phase = std::arg(buf[k]);
                const double omega = kTwoPi() * static_cast<double>(k) / N;

                if (frame == 0) {
                    sum_phase[k] = phase;
                } else {
                    const double expected = static_cast<double>(Ha) * omega;
                    const double dev = princarg((phase - prev_phase[k]) - expected);
                    const double true_freq = omega + dev / static_cast<double>(Ha);
                    sum_phase[k] += static_cast<double>(Hs) * true_freq;
                }
                prev_phase[k] = phase;
                buf[k] = std::polar(static_cast<SampleType>(mag),
                                    static_cast<SampleType>(sum_phase[k]));
            }

            fft.inverse(buf.data());

            for (int k = 0; k < N; ++k) {
                out[out_pos + k] += buf[k].real() * window_[k];
                norm[out_pos + k] += window_[k] * window_[k];
            }
        }

        for (std::size_t i = 0; i < out_len; ++i)
            if (norm[i] > static_cast<SampleType>(1e-8)) out[i] /= norm[i];

        // Deliver exactly round(n_in * factor) frames: trim the overlap-add
        // tail, or zero-pad when the framed output falls a hop short (so callers
        // can size a destination buffer off round(n_in * factor)). resize(0) is
        // well-defined and yields the correct empty buffer for a tiny factor.
        out.resize(target, SampleType{0});
        return out;
    }

    /// Pitch-shift @p input by @p semitones (positive = up), preserving
    /// duration. Returns a new buffer the same length as @p input. Inputs
    /// shorter than fft_size are returned unshifted (one analysis frame is the
    /// minimum the phase vocoder can process).
    std::vector<SampleType> pitch_shift(const std::vector<SampleType>& input,
                                        double semitones) const {
        if (input.empty() || semitones == 0.0) return input;
        const double ratio = std::pow(2.0, semitones / 12.0);
        // Stretch by ratio (same pitch, longer), then resample back to the
        // original length (speeds up by ratio), shifting pitch by ratio.
        const std::vector<SampleType> stretched = time_stretch(input, ratio);
        return resample_linear(stretched, input.size());
    }

private:
    static constexpr double kPi() { return 3.14159265358979323846; }
    static constexpr double kTwoPi() { return 2.0 * kPi(); }
    // Upper bound on the stretch factor — caps output allocation at 100x the
    // input so a pathological factor can't request a multi-gigabyte buffer.
    static constexpr double kMaxStretch() { return 100.0; }

    // Nearest power of two >= 2 (rounds down on a tie).
    static int round_to_pow2(int n) {
        if (n < 2) return 2;
        int lower = 1;
        while (lower * 2 <= n) lower *= 2;
        const int upper = lower * 2;
        return (n - lower <= upper - n) ? lower : upper;
    }

    static double princarg(double phase) {
        return phase - kTwoPi() * std::round(phase / kTwoPi());
    }

    static std::vector<SampleType> resample_linear(const std::vector<SampleType>& in,
                                                   std::size_t out_len) {
        std::vector<SampleType> out(out_len, SampleType{0});
        if (in.empty() || out_len == 0) return out;
        if (in.size() == 1) {
            std::fill(out.begin(), out.end(), in[0]);
            return out;
        }
        const double step = static_cast<double>(in.size() - 1) /
                            static_cast<double>(out_len > 1 ? out_len - 1 : 1);
        for (std::size_t i = 0; i < out_len; ++i) {
            const double pos = static_cast<double>(i) * step;
            const std::size_t i0 = static_cast<std::size_t>(pos);
            const std::size_t i1 = std::min(i0 + 1, in.size() - 1);
            const SampleType frac = static_cast<SampleType>(pos - static_cast<double>(i0));
            out[i] = in[i0] * (SampleType{1} - frac) + in[i1] * frac;
        }
        return out;
    }

    PhaseVocoderConfig config_{};
    int fft_size_ = 2048;
    int synthesis_hop_ = 512;
    std::vector<SampleType> window_;
};

using PhaseVocoder = PhaseVocoderT<float>;
using PhaseVocoder64 = PhaseVocoderT<double>;

}  // namespace pulp::signal
