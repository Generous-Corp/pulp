// Tests for the TR-808 bass drum's bridged-T resonator and the voice built on
// it. The circuit is a struck resonator whose ringing is the sound, so most of
// what matters here is state behaviour over time rather than a static response:
// what the drum does on the second hit, at full decay, and at different strike
// forces is where every naive model of it fails.
//
// Reference values come from the published circuit equations (Werner, Abel &
// Smith, DAFx-14) and from an independent numerical study of them, never from
// measurements of any commercial product.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/bridged_t_resonator.hpp>

#include <va_drum_voice.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace {

using pulp::examples::BassDrumVoice;
using pulp::signal::BridgedTComponents;
using pulp::signal::Q43Leakage;

constexpr double kFs = 48000.0;
constexpr double kPi = 3.14159265358979323846;

/// A soft strike and a hard strike, in volts at the trigger pulse. The accent
/// range is set by the global accent pot in hardware.
constexpr double kAccentMin = 4.0;
constexpr double kAccentMax = 14.0;

struct RenderSpec {
    double duration_s = 3.0;
    double decay = 0.5;
    double accent = kAccentMax;
    double level = 1.0;
    double tune = 1.0;
    bool sigh = true;
    std::vector<double> hit_times_s{0.05};
};

std::vector<double> render(const RenderSpec& spec) {
    BassDrumVoice voice;
    voice.prepare(kFs);
    voice.set_decay(spec.decay);
    voice.set_level(spec.level);
    voice.set_tune(spec.tune);
    voice.set_tone(0.5);
    voice.set_sigh_enabled(spec.sigh);

    std::vector<int> hits;
    hits.reserve(spec.hit_times_s.size());
    for (double t : spec.hit_times_s) hits.push_back(static_cast<int>(t * kFs));

    const int n_samples = static_cast<int>(spec.duration_s * kFs);
    std::vector<double> out(static_cast<std::size_t>(n_samples));
    std::size_t next_hit = 0;
    for (int n = 0; n < n_samples; ++n) {
        if (next_hit < hits.size() && n == hits[next_hit]) {
            voice.trigger(spec.accent);
            ++next_hit;
        }
        out[static_cast<std::size_t>(n)] = voice.process();
    }
    return out;
}

double rms(const std::vector<double>& x, double t0, double t1) {
    auto a = static_cast<std::size_t>(t0 * kFs);
    auto b = std::min(static_cast<std::size_t>(t1 * kFs), x.size());
    if (a >= b) return 0.0;
    double sum = 0.0;
    for (std::size_t i = a; i < b; ++i) sum += x[i] * x[i];
    return std::sqrt(sum / static_cast<double>(b - a));
}

double peak(const std::vector<double>& x) {
    double m = 0.0;
    for (double v : x) m = std::max(m, std::fabs(v));
    return m;
}

bool all_finite(const std::vector<double>& x) {
    return std::all_of(x.begin(), x.end(), [](double v) { return std::isfinite(v); });
}

double l2_norm(const std::vector<double>& x) {
    double sum = 0.0;
    for (double v : x) sum += v * v;
    return std::sqrt(sum);
}

/// Upward zero crossings, linearly interpolated, in seconds.
std::vector<double> zero_crossings(const std::vector<double>& x, double t0, double t1) {
    auto a = static_cast<std::size_t>(t0 * kFs);
    auto b = std::min(static_cast<std::size_t>(t1 * kFs), x.size());
    std::vector<double> out;
    for (std::size_t i = a; i + 1 < b; ++i) {
        if (x[i] < 0.0 && x[i + 1] >= 0.0) {
            out.push_back((static_cast<double>(i) + (-x[i] / (x[i + 1] - x[i]))) / kFs);
        }
    }
    return out;
}

/// Mean frequency over a window, from the zero-crossing count. This averages
/// over each cycle, so it reports the pitch the ring is heard at rather than
/// the instantaneous centre frequency, which swings within every cycle.
double mean_frequency(const std::vector<double>& x, double t0, double t1) {
    const auto zc = zero_crossings(x, t0, t1);
    if (zc.size() < 3) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(zc.size() - 1) / (zc.back() - zc.front());
}

