// CyclicStretchT — the fixed-cycle splice stretcher's acceptance suite
// (spec §7.1–§7.7a).
//
// This module is unusual in the series: its ARTEFACTS ARE THE PRODUCT, so this
// suite characterises them rather than bounding them. There is no "distortion
// stays below" test here and there should not be. What is asserted is that the
// splice period is exactly what `cycle_hz` and the overlap say it is, that the
// duration scaling is exactly the ratio, that transients really are duplicated
// and dropped, and that the two documented regimes are measurably different
// rather than two names for one sound.
//
// Expected values are COMPUTED from the shipped constants — every frequency
// comes from the module's own resolved `L/N/X/S`, every repeat and skip count
// from the closed forms evaluated in-test, and the √2 bound from the algebra
// rather than from a previous run.
//
// ── Measurement recipe ────────────────────────────────────────────────────
//
// Spectra: 65536-point Hann-windowed FFT at 48 kHz, so the bin width is
// 0.7324 Hz and "within one bin" is a ±0.73 Hz gate. The analysis window starts
// well past the initial fill. Peak location searches ±2 bins around a predicted
// frequency; the local noise floor is the mean magnitude over a band offset
// 0.35–0.65 of a flutter interval away, which keeps the floor estimate out of
// both the sideband being measured and its neighbours.
//
// Schedules: asserted on `schedule_input_position`, the pure closed form, NOT
// on a render. The bounded-ring fold deliberately perturbs the live cursor, so
// a render measures the schedule and the fold at once; the two are tested
// separately and then their composition is tested where it matters.
//
// ── Four spec criteria this suite corrects, with the numbers ─────────────
//
// **§7.1's stimulus is degenerate, and its first assertion is false for it.**
// The spec asks for a peak at `f₀ = 1000 Hz` with `cycle_hz = 200`, plus
// sidebands at `f₀ ± k·f_flutter`. But 1000 Hz is the fifth harmonic of 200 Hz,
// so `L = 240` samples is exactly five tone periods: the snap is phase-coherent,
// every splice advances the tone's phase by the SAME amount, and the result is a
// coherent carrier displaced from `f₀`, not a carrier at `f₀`. Measured: energy
// at 1000 Hz sits **157 dB below** the carrier, which is at
// **1090.909 Hz = f₀ + 0.25·f_flutter**. The 0.25 is not a fudge — it is
// `frac(−f₀·S/fs)`, the per-splice phase step in turns, and this suite computes
// it from the shipped constants and asserts the carrier lands there. The
// SPACING claim, which is the point of the test, is exactly right: the line
// spacing is `f_flutter` to within a bin in both regimes.
//
// **§7.3's "separated by exactly S" holds only in the repeat regime.** A source
// sample carried by two consecutive grains lands `S − Δin` apart, where `Δin`
// is how far the schedule advanced between them — a whole number of cycles.
// `Δin` is zero only when `S ≤ L·r`. The long-frame regime the criterion names
// is not in that regime at `r = 2` (`S = 1766` against `L·r = 480`), and
// measures 1046 = `S − 3L`. Both forms are asserted, each where it applies.
//
// **§7.4's stimulus never reaches the bound it is testing.** It asks for a sine
// at `f₀ = cycle_hz` "so repeats are exactly in phase". What has to be in phase
// for the splice to sum coherently is the pair of samples separated by the HOP,
// not by the cycle: measured peak 0.5685 against a bound of 1.4142. DC attains
// the bound exactly (1.414214) and a tone at `f_flutter` all but exactly
// (1.412868), so both are used, plus a 600-combination sweep.
//
// **§3.5's fold is stated two ways that behave differently.** Its prose says
// long stretches "loop within the trailing window"; its formula clamps to the
// window edge. A clamp does not loop — it latches, and the stretch silently
// becomes 1×. The loop is implemented and asserted here as a sawtooth.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/cyclic_stretch.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFftSize = 65536;
constexpr double kBinHz = kSr / static_cast<double>(kFftSize);
constexpr double kProbeHz = 1000.0;

/// Renders `n` samples of `in` in one call.
std::vector<float> render(CyclicStretch& stretch, const std::vector<float>& in) {
    std::vector<float> out(in.size(), 0.0f);
    stretch.process(in.data(), out.data(), static_cast<int>(in.size()));
    return out;
}

std::vector<float> tone(double hz, double amplitude, int n) {
    std::vector<float> v(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k)
        v[static_cast<std::size_t>(k)] =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * k / kSr));
    return v;
}

/// Deterministic pseudo-noise for the render-equality tests. Not part of the
/// module — the module has no RNG — so it lives here where its seed is visible.
std::vector<float> noise(int n, std::uint32_t seed = 12345u) {
    std::vector<float> v(static_cast<std::size_t>(n));
    std::uint32_t state = seed;
    for (int k = 0; k < n; ++k) {
        state = state * 1664525u + 1013904223u;
        v[static_cast<std::size_t>(k)] =
            static_cast<float>((state >> 8) / 8388608.0 - 1.0);
    }
    return v;
}

/// Hann-windowed magnitude spectrum of `x` starting at `skip`.
std::vector<double> spectrum_of(const std::vector<float>& x, int skip) {
    REQUIRE(static_cast<int>(x.size()) >= skip + kFftSize);
    std::vector<std::complex<double>> buffer(static_cast<std::size_t>(kFftSize));
    for (int k = 0; k < kFftSize; ++k) {
        const double window =
            0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * k / static_cast<double>(kFftSize - 1));
        buffer[static_cast<std::size_t>(k)] = {
            static_cast<double>(x[static_cast<std::size_t>(skip + k)]) * window, 0.0};
    }
    FftT<double> fft(kFftSize);
    fft.forward(buffer.data());

    std::vector<double> magnitude(static_cast<std::size_t>(kFftSize / 2));
    for (std::size_t bin = 0; bin < magnitude.size(); ++bin)
        magnitude[bin] = std::abs(buffer[bin]) * 4.0 / static_cast<double>(kFftSize);
    return magnitude;
}

