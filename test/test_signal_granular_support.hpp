#pragma once

// test_signal_granular.cpp — acceptance suite for the granular engine.
//
// Two things in this module are easy to assert and hard to actually test, and
// the suite is organised around both.
//
// The first is determinism, and it has two independent halves that are easy to
// conflate.
//
// Block-size independence is a property of the SCHEDULER: onsets are driven by
// an absolute sample clock, so where the block boundaries fall cannot matter.
// It is worth testing, but it does not test the draw mechanism at all — a
// sequential generator advanced once per spawn passes every block-size check in
// this file, which was verified by building exactly that and watching the suite
// stay green.
//
// What the stateless keyed draw actually buys is that a grain's parameters
// depend on its own index and on nothing else — not on which pool slot it
// landed in, not on how big the pool is, and not on whether earlier grains were
// dropped. A sequential generator fails that the moment a grain is dropped,
// because the draws it would have consumed never happen and every later grain
// shifts. That is the discriminating test, and it is "Grain draws depend only
// on the grain index" below.
//
// The second is the √N energy law. Its premise is that grains are UNCORRELATED,
// and grains are only uncorrelated if something separates the source positions
// they read. With the playhead at realtime, unity ratio, and no spray, every
// live grain reads the same source sample at the same instant: they are
// perfectly coherent, and the incoherent law is then the wrong law by 8.24 dB.
// The suite asserts the invariance the law promises AND the +3 dB failure of
// the configuration that omits spray, so the reason the one is configured the
// way it is stays recorded next to it.
//
// Expected values are computed from shipped constants throughout: window mean
// and RMS come off the engine's own table, the per-grain gain comes from
// `grain_gain()`, and the worst-case bound comes from a scan of the shipped
// interpolator.

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/fft.hpp>
#include <pulp/signal/granular.hpp>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <utility>
#include <vector>

using Catch::Approx;
using namespace pulp::signal;

namespace {

// ── Measurement-recipe constants (acceptance class) ───────────────────────
constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kFs = 48000.0;

/// Welch segment length for the pitch measurements, and the render length fed
/// to it. A single periodogram of a random-phase grain cloud is a poor estimate
/// of where its energy sits; averaging over the ~45 half-overlapping segments
/// an 8 s render provides recovers the expected spectrum, which is the thing
/// the transposition claim is actually about.
constexpr int kSpectrumSize = 16384;
constexpr int kSpectrumRender = 384000;  // 8 s

/// Position spray used by every measurement that needs grains to be mutually
/// uncorrelated. 30 ms at a 400 Hz density separates grain source positions by
/// far more than the correlation length of the material under test.
constexpr double kDecorrelationSprayMs = 30.0;

std::vector<double> sine_buffer(int count, double hz) {
    std::vector<double> out(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        out[static_cast<std::size_t>(i)] = std::sin(kTwoPi * hz * static_cast<double>(i) / kFs);
    }
    return out;
}

std::vector<double> noise_buffer(int count, std::uint32_t seed) {
    Xorshift32 rng(seed);
    std::vector<double> out(static_cast<std::size_t>(count));
    for (auto& v : out) v = rng.next_bipolar<double>();
    return out;
}

double rms(const std::vector<double>& v, int from = 0) {
    double sum = 0.0;
    for (std::size_t i = static_cast<std::size_t>(from); i < v.size(); ++i) sum += v[i] * v[i];
    return std::sqrt(sum / static_cast<double>(v.size() - static_cast<std::size_t>(from)));
}

/// Welch-averaged power spectrum.
///
/// Averaging is load-bearing, and so is NOT reading the answer off an argmax.
/// A grain cloud's spectrum is the source line convolved with the grain
/// window's main lobe — 80 Hz wide for a 50 ms Hann grain — and inside that
/// lobe neighbouring bins are near-tied, so the largest-bin index flips by
/// several bins under a last-bit change in the arithmetic. Band power and a
/// band centroid are stable where the argmax is not; an earlier version of this
/// suite believed an argmax and drew a confident, wrong conclusion from it.
std::vector<double> welch_power(const std::vector<double>& x, int nfft = kSpectrumSize) {
    FftT<double> fft(nfft);
    std::vector<double> window(static_cast<std::size_t>(nfft));
    for (int i = 0; i < nfft; ++i) {
        window[static_cast<std::size_t>(i)] =
            0.5 * (1.0 - std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(nfft)));
    }
    std::vector<double> segment(static_cast<std::size_t>(nfft));
    std::vector<std::complex<double>> spectrum(static_cast<std::size_t>(nfft / 2 + 1));
    std::vector<double> power(static_cast<std::size_t>(nfft / 2 + 1), 0.0);

    for (int offset = 0; offset + nfft <= static_cast<int>(x.size()); offset += nfft / 2) {
        for (int i = 0; i < nfft; ++i) {
            segment[static_cast<std::size_t>(i)] =
                x[static_cast<std::size_t>(offset + i)] * window[static_cast<std::size_t>(i)];
        }
        fft.forward_real(segment.data(), spectrum.data());
        for (int k = 0; k <= nfft / 2; ++k) {
            power[static_cast<std::size_t>(k)] += std::norm(spectrum[static_cast<std::size_t>(k)]);
        }
    }
    return power;
}

