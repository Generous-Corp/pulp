#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <pulp/signal/spectral_frame_codec.hpp>
#include <stdexcept>
#include <string>
#include <vector>

namespace asset_oracle {
// Frozen before candidate execution: float FFT/WOLA versus direct double DFT.
inline constexpr double peak_tolerance = 2e-5, rms_tolerance = 2e-6;
using Frame = std::array<std::complex<double>, 129>;
inline void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
inline double window(int i) {
    return 0.5 - 0.5 * std::cos(2 * std::numbers::pi * i / 255);
}
inline std::vector<Frame> analyze(const std::vector<float>& audio) {
    std::vector<Frame> frames((audio.size() + 255) / 64);
    for (std::size_t f = 0; f < frames.size(); ++f)
        for (int k = 0; k < 129; ++k) {
            std::complex<double> sum{};
            for (int i = 0; i < 256; ++i) {
                const auto t = static_cast<std::int64_t>(f * 64) - 192 + i;
                if (t >= 0 && t < static_cast<std::int64_t>(audio.size()))
                    sum += double(audio[t]) * window(i) *
                           std::polar(1.0, -2 * std::numbers::pi * k * i / 256);
            }
            frames[f][k] = k == 0 || k == 128 ? std::complex<double>{sum.real(), 0} : sum;
        }
    return frames;
}
inline auto encode(const std::vector<Frame>& frames, double rate = 48000) {
    auto stream = pulp::signal::make_spectral_stream({256, 64, 1, rate}, frames.size());
    require(bool(stream), "make canonical stream");
    for (std::size_t f = 0; f < frames.size(); ++f)
        for (int k = 0; k < 129; ++k) {
            stream.value->components[f * 258 + 2 * k] = float(frames[f][k].real());
            stream.value->components[f * 258 + 2 * k + 1] = float(frames[f][k].imag());
        }
    auto bytes = pulp::signal::encode_spectral_stream(*stream.value);
    require(bool(bytes), "encode canonical stream");
    return std::move(*bytes.value);
}
inline std::complex<double> interpolate(std::complex<double> a, std::complex<double> b,
                                        double amount, int k) {
    if (amount == 0)
        return a;
    if (amount == 1)
        return b;
    const double ma = std::abs(a), mb = std::abs(b), magnitude = (1 - amount) * ma + amount * mb;
    double pa = std::arg(a), pb = std::arg(b);
    if (ma == 0)
        pa = pb;
    if (mb == 0)
        pb = pa;
    if (k == 0 || k == 128)
        return {std::cos(amount < 0.5 ? pa : pb) < 0 ? -magnitude : magnitude, 0};
    const double phase = pa + amount * std::remainder(pb - pa, 2 * std::numbers::pi);
    return {magnitude * std::cos(phase), magnitude * std::sin(phase)};
}
inline std::vector<double> expected(const std::vector<Frame>& a, const std::vector<Frame>& b,
                                    const std::vector<float>& control) {
    const std::size_t end = a.size() * 64;
    std::vector<double> numerator(end), norm(end), output(control.size());
    for (std::size_t f = 0; f < a.size() + 4; ++f) {
        double amount = 0;
        for (int i = 0; i < 64; ++i)
            amount += control[f * 64 + i] / 64.0;
        Frame frame{};
        if (f < a.size())
            for (int k = 0; k < 129; ++k)
                frame[k] = interpolate(a[f][k], b[f][k], amount, k);
        for (int i = 0; i < 256; ++i) {
            const auto t = static_cast<std::int64_t>(f * 64) - 192 + i;
            if (t < 0 || t >= static_cast<std::int64_t>(end))
                continue;
            double value = frame[0].real() + (i % 2 ? -1 : 1) * frame[128].real();
            for (int k = 1; k < 128; ++k)
                value +=
                    2 * (frame[k] * std::polar(1.0, 2 * std::numbers::pi * k * i / 256)).real();
            numerator[t] += value / 256 * window(i);
            norm[t] += window(i) * window(i);
        }
    }
    for (std::size_t t = 0; t < end; ++t)
        output[t + 320] = norm[t] > 1e-9 ? numerator[t] / norm[t] : 0;
    return output;
}
inline void compare(const std::vector<float>& actual, const std::vector<double>& wanted) {
    require(actual.size() == wanted.size(), "output length");
    double peak = 0, square = 0;
    for (std::size_t i = 0; i < actual.size(); ++i) {
        require(std::isfinite(actual[i]), "nonfinite output");
        const double error = std::abs(actual[i] - wanted[i]);
        peak = std::max(peak, error);
        square += error * error;
    }
    if (peak > peak_tolerance || std::sqrt(square / actual.size()) > rms_tolerance)
        throw std::runtime_error(
            "independent direct DFT/Hann/OLA audio oracle peak=" + std::to_string(peak) +
            " rms=" + std::to_string(std::sqrt(square / actual.size())));
}
inline std::vector<float> stimulus(int endpoint, int length = 769) {
    std::vector<float> audio(length);
    for (int i = 0; i < length; ++i)
        audio[i] =
            float((endpoint ? -0.07 : 0.05) +
                  0.12 * std::cos(2 * std::numbers::pi * 9 * i / 256 + (endpoint ? -3.10 : 3.10)) +
                  (i % 2 ? -0.03 : 0.03));
    audio[0] += 0.3f;
    audio[13] -= 0.25f;
    audio.back() += endpoint ? -0.4f : 0.5f;
    return audio;
}
} // namespace asset_oracle
