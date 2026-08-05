// Does the behaviour measurement tell these signals apart?
//
// A measurement that always answers the same thing passes every check written
// against a single example, and the whole point of this one is to distinguish a
// held note from a melody. So the suite is built as a MATRIX, not a list: each
// signal carries the expectations that describe it, and `--prove` then runs
// every signal's expectations against every OTHER signal and requires each set
// to reject at least one of them. An expectation set that nothing can fail is
// reported as a defect in the test, which is the only way to find out that a
// green suite was measuring nothing.
//
// Nothing here needs Rack, an SDK, or an audio device — the signals are
// synthesised, so the answers are known in advance rather than eyeballed.
//
// Build:
//   clang++ -std=c++20 -O1 -o /tmp/test-patch-behaviour \
//       tools/rack/test_patch_behaviour.cpp -I core/signal/include \
//       -framework Accelerate

#include "patch_behaviour.hpp"
#include "patch_behaviour_json.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

constexpr double kSr = 48000.0;
constexpr double kSeconds = 6.0;
const std::size_t kN = static_cast<std::size_t>(kSr * kSeconds);

double hz_of(int semitone) { return 440.0 * std::pow(2.0, (semitone - 69) / 12.0); }

/// A steady tone. One pitch, one level, forever — the patch that passed the
/// audibility gate with a perfect score and was the bug.
std::vector<float> held_tone() {
    std::vector<float> x(kN);
    const double w = 2.0 * M_PI * hz_of(57) / kSr;   // A3, 220 Hz
    for (std::size_t i = 0; i < kN; ++i)
        x[i] = static_cast<float>(5.0 * std::sin(w * static_cast<double>(i)));
    return x;
}

/// Five pitches in sequence, each held long enough to hear, each re-attacked.
std::vector<float> five_note_sequence() {
    const int notes[] = {60, 62, 64, 65, 67};
    std::vector<float> x(kN, 0.f);
    const std::size_t per = kN / 5;
    for (int n = 0; n < 5; ++n) {
        const double w = 2.0 * M_PI * hz_of(notes[n]) / kSr;
        for (std::size_t i = 0; i < per; ++i) {
            // A short attack and a long body: a note, not a gate.
            const double t = static_cast<double>(i) / kSr;
            const double env = (1.0 - std::exp(-t / 0.01)) * std::exp(-t / 2.0);
            x[static_cast<std::size_t>(n) * per + i] =
                static_cast<float>(5.0 * env * std::sin(w * static_cast<double>(i)));
        }
    }
    return x;
}

/// Struck notes on one pitch at a steady tempo. Rhythm without melody.
std::vector<float> pulse_train(double period_ms, double jitter_ms = 0.0,
                               double accel = 0.0) {
    std::vector<float> x(kN, 0.f);
    const double w = 2.0 * M_PI * hz_of(45) / kSr;   // A2, so the decay is audible
    // A deterministic wobble: a test that reseeds a random generator is a test
    // whose failures nobody can reproduce.
    unsigned seed = 12345u;
    double t = 0.05, interval = period_ms / 1000.0;
    int k = 0;
    while (t < kSeconds - 0.2) {
        const std::size_t at = static_cast<std::size_t>(t * kSr);
        for (std::size_t i = 0; at + i < kN && i < static_cast<std::size_t>(0.25 * kSr); ++i) {
            const double dt = static_cast<double>(i) / kSr;
            const double env = std::exp(-dt / 0.06);
            x[at + i] += static_cast<float>(5.0 * env * std::sin(w * static_cast<double>(i)));
        }
        seed = seed * 1664525u + 1013904223u;
        const double wobble = jitter_ms / 1000.0 *
                              ((static_cast<double>(seed >> 16 & 0x7fff) / 32767.0) - 0.5) * 2.0;
        t += interval + wobble;
        interval *= (1.0 - accel);
        ++k;
    }
    return x;
}

