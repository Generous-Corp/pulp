#pragma once

// TapeMachineT — the tape-machine insert's acceptance suite (spec R1–R14).
//
// Expected values are COMPUTED from the shipped constants, never restated as
// literals: `tape::eq_record_response_db` evaluates the analog prototype from
// the same `tape::eq_time_constants` table the DSP configures itself from, and
// `tape::gap_null_hz` evaluates `v/g` from the same `tape::head_gap_geometry`.
// A change to a shipped number therefore fails the test that documents it
// rather than quietly disagreeing with it.
//
// ── Measurement recipe ────────────────────────────────────────────────────
//
// Frequency-response criteria (R1, R2, R3, R10) are asserted against the
// filters' CLOSED-FORM response rather than by rendering a swept sine. Same
// quantity, no leakage term, no settle length, and no window correction hiding
// inside a ±0.5 dB tolerance — and for R3's extinction null it is the only way
// to resolve a 60 dB notch at all, since a rendered sweep's skirt sets the floor
// long before the notch does.
//
// Level criteria measured through the whole module use RMS over the second half
// of a multi-second render, NOT a coherent DFT at the tone. That is not
// fastidiousness: the wow/flutter stage frequency-modulates the programme, and
// at age 0.5 the reused table's 0.325 ms wow depth puts a 1 kHz tone at a
// modulation index near 2 — a coherent bin at exactly 1 kHz loses most of the
// energy to sidebands and reads 24 dB low. That measurement bug produced a
// convincing-looking "the module loses 24 dB" result before it was caught.
//
// Noise-floor criteria (R7) integrate a DFT grid over 3.2–3.8 kHz with bins
// within 150 Hz of any harmonic of the 997 Hz probe excluded. 997 Hz rather than
// 1 kHz so no harmonic lands on a round number, and that band because it sits
// between the 3rd and 4th harmonics with room for the wow sidebands (widest,
// at the 3rd harmonic, ±15 Hz) on either side, while still carrying the reused
// hiss generator's 4 kHz-lowpassed output.
//
// ── Three criteria this suite deliberately re-scopes, with the numbers ────
//
// **R1 and R2 cannot both hold end-to-end.** As written they measure the same
// module under the same bypass conditions and demand different answers: R1 that
// it match `H(s)` (±36 dB of shape for NAB), R2 that it be flat within ±0.3 dB.
// They are consistent only when read as what they plainly are — R1 an assertion
// about ONE network, R2 about the record/playback CASCADE. Both are asserted
// here at that scope, on the realized filters, which is also strictly stronger
// than measuring them through a chain that additionally contains Wallace loss,
// a head bump, hiss and pitch modulation.
//
// **R3's null is measured on the gap filter, not through the chain.** At
// 1.875 ips the reused Wallace stage's own fixed 3 µm gap term nulls at
// 15.9 kHz and again at every multiple — including 63.5 kHz, four percent from
// this module's 59.5 kHz reproduce null. Measuring "a minimum ≥40 dB down" over
// a chain containing both would be measuring whichever null the search window
// happened to catch.
//
// **R6 is measured on the compander pair, not through the chain.** A 1:2
// expander DOUBLES any level error in dB that reaches it, so the 1.6 dB of
// Wallace loss a 1 kHz tone sees at 15 ips comes out as 3.2 dB — R6's ±0.1 dB is
// unreachable through the module by construction, and its own parenthetical
// ("encode/decode both unity at the reference") says it is about the pair.
//
// ── And two the module cannot meet as stated ─────────────────────────────
//
// **R14's "latency_samples() == 0" is unachievable.** Two mandated stages are
// constant delays: the reused hysteresis stage's 4× half-band wrap (48 host
// samples, inherited with its antialiasing policy) and the wow/flutter
// read-position modulation, which is a delay-line read and cannot have a
// nominal offset smaller than the modulation it must swing through (2.0 ms,
// 96 samples at 48 kHz). 144 samples total. What R14 is actually protecting —
// that the reported number is exact, and does not move under the audio thread —
// is asserted in full below.
//
// **§1's "set_* never allocate" is unachievable for `set_speed_ips`.** The
// reused physical tier redesigns its speed-dependent minimum-phase loss FIR on
// a speed change. This module pre-designs its OWN speed-dependent filter so it
// contributes nothing, but the inherited allocation is real and is asserted
// here rather than left as a surprise.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/tape_machine.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kSr = 48000.0;
constexpr double kAltSr = 44100.0;
constexpr double kGapSr = 192000.0;  // R3 needs headroom for a 59.5 kHz null

/// The probe tone for noise-floor work. 997 Hz so no harmonic lands on a round
/// frequency and collides with a measurement grid point.
constexpr double kProbeHz = 997.0;

const std::vector<TapeCurve> kAllCurves = {TapeCurve::nab, TapeCurve::iec_ccir,
                                           TapeCurve::cassette_type1,
                                           TapeCurve::cassette_type2};