/// Frequency of each individual cycle, paired with its start time.
std::vector<std::pair<double, double>> cycle_frequencies(const std::vector<double>& x,
                                                         double t0, double t1) {
    const auto zc = zero_crossings(x, t0, t1);
    std::vector<std::pair<double, double>> out;
    for (std::size_t i = 0; i + 1 < zc.size(); ++i) {
        out.emplace_back(zc[i], 1.0 / (zc[i + 1] - zc[i]));
    }
    return out;
}

/// Exponential decay rate in nepers/second, from the ratio of two short RMS
/// windows. Both windows must sit late enough that the leakage has spent
/// itself, otherwise this measures the sigh's extra dissipation too.
double decay_sigma(const std::vector<double>& x, double t0, double t1, double win = 0.2) {
    const double r0 = rms(x, t0, t0 + win);
    const double r1 = rms(x, t1, t1 + win);
    return std::log(r0 / r1) / (t1 - t0);
}

/// The gain that best explains @p ref as a scaled copy of @p y, and the
/// fraction of ref's energy that model still fails to account for. Least
/// squares picks the most favourable gain that exists, so a residual near zero
/// means the difference genuinely is a gain and nothing else.
struct GainNull {
    double gain = 0.0;
    double residual = 0.0;
};

GainNull best_fit_gain_null(const std::vector<double>& y, const std::vector<double>& ref) {
    double cross = 0.0, energy = 0.0, ref_energy = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        cross += y[i] * ref[i];
        energy += y[i] * y[i];
        ref_energy += ref[i] * ref[i];
    }
    GainNull out;
    out.gain = cross / energy;
    double resid = 0.0;
    for (std::size_t i = 0; i < y.size(); ++i) {
        const double e = ref[i] - out.gain * y[i];
        resid += e * e;
    }
    out.residual = std::sqrt(resid / ref_energy);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// Topology. If the centre frequency is wrong, every component value or the
// R_eff substitution is misread, and nothing downstream is worth measuring.
// ---------------------------------------------------------------------------

TEST_CASE("bridged-T centre frequency at nominal component values", "[bridged-t][topology]") {
    const BridgedTComponents c{};
    const double r_series = c.r165 + c.r166;
    const double r_eff = pulp::signal::bridged_t_shunt_resistance(r_series, c);

    // R161 || (R165+R166) || R170 = 1M || 53.8k || 470k
    REQUIRE(r_eff == Catch::Approx(46.05e3).epsilon(0.001));

    const double fc = pulp::signal::bridged_t_center_frequency(r_eff, c);
    REQUIRE(fc == Catch::Approx(49.44).epsilon(0.02));
    REQUIRE(pulp::signal::bridged_t_q(r_eff, c) == Catch::Approx(2.33).epsilon(0.01));
}

TEST_CASE("attack shunt lifts the centre frequency by over an octave",
          "[bridged-t][topology]") {
    const BridgedTComponents c{};
    const double fc_normal = pulp::signal::bridged_t_center_frequency(
        pulp::signal::bridged_t_shunt_resistance(c.r165 + c.r166, c), c);
    // Grounding Q43's collector shorts R165 out, leaving R166 alone.
    const double fc_attack = pulp::signal::bridged_t_center_frequency(
        pulp::signal::bridged_t_shunt_resistance(c.r166, c), c);

    REQUIRE(fc_attack == Catch::Approx(130.03).epsilon(0.02));
    REQUIRE(std::log2(fc_attack / fc_normal) > 1.0);
}

TEST_CASE("Q43 leakage turns on below its knee and stays off above it",
          "[bridged-t][topology]") {
    const Q43Leakage leak{};
    const BridgedTComponents c{};

    // Above the knee the transistor is effectively off, so the branch is the
    // plain series resistance and the drum sits at its nominal tuning.
    REQUIRE(std::fabs(pulp::signal::q43_collector_current(0.0, leak)) < 1e-9);
    REQUIRE(pulp::signal::q43_branch_resistance(-0.1, c, leak) == Catch::Approx(53.8e3).epsilon(0.005));

    // Below it the current grows and pulls the branch resistance down, which is
    // what raises the centre frequency.
    REQUIRE(pulp::signal::q43_collector_current(-1.5, leak) == Catch::Approx(-13.9e-6).epsilon(0.02));
    REQUIRE(pulp::signal::q43_branch_resistance(-1.5, c, leak) == Catch::Approx(37.4e3).epsilon(0.02));

    // Monotone all the way to the rails: a bigger negative swing must always
    // mean more leakage and a lower branch resistance, so the pitch bends one
    // way only. The loudest hits reach several volts here, well past where a
    // naive exp() overflows, and a model that saturates instead of staying
    // monotone would bend the pitch back down exactly on those hits.
    double previous = pulp::signal::q43_branch_resistance(-0.1, c, leak);
    for (double v = -0.2; v > -15.0; v -= 0.05) {
        const double r = pulp::signal::q43_branch_resistance(v, c, leak);
        INFO("Vcomm " << v << " branch resistance " << r << " previous " << previous);
        REQUIRE(r <= previous);
        previous = r;
    }
    // Deep below the knee the leakage is linear, so the branch tends to a
    // finite floor rather than collapsing toward zero.
    REQUIRE(pulp::signal::q43_branch_resistance(-15.0, c, leak) > c.r166);
}

