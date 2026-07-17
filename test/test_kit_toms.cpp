// Tests for the PulpKit tom / conga voice (MembraneVoice): the bass-drum
// bridged-T resonator retuned into the membrane band. What matters for a struck
// drum is behaviour over time, so these render a hit and measure two numbers the
// reference instrument was itself measured with: the ring pitch (mean zero-
// crossing frequency) and the T60 (time of the last sample above peak/1000, the
// -60 dB point). Each of the three reference pads is asserted to land within 7%
// of its measured body pitch and decay.
//
// Reference targets are numbers measured offline from the reference AU's
// renders (dominant body ~347 Hz for all three pads; T60 = 1.043 / 1.182 /
// 0.981 s for pads A / B / C). No reference audio is stored or replayed -- every
// sample here is produced by the circuit model, and the targets are the fit
// goal, not a lookup table.
//
// Measurement is self-contained (zero crossings + a peak/1000 scan) so the test
// carries no FFT / analysis-library dependency; the definitions match the ones
// the reference numbers were taken with.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <membrane_voice.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using pulp::examples::apply_membrane_preset;
using pulp::examples::MembranePreset;
using pulp::examples::MembraneVoice;

constexpr double kFs = 48000.0;

/// A hard strike, in volts at the trigger pulse (top of the accent range).
constexpr double kAccent = 14.0;

struct RenderSpec {
    double duration_s = 4.0;
    double accent = kAccent;
    std::vector<double> hit_times_s{0.05};
};

std::vector<double> render_preset(MembranePreset preset, RenderSpec spec = {}) {
    MembraneVoice voice;
    voice.prepare(kFs);
    apply_membrane_preset(voice, preset);

    std::vector<int> hits;
    for (double t : spec.hit_times_s) hits.push_back(static_cast<int>(t * kFs));

    const int n = static_cast<int>(spec.duration_s * kFs);
    std::vector<double> out(static_cast<std::size_t>(n));
    std::size_t next = 0;
    for (int i = 0; i < n; ++i) {
        if (next < hits.size() && i == hits[next]) {
            voice.trigger(spec.accent);
            ++next;
        }
        out[static_cast<std::size_t>(i)] = voice.process();
    }
    return out;
}

double peak_abs(const std::vector<double>& x) {
    double m = 0.0;
    for (double v : x) m = std::max(m, std::fabs(v));
    return m;
}

bool all_finite(const std::vector<double>& x) {
    return std::all_of(x.begin(), x.end(), [](double v) { return std::isfinite(v); });
}

/// T60 as the reference was measured: time of the last sample above peak/1000.
double t60_s(const std::vector<double>& x) {
    const double thr = peak_abs(x) / 1000.0;
    int last = 0;
    for (int i = 0; i < static_cast<int>(x.size()); ++i)
        if (std::fabs(x[i]) > thr) last = i;
    return static_cast<double>(last) / kFs;
}

/// Mean ring frequency over a window, from interpolated upward zero crossings.
/// For a single-mode ring this equals the body pitch, which is what the
/// reference's dominant spectral peak reports.
double ring_hz(const std::vector<double>& x, double t0, double t1) {
    const auto a = static_cast<std::size_t>(t0 * kFs);
    const auto b = std::min(static_cast<std::size_t>(t1 * kFs), x.size());
    std::vector<double> zc;
    for (std::size_t i = a; i + 1 < b; ++i)
        if (x[i] < 0.0 && x[i + 1] >= 0.0)
            zc.push_back((static_cast<double>(i) + (-x[i] / (x[i + 1] - x[i]))) / kFs);
    if (zc.size() < 3) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(zc.size() - 1) / (zc.back() - zc.front());
}

double rms(const std::vector<double>& x, double t0, double win = 0.05) {
    const auto a = static_cast<std::size_t>(t0 * kFs);
    const auto b = std::min(static_cast<std::size_t>((t0 + win) * kFs), x.size());
    if (a >= b) return 0.0;
    double s = 0.0;
    for (std::size_t i = a; i < b; ++i) s += x[i] * x[i];
    return std::sqrt(s / static_cast<double>(b - a));
}

struct PadTarget {
    MembranePreset preset;
    double body_hz;   // reference dominant body peak (all three ~347 Hz)
    double t60;       // reference -60 dB decay
};

// Reference-measured targets. Body pitch is the dominant spectral peak, shared
// across the family; the three pads differ by decay (and level).
constexpr PadTarget kPads[] = {
    {MembranePreset::TomA, 347.0, 1.043},
    {MembranePreset::TomB, 347.0, 1.182},
    {MembranePreset::TomC, 347.0, 0.981},
};

}  // namespace

TEST_CASE("membrane voice pads land within 7% of reference pitch and decay",
          "[signal][kit][membrane][tom]") {
    for (const auto& pad : kPads) {
        const auto x = render_preset(pad.preset);
        REQUIRE(all_finite(x));

        // Measure the ring after the strike transient has settled.
        const double f = ring_hz(x, 0.1, 0.5);
        const double t = t60_s(x);

        // Body pitch within 7% of the reference dominant peak.
        REQUIRE(std::fabs(f - pad.body_hz) / pad.body_hz < 0.07);
        // Decay within 7% of the reference T60.
        REQUIRE(std::fabs(t - pad.t60) / pad.t60 < 0.07);
    }
}

