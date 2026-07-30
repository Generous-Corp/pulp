#pragma once

// LeslieRotaryT and ScannerVibratoT — the two mechanical modulators.
//
// Everything here is measured out of rendered audio rather than read off a
// setter, and every expectation is computed from the shipped constants. The
// module's claims are physical claims — a pitch deviation of a stated depth, a
// rotor arriving at a stated time, a band split at a stated corner — so a test
// that only checked that a parameter round-tripped would prove none of them.
//
// One instrument does most of the work: complex demodulation at a known carrier
// gives BOTH the amplitude envelope (the tremolo) and the instantaneous
// frequency (the Doppler) from a single pass, which is exactly the pair of
// quantities this module modulates.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "rt_allocation_probe.hpp"

#include <pulp/signal/leslie.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <utility>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Leslie = LeslieRotaryT<double>;
using Scanner = ScannerVibratoT<double>;

constexpr double kSr = 48000.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

// ── The instrument ────────────────────────────────────────────────────────

/// One pass of complex demodulation at `carrier_hz`: multiply by
/// `e^{-j2*pi*f0*t}`, low-pass both quadratures, and read off the magnitude and
/// the phase derivative.
///
/// Chosen over peak-tracking on the waveform for the reason the series keeps
/// relearning: a peak detector on a modulated carrier samples the modulator
/// only at the carrier's peaks, so it measures the modulation ALIASED down to
/// the beat between the two, and the answer moves when the carrier moves.
/// Demodulation has no such coupling.
struct Demodulated {
    std::vector<double> envelope;  ///< |analytic|, at `rate_hz`.
    std::vector<double> freq_hz;   ///< Instantaneous frequency, at `rate_hz`.
    double rate_hz = 0.0;          ///< The decimated rate the traces live at.
};

/// `lowpass_hz` must pass the modulation and reject the image at twice the
/// carrier. The image is what limits it: a residual image of relative amplitude
/// `a` puts a ripple of `a*2*f0` Hz on the instantaneous-frequency trace, which
/// is small against a Leslie's tens-of-Hz deviation and NOT small against a
/// scanner's, so the two callers pass different corners on purpose.
[[maybe_unused]] Demodulated demodulate(const std::vector<double>& x, double carrier_hz, double input_rate_hz,
                       double lowpass_hz, int decimation) {
    Demodulated out;
    out.rate_hz = input_rate_hz / decimation;
    const double pole = std::exp(-kTwoPi * lowpass_hz / input_rate_hz);
    double state_i[4] = {0.0, 0.0, 0.0, 0.0};
    double state_q[4] = {0.0, 0.0, 0.0, 0.0};
    double previous_phase = 0.0;
    bool have_previous = false;

    for (std::size_t n = 0; n < x.size(); ++n) {
        const double w = kTwoPi * carrier_hz * static_cast<double>(n) / input_rate_hz;
        double i = x[n] * std::cos(w);
        double q = -x[n] * std::sin(w);
        for (int k = 0; k < 4; ++k) {
            state_i[k] = pole * state_i[k] + (1.0 - pole) * i;
            i = state_i[k];
            state_q[k] = pole * state_q[k] + (1.0 - pole) * q;
            q = state_q[k];
        }
        if (static_cast<int>(n) % decimation != 0) continue;

        out.envelope.push_back(2.0 * std::hypot(i, q));
        const double phase = std::atan2(q, i);
        if (have_previous) {
            double d = phase - previous_phase;
            while (d > std::numbers::pi)
                d -= kTwoPi;
            while (d < -std::numbers::pi)
                d += kTwoPi;
            out.freq_hz.push_back(carrier_hz + d * out.rate_hz / kTwoPi);
        }
        previous_phase = phase;
        have_previous = true;
    }
    return out;
}

/// Coherent DFT of a trace at an arbitrary frequency — not an FFT bin, so the
/// probe frequency can be the exact rate a constant predicts instead of the
/// nearest bin to it.
std::complex<double> coherent(const std::vector<double>& x, double hz, double rate_hz,
                             std::size_t begin) {
    std::complex<double> acc{0.0, 0.0};
    for (std::size_t n = begin; n < x.size(); ++n) {
        const double w = kTwoPi * hz * static_cast<double>(n) / rate_hz;
        acc += x[n] * std::complex<double>(std::cos(w), -std::sin(w));
    }
    return acc * (2.0 / static_cast<double>(x.size() - begin));
}

/// The frequency of the strongest component in a band, by scanning the coherent
/// DFT on a fine grid. Resolution is the grid, not the render length, so a rate
/// can be pinned far below one FFT bin.
[[maybe_unused]] double locate_peak(const std::vector<double>& x, double lo_hz, double hi_hz, double rate_hz,
                   int steps) {
    double best_hz = lo_hz;
    double best_mag = -1.0;
    for (int k = 0; k <= steps; ++k) {
        const double hz = lo_hz + (hi_hz - lo_hz) * k / steps;
        const double mag = std::abs(coherent(x, hz, rate_hz, 0));
        if (mag > best_mag) {
            best_mag = mag;
            best_hz = hz;
        }
    }
    return best_hz;
}

[[maybe_unused]] std::vector<double> remove_mean(const std::vector<double>& x) {
    double sum = 0.0;
    for (double v : x) sum += v;
    const double mean = sum / static_cast<double>(x.size());
    std::vector<double> y = x;
    for (auto& v : y) v -= mean;
    return y;
}