/// One pitch, one level, a filter opening across the whole run. The thing
/// `spectrally_varying` names, and invisible to every other measure here.
std::vector<float> filter_sweep() {
    std::vector<float> x(kN);
    const double f0 = hz_of(45);
    double phase = 0.0, lp = 0.0;
    for (std::size_t i = 0; i < kN; ++i) {
        phase += f0 / kSr;
        if (phase >= 1.0) phase -= 1.0;
        const double saw = 2.0 * phase - 1.0;
        const double frac = static_cast<double>(i) / static_cast<double>(kN);
        const double cutoff = 200.0 * std::pow(6000.0 / 200.0, frac);
        const double a = 1.0 - std::exp(-2.0 * M_PI * cutoff / kSr);
        lp += a * (saw - lp);
        x[i] = static_cast<float>(5.0 * lp);
    }
    return x;
}

/// A tone that stops. `keeps_going` is the only measure that sees it.
std::vector<float> decaying_tone() {
    std::vector<float> x(kN);
    const double w = 2.0 * M_PI * hz_of(57) / kSr;
    for (std::size_t i = 0; i < kN; ++i) {
        const double t = static_cast<double>(i) / kSr;
        x[i] = static_cast<float>(5.0 * std::exp(-t / 0.8) * std::sin(w * static_cast<double>(i)));
    }
    return x;
}

std::vector<float> silence() { return std::vector<float>(kN, 0.f); }

// ── expectations ────────────────────────────────────────────────────────────

struct Check {
    std::string what;
    std::function<bool(const patch_behaviour::Behaviour&)> ok;
    std::function<std::string(const patch_behaviour::Behaviour&)> saw;
};

std::string i2s(int v) { return std::to_string(v); }
std::string d2s(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

struct Case {
    std::string name;
    std::vector<float> signal;
    std::vector<Check> checks;
};

int failures = 0;

void report(bool ok, const std::string& line) {
    if (ok) { std::printf("  ok     %s\n", line.c_str()); return; }
    std::printf("  WRONG  %s\n", line.c_str());
    ++failures;
}

}  // namespace