double magnitude_at(const std::vector<double>& spectrum, double hz) {
    const auto bin = static_cast<std::size_t>(std::llround(hz / kBinHz));
    return bin < spectrum.size() ? spectrum[bin] : 0.0;
}

/// Largest magnitude within `±2` bins of `hz`, and where it was, in Hz.
std::pair<double, double> peak_near(const std::vector<double>& spectrum, double hz) {
    const auto centre = static_cast<long long>(std::llround(hz / kBinHz));
    double best = -1.0;
    double best_hz = hz;
    for (long long bin = centre - 2; bin <= centre + 2; ++bin) {
        if (bin < 0 || bin >= static_cast<long long>(spectrum.size())) continue;
        const double m = spectrum[static_cast<std::size_t>(bin)];
        if (m > best) {
            best = m;
            best_hz = static_cast<double>(bin) * kBinHz;
        }
    }
    return {best, best_hz};
}

/// Mean magnitude 0.35–0.65 of a flutter interval either side of `hz` — far
/// enough out to miss this sideband's skirt, near enough to miss the next one.
double local_floor(const std::vector<double>& spectrum, double hz, double flutter_hz) {
    double total = 0.0;
    int counted = 0;
    for (double offset = 0.35 * flutter_hz; offset <= 0.65 * flutter_hz;
         offset += flutter_hz / 24.0) {
        for (const int sign : {-1, 1}) {
            const double f = hz + sign * offset;
            if (f < 40.0 || f > 0.45 * kSr) continue;
            total += magnitude_at(spectrum, f);
            ++counted;
        }
    }
    return total / std::max(counted, 1);
}

/// The per-splice phase step, in turns, that a tone at `hz` picks up when the
/// schedule HOLDS — the closed form behind the carrier displacement.
double splice_phase_turns(double hz, long long hop) {
    double turns = std::fmod(hz * static_cast<double>(-hop) / kSr, 1.0);
    if (turns < 0.0) turns += 1.0;
    return turns;
}

/// Is `f0` commensurate with the cycle grid — is `L` a whole number of tone
/// periods? When it is, holding and advancing produce the SAME phase step and
/// the spectrum collapses to one displaced carrier plus its sidebands.
bool commensurate(double hz, long long cycle) {
    const double periods = hz * static_cast<double>(cycle) / kSr;
    return std::abs(periods - std::round(periods)) < 1e-9;
}

CyclicStretch make(const CyclicStretchRegime& regime, double ratio) {
    CyclicStretch stretch;
    stretch.prepare(kSr);
    stretch.set_regime(regime);
    stretch.set_stretch_ratio(ratio);
    return stretch;
}

double peak_of(const std::vector<float>& x, int skip) {
    double best = 0.0;
    for (std::size_t k = static_cast<std::size_t>(skip); k < x.size(); ++k)
        best = std::max(best, std::abs(static_cast<double>(x[k])));
    return best;
}

}  // namespace

// ── Resolved lengths ──────────────────────────────────────────────────────

TEST_CASE("Cyclic stretch: the resolved lengths are the documented ones",
          "[signal][cyclic-stretch][lengths]") {
    // Both regimes' worked figures, recomputed from `fs` and the shipped regime
    // constants rather than restated. If a regime constant moves, this fails
    // instead of the sound quietly changing.
    {
        CyclicStretch stretch = make(kCyclicStretchShortFrame, 1.0);
        const auto cycle = static_cast<long long>(
            std::llround(kSr / kCyclicStretchShortFrame.cycle_hz));
        const long long grain = cycle * kCyclicStretchShortFrame.grain_periods;
        const long long overlap =
            std::llround(kCyclicStretchShortFrame.crossfade_pct * 0.01 *
                         static_cast<double>(grain));
        REQUIRE(stretch.cycle_samples() == cycle);
        REQUIRE(stretch.grain_samples() == grain);
        REQUIRE(stretch.crossfade_samples() == overlap);
        REQUIRE(stretch.hop_samples() == grain - overlap);
        REQUIRE_THAT(stretch.flutter_hz(),
                     WithinAbs(kSr / static_cast<double>(grain - overlap), 1e-9));
    }
    {
        CyclicStretch stretch = make(kCyclicStretchLongFrame, 1.0);
        const auto cycle = static_cast<long long>(
            std::llround(kSr / kCyclicStretchLongFrame.cycle_hz));
        const long long grain = cycle * kCyclicStretchLongFrame.grain_periods;
        const long long overlap =
            std::llround(kCyclicStretchLongFrame.crossfade_pct * 0.01 *
                         static_cast<double>(grain));
        REQUIRE(stretch.grain_samples() == grain);
        REQUIRE(stretch.crossfade_samples() == overlap);
        REQUIRE(stretch.hop_samples() == grain - overlap);
    }

    // The two regimes are genuinely different, not two names for one sound: an
    // order of magnitude between their flutter fundamentals is what separates
    // "metallic sheen above the pitch" from "audible robotic buzz".
    const CyclicStretch shortf = make(kCyclicStretchShortFrame, 1.0);
    const CyclicStretch longf = make(kCyclicStretchLongFrame, 1.0);
    REQUIRE(shortf.flutter_hz() > 10.0 * longf.flutter_hz());
}