/// Median of `|trace − centre|` over the settled tail.
///
/// The right statistic for a SQUARE-wave deviation, which is what a linearly
/// ramping delay produces: the plateau is the physical quantity and the
/// turnarounds are edges. Taking the peak instead reads the edge transient and
/// reports 2–4 % high, by an amount that depends only on the measurement filter
/// — an artefact of the instrument, not of the model.
[[maybe_unused]] double median_deviation(const std::vector<double>& trace, double centre) {
    std::vector<double> d;
    for (std::size_t i = trace.size() / 3; i < trace.size(); ++i)
        d.push_back(std::abs(trace[i] - centre));
    std::sort(d.begin(), d.end());
    return d[d.size() / 2];
}

/// Peak of `|trace − centre|` over the settled tail. The right statistic for a
/// SINE deviation, which is what a rotating source produces.
[[maybe_unused]] double peak_deviation(const std::vector<double>& trace, double centre) {
    double peak = 0.0;
    for (std::size_t i = trace.size() / 3; i < trace.size(); ++i)
        peak = std::max(peak, std::abs(trace[i] - centre));
    return peak;
}

/// Per-cycle rate of an oscillating trace, from interpolated upward zero
/// crossings: `{time_seconds, hz}` for each completed cycle.
///
/// Used where the trace's frequency is SWEEPING, which is the one case
/// demodulation handles badly — a narrow enough filter to reject the image is
/// also narrow enough to reject the sweep's far end, and a wide enough one lets
/// the image back in. Zero crossings have no centre frequency to be detuned
/// from, so they track a rate from rest to full speed with the same fidelity.
///
/// Interpolating the crossing rather than taking the sample index matters: at a
/// 1 kHz trace rate a 6 Hz cycle is 167 samples, so rounding to the nearest
/// sample would quantise the measured rate by 0.6 %.
[[maybe_unused]] std::vector<std::pair<double, double>> cycle_rates(const std::vector<double>& trace,
                                                   double rate_hz) {
    std::vector<std::pair<double, double>> out;
    double previous_crossing = -1.0;
    for (std::size_t n = 1; n < trace.size(); ++n) {
        if (!(trace[n - 1] <= 0.0 && trace[n] > 0.0)) continue;
        const double frac = trace[n] != trace[n - 1]
                                ? -trace[n - 1] / (trace[n] - trace[n - 1])
                                : 0.0;
        const double t = (static_cast<double>(n - 1) + frac) / rate_hz;
        if (previous_crossing >= 0.0) out.emplace_back(t, 1.0 / (t - previous_crossing));
        previous_crossing = t;
    }
    return out;
}

// ── Fixtures ──────────────────────────────────────────────────────────────

[[maybe_unused]] Leslie make_leslie(LeslieSpeed speed = LeslieSpeed::tremolo) {
    Leslie l;
    l.prepare(kSr);
    l.set_speed(speed);
    l.reset();
    return l;
}

/// A cabinet with everything except the rotors' motion switched off, so one
/// mechanism at a time can be measured.
[[maybe_unused]] Leslie make_bare_leslie(LeslieSpeed speed = LeslieSpeed::tremolo) {
    Leslie l = make_leslie(speed);
    l.set_am_depth(0.0);
    l.set_dir_depth_db(0.0);
    l.set_drum_dir_depth_db(0.0);
    l.set_reflection_db(-60.0);
    l.reset();
    return l;
}

enum class Channel { left, right, sum };

[[maybe_unused]] std::vector<double> render_tone(Leslie& l, double hz, double amplitude, double seconds,
                                Channel channel) {
    const int n = static_cast<int>(kSr * seconds);
    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) {
        double a = 0.0;
        double b = 0.0;
        l.process(amplitude * std::sin(kTwoPi * hz * i / kSr), a, b);
        out.push_back(channel == Channel::left ? a
                                               : (channel == Channel::right ? b : 0.5 * (a + b)));
    }
    return out;
}

/// The magnitude of the cabinet's mic-sum response at one frequency, by steady
/// tone and coherent detection.
[[maybe_unused]] double response_at(Leslie& l, double hz, double seconds = 0.4) {
    const int n = static_cast<int>(kSr * seconds);
    const int skip = n / 2;
    double re = 0.0;
    double im = 0.0;
    for (int i = 0; i < n; ++i) {
        double a = 0.0;
        double b = 0.0;
        l.process(std::sin(kTwoPi * hz * i / kSr), a, b);
        if (i >= skip) {
            const double w = kTwoPi * hz * i / kSr;
            re += (a + b) * std::cos(w);
            im += (a + b) * std::sin(w);
        }
    }
    // The mic sum of a unit-amplitude tone; the extra 0.5 folds the two mics
    // back to one so an unmodulated cabinet reads 0 dB.
    return std::hypot(re, im) / static_cast<double>(n - skip);
}

/// The peak Doppler deviation the shipped geometry predicts, evaluated from the
/// module's OWN delay function rather than from a formula restated here — so
/// the expectation cannot drift from the implementation's geometry, only from
/// its physics.
[[maybe_unused]] double geometric_doppler_ratio(const Leslie& l, double rotor_hz, bool drum) {
    double peak = 0.0;
    constexpr int kSteps = 20000;
    constexpr double kDt = 1e-6;
    for (int k = 0; k < kSteps; ++k) {
        const double theta = static_cast<double>(k) / kSteps;
        const double d0 =
            drum ? l.drum_delay_seconds(theta, false) : l.horn_delay_seconds(theta, false);
        const double d1 = drum ? l.drum_delay_seconds(theta + rotor_hz * kDt, false)
                               : l.horn_delay_seconds(theta + rotor_hz * kDt, false);
        peak = std::max(peak, std::abs((d1 - d0) / kDt));
    }
    return peak;
}

}  // namespace