int main(int argc, char** argv) {
    const bool prove = argc > 1 && std::strcmp(argv[1], "--prove") == 0;
    const bool dump = argc > 1 && std::strcmp(argv[1], "--dump") == 0;

    patch_behaviour::Settings s;
    s.sample_rate = kSr;

    using B = patch_behaviour::Behaviour;
    std::vector<Case> cases = {
        {"held tone", held_tone(), {
            {"one pitch and no changes",
             [](const B& b) { return b.pitch.distinct_pitches == 1 && b.pitch.pitch_changes == 0; },
             [](const B& b) { return i2s(b.pitch.distinct_pitches) + " pitches, " +
                                     i2s(b.pitch.pitch_changes) + " changes"; }},
            {"voiced throughout",
             [](const B& b) { return b.pitch.voiced_fraction > 0.9; },
             [](const B& b) { return d2s(b.pitch.voiced_fraction); }},
            {"the pitch it was given",
             [](const B& b) { return std::fabs(b.pitch.median_hz - 220.0) < 3.0; },
             [](const B& b) { return d2s(b.pitch.median_hz) + " Hz"; }},
            {"never stops",
             [](const B& b) { return b.dynamics.end_over_peak > 0.9 && b.dynamics.duty_cycle > 0.99; },
             [](const B& b) { return d2s(b.dynamics.end_over_peak) + "x, duty " +
                                     d2s(b.dynamics.duty_cycle); }},
            {"no rhythm to find",
             [](const B& b) { return b.onsets.onsets <= 1; },
             [](const B& b) { return i2s(b.onsets.onsets) + " onsets"; }},
            {"brightness does not move",
             [](const B& b) { return b.spectrum.centroid_range_octaves < 0.2; },
             [](const B& b) { return d2s(b.spectrum.centroid_range_octaves) + " octaves"; }},
        }},
        {"five-note sequence", five_note_sequence(), {
            {"five distinct pitches",
             [](const B& b) { return b.pitch.distinct_pitches == 5; },
             [](const B& b) { return i2s(b.pitch.distinct_pitches); }},
            {"four changes between them",
             [](const B& b) { return b.pitch.pitch_changes == 4; },
             [](const B& b) { return i2s(b.pitch.pitch_changes); }},
            {"a seven-semitone span",
             [](const B& b) { return b.pitch.semitone_range == 7; },
             [](const B& b) { return i2s(b.pitch.semitone_range); }},
            {"one attack per note",
             [](const B& b) { return b.onsets.onsets >= 4 && b.onsets.onsets <= 6; },
             [](const B& b) { return i2s(b.onsets.onsets); }},
        }},
        {"steady pulse train", pulse_train(500.0), {
            {"one pitch, struck repeatedly",
             [](const B& b) { return b.pitch.distinct_pitches == 1; },
             [](const B& b) { return i2s(b.pitch.distinct_pitches); }},
            {"an attack for every strike",
             [](const B& b) { return b.onsets.onsets >= 10; },
             [](const B& b) { return i2s(b.onsets.onsets); }},
            {"evenly spaced",
             [](const B& b) { return b.onsets.interval_cv < 0.1; },
             [](const B& b) { return d2s(b.onsets.interval_cv); }},
            {"a pulse the autocorrelation finds, at the tempo it was given",
             [](const B& b) { return b.onsets.periodicity > 0.4 &&
                                     std::fabs(b.onsets.period_ms - 500.0) < 40.0; },
             [](const B& b) { return d2s(b.onsets.periodicity) + " at " +
                                     d2s(b.onsets.period_ms) + " ms"; }},
            {"gaps between the strikes",
             [](const B& b) { return b.dynamics.duty_cycle < 0.9; },
             [](const B& b) { return d2s(b.dynamics.duty_cycle); }},
        }},
        {"jittered pulse train", pulse_train(500.0, 260.0), {
            {"unevenly spaced",
             [](const B& b) { return b.onsets.interval_cv > 0.2; },
             [](const B& b) { return d2s(b.onsets.interval_cv); }},
            {"still plainly a stream of attacks",
             [](const B& b) { return b.onsets.onsets >= 8; },
             [](const B& b) { return i2s(b.onsets.onsets); }},
            // Uneven is not the same as speeding up, and the interval CV alone
            // cannot tell them apart -- an accelerating train has a high CV
            // too. Only the trend separates them, which is why both numbers are
            // measured and why a caller wanting `varies_timing` has to read
            // both.
            {"varying rather than accelerating",
             [](const B& b) { return b.onsets.interval_trend > -0.4; },
             [](const B& b) { return d2s(b.onsets.interval_trend); }},
            {"no pulse underneath",
             [](const B& b) { return b.onsets.periodicity < 0.4; },
             [](const B& b) { return d2s(b.onsets.periodicity); }},
        }},
        {"accelerating pulse train", pulse_train(700.0, 0.0, 0.12), {
            {"intervals shrinking across the run",
             [](const B& b) { return b.onsets.interval_trend < -0.4; },
             [](const B& b) { return d2s(b.onsets.interval_trend); }},
            {"more attacks than a steady train at the same starting tempo",
             [](const B& b) { return b.onsets.onsets >= 10; },
             [](const B& b) { return i2s(b.onsets.onsets); }},
        }},
        {"filter sweep", filter_sweep(), {
            {"brightness climbing across the run",
             [](const B& b) { return b.spectrum.centroid_trend > 0.5; },
             [](const B& b) { return d2s(b.spectrum.centroid_trend); }},
            {"more than an octave of it",
             [](const B& b) { return b.spectrum.centroid_range_octaves > 1.0; },
             [](const B& b) { return d2s(b.spectrum.centroid_range_octaves); }},
            {"one pitch throughout",
             [](const B& b) { return b.pitch.distinct_pitches == 1; },
             [](const B& b) { return i2s(b.pitch.distinct_pitches); }},
        }},
        {"decaying tone", decaying_tone(), {
            {"a fraction of its peak left at the end",
             [](const B& b) { return b.dynamics.end_over_peak < 0.1; },
             [](const B& b) { return d2s(b.dynamics.end_over_peak); }},
            {"level falling across the run",
             [](const B& b) { return b.dynamics.rms_trend < -0.5; },
             [](const B& b) { return d2s(b.dynamics.rms_trend); }},
        }},
        {"silence", silence(), {
            {"nothing measured, nothing invented",
             [](const B& b) { return b.pitch.distinct_pitches == 0 && b.onsets.onsets == 0 &&
                                     b.dynamics.peak_rms == 0.0 && b.spectrum.active_windows == 0; },
             [](const B& b) { return i2s(b.pitch.distinct_pitches) + " pitches, " +
                                     i2s(b.onsets.onsets) + " onsets"; }},
            {"every number finite",
             [](const B& b) { return std::isfinite(b.dynamics.rms_trend) &&
                                     std::isfinite(b.onsets.periodicity) &&
                                     std::isfinite(b.spectrum.centroid_mean_hz) &&
                                     std::isfinite(b.pitch.voiced_fraction); },
             [](const B&) { return std::string("a NaN or Inf reached the report"); }},
        }},
    };

    std::map<std::string, B> measured;
    for (const Case& c : cases)
        measured[c.name] = patch_behaviour::measure(c.signal, s, c.name);

    if (dump) {
        for (const Case& c : cases)
            std::printf("%s", patch_behaviour::cable_summary(measured[c.name]).c_str());
        std::vector<B> all;
        for (const Case& c : cases) all.push_back(measured[c.name]);
        std::printf("%s%s\n", patch_behaviour::kJsonMarker,
                    patch_behaviour::report_json(all, s, false).c_str());
        return 0;
    }

    for (const Case& c : cases) {
        const B& b = measured[c.name];
        for (const Check& chk : c.checks)
            report(chk.ok(b), c.name + ": " + chk.what + " (" + chk.saw(b) + ")");
    }

    if (!prove) {
        std::printf("\n%s: %d wrong\n", failures ? "FAILED" : "behaviour measurement agrees",
                    failures);
        return failures ? 1 : 0;
    }

    // THE PROOF THAT THESE CHECKS CAN FAIL.
    //
    // Every expectation above is run against every OTHER signal. A set that
    // nothing rejects describes no signal in particular, and a suite built from
    // sets like that goes green whatever the measurement does — which is the
    // failure mode this whole lane exists to stop repeating.
    std::printf("\nproving each expectation set can reject something:\n");
    int weak = 0;
    for (const Case& c : cases) {
        int rejected = 0;
        std::string rejected_by;
        std::string accepted;
        for (const Case& other : cases) {
            if (other.name == c.name) continue;
            const B& b = measured[other.name];
            bool any = false;
            for (const Check& chk : c.checks) {
                if (chk.ok(b)) continue;
                any = true;
                if (rejected_by.empty()) rejected_by = other.name + " fails \"" + chk.what + "\"";
                break;
            }
            if (any) { ++rejected; continue; }
            if (!accepted.empty()) accepted += ", ";
            accepted += other.name;
        }
        if (!accepted.empty()) {
            ++weak;
            std::printf("  WEAK   \"%s\" is satisfied by %s as well, so it does not "
                        "describe this signal in particular\n",
                        c.name.c_str(), accepted.c_str());
        } else {
            std::printf("  ok     \"%s\" rejects all %d other signals (%s)\n",
                        c.name.c_str(), rejected, rejected_by.c_str());
        }
    }
    std::printf("\n%s: %d wrong, %d non-discriminating\n",
                (failures || weak) ? "FAILED" : "behaviour measurement agrees and discriminates",
                failures, weak);
    return (failures || weak) ? 1 : 0;
}