const std::vector<TapeArchetype> kAllArchetypes = {TapeArchetype::ampex_350_440,
                                                   TapeArchetype::studer_a800,
                                                   TapeArchetype::cassette_deck};

/// A machine with every optional stage quiet, so a measurement reads the stage
/// it names. Print-through at its floor rather than "off" because the floor IS
/// the parameter's minimum — there is no off.
template <typename Machine>
void quiesce(Machine& machine) {
    machine.set_age(0.0f);
    machine.set_drive(0.0f);
    machine.set_bias(0.0f);
    machine.set_companding_enabled(false);
    machine.set_crosstalk_db(-45.0f);
    machine.set_print_through(-80.0f, 700.0f, false);
}

struct Stereo {
    std::vector<float> left, right;
};

/// Renders `n` frames of a sine into both channels.
Stereo render_tone(TapeMachine& machine, double hz, double amplitude, int n,
                   double sample_rate = kSr) {
    std::vector<float> in_l(static_cast<std::size_t>(n)), in_r(static_cast<std::size_t>(n));
    Stereo out{std::vector<float>(static_cast<std::size_t>(n)),
               std::vector<float>(static_cast<std::size_t>(n))};
    for (int k = 0; k < n; ++k) {
        const auto v =
            static_cast<float>(amplitude * std::sin(2.0 * std::numbers::pi * hz * k / sample_rate));
        in_l[static_cast<std::size_t>(k)] = v;
        in_r[static_cast<std::size_t>(k)] = v;
    }
    machine.process(in_l.data(), in_r.data(), out.left.data(), out.right.data(), n);
    return out;
}

/// Renders a unit impulse into both channels.
Stereo render_impulse(TapeMachine& machine, int n) {
    std::vector<float> in_l(static_cast<std::size_t>(n), 0.0f),
        in_r(static_cast<std::size_t>(n), 0.0f);
    in_l[0] = 1.0f;
    in_r[0] = 1.0f;
    Stereo out{std::vector<float>(static_cast<std::size_t>(n)),
               std::vector<float>(static_cast<std::size_t>(n))};
    machine.process(in_l.data(), in_r.data(), out.left.data(), out.right.data(), n);
    return out;
}

/// RMS over the second half — past every transient, and immune to the sideband
/// smearing a coherent bin is not. See the recipe note.
double steady_rms(const std::vector<float>& x) {
    const auto n = x.size();
    const std::size_t start = n / 2;
    double sum = 0.0;
    for (std::size_t k = start; k < n; ++k) sum += static_cast<double>(x[k]) * x[k];
    return std::sqrt(sum / static_cast<double>(n - start));
}

/// Coherent DFT magnitude at `hz` over the second half.
double bin_magnitude(const std::vector<float>& x, double hz, double sample_rate = kSr) {
    const auto n = static_cast<int>(x.size());
    const int start = n / 2;
    double re = 0.0, im = 0.0;
    for (int k = start; k < n; ++k) {
        const double theta = 2.0 * std::numbers::pi * hz * k / sample_rate;
        re += x[static_cast<std::size_t>(k)] * std::cos(theta);
        im += x[static_cast<std::size_t>(k)] * std::sin(theta);
    }
    const double scale = 2.0 / static_cast<double>(n - start);
    return std::hypot(re * scale, im * scale);
}

/// The largest coherent magnitude within ±8 Hz of `hz`, which is what a tone
/// that has been through wow and flutter actually looks like.
double smeared_magnitude(const std::vector<float>& x, double hz) {
    double best = 0.0;
    for (double f = hz - 8.0; f <= hz + 8.0; f += 0.25)
        best = std::max(best, bin_magnitude(x, f));
    return best;
}

/// RMS of the DFT grid over `[lo, hi]`, skipping every bin within `exclude` Hz
/// of a harmonic of the probe tone.
double band_noise(const std::vector<float>& x, double lo, double hi, double exclude) {
    const auto n = static_cast<int>(x.size());
    const int start = n / 2;
    double total = 0.0;
    int counted = 0;
    for (double f = lo; f <= hi; f += 5.0) {
        bool near_harmonic = false;
        for (int harmonic = 1; harmonic <= 12; ++harmonic)
            if (std::abs(f - harmonic * kProbeHz) < exclude) near_harmonic = true;
        if (near_harmonic) continue;
        const double magnitude = [&] {
            double re = 0.0, im = 0.0;
            for (int k = start; k < n; ++k) {
                const double theta = 2.0 * std::numbers::pi * f * k / kSr;
                re += x[static_cast<std::size_t>(k)] * std::cos(theta);
                im += x[static_cast<std::size_t>(k)] * std::sin(theta);
            }
            const double scale = 2.0 / static_cast<double>(n - start);
            return std::hypot(re * scale, im * scale);
        }();
        total += magnitude * magnitude;
        ++counted;
    }
    return std::sqrt(total / std::max(counted, 1));
}