// ---------------------------------------------------------------------------
// The Eq. 9 singularity. Vcomm crosses zero twice per ring cycle, so the pole
// at +17 uV is not a corner case -- it is on the hot path.
// ---------------------------------------------------------------------------

TEST_CASE("Eq. 9's singularity is guarded rather than merely clamped",
          "[bridged-t][stability]") {
    const BridgedTComponents c{};
    const Q43Leakage leak{};
    const double series = c.r165 + c.r166;

    SECTION("a clamp alone would emit the attack tuning near the pole") {
        // Just below the pole the raw expression is negative, and a lone clamp
        // maps negative to R166 -- the attack tuning. That is not a glitch that
        // reads as a glitch: it is a spurious one-sample jump an octave up.
        const double v = 1e-6;
        const double ic = pulp::signal::q43_collector_current(v, leak);
        const double raw = series * v / (v + c.r165 * ic);
        REQUIRE(raw < 0.0);
        REQUIRE(std::clamp(raw, c.r166, series) == c.r166);

        // The guard never evaluates the expression there at all.
        REQUIRE(pulp::signal::q43_branch_resistance(v, c, leak) == series);
    }

    SECTION("the guarded branch stays physical across the pole") {
        for (double v = -0.05; v <= 0.05; v += 1e-6) {
            const double r = pulp::signal::q43_branch_resistance(v, c, leak);
            REQUIRE(std::isfinite(r));
            REQUIRE(r >= c.r166);
            REQUIRE(r <= series);
        }
    }

    SECTION("the guard discards a fit artefact, not real behaviour") {
        // What the guard costs, measured at the threshold: a 0.2% correction to
        // the branch resistance, worth ~0.04 Hz of centre frequency.
        const double ic = pulp::signal::q43_collector_current(-0.01, leak);
        const double exact = series * -0.01 / (-0.01 + c.r165 * ic);
        REQUIRE(exact == Catch::Approx(series).epsilon(0.003));
        REQUIRE(exact < series);

        // What it buys: Eq. 9's own value is unbounded as Vcomm approaches the
        // pole, because Eq. 8's softplus tail reports leakage at a voltage
        // where a real Q43 is off. Inside the guard band the equation is
        // reporting the fit's tail, not the circuit -- by 1 mV it is already
        // 1.5% off, and it only gets worse from there.
        const double near_ic = pulp::signal::q43_collector_current(-1e-3, leak);
        const double near_pole = series * -1e-3 / (-1e-3 + c.r165 * near_ic);
        REQUIRE(near_pole < series * 0.99);
    }
}

TEST_CASE("no sample of a live ring escapes the physical branch range",
          "[bridged-t][stability]") {
    BassDrumVoice voice;
    voice.prepare(kFs);
    voice.set_decay(1.0);
    const BridgedTComponents c = voice.bridged_t_components();

    int out_of_range = 0;
    for (int n = 0; n < static_cast<int>(4.0 * kFs); ++n) {
        if (n == static_cast<int>(0.05 * kFs)) voice.trigger(kAccentMax);
        voice.process();
        const double r = voice.resonator().branch_resistance();
        if (!std::isfinite(r) || r < c.r166 - 1e-6 || r > c.r165 + c.r166 + 1e-6) {
            ++out_of_range;
        }
    }
    REQUIRE(out_of_range == 0);
}

