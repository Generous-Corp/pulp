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
/// real-time-audio-safe. Input/output are mono float buffers. Header-only to
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

class PhaseVocoder {
public:
    PhaseVocoder() { configure({}); }
    explicit PhaseVocoder(const PhaseVocoderConfig& config) { configure(config); }

    void configure(const PhaseVocoderConfig& config) {
        config_ = config;
        // Fft requires a power-of-two size; round the requested size to the
        // nearest power of two (down on a tie) so a non-pow2 request can't
        // silently transform only part of each frame.
        fft_size_ = round_to_pow2(config.fft_size > 1 ? config.fft_size : 2048);
        synthesis_hop_ =
            config.synthesis_hop > 0 ? config.synthesis_hop : fft_size_ / 4;
        if (synthesis_hop_ < 1) synthesis_hop_ = 1;
        window_ = WindowFunction::generate(fft_size_, config.window, 0.0f);
    }

    const PhaseVocoderConfig& config() const noexcept { return config_; }

    /// Time-stretch @p input by @p factor (> 0). Output is ~factor times as long
    /// as the input and keeps the same pitch. factor == 1 is ~identity.
    std::vector<float> time_stretch(const std::vector<float>& input,
                                    double factor) const {
        const int N = fft_size_;
        if (input.empty() || factor <= 0.0 || N <= 1) return input;

        const int Hs = synthesis_hop_;
        int Ha = static_cast<int>(std::lround(static_cast<double>(Hs) / factor));
        Ha = std::max(1, Ha);

        const std::size_t n_in = input.size();
        if (n_in < static_cast<std::size_t>(N)) return input;  // too short to frame

        const int num_frames =
            static_cast<int>((n_in - static_cast<std::size_t>(N)) / Ha) + 1;
        const std::size_t out_len =
            static_cast<std::size_t>(num_frames - 1) * Hs + N;

        std::vector<float> out(out_len, 0.0f);
        std::vector<float> norm(out_len, 0.0f);

        Fft fft(N);
        std::vector<std::complex<float>> buf(N);
        std::vector<double> prev_phase(N, 0.0);
        std::vector<double> sum_phase(N, 0.0);

        for (int frame = 0; frame < num_frames; ++frame) {
            const std::size_t in_pos = static_cast<std::size_t>(frame) * Ha;
            const std::size_t out_pos = static_cast<std::size_t>(frame) * Hs;

            for (int k = 0; k < N; ++k)
                buf[k] = std::complex<float>(input[in_pos + k] * window_[k], 0.0f);

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
                buf[k] = std::polar(static_cast<float>(mag),
                                    static_cast<float>(sum_phase[k]));
            }

            fft.inverse(buf.data());

            for (int k = 0; k < N; ++k) {
                out[out_pos + k] += buf[k].real() * window_[k];
                norm[out_pos + k] += window_[k] * window_[k];
            }
        }

        for (std::size_t i = 0; i < out_len; ++i)
            if (norm[i] > 1e-8f) out[i] /= norm[i];

        // Deliver exactly round(n_in * factor) frames: trim the overlap-add
        // tail, or zero-pad when the framed output falls a hop short (so callers
        // can size a destination buffer off round(n_in * factor)).
        const std::size_t target = static_cast<std::size_t>(
            std::lround(static_cast<double>(n_in) * factor));
        if (target > 0) out.resize(target, 0.0f);
        return out;
    }

    /// Pitch-shift @p input by @p semitones (positive = up), preserving
    /// duration. Returns a new buffer the same length as @p input. Inputs
    /// shorter than fft_size are returned unshifted (one analysis frame is the
    /// minimum the phase vocoder can process).
    std::vector<float> pitch_shift(const std::vector<float>& input,
                                   double semitones) const {
        if (input.empty() || semitones == 0.0) return input;
        const double ratio = std::pow(2.0, semitones / 12.0);
        // Stretch by ratio (same pitch, longer), then resample back to the
        // original length (speeds up by ratio), shifting pitch by ratio.
        const std::vector<float> stretched = time_stretch(input, ratio);
        return resample_linear(stretched, input.size());
    }

private:
    static constexpr double kPi() { return 3.14159265358979323846; }
    static constexpr double kTwoPi() { return 2.0 * kPi(); }

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

    static std::vector<float> resample_linear(const std::vector<float>& in,
                                              std::size_t out_len) {
        std::vector<float> out(out_len, 0.0f);
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
            const float frac = static_cast<float>(pos - static_cast<double>(i0));
            out[i] = in[i0] * (1.0f - frac) + in[i1] * frac;
        }
        return out;
    }

    PhaseVocoderConfig config_{};
    int fft_size_ = 2048;
    int synthesis_hop_ = 512;
    std::vector<float> window_;
};

}  // namespace pulp::signal
