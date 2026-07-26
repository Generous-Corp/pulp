#pragma once

#include <pulp/signal/noise_source.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace pulp::signal::drum {

/// A compact, license-clean bank of 26 phase-readable FM waves.
///
/// Each row is an authored four-harmonic spectrum. Reading the table at a
/// phase offset makes it suitable for phase modulation; normalizing by the
/// coefficient L1 norm keeps every row bounded without a runtime limiter.
struct FmWaveTable {
    static constexpr int wave_count = 26;
    static constexpr int harmonic_count = 4;

    static constexpr std::array<std::array<double, harmonic_count>, wave_count>
        harmonics{{
            {{1.00, 0.00, 0.000, 0.00}},
            {{1.00, 0.50, 0.333, 0.25}},
            {{1.00, -0.50, 0.333, -0.25}},
            {{1.00, 0.00, 0.333, 0.00}},
            {{1.00, 0.00, -0.111, 0.00}},
            {{1.00, 0.70, 0.000, 0.00}},
            {{1.00, -0.70, 0.000, 0.00}},
            {{1.00, 0.00, 0.700, 0.00}},
            {{1.00, 0.00, -0.700, 0.00}},
            {{1.00, 0.00, 0.000, 0.70}},
            {{1.00, 0.00, 0.000, -0.70}},
            {{0.50, 1.00, 0.000, 0.00}},
            {{0.50, 0.00, 1.000, 0.00}},
            {{0.50, 0.00, 0.000, 1.00}},
            {{1.00, 0.50, -0.333, 0.25}},
            {{1.00, -0.50, -0.333, -0.25}},
            {{1.00, 0.25, 0.750, 0.125}},
            {{1.00, -0.25, 0.750, -0.125}},
            {{0.75, 1.00, 0.500, 0.25}},
            {{0.75, -1.00, 0.500, -0.25}},
            {{1.00, 1.00, 1.000, 1.00}},
            {{1.00, -1.00, 1.000, -1.00}},
            {{1.00, 0.80, 0.400, 0.10}},
            {{1.00, 0.10, 0.400, 0.80}},
            {{0.25, 0.50, 0.750, 1.00}},
            {{1.00, -0.25, -0.500, 0.75}},
        }};

    static double read(int wave, double phase,
                       double phase_increment = 0.0) {
        const auto index =
            static_cast<std::size_t>(std::clamp(wave, 0, wave_count - 1));
        phase -= std::floor(phase);
        const double increment = std::fabs(phase_increment);
        const int available_harmonics =
            increment > 0.0
                ? std::clamp(
                      static_cast<int>(std::floor(0.5 / increment)), 1,
                      harmonic_count)
                : harmonic_count;
        double sample = 0.0;
        double normalization = 0.0;
        for (int harmonic = 0; harmonic < available_harmonics; ++harmonic) {
            const double coefficient =
                harmonics[index][static_cast<std::size_t>(harmonic)];
            sample += coefficient *
                      std::sin(2.0 * 3.14159265358979323846 *
                               static_cast<double>(harmonic + 1) * phase);
            normalization += std::fabs(coefficient);
        }
        return normalization > 0.0 ? sample / normalization : 0.0;
    }

    /// Casio-family phase warp: amount zero is identity; one moves the bend
    /// point close to the start of the cycle and produces a saw-like spectrum.
    static double warp(double phase, double amount) {
        phase -= std::floor(phase);
        const double d = 0.5 * (1.0 - 0.98 * std::clamp(amount, 0.0, 1.0));
        return phase < d ? phase * (0.5 / d)
                         : 0.5 + (phase - d) * (0.5 / (1.0 - d));
    }
};

struct FmTransient {
    NoiseColor color;
    double noise_level;
    double noise_decay_ms;
    double click_level;
    double click_cutoff_hz;
    double click_decay_ms;
};

/// Twenty-four original procedural strike recipes. These are synthesis
/// settings, not sampled or matched values.
inline constexpr std::array<FmTransient, 24> kFmTransients{{
    {NoiseColor::white, 0.00, 3.0, 0.10, 9000.0, 0.7},
    {NoiseColor::white, 0.05, 4.0, 0.14, 7500.0, 1.0},
    {NoiseColor::white, 0.10, 5.0, 0.18, 6200.0, 1.3},
    {NoiseColor::pink, 0.08, 7.0, 0.12, 5200.0, 1.8},
    {NoiseColor::pink, 0.15, 10.0, 0.16, 4200.0, 2.4},
    {NoiseColor::pink, 0.22, 14.0, 0.20, 3400.0, 3.0},
    {NoiseColor::brown, 0.12, 18.0, 0.08, 2800.0, 3.8},
    {NoiseColor::brown, 0.20, 24.0, 0.12, 2400.0, 4.8},
    {NoiseColor::violet, 0.05, 2.0, 0.18, 12000.0, 0.5},
    {NoiseColor::violet, 0.10, 3.0, 0.24, 10000.0, 0.8},
    {NoiseColor::violet, 0.18, 5.0, 0.30, 8200.0, 1.1},
    {NoiseColor::blue, 0.08, 7.0, 0.20, 7000.0, 1.5},
    {NoiseColor::blue, 0.16, 10.0, 0.26, 5800.0, 2.0},
    {NoiseColor::blue, 0.24, 14.0, 0.32, 4800.0, 2.7},
    {NoiseColor::white, 0.30, 20.0, 0.10, 3600.0, 3.5},
    {NoiseColor::pink, 0.35, 28.0, 0.14, 3000.0, 4.5},
    {NoiseColor::brown, 0.30, 40.0, 0.08, 2200.0, 6.0},
    {NoiseColor::violet, 0.28, 8.0, 0.36, 14000.0, 0.6},
    {NoiseColor::blue, 0.32, 12.0, 0.40, 11000.0, 0.9},
    {NoiseColor::white, 0.40, 18.0, 0.44, 8800.0, 1.4},
    {NoiseColor::pink, 0.45, 26.0, 0.30, 6500.0, 2.2},
    {NoiseColor::brown, 0.50, 38.0, 0.22, 4600.0, 3.4},
    {NoiseColor::violet, 0.38, 16.0, 0.50, 12500.0, 1.0},
    {NoiseColor::blue, 0.55, 30.0, 0.55, 7200.0, 2.8},
}};

}  // namespace pulp::signal::drum