// ---------------------------------------------------------------------------
// The pitch sigh. It is a parasitic leakage effect, not a pitch envelope: the
// centre frequency tracks the ring's own instantaneous amplitude, so it has to
// emerge from the loop and has to vanish when the leakage path is cut.
// ---------------------------------------------------------------------------

TEST_CASE("the pitch sigh emerges from leakage and vanishes when the path is cut",
          "[bridged-t][sigh]") {
    RenderSpec spec;
    spec.duration_s = 2.0;
    spec.accent = kAccentMax;

    spec.sigh = true;
    const auto with_sigh = cycle_frequencies(render(spec), 0.05, 0.35);
    spec.sigh = false;
    const auto without_sigh = cycle_frequencies(render(spec), 0.05, 0.35);

    REQUIRE(with_sigh.size() > 8);
    REQUIRE(without_sigh.size() > 8);

    // With leakage the pitch starts high and settles toward the nominal tuning
    // over a couple of hundred milliseconds.
    const double first = with_sigh.front().second;
    const double settled = with_sigh.back().second;
    REQUIRE(first > settled + 1.5);
    REQUIRE(settled == Catch::Approx(49.44).epsilon(0.02));

    // Monotone descent: the sigh follows the decaying ring, it does not wobble.
    for (std::size_t i = 1; i < with_sigh.size(); ++i) {
        REQUIRE(with_sigh[i].second <= with_sigh[i - 1].second + 0.05);
    }

    // Cut the leakage and the trajectory is flat from the first cycle. Nothing
    // else in the circuit moves the pitch, which is the whole claim.
    const double flat_first = without_sigh.front().second;
    const double flat_last = without_sigh.back().second;
    REQUIRE(std::fabs(flat_first - flat_last) < 0.25);
    REQUIRE(flat_last == Catch::Approx(49.44).epsilon(0.02));
}

TEST_CASE("sigh depth tracks strike amplitude", "[bridged-t][sigh]") {
    // A bigger ring drives Vcomm further below Q43's knee, leaks more, and
    // pulls the centre frequency higher. There is no path by which accent
    // reaches the pitch other than the ring's own amplitude.
    auto early_mean_f = [](double accent) {
        RenderSpec spec;
        spec.duration_s = 2.0;
        spec.accent = accent;
        // Start after the attack shunt has released, so this measures the sigh
        // rather than the separate frequency jump.
        return mean_frequency(render(spec), 0.060, 0.150);
    };

    const double soft = early_mean_f(kAccentMin);
    const double medium = early_mean_f(9.0);
    const double hard = early_mean_f(kAccentMax);

    REQUIRE(soft < medium);
    REQUIRE(medium < hard);
    REQUIRE(hard > soft + 1.5);
    // A soft hit barely leaves the nominal tuning.
    REQUIRE(soft == Catch::Approx(49.44).epsilon(0.01));
}

TEST_CASE("instantaneous centre frequency swings within each cycle",
          "[bridged-t][sigh]") {
    // The sigh is driven by Vcomm sample by sample, and Vcomm swings through
    // zero every cycle, so the centre frequency oscillates within the cycle and
    // the excursion damps out as the note decays. The measured envelope is what
    // an instantaneous-frequency estimate of the real circuit shows.
    BassDrumVoice voice;
    voice.prepare(kFs);
    voice.set_decay(0.5);

    double fc_min = 1e9, fc_max = 0.0;
    for (int n = 0; n < static_cast<int>(1.0 * kFs); ++n) {
        if (n == static_cast<int>(0.05 * kFs)) voice.trigger(kAccentMax);
        voice.process();
        const double t = static_cast<double>(n) / kFs;
        if (t >= 0.060 && t <= 0.150) {
            const double fc = voice.resonator().center_frequency_hz();
            fc_min = std::min(fc_min, fc);
            fc_max = std::max(fc_max, fc);
        }
    }
    // Floor is the nominal tuning: on the positive half-cycle Q43 is off.
    REQUIRE(fc_min == Catch::Approx(49.44).epsilon(0.01));
    REQUIRE(fc_max > 55.0);
    REQUIRE(fc_max < 60.0);
}

// ---------------------------------------------------------------------------
// Stability. R_eff is remodulated every sample at the ring frequency itself,
// which is the worst case for any realization that stores state whose meaning
// depends on the coefficients.
// ---------------------------------------------------------------------------

