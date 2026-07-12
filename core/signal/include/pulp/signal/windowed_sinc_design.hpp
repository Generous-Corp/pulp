#pragma once

/// @file windowed_sinc_design.hpp
/// Shared Kaiser-windowed-sinc FIR design helpers.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace pulp::signal {

inline double bessel_i0(double x) {
    const double half_x = 0.5 * x;
    double sum = 1.0;
    double term = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= half_x / static_cast<double>(k);
        const double squared = term * term;
        sum += squared;
        if (squared < 1e-20 * sum)
            break;
    }
    return sum;
}

inline double kaiser_beta_for_stopband(double stopband_db) {
    if (stopband_db > 50.0)
        return 0.1102 * (stopband_db - 8.7);
    if (stopband_db >= 21.0) {
        return 0.5842 * std::pow(stopband_db - 21.0, 0.4) + 0.07886 * (stopband_db - 21.0);
    }
    return 0.0;
}

inline std::size_t kaiser_length_for_transition(double stopband_db, double normalized_width) {
    if (normalized_width <= 0.0)
        normalized_width = 1e-6;
    const double order = (stopband_db - 7.95) / (14.36 * normalized_width);
    std::size_t taps = static_cast<std::size_t>(std::ceil(order)) + 1;
    if ((taps & 1u) == 0u)
        ++taps;
    return std::max<std::size_t>(taps, 3u);
}

inline void kaiser_window(std::vector<double>& output, double beta) {
    if (output.empty())
        return;
    const double denominator = bessel_i0(beta);
    const double half = 0.5 * static_cast<double>(output.size() - 1);
    for (std::size_t i = 0; i < output.size(); ++i) {
        const double x = (static_cast<double>(i) - half) / half;
        const double argument = beta * std::sqrt(std::max(0.0, 1.0 - x * x));
        output[i] = bessel_i0(argument) / denominator;
    }
}

inline std::vector<double> design_windowed_sinc(std::size_t taps, double cutoff, double beta) {
    std::vector<double> coefficients(taps, 0.0);
    std::vector<double> window(taps, 0.0);
    kaiser_window(window, beta);
    const double half = 0.5 * static_cast<double>(taps - 1);
    constexpr double pi = 3.14159265358979323846;
    const double doubled_cutoff = 2.0 * cutoff;
    for (std::size_t i = 0; i < taps; ++i) {
        const double offset = static_cast<double>(i) - half;
        const double sinc = std::abs(offset) < 1e-12
                                ? doubled_cutoff
                                : std::sin(pi * doubled_cutoff * offset) / (pi * offset);
        coefficients[i] = sinc * window[i];
    }
    double sum = 0.0;
    for (double coefficient : coefficients)
        sum += coefficient;
    if (std::abs(sum) > 1e-18) {
        for (double& coefficient : coefficients)
            coefficient /= sum;
    }
    return coefficients;
}

} // namespace pulp::signal