TEST_CASE("Cyclic stretch: the clamp floors hold at every extreme",
          "[signal][cyclic-stretch][lengths]") {
    CyclicStretch stretch;
    stretch.prepare(kSr);

    // `X` stays inside [1, N/2] so the overlap-add never goes three-deep —
    // which is what both the unity interior and the √2 bound rest on.
    for (const double hz : {CyclicStretch::kCycleHzMin, 200.0, 999.0,
                            CyclicStretch::kCycleHzMax}) {
        for (const int periods : {1, 2, 7, CyclicStretch::kGrainPeriodsMax}) {
            for (const double pct : {CyclicStretch::kCrossfadePctMin, 8.0, 25.0,
                                     CyclicStretch::kCrossfadePctMax}) {
                stretch.set_cycle_hz(hz);
                stretch.set_grain_periods(periods);
                stretch.set_crossfade_pct(pct);
                REQUIRE(stretch.cycle_samples() >= CyclicStretch::kMinCycleSamples);
                REQUIRE(stretch.crossfade_samples() >= 1);
                REQUIRE(stretch.crossfade_samples() * 2 <= stretch.grain_samples());
                REQUIRE(stretch.hop_samples() >= CyclicStretch::kMinHopSamples);
                REQUIRE(stretch.hop_samples() ==
                        stretch.grain_samples() - stretch.crossfade_samples());
            }
        }
    }

    // Out-of-range requests clamp rather than misbehave.
    stretch.set_cycle_hz(1e9);
    REQUIRE(stretch.cycle_samples() >= CyclicStretch::kMinCycleSamples);
    stretch.set_stretch_ratio(1e9);
    REQUIRE_THAT(stretch.stretch_ratio(),
                 WithinAbs(CyclicStretch::kStretchRatioMax, 1e-12));
    stretch.set_stretch_ratio(-5.0);
    REQUIRE_THAT(stretch.stretch_ratio(),
                 WithinAbs(CyclicStretch::kStretchRatioMin, 1e-12));
}

// ── §7.1 ──────────────────────────────────────────────────────────────────

TEST_CASE("Cyclic stretch: the splice spectrum is spaced at exactly fs/S",
          "[signal][cyclic-stretch][flutter]") {
    // The headline criterion, and the one that guards against shipping a
    // clean, well-integrated module that does not actually flutter.
    for (const auto& regime : {kCyclicStretchShortFrame, kCyclicStretchLongFrame}) {
        CyclicStretch stretch = make(regime, 2.0);
        const long long hop = stretch.hop_samples();
        const double flutter = stretch.flutter_hz();
        REQUIRE_THAT(flutter, WithinAbs(kSr / static_cast<double>(hop), 1e-12));

        const int skip = 60000;
        const auto out = render(stretch, tone(kProbeHz, 0.5, skip + kFftSize + 1024));
        const auto spectrum = spectrum_of(out, skip);

        // The carrier is displaced from f0 by the per-splice phase step. Both
        // the displacement and the fact that it is a SINGLE coherent carrier
        // follow from f0 being commensurate with the cycle grid.
        REQUIRE(commensurate(kProbeHz, stretch.cycle_samples()));
        const double turns = splice_phase_turns(kProbeHz, hop);
        const double carrier_hz = kProbeHz + turns * flutter;

        const auto [carrier_mag, carrier_at] = peak_near(spectrum, carrier_hz);
        REQUIRE_THAT(carrier_at, WithinAbs(carrier_hz, kBinHz));
        REQUIRE(carrier_mag > 0.1);

        // ...and the spec's own first assertion — a peak AT f0 — is false for
        // this stimulus. Asserted in the direction it is actually true, because
        // a suite that quietly skipped it would leave the next reader to
        // rediscover why.
        REQUIRE(magnitude_at(spectrum, kProbeHz) < carrier_mag * 1e-3);

        // The sidebands, at exactly one flutter interval apart, each well clear
        // of the local floor. This is the assertion that the splice period is
        // what `S` says it is.
        for (const int k : {-2, -1, 1, 2}) {
            const double predicted = carrier_hz + k * flutter;
            if (predicted < 60.0 || predicted > 0.4 * kSr) continue;
            const auto [mag, at] = peak_near(spectrum, predicted);
            REQUIRE_THAT(at, WithinAbs(predicted, kBinHz));
            const double floor = local_floor(spectrum, predicted, flutter);
            REQUIRE(20.0 * std::log10(mag / std::max(floor, 1e-18)) >= 20.0);
        }
    }
}

TEST_CASE("Cyclic stretch: identity ratio still colours",
          "[signal][cyclic-stretch][flutter]") {
    // `stretch_ratio = 1` is not a bypass — the hardware's 100 % setting was
    // never transparent and neither is this. A true bypass is `mix = 0`, which
    // the mix test covers.
    CyclicStretch stretch = make(kCyclicStretchShortFrame, 1.0);
    const double flutter = stretch.flutter_hz();
    const int skip = 40000;
    const auto out = render(stretch, tone(kProbeHz, 0.5, skip + kFftSize + 1024));
    const auto spectrum = spectrum_of(out, skip);

    const double carrier_hz =
        kProbeHz + splice_phase_turns(kProbeHz, stretch.hop_samples()) * flutter;
    const auto [carrier_mag, carrier_at] = peak_near(spectrum, carrier_hz);
    REQUIRE_THAT(carrier_at, WithinAbs(carrier_hz, kBinHz));

    bool found_sideband = false;
    for (const int k : {-1, 1}) {
        const double predicted = carrier_hz + k * flutter;
        const auto [mag, at] = peak_near(spectrum, predicted);
        if (20.0 * std::log10(mag / std::max(local_floor(spectrum, predicted, flutter),
                                             1e-18)) >= 20.0) {
            REQUIRE_THAT(at, WithinAbs(predicted, kBinHz));
            found_sideband = true;
        }
    }
    REQUIRE(found_sideband);
}

// ── §7.2 ──────────────────────────────────────────────────────────────────