TEST_CASE("nominal parts decay at maximum decay", "[bridged-t][stability]") {
    // The discriminating case. At full decay the network dissipates only ~3%
    // per cycle, so a realization that perturbs stored energy on each
    // coefficient swap pumps harder than the circuit damps and the drum latches
    // into a limit cycle at the rails -- a note that never ends, at the wrong
    // pitch. One soft hit into nominal components must simply ring down.
    RenderSpec spec;
    spec.duration_s = 12.0;
    spec.decay = 1.0;
    spec.accent = kAccentMin;
    const auto y = render(spec);

    REQUIRE(all_finite(y));
    REQUIRE(peak(y) < 15.0);

    // Monotone ring-down over eight seconds, ending in silence.
    REQUIRE(rms(y, 5.0, 5.5) < rms(y, 3.0, 3.5) * 0.1);
    REQUIRE(rms(y, 7.5, 8.0) < rms(y, 5.0, 5.5) * 0.1);
    REQUIRE(rms(y, 11.5, 12.0) < 1e-6);

    // And it decays at the rate the linear analysis predicts, not merely
    // "eventually": sigma = 1.84 Np/s, T60 = 3.76 s.
    REQUIRE(decay_sigma(y, 1.0, 2.0) == Catch::Approx(1.84).epsilon(0.05));
}

TEST_CASE("decay knob reproduces the predicted ring-down times",
          "[bridged-t][stability]") {
    // Independently predicted closed-loop T60 for each knob position. Measured
    // with a soft strike and late windows so the leakage's own dissipation is
    // spent and this compares against the linear prediction it was drawn from.
    struct Point { double k; double t60; };
    const Point points[] = {{0.25, 0.46}, {0.5, 0.99}, {0.75, 1.90}, {1.0, 3.76}};

    for (const auto& p : points) {
        RenderSpec spec;
        spec.duration_s = 4.0 * p.t60 + 1.0;
        spec.decay = p.k;
        spec.accent = kAccentMin;
        const auto y = render(spec);
        REQUIRE(all_finite(y));

        const double t0 = 0.05 + 0.3 * p.t60;
        const double t1 = 0.05 + 0.8 * p.t60;
        const double measured = 6.907755 / decay_sigma(y, t0, t1, 0.1 * p.t60);
        INFO("decay knob " << p.k << " predicted T60 " << p.t60 << " measured " << measured);
        REQUIRE(measured == Catch::Approx(p.t60).epsilon(0.15));
    }
}

TEST_CASE("a retrigger storm stays bounded and rings down", "[va-drum][stability]") {
    RenderSpec spec;
    spec.duration_s = 16.0;
    spec.decay = 1.0;
    spec.accent = kAccentMax;
    spec.hit_times_s.clear();
    for (int i = 0; i < 80; ++i) spec.hit_times_s.push_back(0.05 + i * 0.025);

    const auto y = render(spec);
    REQUIRE(all_finite(y));
    // The op-amp clips are the bounded-output backstop: whatever the
    // coefficients do, no signal can exceed the rails.
    REQUIRE(peak(y) <= 15.0);
    // And once the hits stop it still returns to silence.
    REQUIRE(rms(y, 15.5, 16.0) < 1e-6);
}

TEST_CASE("knob sweeps during a live ring stay stable", "[va-drum][stability]") {
    // The fixed blocks are direct-form and their coefficients move whenever a
    // knob does, so a knob sweep is coefficient modulation inside the same
    // regenerative loop -- just at block rate rather than sample rate. Swept at
    // maximum decay, where the loop has the least margin to spare.
    auto sweep = [](auto knob) {
        BassDrumVoice voice;
        voice.prepare(kFs);
        voice.set_decay(1.0);
        std::vector<double> out;
        out.reserve(static_cast<std::size_t>(12.0 * kFs));
        int block = 0;
        for (int n = 0; n < static_cast<int>(12.0 * kFs); ++n) {
            if (n % 64 == 0) knob(voice, block++);
            if (n == static_cast<int>(0.05 * kFs)) voice.trigger(kAccentMax);
            out.push_back(voice.process());
        }
        return out;
    };

    SECTION("decay knob slammed between its extremes") {
        const auto y = sweep([](BassDrumVoice& v, int b) { v.set_decay(b % 2 ? 1.0 : 0.0); });
        REQUIRE(all_finite(y));
        REQUIRE(peak(y) <= 15.0);
        REQUIRE(rms(y, 11.5, 12.0) < 1e-6);
    }

    SECTION("tune swept across two octaves") {
        const auto y = sweep([](BassDrumVoice& v, int b) {
            v.set_tune(1.0 + 0.9 * std::sin(b * 0.5));
        });
        REQUIRE(all_finite(y));
        REQUIRE(peak(y) <= 15.0);
        REQUIRE(rms(y, 11.5, 12.0) < 1e-6);
    }
}