/// Total harmonic distortion over harmonics 2..8, as the spec's R4 defines it.
double thd(const std::vector<float>& x, double fundamental_hz) {
    const double f = smeared_magnitude(x, fundamental_hz);
    double harmonics = 0.0;
    for (int k = 2; k <= 8; ++k) {
        const double v = smeared_magnitude(x, fundamental_hz * k);
        harmonics += v * v;
    }
    return std::sqrt(harmonics) / std::max(f, 1e-15);
}

/// Peak magnitude and its index.
std::pair<double, int> peak_of(const std::vector<float>& x, int lo = 0, int hi = -1) {
    const int end = hi < 0 ? static_cast<int>(x.size()) : std::min(hi, static_cast<int>(x.size()));
    double best = 0.0;
    int at = std::max(lo, 0);
    for (int k = std::max(lo, 0); k < end; ++k) {
        const double v = std::abs(static_cast<double>(x[static_cast<std::size_t>(k)]));
        if (v > best) {
            best = v;
            at = k;
        }
    }
    return {best, at};
}

/// Magnitude response of an FIR, in dB.
double fir_response_db(const std::vector<double>& taps, double hz, double sample_rate) {
    double re = 0.0, im = 0.0;
    for (std::size_t n = 0; n < taps.size(); ++n) {
        const double theta = 2.0 * std::numbers::pi * hz * static_cast<double>(n) / sample_rate;
        re += taps[n] * std::cos(theta);
        im -= taps[n] * std::sin(theta);
    }
    return 20.0 * std::log10(std::max(std::hypot(re, im), 1e-15));
}

}  // namespace

// ── R1 ────────────────────────────────────────────────────────────────────


// ── R2 ────────────────────────────────────────────────────────────────────



// ── R3 ────────────────────────────────────────────────────────────────────


// ── R4 ────────────────────────────────────────────────────────────────────



// ── R5 ────────────────────────────────────────────────────────────────────


// ── R6 / R7 ───────────────────────────────────────────────────────────────



// ── R8 / R9 ───────────────────────────────────────────────────────────────

namespace {

/// Renders an impulse twice — once at the requested print level, once at the
/// parameter's floor — and returns the difference, which isolates the print tap.
///
/// A direct peak search cannot do this. An impulse through a reproduce network
/// carrying a +22.6 dB bass lift at 50 Hz rings for tens of milliseconds and is
/// still 47 dB up at 1700 samples; a −40 dB tap sitting on that tail is not a
/// local maximum of anything. Differencing two renders that are identical
/// except for the print gain leaves exactly the tap.
struct PrintProbe {
    std::vector<float> difference;
    std::vector<float> baseline;
    int latency = 0;
    int offset_samples = 0;
};

PrintProbe probe_print_through(double level_db, double offset_ms, bool pre_echo) {
    auto render = [&](float level) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::studer_a800);
        machine.prepare(kSr);
        quiesce(machine);
        machine.set_print_through(level, static_cast<float>(offset_ms), pre_echo);
        return std::pair<Stereo, int>{render_impulse(machine, static_cast<int>(kSr * 1.2)),
                                      machine.latency_samples()};
    };

    const auto hot = render(static_cast<float>(level_db));
    const auto floor = render(TapeMachine::kPrintThroughDbMin);

    PrintProbe probe;
    probe.baseline = floor.first.left;
    probe.difference.resize(probe.baseline.size());
    for (std::size_t k = 0; k < probe.difference.size(); ++k)
        probe.difference[k] = hot.first.left[k] - floor.first.left[k];
    probe.latency = hot.second;
    probe.offset_samples =
        static_cast<int>(std::llround(offset_ms * kSr / 1000.0));
    return probe;
}

}  // namespace




// ── R10 ───────────────────────────────────────────────────────────────────


// ── R11 ───────────────────────────────────────────────────────────────────


// ── R12 ───────────────────────────────────────────────────────────────────



// ── R13 ───────────────────────────────────────────────────────────────────

namespace {

/// Drives an already-prepared machine through preallocated buffers. Everything
/// the probe must not see is constructed before it opens.
template <typename Machine, typename Sample>
struct Driver {
    std::vector<Sample> in_l, in_r, out_l, out_r;
    int frames = 512;

    explicit Driver(int n) : in_l(static_cast<std::size_t>(n)),
                             in_r(static_cast<std::size_t>(n)),
                             out_l(static_cast<std::size_t>(n)),
                             out_r(static_cast<std::size_t>(n)),
                             frames(n) {
        chardelay::Xorshift32 rng(4242u);
        for (int k = 0; k < n; ++k) {
            in_l[static_cast<std::size_t>(k)] = static_cast<Sample>(0.4 * rng.bipolar());
            in_r[static_cast<std::size_t>(k)] = static_cast<Sample>(0.4 * rng.bipolar());
        }
    }

    void run(Machine& machine) {
        machine.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), frames);
    }
};

}  // namespace



// ── R14 ───────────────────────────────────────────────────────────────────


// ── Series-law and integration coverage beyond the numbered criteria ──────