TEST_CASE("Cyclic stretch: repeat and skip counts match the closed forms",
          "[signal][cyclic-stretch][schedule]") {
    // Asserted on the pure schedule, so the bounded-ring fold cannot launder a
    // wrong snap into a right-looking render. The closed forms are
    // `1 − S/(L·r)` for held grains and `1 − L·r/S` for omitted cells; both are
    // evaluated in-test from the module's own resolved lengths.
    constexpr int kGrains = 4000;
    for (const double ratio : {0.5, 1.0, 2.0, 4.0}) {
        for (const double pct : {8.0, 25.0, 45.0}) {
            CyclicStretch stretch;
            stretch.prepare(kSr);
            stretch.set_cycle_hz(200.0);
            stretch.set_grain_periods(1);
            stretch.set_crossfade_pct(pct);
            stretch.set_stretch_ratio(ratio);

            const auto cycle = static_cast<double>(stretch.cycle_samples());
            const auto hop = static_cast<double>(stretch.hop_samples());

            int repeats = 0;
            std::vector<long long> cells;
            long long previous = stretch.grain_input_position(0);
            cells.push_back(previous / stretch.cycle_samples());
            for (int g = 1; g < kGrains; ++g) {
                const long long position = stretch.grain_input_position(g);
                if (position == previous) ++repeats;
                cells.push_back(position / stretch.cycle_samples());
                previous = position;
                // Every scheduled position is on the cycle grid — the invariant
                // the whole algorithm is named for.
                REQUIRE(position % stretch.cycle_samples() == 0);
            }

            const double measured_repeat =
                static_cast<double>(repeats) / static_cast<double>(kGrains - 1);
            const double expected_repeat = std::max(0.0, 1.0 - hop / (cycle * ratio));
            REQUIRE_THAT(measured_repeat, WithinAbs(expected_repeat, 0.01));

            const long long span = cells.back() - cells.front() + 1;
            std::vector<long long> unique = cells;
            std::sort(unique.begin(), unique.end());
            unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
            const double measured_skip =
                span > 0 ? 1.0 - static_cast<double>(unique.size()) /
                                     static_cast<double>(span)
                         : 0.0;
            const double expected_skip = std::max(0.0, 1.0 - (cycle * ratio) / hop);
            REQUIRE_THAT(measured_skip, WithinAbs(expected_skip, 0.01));
        }
    }
}

TEST_CASE("Cyclic stretch: the duration scaling is the ratio, on average",
          "[signal][cyclic-stretch][schedule]") {
    // Locally the input advance is quantised to whole cycles; averaged over
    // many grains it must be exactly `S/r`, or the stretch is not a stretch.
    for (const double ratio : {0.25, 0.5, 1.0, 1.7, 2.0, 4.0}) {
        CyclicStretch stretch;
        stretch.prepare(kSr);
        stretch.set_cycle_hz(200.0);
        stretch.set_grain_periods(1);
        stretch.set_crossfade_pct(25.0);
        stretch.set_stretch_ratio(ratio);

        constexpr int kGrains = 20000;
        const double advance =
            static_cast<double>(stretch.grain_input_position(kGrains) -
                                stretch.grain_input_position(0)) /
            static_cast<double>(kGrains);
        const double expected = static_cast<double>(stretch.hop_samples()) / ratio;
        REQUIRE_THAT(advance / expected, WithinAbs(1.0, 0.01));
    }
}

// ── §7.3 ──────────────────────────────────────────────────────────────────

namespace {

/// Where a single input click reappears in the output, and how loud.
std::vector<std::pair<int, double>> click_hits(CyclicStretch& stretch, int click_at,
                                               int length, double threshold) {
    std::vector<float> in(static_cast<std::size_t>(length), 0.0f);
    in[static_cast<std::size_t>(click_at)] = 1.0f;
    const auto out = render(stretch, in);
    std::vector<std::pair<int, double>> hits;
    for (int k = 0; k < length; ++k) {
        const double v = std::abs(static_cast<double>(out[static_cast<std::size_t>(k)]));
        if (v >= threshold) hits.push_back({k, v});
    }
    return hits;
}

}  // namespace

TEST_CASE("Cyclic stretch: a transient is duplicated, never preserved",
          "[signal][cyclic-stretch][transient]") {
    // The absence assertion. A transient-preserving stretch would emit an onset
    // ONCE; this one emits it on whatever grid cells happen to carry it, and
    // this test guards that no similarity search or onset detector ever creeps
    // in. The duplicate spacing is `S − Δin` — where `Δin` is how far the
    // schedule advanced between the two grains, always a whole number of cycles
    // — and `Δin` is zero exactly in the repeat regime `S ≤ L·r`.
    for (const auto& regime : {kCyclicStretchShortFrame, kCyclicStretchLongFrame}) {
        CyclicStretch stretch = make(regime, 2.0);
        const long long cycle = stretch.cycle_samples();
        const long long hop = stretch.hop_samples();
        const int grain = stretch.grain_samples();

        const auto hits = click_hits(stretch, grain + 5, grain * 30, 1e-6);
        REQUIRE(hits.size() >= 2);  // duplicated — the whole point

        for (std::size_t i = 1; i < hits.size(); ++i) {
            const long long spacing = hits[i].first - hits[i - 1].first;
            const long long advance = hop - spacing;
            // The gap between copies differs from the hop by a whole number of
            // cycles, always. That is the cyclic invariant showing up in the
            // time domain.
            REQUIRE(advance % cycle == 0);
            REQUIRE(advance >= 0);
        }

        // In the repeat regime the advance is zero and the spacing is exactly
        // the hop — the criterion as originally stated, asserted where it holds.
        if (hop <= cycle * 2.0) {
            REQUIRE(hits[1].first - hits[0].first == hop);
        }
    }
}