// ---------------------------------------------------------------------------
// Retrigger. The state is the instrument: a trigger injects into a resonator
// that is still ringing, so hits interfere and no two are alike.
// ---------------------------------------------------------------------------

TEST_CASE("a retrigger superposes onto the live ring", "[va-drum][retrigger]") {
    // Render each hit alone and both together. If the voice were linear and
    // memoryless across triggers, the pair would equal the sum of the solos.
    // The interaction residual is what that model fails to explain.
    //
    // This is the test that catches a voice which resets state on note-on or
    // allocates per hit: either gives a residual of exactly zero at every gap,
    // because each hit would then be independent. It is also the test that
    // catches the opposite error -- a residual that never decays would mean
    // hits keep interacting long after the first has died.
    struct Point { double gap_s; double residual; };
    std::vector<Point> measured;

    for (double gap : {0.020, 0.060, 0.150, 0.400, 1.200}) {
        RenderSpec base;
        base.duration_s = 4.0 + gap;
        base.decay = 0.5;
        base.accent = 10.0;

        RenderSpec solo1 = base;
        solo1.hit_times_s = {0.05};
        RenderSpec solo2 = base;
        solo2.hit_times_s = {0.05 + gap};
        RenderSpec both = base;
        both.hit_times_s = {0.05, 0.05 + gap};

        const auto a = render(solo1);
        const auto b = render(solo2);
        const auto pair = render(both);

        std::vector<double> interaction(pair.size());
        for (std::size_t i = 0; i < pair.size(); ++i) interaction[i] = pair[i] - (a[i] + b[i]);

        measured.push_back({gap, l2_norm(interaction) / l2_norm(pair)});
        INFO("gap " << gap * 1000 << " ms residual " << measured.back().residual);
    }

    // Close together, the second hit lands in a loud ring and the two are
    // deeply entangled.
    REQUIRE(measured[0].residual > 0.5);
    // Strictly weaker as the first hit has more time to die away.
    for (std::size_t i = 1; i < measured.size(); ++i) {
        REQUIRE(measured[i].residual < measured[i - 1].residual);
    }
    // Once the first hit is fully decayed there is nothing left to interact
    // with, and superposition holds again. This is the negative control: it
    // proves the residual above is genuine interaction and not a rendering or
    // bookkeeping artefact that would persist at any gap.
    REQUIRE(measured.back().residual < 0.01);
}

TEST_CASE("repeated notes do not machine-gun", "[va-drum][retrigger]") {
    // The failure this whole model exists to prevent, and the one a single
    // isolated hit can never reveal. Struck repeatedly, each note lands at a
    // different phase of the ring left by the last one, so no two come out
    // alike -- exactly as a real 808 behaves.
    //
    // A voice that clears its resonator on note-on converges instead: by the
    // fourth or fifth hit its notes are identical to each other, which is the
    // machine gun. That failure is what this measures, and it is why the
    // interaction test above cannot stand alone -- a resetting monophonic voice
    // truncates the previous tail, which produces a large decaying interaction
    // residual of its own and passes it.
    RenderSpec spec;
    constexpr double ioi = 0.060;
    spec.duration_s = 3.0;
    spec.decay = 0.5;
    spec.accent = 12.0;
    spec.hit_times_s.clear();
    for (int i = 0; i < 8; ++i) spec.hit_times_s.push_back(0.05 + i * ioi);
    const auto y = render(spec);
    REQUIRE(all_finite(y));

    // Compare each note against the next, once several hits have accumulated
    // and the ring is thoroughly populated.
    auto note_difference = [&](int index) {
        const auto a = static_cast<std::size_t>((0.05 + index * ioi) * kFs);
        const auto b = static_cast<std::size_t>((0.05 + (index + 1) * ioi) * kFs);
        const auto len = static_cast<std::size_t>(ioi * kFs);
        double diff = 0.0, energy = 0.0;
        for (std::size_t i = 0; i < len; ++i) {
            const double d = y[a + i] - y[b + i];
            diff += d * d;
            energy += y[a + i] * y[a + i];
        }
        return std::sqrt(diff / energy);
    };

    for (int i = 2; i <= 5; ++i) {
        const double d = note_difference(i);
        INFO("note " << i << " vs note " << i + 1 << " differ by " << d);
        REQUIRE(d > 0.05);
    }
}

