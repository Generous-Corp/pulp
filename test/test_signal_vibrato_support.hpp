#pragma once

// test_signal_vibrato.cpp — acceptance suite for the three vibrato lineages.
//
// The load-bearing claim of this module is that `DelayVibratoT` shifts pitch and
// the two phase engines do not, so the suite is built around ONE instrument —
// peak instantaneous-frequency deviation in cents — pointed at all three. A
// suite where the three engines pass the same assertions would not be testing
// this module at all.
//
// The instrument is calibrated against a synthetic FM tone of known deviation
// before it is pointed at any engine (first test case below). It has already
// been wrong once: differentiating the demodulated phase pointwise reads almost
// exactly 2x the truth, because the demodulator leaves a ripple at twice the
// carrier that is negligible in the phase and dominant in its derivative. The
// fix, and the reason the fit is coherent over whole modulator cycles, is
// documented on `peak_cents_deviation`.

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/vibrato.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Approx;
using namespace pulp::signal;

namespace {

// ── Measurement-recipe constants (acceptance class: they define how we
// measure, not what ships) ────────────────────────────────────────────────
constexpr double kTwoPi = 2.0 * std::numbers::pi;
constexpr double kFs = 48000.0;
/// Modulator rate used by the pitch measurements. Faster than either engine's
/// default so a render covers several cycles cheaply; 48000/5 is an integer, so
/// the coherent fit lands on whole cycles.
constexpr double kProbeRateHz = 5.0;
/// Modulator harmonics kept by the coherent fit. The demodulator's residual
/// ripple sits at harmonic 2*f0/f_lfo (1600 at the 4 kHz carrier), far above
/// this, and a coherent fit over whole cycles rejects it exactly.
constexpr int kFitHarmonics = 16;
/// Cycles rendered, and cycles skipped before the fit window, so filter and
/// delay-line startup never enters a measurement.
constexpr int kRenderCycles = 3;
constexpr int kSkipCycles = 1;

std::vector<double> sine_buffer(std::size_t count, double hz, double fs = kFs) {
    std::vector<double> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = std::sin(kTwoPi * hz * static_cast<double>(i) / fs);
    }
    return out;
}

void unwrap(std::vector<double>& phase) {
    double offset = 0.0;
    for (std::size_t i = 1; i < phase.size(); ++i) {
        const double step = (phase[i] + offset) - phase[i - 1];
        if (step > std::numbers::pi) {
            offset -= kTwoPi;
        } else if (step < -std::numbers::pi) {
            offset += kTwoPi;
        }
        phase[i] += offset;
    }
}

/// Complex envelope of a signal about `carrier_hz`, from a sliding DFT whose
/// window is exactly one carrier period. One period is the point: the
/// sum-frequency image lands on exactly two cycles inside the window and
/// cancels, so no analysis filter — and therefore no analysis-filter
/// passband droop — sits between the signal and the measurement.
std::vector<std::complex<double>> demodulate(const std::vector<double>& x, double carrier_hz,
                                             double fs = kFs) {
    const auto period = static_cast<int>(std::llround(fs / carrier_hz));
    REQUIRE(std::abs(fs / carrier_hz - static_cast<double>(period)) < 1e-9);
    const int count = static_cast<int>(x.size()) - period;
    REQUIRE(count > 0);

    std::vector<double> cosine(x.size());
    std::vector<double> sine(x.size());
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double angle = kTwoPi * carrier_hz * static_cast<double>(i) / fs;
        cosine[i] = std::cos(angle);
        sine[i] = std::sin(angle);
    }

    std::vector<std::complex<double>> envelope(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        double real = 0.0;
        double imag = 0.0;
        for (int k = 0; k < period; ++k) {
            const auto n = static_cast<std::size_t>(i + k);
            real += x[n] * cosine[n];
            imag -= x[n] * sine[n];
        }
        const double scale = 2.0 / static_cast<double>(period);
        envelope[static_cast<std::size_t>(i)] = {real * scale, imag * scale};
    }
    return envelope;
}

std::vector<double> instantaneous_phase(const std::vector<double>& x, double carrier_hz,
                                        double fs = kFs) {
    const auto envelope = demodulate(x, carrier_hz, fs);
    std::vector<double> phase(envelope.size());
    for (std::size_t i = 0; i < envelope.size(); ++i) phase[i] = std::arg(envelope[i]);
    unwrap(phase);
    return phase;
}

/// Peak-to-peak amplitude ripple of a demodulated carrier, in dB. A pitch
/// vibrato that also modulates amplitude is doing something it was not asked to
/// do, and the size of that ripple is set almost entirely by how flat the
/// fractional-delay interpolator's magnitude response is across the fractional
/// range.
double envelope_ripple_db(const std::vector<double>& x, double carrier_hz, int start,
                          double fs = kFs) {
    const auto envelope = demodulate(x, carrier_hz, fs);
    double low = 1e9;
    double high = -1e9;
    for (std::size_t i = static_cast<std::size_t>(start); i < envelope.size(); ++i) {
        const double magnitude = std::abs(envelope[i]);
        low = std::min(low, magnitude);
        high = std::max(high, magnitude);
    }
    return 20.0 * std::log10(high / low);
}