TEST_CASE("Cyclic stretch: compression drops transients",
          "[signal][cyclic-stretch][transient]") {
    // The other half of "no transient detection": with the analysis advance
    // wider than a grain, whole source regions are never read, and an onset
    // inside one is simply gone. The schedule condition is exact — gaps exist
    // when `S/r > N` — so it is asserted directly first, then behaviourally.
    CyclicStretch stretch = make(kCyclicStretchLongFrame, 0.5);
    const auto grain = static_cast<double>(stretch.grain_samples());
    const double advance = static_cast<double>(stretch.hop_samples()) /
                           stretch.stretch_ratio();
    REQUIRE(advance > grain);

    int gap_grains = 0;
    for (int g = 0; g < 500; ++g) {
        const long long a = stretch.grain_input_position(g);
        const long long b = stretch.grain_input_position(g + 1);
        if (b > a + stretch.grain_samples()) ++gap_grains;
    }
    REQUIRE(gap_grains == 500);

    // Behaviourally, over a render short enough to sit inside one sweep of the
    // capture window: most placements of a click land in a gap and vanish.
    int vanished = 0;
    constexpr int kPlacements = 40;
    for (int placement = 0; placement < kPlacements; ++placement) {
        CyclicStretch probe = make(kCyclicStretchLongFrame, 0.5);
        const int n = probe.grain_samples() * 4;
        const auto hits = click_hits(probe, probe.grain_samples() + 7 + placement * 41,
                                     n, 0.05);
        if (hits.empty()) ++vanished;
    }
    REQUIRE(vanished > kPlacements / 2);
}

// ── §7.4 ──────────────────────────────────────────────────────────────────

TEST_CASE("Cyclic stretch: the splice gain bound is sqrt(2), and is attained",
          "[signal][cyclic-stretch][gain]") {
    // Series law 8. The bound is PROVEN in the header, not measured, so this
    // test does two things: shows the proof is tight (a stimulus attains it) and
    // shows it is a bound (nothing exceeds it across the parameter space).
    //
    // The attaining stimulus is DC, not the spec's `f0 = cycle_hz`. What has to
    // be in phase for a splice to sum coherently is the sample pair separated by
    // the HOP, and `cycle_hz` says nothing about the hop; measured, that
    // stimulus reaches 0.5685 against a 1.4142 bound. DC is in phase with
    // itself at every offset, so it attains the bound identically.
    {
        CyclicStretch stretch = make(kCyclicStretchShortFrame, 2.0);
        stretch.set_crossfade_shape(1.0);
        const std::vector<float> dc(30000, 1.0f);
        const auto out = render(stretch, dc);
        REQUIRE_THAT(peak_of(out, stretch.grain_samples() * 3),
                     WithinAbs(CyclicStretch::kWorstCaseGain, 1e-4));
    }
    {
        // Equal-gain cannot boost at all: `w_in + w_out = 1` identically.
        CyclicStretch stretch = make(kCyclicStretchShortFrame, 2.0);
        stretch.set_crossfade_shape(0.0);
        const std::vector<float> dc(30000, 1.0f);
        const auto out = render(stretch, dc);
        REQUIRE_THAT(peak_of(out, stretch.grain_samples() * 3), WithinAbs(1.0, 1e-4));
        REQUIRE_THAT(stretch.worst_case_gain(), WithinAbs(1.0, 1e-12));
    }
    {
        // A tone whose period is exactly the hop is the in-phase case the spec
        // was reaching for, and it very nearly attains the bound too.
        CyclicStretch stretch = make(kCyclicStretchShortFrame, 2.0);
        const auto out = render(stretch, tone(stretch.flutter_hz(), 1.0, 60000));
        const double peak = peak_of(out, stretch.grain_samples() * 3);
        REQUIRE(peak > 1.40);
        REQUIRE(peak <= CyclicStretch::kWorstCaseGain + 1e-4);
    }

    // ...and it is a bound: nothing across the parameter space exceeds it, on
    // either an adversarial constant or broadband noise.
    double worst = 0.0;
    for (const double shape : {0.0, 0.5, 1.0}) {
        for (const double pct : {1.0, 8.0, 25.0, 50.0}) {
            for (const int periods : {1, 3, 16}) {
                for (const double ratio : {0.25, 1.0, 2.0, 4.0}) {
                    for (int source = 0; source < 2; ++source) {
                        CyclicStretch stretch;
                        stretch.prepare(kSr);
                        stretch.set_cycle_hz(200.0);
                        stretch.set_grain_periods(periods);
                        stretch.set_crossfade_pct(pct);
                        stretch.set_crossfade_shape(shape);
                        stretch.set_stretch_ratio(ratio);
                        const auto in = source == 0 ? std::vector<float>(20000, 1.0f)
                                                    : noise(20000);
                        const auto out = render(stretch, in);
                        worst = std::max(worst,
                                         peak_of(out, stretch.grain_samples() * 2));
                        for (const float v : out) REQUIRE(std::isfinite(v));
                    }
                }
            }
        }
    }
    REQUIRE(worst <= CyclicStretch::kWorstCaseGain + 1e-6);
    // Tight, not merely satisfied — a bound nothing comes near would not be
    // evidence the derivation is right.
    REQUIRE(worst > CyclicStretch::kWorstCaseGain - 1e-4);

    // The blended bound is the interpolation the derivation claims.
    for (const double shape : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        CyclicStretch stretch;
        stretch.prepare(kSr);
        stretch.set_crossfade_shape(shape);
        REQUIRE_THAT(stretch.worst_case_gain(),
                     WithinAbs((1.0 - shape) + shape * CyclicStretch::kWorstCaseGain,
                               1e-12));
    }
}

TEST_CASE("Cyclic stretch: output_db trims the splice boost exactly",
          "[signal][cyclic-stretch][gain]") {
    const std::vector<float> dc(30000, 1.0f);
    CyclicStretch reference = make(kCyclicStretchShortFrame, 2.0);
    const double unity = peak_of(render(reference, dc), reference.grain_samples() * 3);

    for (const double db : {-24.0, -6.0, 6.0, 12.0}) {
        CyclicStretch stretch = make(kCyclicStretchShortFrame, 2.0);
        stretch.set_output_db(db);
        const double scaled = peak_of(render(stretch, dc), stretch.grain_samples() * 3);
        REQUIRE_THAT(20.0 * std::log10(scaled / unity), WithinAbs(db, 0.01));
    }
}