TEST_CASE("a strike lands in the ring it finds, not a cleared one",
          "[va-drum][retrigger]") {
    // Two voices struck at the same instant with the same force. One has been
    // ringing since an earlier hit; the other starts from silence. If the
    // trigger is genuinely additive the two must diverge, because the first
    // voice's new excitation superposes onto state the second one does not
    // have. A voice that clears state on note-on renders them nearly alike.
    constexpr double gap = 0.060;
    BassDrumVoice ringing, fresh;
    ringing.prepare(kFs);
    fresh.prepare(kFs);
    for (auto* v : {&ringing, &fresh}) {
        v->set_decay(0.5);
        v->set_level(1.0);
        v->set_tone(0.5);
    }

    const auto second_hit = static_cast<int>((0.05 + gap) * kFs);
    const int total = static_cast<int>((0.05 + gap + 0.5) * kFs);
    double diff = 0.0, energy = 0.0;
    for (int n = 0; n < total; ++n) {
        if (n == static_cast<int>(0.05 * kFs)) ringing.trigger(12.0);
        if (n == second_hit) {
            ringing.trigger(12.0);
            fresh.trigger(12.0);
        }
        const double a = ringing.process();
        const double b = fresh.process();
        if (n >= second_hit) {
            diff += (a - b) * (a - b);
            energy += b * b;
        }
    }
    REQUIRE(std::sqrt(diff / energy) > 0.2);
}

// ---------------------------------------------------------------------------
// Accent. It is the strike force at the input of a nonlinear system, not a
// gain at its output.
// ---------------------------------------------------------------------------

TEST_CASE("accent gain null recovers a true gain and is blind to pitch",
          "[va-drum][accent]") {
    // The best-fit-gain null is a valid ruler for one question only: is the
    // whole difference between two renders a single scalar gain? It answers
    // that by least-squares over the raw waveform, which makes it exact for a
    // real gain and useless for anything that shifts phase.
    RenderSpec ref_spec;
    ref_spec.accent = kAccentMax;
    const auto reference = render(ref_spec);

    SECTION("a true output gain nulls to nothing") {
        // The level pot is a linear divider at the same accent, so pitch and
        // phase are identical and the null must recover the ratio exactly.
        RenderSpec quiet = ref_spec;
        quiet.level = 0.5;
        const auto null = best_fit_gain_null(render(quiet), reference);
        REQUIRE(null.gain == Catch::Approx(2.0).epsilon(1e-6));
        REQUIRE(null.residual < 1e-9);
    }

    SECTION("a pure pitch shift is not a gain but the raw null cannot tell") {
        // Two rings with identical envelope and amplitude differing only in
        // frequency by the size of an accent pitch shift. They are emphatically
        // not related by a gain, yet over a multi-cycle window they drift out of
        // phase and decorrelate, so the raw-waveform null reports a large
        // residual for a reason that has nothing to do with timbre. This is why
        // a raw-null residual must never be read as "how un-gain-like": it
        // conflates pitch with timbre. The artifact-free axes below replace it.
        const double fs = kFs;
        const int n = static_cast<int>(1.5 * fs);
        auto decaying_sine = [&](double f0) {
            std::vector<double> y(static_cast<std::size_t>(n));
            const double sigma = 6.9077623 / 0.8;
            for (int i = 0; i < n; ++i) {
                const double t = i / fs;
                y[static_cast<std::size_t>(i)] =
                    std::exp(-sigma * t) * std::sin(2.0 * M_PI * f0 * t);
            }
            return y;
        };
        const auto null = best_fit_gain_null(decaying_sine(46.7), decaying_sine(50.0));
        INFO("pure pitch shift raw-null residual " << null.residual);
        REQUIRE(null.residual > 0.5);
    }
}