struct Deviation {
    double up_cents = 0.0;
    double down_cents = 0.0;

    double span() const { return up_cents - down_cents; }
    /// How lopsided the sweep is. 1 is symmetric; a vactrol pushes it away.
    double asymmetry() const { return std::abs(up_cents) / std::abs(down_cents); }
};

/// Peak cents deviation from a coherent harmonic fit of the phase.
///
/// Pointwise differencing does not work here and the failure is not subtle: the
/// demodulator's residual ripple at twice the carrier carries a tiny phase
/// amplitude but an enormous slope, and reading the max of a pointwise
/// derivative returns roughly twice the true deviation. Fitting the first
/// `kFitHarmonics` modulator harmonics over a whole number of modulator cycles
/// removes it exactly rather than approximately, because a coherent DFT over
/// whole cycles has no leakage between harmonics.
Deviation peak_cents_deviation(const std::vector<double>& phase, int start, double carrier_hz,
                               double modulator_hz, int harmonics = kFitHarmonics,
                               double fs = kFs) {
    const auto cycle = static_cast<int>(std::llround(fs / modulator_hz));
    const int cycles = (static_cast<int>(phase.size()) - start) / cycle;
    REQUIRE(cycles >= 1);
    const int count = cycles * cycle;

    std::vector<double> cos_term(static_cast<std::size_t>(harmonics) + 1, 0.0);
    std::vector<double> sin_term(static_cast<std::size_t>(harmonics) + 1, 0.0);
    for (int k = 1; k <= harmonics; ++k) {
        double a = 0.0;
        double b = 0.0;
        for (int i = 0; i < count; ++i) {
            const double angle =
                kTwoPi * k * modulator_hz * static_cast<double>(start + i) / fs;
            a += phase[static_cast<std::size_t>(start + i)] * std::cos(angle);
            b += phase[static_cast<std::size_t>(start + i)] * std::sin(angle);
        }
        cos_term[static_cast<std::size_t>(k)] = 2.0 * a / count;
        sin_term[static_cast<std::size_t>(k)] = 2.0 * b / count;
    }

    Deviation result{-1e9, 1e9};
    for (int i = 0; i < cycle; ++i) {
        double hz = 0.0;
        for (int k = 1; k <= harmonics; ++k) {
            const double angle =
                kTwoPi * k * modulator_hz * static_cast<double>(start + i) / fs;
            hz += k * modulator_hz * (sin_term[static_cast<std::size_t>(k)] * std::cos(angle) -
                                      cos_term[static_cast<std::size_t>(k)] * std::sin(angle));
        }
        const double cents = 1200.0 * std::log2((carrier_hz + hz) / carrier_hz);
        result.up_cents = std::max(result.up_cents, cents);
        result.down_cents = std::min(result.down_cents, cents);
    }
    return result;
}

/// Renders a carrier through a mono engine and returns its peak deviation.
template <typename Engine>
Deviation measure_engine(Engine& engine, double carrier_hz, double modulator_hz) {
    const auto cycle = static_cast<std::size_t>(std::llround(kFs / modulator_hz));
    const auto input = sine_buffer(cycle * kRenderCycles, carrier_hz);
    std::vector<double> output(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) output[i] = engine.process(input[i]);
    return peak_cents_deviation(instantaneous_phase(output, carrier_hz),
                                static_cast<int>(cycle * kSkipCycles), carrier_hz, modulator_hz);
}

/// Quasi-static prediction of what an allpass cascade summed with a direct path
/// does to a carrier's instantaneous frequency, given the cascade's per-sample
/// corner trajectory. This is an INDEPENDENT model — textbook allpass phase
/// `-2*atan(tan(pi*f/fs) / tan(pi*fc/fs))` plus the blend geometry — not a
/// second copy of the engine, so agreement between the two is evidence and not
/// a tautology.
Deviation predict_from_corners(const std::vector<std::vector<double>>& corners_per_sample,
                               double carrier_hz, double mix, int start) {
    const double omega = std::tan(std::numbers::pi * carrier_hz / kFs);
    std::vector<double> argument(corners_per_sample.size());
    for (std::size_t i = 0; i < corners_per_sample.size(); ++i) {
        double phi = 0.0;
        for (double fc : corners_per_sample[i]) {
            const double clamped = std::clamp(fc, 1.0, kFs * 0.49);
            phi -= 2.0 * std::atan(omega / std::tan(std::numbers::pi * clamped / kFs));
        }
        const std::complex<double> response =
            (1.0 - mix) + mix * std::polar(1.0, phi);
        argument[i] = std::arg(response);
    }
    unwrap(argument);

    Deviation result{-1e9, 1e9};
    for (std::size_t i = static_cast<std::size_t>(start); i + 1 < argument.size(); ++i) {
        const double slope = 0.5 * (argument[i + 1] - argument[i - 1]);
        const double cents =
            1200.0 * std::log2((carrier_hz + slope * kFs / kTwoPi) / carrier_hz);
        result.up_cents = std::max(result.up_cents, cents);
        result.down_cents = std::min(result.down_cents, cents);
    }
    return result;
}