// ── §7.5 / §7.7a ──────────────────────────────────────────────────────────

TEST_CASE("Cyclic stretch: renders are bit-identical, and block size is irrelevant",
          "[signal][cyclic-stretch][determinism]") {
    // Series law 2. There is no RNG in this module at all, so determinism is
    // structural rather than seeded — which makes the interesting failure not
    // "the seed moved" but "some state survived a reset" or "a block boundary
    // perturbed the schedule". Both are what this asserts.
    const auto input = noise(60000);

    CyclicStretch stretch = make(kCyclicStretchLongFrame, 1.7);
    const auto first = render(stretch, input);
    stretch.reset();
    const auto second = render(stretch, input);
    for (std::size_t k = 0; k < first.size(); ++k) REQUIRE(first[k] == second[k]);

    // Two freshly prepared instances agree — no hidden global or static state.
    CyclicStretch fresh = make(kCyclicStretchLongFrame, 1.7);
    const auto third = render(fresh, input);
    for (std::size_t k = 0; k < first.size(); ++k) REQUIRE(first[k] == third[k]);

    // §7.7a: the schedule is driven by absolute counters, so splitting the
    // render cannot change a sample of it.
    for (const int block : {1, 64, 577, 4096}) {
        CyclicStretch chunked = make(kCyclicStretchLongFrame, 1.7);
        std::vector<float> out(input.size(), 0.0f);
        for (std::size_t offset = 0; offset < input.size(); offset += block) {
            const auto count =
                static_cast<int>(std::min<std::size_t>(block, input.size() - offset));
            chunked.process(input.data() + offset, out.data() + offset, count);
        }
        for (std::size_t k = 0; k < first.size(); ++k) REQUIRE(first[k] == out[k]);
    }
}

TEST_CASE("Cyclic stretch: the double instantiation runs and stays finite",
          "[signal][cyclic-stretch][determinism]") {
    CyclicStretch64 stretch;
    stretch.prepare(kSr);
    stretch.set_regime(kCyclicStretchLongFrame);
    stretch.set_stretch_ratio(2.0);

    const int n = 40000;
    std::vector<double> in(static_cast<std::size_t>(n)), a(static_cast<std::size_t>(n)),
        b(static_cast<std::size_t>(n));
    std::uint32_t state = 999u;
    for (int k = 0; k < n; ++k) {
        state = state * 1664525u + 1013904223u;
        in[static_cast<std::size_t>(k)] = (state >> 8) / 8388608.0 - 1.0;
    }
    stretch.process(in.data(), a.data(), n);
    stretch.reset();
    stretch.process(in.data(), b.data(), n);
    for (int k = 0; k < n; ++k) {
        REQUIRE(a[static_cast<std::size_t>(k)] == b[static_cast<std::size_t>(k)]);
        REQUIRE(std::isfinite(a[static_cast<std::size_t>(k)]));
    }
    // The long-frame regime is equal-gain, whose bound is exactly 1 — the
    // shape-dependent form, not the across-all-shapes constant.
    REQUIRE_THAT(stretch.worst_case_gain(), WithinAbs(1.0, 1e-12));
    stretch.set_crossfade_shape(1.0);
    REQUIRE_THAT(stretch.worst_case_gain(),
                 WithinAbs(CyclicStretch64::kWorstCaseGain, 1e-12));
}

// ── §7.6 ──────────────────────────────────────────────────────────────────

TEST_CASE("Cyclic stretch: reported latency is the measured initial fill",
          "[signal][cyclic-stretch][latency]") {
    // Series law 5. `latency_samples()` is `N`, the initial fill — the only
    // latency a time-scaling process has, because for `r ≠ 1` the output is not
    // a delayed copy of the input and has no steady-state group delay at all.
    //
    // The measured onset is `N + 1`, not `N`, and the extra sample is exact
    // rather than approximate: the splice window is identically zero at `θ = 0`,
    // so output sample 0 is multiplied by zero no matter what the input does.
    // Asserting `N + 1` exactly is stronger than the criterion's `± 1`.
    for (const double hz : {80.0, 200.0, 1000.0}) {
        for (const int periods : {1, 4}) {
            CyclicStretch stretch;
            stretch.prepare(kSr);
            stretch.set_cycle_hz(hz);
            stretch.set_grain_periods(periods);
            stretch.set_crossfade_pct(25.0);
            stretch.set_stretch_ratio(1.0);
            stretch.set_mix(100.0);

            const int grain = stretch.grain_samples();
            REQUIRE(stretch.latency_samples() == grain);

            const std::vector<float> step(static_cast<std::size_t>(grain * 6), 0.5f);
            const auto out = render(stretch, step);
            int first_nonzero = -1;
            for (std::size_t k = 0; k < out.size(); ++k)
                if (out[k] != 0.0f) {
                    first_nonzero = static_cast<int>(k);
                    break;
                }
            REQUIRE(first_nonzero == stretch.latency_samples() + 1);
            // Nothing at all before the fill completes.
            for (int k = 0; k <= stretch.latency_samples(); ++k)
                REQUIRE(out[static_cast<std::size_t>(k)] == 0.0f);
        }
    }
}

TEST_CASE("Cyclic stretch: the dry path is delayed by exactly N",
          "[signal][cyclic-stretch][latency]") {
    // So `mix` blends two things talking about the same moment. Measured
    // directly at `mix = 0`, which is also the module's only true bypass.
    CyclicStretch stretch;
    stretch.prepare(kSr);
    stretch.set_cycle_hz(200.0);
    stretch.set_grain_periods(1);
    stretch.set_stretch_ratio(1.0);
    stretch.set_mix(0.0);

    const int grain = stretch.grain_samples();
    const int click_at = grain + 37;
    const auto hits = click_hits(stretch, click_at, grain * 6, 0.5);
    REQUIRE(hits.size() == 1u);
    REQUIRE(hits.front().first - click_at == grain);
    REQUIRE_THAT(hits.front().second, WithinAbs(1.0, 1e-6));
}

