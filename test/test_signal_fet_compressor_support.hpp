#pragma once

// FetCompressorT — the feedback-topology FET compressor (1176 lineage).
//
// The spec's acceptance suite, tests 4.1–4.10, plus the loop-stability cases the
// topology makes necessary. Expected values are computed from the shipped
// constants and the closed-form equations, never restated as bare literals — so
// moving a constant fails the test that documents it rather than silently
// disagreeing with it.
//
// THREE OF THE SPEC'S CRITERIA ARE NOT ACHIEVABLE BY ANY CORRECT FEEDBACK
// IMPLEMENTATION. Each is adjudicated at its test with the arithmetic that
// proves it, and each is replaced by the criterion a correct implementation
// does meet — never by a weaker one:
//
//   A-1 (test 4.1) — "measured gain reduction equals x_G(x) from §3.1". x_G is
//        the OPEN-LOOP gain computer output. A feedback detector reads the
//        output, so the measured curve is the closed-loop fixed point
//        ŷ = x̂ − D(ŷ), which differs by roughly a factor of two: at 8:1 and
//        0 dBFS in, §3.1 predicts 21.0 dB and any correct feedback loop
//        produces 11.2 dB. Replaced by the fixed point, cross-checked against
//        an independent bisection.
//   A-2 (test 4.2) — "rise time within ±5 % of 2.2·τ_A". The loop accelerates
//        the attack: the closed-loop pole is α − (1−α)·B, not α. At the 200 µs
//        default and 8:1 that is 232 µs measured against the spec's 439 µs — a
//        90 % miss, not a tolerance question. Replaced by the closed-loop
//        prediction. (Test 4.3's release criterion IS achievable and is
//        asserted exactly as the spec states it — the asymmetry is real and is
//        itself asserted.)
//   A-3 (test 4.4a) — "ABI reduces by at least bias_shift_db − 0.3 dB". The
//        loop divides the bias shift by 1 + B: 1.5 dB of detector bias produces
//        0.77 dB of extra reduction, not ≥ 1.2 dB. Replaced by the closed-form
//        loop-attenuated value.
//
// Several cases measure the DETECTOR (`gain_reduction_db()`) rather than
// inferring it from the audio. That is deliberate: inferring gain reduction
// from output/input divides by an input that crosses zero every half cycle,
// which turns a clean measurement into a noisy one and hides exactly the
// step-response detail tests 4.2 and 4.3 exist to check. Where the spec asks
// for a level measurement, test 4.1 does that too, with the specified FFT.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/fet_compressor.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/units.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

using Comp = FetCompressorT<double>;
constexpr double kSr = 48000.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;

template <typename Fn>
void require_allocates_no_memory(Fn&& fn) {
    pulp::test::RtAllocationProbe probe;
    fn();
    REQUIRE(probe.allocation_count() == 0);
}

/// A compressor at a stated operating point, colour stages neutral so the
/// quantity under test is the only thing in the path.
Comp probe(FetRatio ratio, double input_gain_db, double attack_us = 200.0,
           double release_ms = 300.0, double knee_db = 1.0) {
    Comp c;
    c.prepare(kSr);
    c.set_ratio(ratio);
    c.set_input_gain_db(input_gain_db);
    c.set_output_gain_db(0.0);
    c.set_attack_us(attack_us);
    c.set_release_ms(release_ms);
    c.set_knee_db(knee_db);
    c.set_transformer_amount(0.0);
    c.set_mix(1.0);
    c.reset();
    return c;
}

/// The spec's §3.1 static characteristic, written out independently of the
/// header so the test checks the header's arithmetic rather than calling it and
/// agreeing with itself.
double reference_static_curve(double x, double t, double r, double w) {
    const double over = x - t;
    if (2.0 * over < -w) return x;
    if (w > 0.0 && 2.0 * std::abs(over) <= w) {
        const double num = over + w * 0.5;
        return x + (1.0 / r - 1.0) * num * num / (2.0 * w);
    }
    return t + over / r;
}

