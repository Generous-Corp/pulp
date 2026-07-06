#pragma once

/// @file audio_test_signals.hpp
/// Shared deterministic test-signal generators.
///
/// Replaces the per-TU `make_sine` / `make_sine_vec` helpers that were
/// duplicated across the golden and determinism-matrix suites. The exact
/// floating-point expressions of the originals are preserved (including
/// their float/double promotion behavior) so converted tests keep
/// byte-identical stimulus. Header-only; no analysis code here — see
/// audio_metrics.hpp for measurements.

#include <pulp/audio/buffer.hpp>

#include <cmath>
#include <numbers>
#include <vector>

namespace pulp::test::audio {

/// Sine-wave Buffer. Same expression as the golden tests' local helper:
/// the float product is promoted to double by the sample-rate division,
/// then narrowed on store. `amplitude` defaults to full scale, which
/// multiplies by exactly 1.0f and preserves the original samples bit-for-bit.
inline pulp::audio::Buffer<float> make_sine(int channels, int samples,
                                            float freq = 440.0f,
                                            double sample_rate = 48000.0,
                                            float amplitude = 1.0f) {
    pulp::audio::Buffer<float> buf(channels, samples);
    for (int ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < samples; ++i) {
            buf.channel(ch)[i] = amplitude *
                std::sin(2.0f * std::numbers::pi_v<float> * freq * i / sample_rate);
        }
    }
    return buf;
}

/// Sine-wave Buffer computed in **all-float** arithmetic (index and sample
/// rate narrowed to float before the division, so the whole phase expression
/// stays single-precision). Matches the local helper that `test_audio_support`
/// originally used, whose stimulus differs bit-for-bit from `make_sine`'s
/// double-promoted phase. Both are preserved verbatim so each converted suite
/// keeps byte-identical stimulus.
inline pulp::audio::Buffer<float> make_sine_f(int channels, int samples,
                                              float freq = 440.0f,
                                              double sample_rate = 48000.0,
                                              float amplitude = 1.0f) {
    pulp::audio::Buffer<float> buf(channels, samples);
    for (int ch = 0; ch < channels; ++ch) {
        for (int i = 0; i < samples; ++i) {
            buf.channel(ch)[i] = amplitude *
                std::sin(2.0f * std::numbers::pi_v<float> * freq *
                         static_cast<float>(i) /
                         static_cast<float>(sample_rate));
        }
    }
    return buf;
}

/// Sine-wave vector. Same all-float expression as the determinism matrix's
/// local helper (note: NOT bit-identical to make_sine, which promotes to
/// double — both are preserved verbatim so converted suites keep their
/// original stimulus).
inline std::vector<float> make_sine_vec(int samples, float freq, double sr) {
    std::vector<float> v(static_cast<std::size_t>(samples));
    for (int i = 0; i < samples; ++i) {
        v[static_cast<std::size_t>(i)] = std::sin(
            2.0f * std::numbers::pi_v<float> * freq
            * static_cast<float>(i) / static_cast<float>(sr));
    }
    return v;
}

} // namespace pulp::test::audio