// ── The bounded ring ──────────────────────────────────────────────────────

TEST_CASE("Cyclic stretch: a long stretch loops the window instead of latching",
          "[signal][cyclic-stretch][ring]") {
    // The failure this guards is quiet and slow: with the read position folded
    // by a clamp rather than a jumped cursor, a 2× stretch stops stretching
    // after `W/(1 − 1/r)` samples — 4 s at the default capture — and every
    // second after that is an unstretched 1× read with splices on top. The
    // audio still sounds "processed", so nothing screams.
    CyclicStretch stretch = make(kCyclicStretchShortFrame, 2.0);
    stretch.set_capture_ms(2000.0);
    const long long window = stretch.capture_window_samples();

    const int n = static_cast<int>(kSr) * 20;
    const std::vector<float> quiet(static_cast<std::size_t>(n), 0.0f);
    std::vector<float> out(static_cast<std::size_t>(n), 0.0f);

    long long min_behind = window * 4;
    long long max_behind = -1;
    constexpr int kBlock = 480;
    for (int offset = 0; offset + kBlock <= n; offset += kBlock) {
        stretch.process(quiet.data() + offset, out.data() + offset, kBlock);
        const long long behind = stretch.total_captured() - stretch.read_position();
        min_behind = std::min(min_behind, behind);
        max_behind = std::max(max_behind, behind);
        // The read cursor never leaves what the ring actually holds — the
        // property that makes an unbounded stretch bounded.
        REQUIRE(behind >= 0);
        REQUIRE(behind <= window);
    }

    // A latched cursor would sit at the window edge forever, so min and max
    // would coincide. A sawtooth sweeps most of the window.
    REQUIRE(max_behind - min_behind > window / 2);
}

TEST_CASE("Cyclic stretch: compression never reads past what has been captured",
          "[signal][cyclic-stretch][ring]") {
    // The clamp the source spec says is unnecessary. Without it, every
    // compression setting reads ring slots that have not been written yet.
    for (const double ratio : {0.25, 0.5, 0.9}) {
        CyclicStretch stretch = make(kCyclicStretchLongFrame, ratio);
        const int n = static_cast<int>(kSr) * 4;
        const auto in = noise(n);
        std::vector<float> out(static_cast<std::size_t>(n), 0.0f);
        constexpr int kBlock = 256;
        for (int offset = 0; offset + kBlock <= n; offset += kBlock) {
            stretch.process(in.data() + offset, out.data() + offset, kBlock);
            if (stretch.grain_count() == 0) continue;  // still in the initial fill
            REQUIRE(stretch.read_position() >= 0);
            REQUIRE(stretch.read_position() <=
                    stretch.total_captured() - stretch.grain_samples());
        }
        for (const float v : out) REQUIRE(std::isfinite(v));
    }
}

// ── State, parameters, RT safety ──────────────────────────────────────────

TEST_CASE("Cyclic stretch: a parameter change continues rather than restarting",
          "[signal][cyclic-stretch][state]") {
    // §3.7: a live automation move on the cycle must slide the buzz, not
    // restart the machine. Two concrete hazards, both of which produced real
    // bugs during development:
    //
    //   * deriving the grain's output position as `g·S` makes it jump BACKWARDS
    //     when `S` shrinks, dumping a burst of grains into accumulator slots
    //     that have already been read;
    //   * changing the grain geometry under grains that are still in flight
    //     breaks the two-grain overlap the gain bound rests on.
    //
    // The stimulus is DC as well as noise, and the sweep is run from several
    // starting phases. That matters: a single noise sweep measures 1.42 and
    // would pass with BOTH safeguards deleted. DC is the adversarial input for
    // an overlap-add — every pair of samples is in phase with every other — and
    // it is what exposes the pile-up (2.35 with the guards removed, 1.97 with
    // them in place).
    for (int trial = 0; trial < 6; ++trial) {
        const bool use_dc = trial % 2 == 0;
        const auto in = use_dc ? std::vector<float>(120000, 1.0f) : noise(120000, 7u + trial);

        CyclicStretch stretch = make(kCyclicStretchShortFrame, 1.5);
        std::vector<float> out(in.size(), 0.0f);

        long long previous_grains = 0;
        long long previous_out_pos = 0;
        constexpr int kBlock = 512;
        int step = trial * 7;
        for (std::size_t offset = 0; offset + kBlock <= in.size(); offset += kBlock) {
            // Sweep every length-changing control, hard, while audio is running.
            stretch.set_cycle_hz(60.0 + 900.0 * (0.5 + 0.5 * std::sin(0.03 * step)));
            stretch.set_grain_periods(1 + (step % 8));
            stretch.set_crossfade_pct(1.0 + 49.0 * (0.5 + 0.5 * std::cos(0.05 * step)));
            stretch.set_crossfade_shape(0.5 + 0.5 * std::sin(0.11 * step));
            stretch.set_stretch_ratio(0.25 + 3.0 * (0.5 + 0.5 * std::sin(0.017 * step)));
            ++step;

            stretch.process(in.data() + offset, out.data() + offset, kBlock);

            REQUIRE(stretch.grain_count() >= previous_grains);
            REQUIRE(stretch.next_grain_out_pos() >= previous_out_pos);
            previous_grains = stretch.grain_count();
            previous_out_pos = stretch.next_grain_out_pos();
        }

        // The ceiling while lengths are moving is 2, not √2 — two grains, each
        // weighted at most 1. That is a DIFFERENT claim from the settled-
        // parameter bound: √2 needs the two fades to be complementary, and a
        // length change can make that impossible. What must still hold is that
        // only two grains ever sum.
        for (const float v : out) {
            REQUIRE(std::isfinite(v));
            REQUIRE(std::abs(v) <= 2.0 + 1e-4);
        }
    }
}