double bin_hz(int k, int nfft = kSpectrumSize) {
    return static_cast<double>(k) * kFs / static_cast<double>(nfft);
}

/// Power in [lo, hi] as a fraction of the power over the whole audio band.
double band_fraction(const std::vector<double>& power, double lo, double hi,
                     int nfft = kSpectrumSize) {
    double inside = 0.0;
    double total = 0.0;
    for (int k = 1; k < static_cast<int>(power.size()); ++k) {
        const double hz = bin_hz(k, nfft);
        if (hz < 20.0 || hz > 20000.0) continue;
        total += power[static_cast<std::size_t>(k)];
        if (hz >= lo && hz <= hi) inside += power[static_cast<std::size_t>(k)];
    }
    return total > 0.0 ? inside / total : 0.0;
}

/// Power-weighted centre of a band — stable to a tenth of a percent where the
/// largest-bin index is not stable at all.
double band_centroid(const std::vector<double>& power, double lo, double hi,
                     int nfft = kSpectrumSize) {
    double weighted = 0.0;
    double total = 0.0;
    for (int k = 1; k < static_cast<int>(power.size()); ++k) {
        const double hz = bin_hz(k, nfft);
        if (hz < lo || hz > hi) continue;
        weighted += power[static_cast<std::size_t>(k)] * hz;
        total += power[static_cast<std::size_t>(k)];
    }
    return total > 0.0 ? weighted / total : 0.0;
}

/// Sets up a granulator on a borrowed buffer with everything the pitch and
/// level measurements need in common.
void configure_buffer_engine(GranularEngine64& engine, const std::vector<double>& source) {
    engine.prepare(kFs);
    engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
    engine.set_window_taper(1.0);
    engine.set_grain_ms(50.0);
    engine.set_density_hz(400.0);
    engine.set_position_spray_ms(kDecorrelationSprayMs);
    engine.reset();
}

struct Stereo {
    std::vector<double> left;
    std::vector<double> right;
};

Stereo render(GranularEngine64& engine, int n, int block = 0) {
    Stereo out{std::vector<double>(static_cast<std::size_t>(n)),
               std::vector<double>(static_cast<std::size_t>(n))};
    if (block <= 0) {
        engine.process(out.left.data(), out.right.data(), n);
        return out;
    }
    for (int i = 0; i < n; i += block) {
        const int count = std::min(block, n - i);
        engine.process(out.left.data() + i, out.right.data() + i, count);
    }
    return out;
}

