#pragma once

// Independent double-precision oracles for finite spectral streams: the
// analysis frame grid walked sample by sample, a forward DFT on that grid, and
// an inverse DFT with squared-window overlap-add. None of them calls the
// production frame engine, codec or finite-stream rule.

#include <pulp/signal/spectral_frame_codec.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <utility>
#include <vector>

namespace pulp::test::spectral_oracle {

struct Geometry {
    int fft, hop, channels;
};

using Frame = std::vector<std::complex<double>>; // channel-major, channels * bins
using Stream = std::vector<Frame>;
using Audio = std::vector<std::vector<float>>;

inline double window(int i, int n) {
    return 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / (n - 1));
}

// The first analysis start: the smallest hop multiple above -N.
inline std::int64_t grid_start(const Geometry& g) {
    std::int64_t s = 0;
    while (s - g.hop > -g.fft)
        s -= g.hop;
    return s;
}

// Analysis frames whose window [s, s + N) overlaps [0, L), walking the grid.
inline std::uint64_t grid_frames(const Geometry& g, std::uint64_t length) {
    std::uint64_t count = 0;
    for (auto s = grid_start(g); s < static_cast<std::int64_t>(length); s += g.hop)
        if (s + g.fft > 0)
            ++count;
    return count;
}

// End of the last overlapping frame's window, walking the grid.
inline std::int64_t last_window_end(const Geometry& g, std::uint64_t length) {
    std::int64_t end = 0;
    for (auto s = grid_start(g); s < static_cast<std::int64_t>(length); s += g.hop)
        end = s + g.fft;
    return end;
}

// Smallest count of zero frames after the last stored one whose first start
// reaches that frame's window end.
inline std::uint64_t drain_frames(const Geometry& g) {
    std::uint64_t d = 0;
    while (static_cast<std::int64_t>(d) * g.hop < g.fft)
        ++d;
    return d;
}

struct Twiddles {
    int n = 0;
    std::vector<double> c, s;
    explicit Twiddles(int size) : n(size), c(size), s(size) {
        for (int i = 0; i < n; ++i) {
            c[i] = std::cos(2 * std::numbers::pi * i / n);
            s[i] = std::sin(2 * std::numbers::pi * i / n);
        }
    }
};

inline Audio stimulus(const Geometry& g, std::size_t length, int seed = 0) {
    Audio audio(g.channels, std::vector<float>(length));
    for (int c = 0; c < g.channels; ++c) {
        const double cycle = 5 + 3 * c + 2 * seed;
        for (std::size_t i = 0; i < length; ++i)
            audio[c][i] =
                float(0.04 + 0.02 * c +
                      0.12 * std::cos(2 * std::numbers::pi * cycle * double(i) / g.fft + 3.1 + c) +
                      (i % 2 ? -0.03 : 0.03));
        audio[c][0] += 0.3f;
        if (length > std::size_t(13 + c))
            audio[c][13 + c] -= 0.25f;
        audio[c].back() += 0.5f + 0.1f * c;
    }
    return audio;
}

inline Stream analyze(const Geometry& g, const Audio& audio) {
    const int bins = g.fft / 2 + 1;
    const auto length = static_cast<std::int64_t>(audio.front().size());
    const Twiddles tw(g.fft);
    Stream frames(grid_frames(g, static_cast<std::uint64_t>(length)),
                  Frame(static_cast<std::size_t>(g.channels) * bins));
    std::vector<double> x(g.fft);
    for (std::size_t f = 0; f < frames.size(); ++f)
        for (int c = 0; c < g.channels; ++c) {
            for (int i = 0; i < g.fft; ++i) {
                const auto t = grid_start(g) + static_cast<std::int64_t>(f) * g.hop + i;
                x[i] = t >= 0 && t < length ? double(audio[c][t]) * window(i, g.fft) : 0.0;
            }
            for (int k = 0; k < bins; ++k) {
                double re = 0, im = 0;
                for (int i = 0; i < g.fft; ++i) {
                    const auto p = static_cast<std::size_t>((std::int64_t(k) * i) % g.fft);
                    re += x[i] * tw.c[p];
                    im -= x[i] * tw.s[p];
                }
                frames[f][c * bins + k] = {re, k == 0 || k == bins - 1 ? 0.0 : im};
            }
        }
    return frames;
}

// Frames stored as float, as the codec stores them.
inline Stream rounded(const Stream& frames) {
    Stream out = frames;
    for (auto& frame : out)
        for (auto& bin : frame)
            bin = {double(float(bin.real())), double(float(bin.imag()))};
    return out;
}