TEST_CASE("Cyclic stretch: mix at zero is the only true bypass",
          "[signal][cyclic-stretch][state]") {
    CyclicStretch stretch = make(kCyclicStretchShortFrame, 1.0);
    stretch.set_mix(0.0);
    const auto in = noise(20000);
    const auto out = render(stretch, in);
    const int grain = stretch.grain_samples();
    for (std::size_t k = static_cast<std::size_t>(grain + 1); k < in.size(); ++k)
        REQUIRE_THAT(static_cast<double>(out[k]),
                     WithinAbs(static_cast<double>(in[k - static_cast<std::size_t>(grain)]),
                               1e-6));
}

TEST_CASE("Cyclic stretch: a reset instance matches a fresh one",
          "[signal][cyclic-stretch][state]") {
    const auto in = noise(30000);

    CyclicStretch fresh = make(kCyclicStretchShortFrame, 2.0);
    const auto expected = render(fresh, in);

    CyclicStretch used = make(kCyclicStretchShortFrame, 2.0);
    std::vector<float> scratch(in.size(), 0.0f);
    used.process(in.data(), scratch.data(), static_cast<int>(in.size()));
    used.reset();
    const auto actual = render(used, in);

    for (std::size_t k = 0; k < expected.size(); ++k) REQUIRE(expected[k] == actual[k]);
    REQUIRE(used.grain_count() > 0);
}

TEST_CASE("Cyclic stretch: process and reset allocate nothing",
          "[signal][cyclic-stretch][rt-safety]") {
    // §7.7, across every parameter extreme the ranges permit. The buffers are
    // built before the probe opens; `prepare` bought the worst case, so no
    // setter can need more.
    CyclicStretch stretch;
    stretch.prepare(kSr);
    const auto in = noise(1024);
    std::vector<float> out(in.size(), 0.0f);
    stretch.process(in.data(), out.data(), static_cast<int>(in.size()));  // warm

    pulp::test::RtAllocationProbe probe;
    for (const double hz : {CyclicStretch::kCycleHzMin, 200.0, CyclicStretch::kCycleHzMax}) {
        for (const int periods : {CyclicStretch::kGrainPeriodsMin, 5,
                                  CyclicStretch::kGrainPeriodsMax}) {
            for (const double pct : {CyclicStretch::kCrossfadePctMin,
                                     CyclicStretch::kCrossfadePctMax}) {
                for (const double ratio : {CyclicStretch::kStretchRatioMin, 1.0,
                                           CyclicStretch::kStretchRatioMax}) {
                    for (const double capture : {CyclicStretch::kCaptureMsMin,
                                                 CyclicStretch::kCaptureMsMax}) {
                        stretch.set_cycle_hz(hz);
                        stretch.set_grain_periods(periods);
                        stretch.set_crossfade_pct(pct);
                        stretch.set_crossfade_shape(0.5);
                        stretch.set_stretch_ratio(ratio);
                        stretch.set_capture_ms(capture);
                        stretch.set_mix(50.0);
                        stretch.set_output_db(-3.0);
                        stretch.process(in.data(), out.data(),
                                        static_cast<int>(in.size()));
                    }
                }
            }
        }
    }
    stretch.reset();
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("Cyclic stretch: round_half_even ties break toward even",
          "[signal][cyclic-stretch][determinism]") {
    // The snap's rounding rule is normative because it decides the schedule, and
    // it is spelled out rather than delegated precisely so this can be pinned.
    // `llround` would give 1, 2, 3 for the first three; `nearbyint` agrees only
    // while the FP rounding mode is untouched.
    REQUIRE(round_half_even(0.5) == 0);
    REQUIRE(round_half_even(1.5) == 2);
    REQUIRE(round_half_even(2.5) == 2);
    REQUIRE(round_half_even(3.5) == 4);
    REQUIRE(round_half_even(-0.5) == 0);
    REQUIRE(round_half_even(-1.5) == -2);
    REQUIRE(round_half_even(-2.5) == -2);
    REQUIRE(round_half_even(0.4999999) == 0);
    REQUIRE(round_half_even(0.5000001) == 1);
    REQUIRE(round_half_even(7.0) == 7);
    REQUIRE(round_half_even(-7.0) == -7);
}

TEST_CASE("Cyclic stretch: default and non-finite inputs are silent-safe",
          "[signal][cyclic-stretch][nan-recovery][rt-safety]") {
    CyclicStretch raw;
    const std::vector<float> input(64, 0.5f);
    std::vector<float> output(input.size(), 1.0f);
    raw.process(input.data(), output.data(), static_cast<int>(input.size()));
    REQUIRE(std::all_of(output.begin(), output.end(), [](float v) { return v == 0.0f; }));

    CyclicStretch poisoned = make(kCyclicStretchShortFrame, 2.0);
    CyclicStretch fresh = make(kCyclicStretchShortFrame, 2.0);
    poisoned.set_cycle_hz(std::numeric_limits<double>::quiet_NaN());
    poisoned.set_crossfade_pct(std::numeric_limits<double>::infinity());
    poisoned.set_stretch_ratio(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(std::isfinite(poisoned.flutter_hz()));

    float bad = std::numeric_limits<float>::quiet_NaN();
    float rejected = 1.0f;
    {
        pulp::test::RtAllocationProbe probe;
        poisoned.process(&bad, &rejected, 1);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(rejected == 0.0f);
    fresh.reset();
    const auto continuation = noise(4096);
    std::vector<float> a(continuation.size()), b(continuation.size());
    poisoned.process(continuation.data(), a.data(), static_cast<int>(a.size()));
    fresh.process(continuation.data(), b.data(), static_cast<int>(b.size()));
    REQUIRE(a == b);
}