/// The four Catmull-Rom basis weights the shipped interpolator produces at a
/// fractional offset. Used to derive the worst-case sample gain and the
/// variance a fractional read of white noise keeps — both from shipped code
/// rather than from restated coefficients.
void hermite_basis(double t, double (&h)[4]) {
    h[0] = Interpolator::hermite(t, 1.0, 0.0, 0.0, 0.0);
    h[1] = Interpolator::hermite(t, 0.0, 1.0, 0.0, 0.0);
    h[2] = Interpolator::hermite(t, 0.0, 0.0, 1.0, 0.0);
    h[3] = Interpolator::hermite(t, 0.0, 0.0, 0.0, 1.0);
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// Ground truth, before anything is measured against it.
// ─────────────────────────────────────────────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────
// T-2 — window fidelity, in the table and in the audio.
// ─────────────────────────────────────────────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────
// T-1 — the scheduler.
// ─────────────────────────────────────────────────────────────────────────

namespace {

/// Onset sample indices over a render, recovered by stepping one sample at a
/// time and watching the monotonic grain counter.
std::vector<int> onset_indices(GranularEngine64& engine, int samples) {
    std::vector<int> onsets;
    std::uint64_t previous = engine.grain_index();
    double left = 0.0;
    double right = 0.0;
    for (int n = 0; n < samples; ++n) {
        engine.process(&left, &right, 1);
        if (engine.grain_index() != previous) {
            onsets.push_back(n);
            previous = engine.grain_index();
        }
    }
    return onsets;
}

double interval_cv(const std::vector<int>& onsets) {
    if (onsets.size() < 3) return 0.0;
    std::vector<double> intervals;
    for (std::size_t i = 1; i < onsets.size(); ++i) {
        intervals.push_back(static_cast<double>(onsets[i] - onsets[i - 1]));
    }
    double mean = 0.0;
    for (double v : intervals) mean += v;
    mean /= static_cast<double>(intervals.size());
    double variance = 0.0;
    for (double v : intervals) variance += (v - mean) * (v - mean);
    variance /= static_cast<double>(intervals.size());
    return std::sqrt(variance) / mean;
}

}  // namespace



// ─────────────────────────────────────────────────────────────────────────
// T-3 / T-4 — pitch and time, decoupled.
// ─────────────────────────────────────────────────────────────────────────




// ─────────────────────────────────────────────────────────────────────────
// T-5 — determinism, including the part a sequential RNG would fail.
// ─────────────────────────────────────────────────────────────────────────

namespace {

void configure_stress(GranularEngine64& engine, const std::vector<double>& source,
                      GrainSource mode) {
    engine.prepare(kFs);
    engine.set_source(mode);
    engine.set_buffer(source.data(), static_cast<int>(source.size()), 1);
    engine.set_density_hz(600.0);
    engine.set_grain_ms(60.0);
    engine.set_async_jitter(0.7);
    engine.set_position_spray_ms(120.0);
    engine.set_pitch_spray_semitones(5.0);
    engine.set_pan_spray(1.0);
    engine.set_max_grains(32);  // guarantees stealing at this density
    engine.reset();
}

}  // namespace


namespace {

/// Steps an engine one sample at a time and records, for every grain that is
/// actually spawned, the parameters it was born with, keyed by its monotonic
/// grain index.
///
/// A spawn is observable from outside: the grain counter advances by one (never
/// more, because the inter-onset interval has a one-sample floor), and the slot
/// that received the grain either turned active or had its phase reset. When
/// the counter advances and no slot changes, the grain was dropped.
std::vector<std::pair<std::uint64_t, double>> spawned_ratios(GranularEngine64& engine,
                                                             int samples) {
    std::vector<std::pair<std::uint64_t, double>> spawns;
    const int slots = engine.max_grains();
    std::vector<char> was_active(static_cast<std::size_t>(slots), 0);
    std::vector<double> was_phase(static_cast<std::size_t>(slots), 0.0);
    double left = 0.0;
    double right = 0.0;

    for (int n = 0; n < samples; ++n) {
        for (int s = 0; s < slots; ++s) {
            const auto view = engine.grain(s);
            was_active[static_cast<std::size_t>(s)] = view.active ? 1 : 0;
            was_phase[static_cast<std::size_t>(s)] = view.phase;
        }
        const std::uint64_t before = engine.grain_index();
        engine.process(&left, &right, 1);
        const std::uint64_t after = engine.grain_index();
        if (after == before) continue;

        for (int s = 0; s < slots; ++s) {
            const auto view = engine.grain(s);
            const bool fresh = view.active && !was_active[static_cast<std::size_t>(s)];
            const bool reused = view.active && was_active[static_cast<std::size_t>(s)] &&
                                view.phase < was_phase[static_cast<std::size_t>(s)];
            if (fresh || reused) {
                spawns.emplace_back(after - 1, view.ratio);
                break;
            }
        }
    }
    return spawns;
}

}  // namespace


// ─────────────────────────────────────────────────────────────────────────
// T-6 — the √N energy law.
// ─────────────────────────────────────────────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────
// T-7 — voice budget and steal.
// ─────────────────────────────────────────────────────────────────────────

namespace {

struct StealReport {
    int steals = 0;
    double worst_window = 0.0;
    bool always_picked_oldest = true;
    int max_active = 0;
};

/// Steps one sample at a time, snapshotting phases, so a steal can be observed
/// from outside: the reused slot is the one whose phase went backwards.
StealReport observe_steals(GranularEngine64& engine, int samples, bool expect_oldest) {
    StealReport report;
    const int slots = engine.max_grains();
    std::vector<double> phases(static_cast<std::size_t>(slots), -1.0);
    std::uint64_t previous_steals = engine.steal_count();
    double left = 0.0;
    double right = 0.0;

    for (int n = 0; n < samples; ++n) {
        double highest = -1.0;
        for (int s = 0; s < slots; ++s) {
            const auto view = engine.grain(s);
            phases[static_cast<std::size_t>(s)] = view.active ? view.phase : -1.0;
            highest = std::max(highest, phases[static_cast<std::size_t>(s)]);
        }
        engine.process(&left, &right, 1);
        report.max_active = std::max(report.max_active, engine.active_grain_count());

        if (engine.steal_count() != previous_steals) {
            previous_steals = engine.steal_count();
            for (int s = 0; s < slots; ++s) {
                const double before = phases[static_cast<std::size_t>(s)];
                if (before >= 0.0 && engine.grain(s).phase < before) {
                    ++report.steals;
                    report.worst_window = std::max(report.worst_window, engine.window_at(before));
                    if (expect_oldest && before < highest - 1e-12) {
                        report.always_picked_oldest = false;
                    }
                    break;
                }
            }
        }
    }
    return report;
}

}  // namespace



// ─────────────────────────────────────────────────────────────────────────
// T-8 — headroom.
// ─────────────────────────────────────────────────────────────────────────



// ─────────────────────────────────────────────────────────────────────────
// T-9 / T-10 — latency and live causality.
// ─────────────────────────────────────────────────────────────────────────






// ─────────────────────────────────────────────────────────────────────────
// T-11 and parameter hygiene.
// ─────────────────────────────────────────────────────────────────────────