inline signal::EncodedSpectralStream encode(const Geometry& g, const Stream& frames,
                                            double rate = 48000) {
    const int bins = g.fft / 2 + 1;
    auto stream = signal::make_spectral_stream(
        {std::uint32_t(g.fft), std::uint32_t(g.hop), std::uint16_t(g.channels), rate},
        frames.size());
    REQUIRE(stream);
    const std::size_t stride = 2ull * g.channels * bins;
    for (std::size_t f = 0; f < frames.size(); ++f) {
        REQUIRE(stream.value->frames[f].origin == grid_start(g) + std::int64_t(f) * g.hop);
        for (int c = 0; c < g.channels; ++c)
            for (int k = 0; k < bins; ++k) {
                const auto p = f * stride + 2ull * (c * bins + k);
                stream.value->components[p] = float(frames[f][c * bins + k].real());
                stream.value->components[p + 1] = float(frames[f][c * bins + k].imag());
            }
    }
    auto bytes = signal::encode_spectral_stream(*stream.value);
    REQUIRE(bytes);
    return std::move(*bytes.value);
}

inline signal::EncodedSpectralStream zero_stream(const Geometry& g, std::uint64_t frames,
                                                 double rate = 48000) {
    auto stream = signal::make_spectral_stream(
        {std::uint32_t(g.fft), std::uint32_t(g.hop), std::uint16_t(g.channels), rate}, frames);
    REQUIRE(stream);
    auto bytes = signal::encode_spectral_stream(*stream.value);
    REQUIRE(bytes);
    return std::move(*bytes.value);
}

// Inverse DFT and squared-window OLA of `frames` followed by the zero drain
// frames, delayed by N + H, over `total` output samples. Output stops at the
// end of the last stored frame's window.
inline std::vector<std::vector<double>> resynthesize(const Geometry& g, const Stream& frames,
                                                     std::size_t total) {
    const int bins = g.fft / 2 + 1;
    const Twiddles tw(g.fft);
    const auto end = grid_start(g) + std::int64_t(frames.size() - 1) * g.hop + g.fft;
    const std::size_t delay = g.fft + g.hop;
    std::vector<std::vector<double>> output(g.channels, std::vector<double>(total));
    for (int c = 0; c < g.channels; ++c) {
        std::vector<double> numerator(static_cast<std::size_t>(end)),
            norm(static_cast<std::size_t>(end));
        const Frame silent(static_cast<std::size_t>(g.channels) * bins);
        // Drain frames carry no signal but still add to the squared-window sum.
        for (std::size_t f = 0; f < frames.size() + drain_frames(g); ++f) {
            const auto* frame = (f < frames.size() ? frames[f] : silent).data() + c * bins;
            for (int i = 0; i < g.fft; ++i) {
                const auto t = grid_start(g) + static_cast<std::int64_t>(f) * g.hop + i;
                if (t < 0 || t >= end)
                    continue;
                double value = frame[0].real() + (i % 2 ? -1 : 1) * frame[bins - 1].real();
                for (int k = 1; k < bins - 1; ++k) {
                    const auto p = static_cast<std::size_t>((std::int64_t(k) * i) % g.fft);
                    value += 2 * (frame[k].real() * tw.c[p] - frame[k].imag() * tw.s[p]);
                }
                numerator[t] += value / g.fft * window(i, g.fft);
                norm[t] += window(i, g.fft) * window(i, g.fft);
            }
        }
        for (std::size_t t = 0; t < std::size_t(end) && t + delay < total; ++t)
            output[c][t + delay] = norm[t] > 1e-9 ? numerator[t] / norm[t] : 0;
    }
    return output;
}

inline void compare(const Audio& actual, const std::vector<std::vector<double>>& wanted,
                    double peak_tolerance, double rms_tolerance) {
    REQUIRE(actual.size() == wanted.size());
    for (std::size_t c = 0; c < actual.size(); ++c) {
        CAPTURE(c);
        REQUIRE(actual[c].size() == wanted[c].size());
        double peak = 0, square = 0;
        for (std::size_t i = 0; i < actual[c].size(); ++i) {
            REQUIRE(std::isfinite(actual[c][i]));
            const double error = std::abs(actual[c][i] - wanted[c][i]);
            peak = std::max(peak, error);
            square += error * error;
        }
        const double rms = std::sqrt(square / actual[c].size());
        CAPTURE(peak, rms);
        REQUIRE(peak <= peak_tolerance);
        REQUIRE(rms <= rms_tolerance);
    }
}

} // namespace pulp::test::spectral_oracle