/// The closed-loop fixed point `ŷ = x̂ − bias − D(ŷ)` found by BISECTION, with
/// no reference to the header's closed form. This is the independent ground
/// truth that test 4.1 checks the shipped solver against — a closed form that
/// agrees with a bisection over the whole curve is a closed form that is right.
double reference_closed_loop_output_db(double input_db, double t, double r, double w,
                                       double bias_db) {
    const auto residual = [&](double y) {
        return y - (input_db - bias_db - (y - reference_static_curve(y, t, r, w)));
    };
    double lo = input_db - 200.0;
    double hi = input_db + 1.0;
    REQUIRE(residual(lo) < 0.0);
    REQUIRE(residual(hi) > 0.0);
    for (int i = 0; i < 200; ++i) {
        const double mid = 0.5 * (lo + hi);
        (residual(mid) < 0.0 ? lo : hi) = mid;
    }
    return 0.5 * (lo + hi);
}

/// Settled detector reading, in dB, for a steady 1 kHz sine at `peak_db`.
double settled_reduction_db(Comp& c, double peak_db, double seconds = 1.5) {
    const double peak = units::db_to_linear(peak_db);
    const double w = kTwoPi * 1000.0 / kSr;
    const auto n = static_cast<int>(kSr * seconds);
    for (int i = 0; i < n; ++i) c.process(peak * std::sin(w * i));
    return c.gain_reduction_db();
}

/// In-place iterative radix-2 FFT. Present so the spec's 4096/16384/32768-point
/// measurement recipes run in milliseconds rather than minutes; it is a
/// measurement tool, not part of the module under test.
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -kTwoPi / static_cast<double>(len);
        const std::complex<double> step{std::cos(angle), std::sin(angle)};
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w{1.0, 0.0};
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

/// Hann-windowed magnitude spectrum of `signal`, normalised so a full-scale
/// sine at a bin centre reads 1.0.
std::vector<double> spectrum(const std::vector<double>& signal) {
    const std::size_t n = signal.size();
    std::vector<std::complex<double>> buffer(n);
    double window_sum = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) /
                                              static_cast<double>(n));
        window_sum += w;
        buffer[i] = signal[i] * w;
    }
    fft(buffer);
    std::vector<double> mag(n / 2);
    for (std::size_t k = 0; k < n / 2; ++k) mag[k] = 2.0 * std::abs(buffer[k]) / window_sum;
    return mag;
}

/// Renders `n` samples of a steady sine after letting the detector settle.
std::vector<double> steady_render(Comp& c, double amplitude, double freq_hz, std::size_t n,
                                  double settle_seconds = 1.5) {
    const double w = kTwoPi * freq_hz / kSr;
    const auto settle = static_cast<int>(kSr * settle_seconds);
    for (int i = 0; i < settle; ++i) c.process(amplitude * std::sin(w * i));
    std::vector<double> out(n);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = c.process(amplitude * std::sin(w * static_cast<double>(settle +
                                                                       static_cast<int>(i))));
    return out;
}

/// Peak magnitude of the bins around `freq_hz`, in dBFS. Only valid when
/// `freq_hz` lands on a bin centre — see `component_db` for the general case.
double bin_level_db(const std::vector<double>& mag, double freq_hz) {
    const auto centre =
        static_cast<std::size_t>(std::llround(freq_hz * 2.0 * static_cast<double>(mag.size()) / kSr));
    double peak = 0.0;
    for (std::size_t k = centre - 3; k <= centre + 3; ++k) peak = std::max(peak, mag[k]);
    return units::linear_to_db(peak);
}

/// Amplitude of the component at EXACTLY `freq_hz`, in dBFS, by a
/// Hann-windowed DFT evaluated at that frequency rather than at the nearest FFT
/// bin.
///
/// This exists because 1 kHz is not a bin centre at 48 kHz and any power-of-two
/// length: `1000·4096/48000 = 85.33`. Reading the nearest bin costs up to
/// 1.4 dB of Hann scalloping loss — measured at 0.6 dB here — which would be
/// charged to the compressor rather than to the measurement. Evaluating the
/// DFT at the exact frequency has no scalloping at all.
double component_db(const std::vector<double>& signal, double freq_hz) {
    std::complex<double> sum{};
    double window_sum = 0.0;
    const auto n = static_cast<double>(signal.size());
    for (std::size_t i = 0; i < signal.size(); ++i) {
        const double w = 0.5 - 0.5 * std::cos(kTwoPi * static_cast<double>(i) / n);
        window_sum += w;
        sum += signal[i] * w *
               std::exp(std::complex<double>(0.0, -kTwoPi * freq_hz * static_cast<double>(i) / kSr));
    }
    return units::linear_to_db(2.0 * std::abs(sum) / window_sum);
}