TEST_CASE("accent changes the strike, not just its level", "[va-drum][accent]") {
    // Accent is the trigger pulse amplitude at the input of a nonlinear system.
    // Two independent, phase-free signatures separate it from a pure output
    // gain, and neither can be faked by the pitch decorrelation that defeats a
    // raw-waveform null. First: a louder strike rings louder, so amplitude
    // tracks accent. Second: a louder ring leaks more through R161, which
    // raises the centre frequency, so the tail pitch tracks accent upward --
    // and no gain can move a frequency. Both must hold, and both are measured
    // rather than asserted from the pulse-shaper story.
    struct Point { double accent, peak_amp, tail_f0; };
    std::vector<Point> pts;
    for (double accent : {4.0, 6.0, 8.0, 10.0, 12.0, 14.0}) {
        RenderSpec spec;
        spec.accent = accent;
        spec.duration_s = 2.0;
        const auto y = render(spec);
        // Late enough that the attack jump and pulse-shaper transient are gone
        // and only the resonator's own ring remains; wide enough that the
        // interpolated crossings resolve well under the 0.26% shift we expect.
        pts.push_back({accent, peak(y), mean_frequency(y, 0.1, 0.4)});
        INFO("accent " << accent << " peak " << pts.back().peak_amp
                       << " tail_f0 " << pts.back().tail_f0);
    }

    SECTION("amplitude tracks accent") {
        for (std::size_t i = 1; i < pts.size(); ++i)
            REQUIRE(pts[i].peak_amp > pts[i - 1].peak_amp);
    }

    SECTION("tail pitch rises with accent, which a gain cannot do") {
        // Monotone upward, and a real shift rather than estimator noise. The
        // magnitude here is small -- the real TR-808 shifts several times
        // further, which is a fidelity gap tracked against the bench reference,
        // not a regression this gate guards. What this gate guards is that the
        // R161 leakage coupling exists at all: break it and accent collapses to
        // a pure gain, the tail pitch goes flat, and this fails.
        for (std::size_t i = 1; i < pts.size(); ++i)
            REQUIRE(pts[i].tail_f0 > pts[i - 1].tail_f0);
        REQUIRE(pts.back().tail_f0 - pts.front().tail_f0 > 0.05);
    }
}

// ---------------------------------------------------------------------------
// Tuning by component substitution.
// ---------------------------------------------------------------------------

TEST_CASE("tuning scales frequency and leaves Q alone", "[va-drum]") {
    // With matched capacitive arms the bridged-T's Q depends only on the
    // resistances, so scaling the arms retunes the drum without touching its
    // character: the ring-down rate tracks the pitch exactly.
    struct Measurement { double f0; double q; };
    auto measure = [](double tune) {
        RenderSpec spec;
        spec.duration_s = 6.0;
        spec.tune = tune;
        spec.accent = kAccentMax;
        const auto y = render(spec);
        const double f0 = mean_frequency(y, 0.5, 1.5);
        const double sigma = decay_sigma(y, 0.5, 1.5);
        return Measurement{f0, kPi * f0 / sigma};
    };

    const auto half = measure(0.5);
    const auto unity = measure(1.0);
    const auto twice = measure(2.0);

    REQUIRE(half.f0 == Catch::Approx(unity.f0 * 0.5).epsilon(0.01));
    REQUIRE(twice.f0 == Catch::Approx(unity.f0 * 2.0).epsilon(0.01));

    // Q invariant across a two-octave span.
    REQUIRE(half.q == Catch::Approx(unity.q).epsilon(0.02));
    REQUIRE(twice.q == Catch::Approx(unity.q).epsilon(0.02));
}

// ---------------------------------------------------------------------------
// Realtime contract.
// ---------------------------------------------------------------------------

TEST_CASE("the voice neither allocates nor is disturbed by triggering",
          "[va-drum][rt-safety]") {
    BassDrumVoice voice;
    voice.prepare(kFs);
    voice.set_decay(0.75);

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 200; ++block) {
            // Parameter moves, triggers and rendering all happen on the audio
            // thread in a plugin, so none of them may allocate.
            voice.set_decay(0.5 + 0.004 * (block % 100));
            voice.set_tone(0.01 * (block % 100));
            voice.set_tune(0.5 + 0.01 * (block % 100));
            if (block % 8 == 0) voice.trigger(kAccentMin + (block % 10));
            for (int n = 0; n < 64; ++n) voice.process();
            voice.snap_denormals();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}