TEST_CASE("membrane pads rank by decay and loudness as the reference does",
          "[signal][kit][membrane][tom]") {
    // Reference: pad B has the longest decay, pad C the shortest; pad C is the
    // loudest, pad A the quietest.
    const double ta = t60_s(render_preset(MembranePreset::TomA));
    const double tb = t60_s(render_preset(MembranePreset::TomB));
    const double tc = t60_s(render_preset(MembranePreset::TomC));
    REQUIRE(tb > ta);   // B is the long pad
    REQUIRE(ta > tc);   // C is the short pad

    const double pa = peak_abs(render_preset(MembranePreset::TomA));
    const double pb = peak_abs(render_preset(MembranePreset::TomB));
    const double pc = peak_abs(render_preset(MembranePreset::TomC));
    REQUIRE(pc > pb);
    REQUIRE(pb > pa);
}

TEST_CASE("membrane ring decays monotonically (a struck drop, not a build-up)",
          "[signal][kit][membrane][tom]") {
    // Near the oscillation threshold a mistuned loop can build energy after the
    // strike; a real struck drum only decays. RMS must fall across the ring.
    const auto x = render_preset(MembranePreset::TomB);  // longest / closest to threshold
    const double e_early = rms(x, 0.10);
    const double e_mid = rms(x, 0.50);
    const double e_late = rms(x, 1.00);
    REQUIRE(e_early > e_mid);
    REQUIRE(e_mid > e_late);
    REQUIRE(e_late < e_early * 0.05);  // well down by 1 s
}

TEST_CASE("membrane voice is a parametric bridged-T tunable across the band",
          "[signal][kit][membrane][tom]") {
    // The three pads are presets on a real parametric resonator, not three
    // hardcoded tones: the body pitch tracks set_frequency across the range.
    for (double target : {150.0, 250.0, 400.0, 550.0}) {
        MembraneVoice v;
        v.prepare(kFs);
        v.set_frequency(target);
        v.set_decay(0.85);
        std::vector<double> out(static_cast<std::size_t>(1.0 * kFs));
        v.trigger(kAccent);
        for (auto& s : out) s = v.process();
        REQUIRE(all_finite(out));
        const double f = ring_hz(out, 0.05, 0.30);
        REQUIRE(std::fabs(f - target) / target < 0.07);
    }
}

TEST_CASE("membrane fine tune shifts the ring by the ratio",
          "[signal][kit][membrane][tom]") {
    auto ring_at = [](double ratio) {
        MembraneVoice v;
        v.prepare(kFs);
        v.set_frequency(347.0);
        v.set_tune(ratio);
        v.set_decay(0.85);
        std::vector<double> out(static_cast<std::size_t>(1.0 * kFs));
        v.trigger(kAccent);
        for (auto& s : out) s = v.process();
        return ring_hz(out, 0.05, 0.30);
    };
    const double up = ring_at(1.2);
    const double base = ring_at(1.0);
    const double down = ring_at(0.8);
    REQUIRE(up > base);
    REQUIRE(base > down);
    // The shift tracks the ratio to within 7%.
    REQUIRE(std::fabs(up / base - 1.2) < 0.07);
    REQUIRE(std::fabs(down / base - 0.8) < 0.07);
}

TEST_CASE("membrane second strike superposes onto the ring, no machine-gun reset",
          "[signal][kit][membrane][tom]") {
    // A trigger must add excitation to whatever ring is in flight rather than
    // clear it. Striking again while the first ring is still loud produces a
    // sum that differs from a lone strike -- identical successive hits would be
    // the machine-gun artifact this topology exists to avoid.
    RenderSpec one;
    one.hit_times_s = {0.05};
    RenderSpec two;
    two.hit_times_s = {0.05, 0.20};  // second hit while the first still rings
    const auto a = render_preset(MembranePreset::TomA, one);
    const auto b = render_preset(MembranePreset::TomA, two);
    REQUIRE(all_finite(b));

    // Compare a window that starts just after the second strike: with true
    // superposition it is not equal to the single-hit tail there.
    const auto s0 = static_cast<std::size_t>(0.22 * kFs);
    const auto s1 = static_cast<std::size_t>(0.30 * kFs);
    double diff = 0.0;
    for (std::size_t i = s0; i < s1; ++i) diff += std::fabs(a[i] - b[i]);
    REQUIRE(diff > 1e-3);
}

TEST_CASE("membrane trigger and process do not allocate on the audio thread",
          "[signal][kit][membrane][tom][rt-safety]") {
    MembraneVoice voice;
    voice.prepare(kFs);  // allocation allowed here
    apply_membrane_preset(voice, MembranePreset::TomB);

    pulp::test::RtAllocationProbe probe;
    for (int block = 0; block < 200; ++block) {
        if (block % 8 == 0) voice.trigger(kAccent);
        for (int i = 0; i < 64; ++i) (void)voice.process();
        voice.snap_denormals();
    }
    REQUIRE(probe.allocation_count() == 0);
}