/// Coherent magnitude of a static system at one frequency: RMS ratio over a
/// whole number of periods, after an equally long settling render. Exact for a
/// pure tone through an LTI system, and immune to the peak-sample under-read
/// that bites anyone who measures a 20 kHz tone at 48 kHz by its highest
/// sample.
template <typename Process>
double coherent_gain_db(Process process, double hz, double fs = kFs) {
    // 4800 samples is a whole number of periods for every test frequency below,
    // all of which are multiples of 10 Hz.
    constexpr int kWindow = 4800;
    for (int i = 0; i < kWindow; ++i) {
        process(std::sin(kTwoPi * hz * static_cast<double>(i) / fs));
    }
    double energy_in = 0.0;
    double energy_out = 0.0;
    for (int i = kWindow; i < 2 * kWindow; ++i) {
        const double x = std::sin(kTwoPi * hz * static_cast<double>(i) / fs);
        const double y = process(x);
        energy_in += x * x;
        energy_out += y * y;
    }
    return 10.0 * std::log10(energy_out / energy_in);
}

/// L1 norm of a static allpass cascade's impulse response — the exact worst-case
/// sample gain over all bounded inputs, and the honest ceiling for these
/// engines. Unity MAGNITUDE response does not bound sample gain: an allpass's
/// impulse response changes sign, so a sign-matched input accumulates.
double cascade_impulse_l1(const std::vector<double>& corners, int length = 200000) {
    std::vector<TptFilter64> stages(corners.size());
    for (std::size_t i = 0; i < corners.size(); ++i) {
        stages[i].prepare(kFs);
        stages[i].set_cutoff(corners[i]);
    }
    double l1 = 0.0;
    for (int i = 0; i < length; ++i) {
        double x = (i == 0) ? 1.0 : 0.0;
        for (auto& stage : stages) x = stage.process_allpass(x);
        l1 += std::abs(x);
    }
    return l1;
}

/// Peak output magnitude over a battery of unit-amplitude signals a real source
/// can produce: low square waves (the shape that drives an allpass hardest),
/// sines across the band, an impulse, and a step.
template <typename Make>
double battery_peak_gain(Make make) {
    double peak = 0.0;
    for (double hz : {20.0, 40.0, 55.0, 110.0, 220.0, 440.0}) {
        auto engine = make();
        for (int i = 0; i < 96000; ++i) {
            const double x =
                std::sin(kTwoPi * hz * static_cast<double>(i) / kFs) >= 0.0 ? 1.0 : -1.0;
            peak = std::max(peak, std::abs(engine.process(x)));
        }
    }
    for (double hz : {30.0, 70.0, 200.0, 600.0, 2000.0, 6000.0}) {
        auto engine = make();
        for (int i = 0; i < 96000; ++i) {
            peak = std::max(peak, std::abs(engine.process(
                                      std::sin(kTwoPi * hz * static_cast<double>(i) / kFs))));
        }
    }
    {
        auto engine = make();
        for (int i = 0; i < 96000; ++i) peak = std::max(peak, std::abs(engine.process(i == 0 ? 1.0 : 0.0)));
    }
    {
        auto engine = make();
        for (int i = 0; i < 96000; ++i) peak = std::max(peak, std::abs(engine.process(1.0)));
    }
    return peak;
}

/// Scalar face of the stereo Univibe, so the gain probes can treat all three
/// engines the same way. Returns the wet output in either mode.
struct UniVibeWetTap {
    UniVibe64 engine;
    double process(double x) {
        double left = 0.0;
        double right = 0.0;
        engine.process(x, left, right);
        return right;
    }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// The instrument, before anything is measured with it.
// ─────────────────────────────────────────────────────────────────────────


// ─────────────────────────────────────────────────────────────────────────
// DelayVibratoT — the only engine that shifts pitch.
// ─────────────────────────────────────────────────────────────────────────






// ─────────────────────────────────────────────────────────────────────────
// PhaseVibratoT — Magnatone lineage. Moves notches, not pitch.
// ─────────────────────────────────────────────────────────────────────────





// ─────────────────────────────────────────────────────────────────────────
// Allpass unity gain — series law 1, asserted rather than assumed.
// ─────────────────────────────────────────────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────
// UniVibeT — staggered corners behind a vactrol.
// ─────────────────────────────────────────────────────────────────────────






// ─────────────────────────────────────────────────────────────────────────
// Gain bounds, determinism, and RT safety.
// ─────────────────────────────────────────────────────────────────────────