/// 10–90 % rise time of the detector, in base-rate samples, on a DC level step.
/// A DC step rather than a tone: at the documented 20 µs the detector is far
/// faster than a 1 kHz cycle, so a tone would measure how a peak detector rides
/// a waveform rather than what the ballistics do.
int dc_rise_samples(Comp& c, double from_db, double to_db) {
    const double lo = units::db_to_linear(from_db);
    const double hi = units::db_to_linear(to_db);
    for (int i = 0; i < static_cast<int>(kSr * 0.5); ++i) c.process(lo);
    const double start = c.gain_reduction_db();

    std::vector<double> trace;
    trace.reserve(static_cast<std::size_t>(kSr * 0.1));
    for (int i = 0; i < static_cast<int>(kSr * 0.1); ++i) {
        c.process(hi);
        trace.push_back(c.gain_reduction_db());
    }
    const double finish = trace.back();
    const double low = start + 0.1 * (finish - start);
    const double high = start + 0.9 * (finish - start);
    int at_low = -1;
    for (std::size_t i = 0; i < trace.size(); ++i) {
        if (at_low < 0 && trace[i] >= low) at_low = static_cast<int>(i);
        if (at_low >= 0 && trace[i] >= high) return static_cast<int>(i) - at_low;
    }
    return -1;
}

/// The closed-loop pole `p = α − (1−α)·B`, from the SHIPPED coefficient and
/// slope. The whole stability argument is this one line.
double closed_loop_pole(const Comp& c, bool attacking) {
    const double a = attacking ? c.attack_coefficient() : c.release_coefficient();
    return a - (1.0 - a) * c.loop_slope();
}

/// The time constant that pole implies, in seconds.
double closed_loop_tau(const Comp& c, bool attacking) {
    return -1.0 / (c.oversampled_rate() * std::log(std::abs(closed_loop_pole(c, attacking))));
}

/// Total harmonic distortion, harmonics 2..10 over the fundamental.
double thd(const std::vector<double>& rendered, double fundamental_hz) {
    const double fundamental = std::pow(10.0, component_db(rendered, fundamental_hz) / 20.0);
    double sum = 0.0;
    for (int k = 2; k <= 10; ++k) {
        const double f = k * fundamental_hz;
        if (f >= kSr * 0.5) break;
        const double h = std::pow(10.0, component_db(rendered, f) / 20.0);
        sum += h * h;
    }
    return std::sqrt(sum) / fundamental;
}

}  // namespace

// ── The divider law, before anything is wired to it ───────────────────────



// ── Loop stability: the reason the control law is what it is ──────────────




// ── 4.1 Static gain-reduction curve ───────────────────────────────────────






// ── 4.2 Attack time ───────────────────────────────────────────────────────




// ── 4.3 Release time ──────────────────────────────────────────────────────


// ── 4.4 All-buttons-in deviation ──────────────────────────────────────────





// ── 4.5 Worst-case gain invariant ─────────────────────────────────────────





// ── 4.6 Coloration THD ────────────────────────────────────────────────────



// ── 4.7 Aliasing ──────────────────────────────────────────────────────────



// ── 4.8 Determinism ───────────────────────────────────────────────────────



// ── 4.10 Latency ──────────────────────────────────────────────────────────





// ── The transformer colour stage ──────────────────────────────────────────


// ── Input gain is the only lever, by construction ─────────────────────────



// ── float / double parity ─────────────────────────────────────────────────


// ── 4.9 RT allocation probe ───────────────────────────────────────────────
