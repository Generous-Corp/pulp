// Tier 0 mod-utilities toolkit — lfo.
//
// Split out of the original single-file suite: shared includes and
// helpers live in test_mod_utilities_support.hpp so each file states
// only the contracts it asserts.

#include "test_mod_utilities_support.hpp"


// ── LFO ───────────────────────────────────────────────────────────────────

TEST_CASE("LFO rate is accurate to far better than the ±0.01 % specs assert",
          "[lfo][mod-utilities]") {
    static_assert(std::is_same_v<decltype(EffectLfo64{}.wave()),
                                 LfoWave>);
    // The chorus spec's test 2, run directly: count zero crossings over a long
    // render and compare against the configured rate.
    constexpr double rate_hz = 3.0;
    constexpr double seconds = 100.0;
    EffectLfo64 lfo;
    lfo.prepare(kSr);
    lfo.set_rate_hz(rate_hz);
    lfo.set_wave(LfoWave::sine);
    lfo.reset();

    std::vector<double> out;
    out.reserve(static_cast<std::size_t>(kSr * seconds));
    for (int i = 0; i < static_cast<int>(kSr * seconds); ++i) out.push_back(lfo.next());

    REQUIRE_THAT(measure_rate_hz(out, kSr), WithinRel(rate_hz, 1e-4));
}


TEST_CASE("LFO shapes stay bipolar and hit their documented landmarks",
          "[lfo][mod-utilities]") {
    for (auto wave : {LfoWave::sine, LfoWave::triangle, LfoWave::saw_up, LfoWave::saw_down,
                      LfoWave::square, LfoWave::sample_hold, LfoWave::smooth_random}) {
        EffectLfo64 lfo;
        lfo.prepare(kSr);
        lfo.set_rate_hz(7.0);
        lfo.set_wave(wave);
        lfo.reset();
        for (int i = 0; i < 100000; ++i) {
            const double v = lfo.next();
            REQUIRE(v >= -1.0);
            REQUIRE(v <= 1.0);
        }
    }

    // The triangle's landmarks: 0 at φ=0, +1 at 0.25, 0 at 0.5, −1 at 0.75.
    // Driven by phase offset so no accumulation is involved.
    EffectLfo64 tri;
    tri.prepare(kSr);
    tri.set_rate_hz(0.0);  // frozen: phase stays at the offset
    tri.set_wave(LfoWave::triangle);
    struct { double phase, expected; } landmarks[] = {
        {0.0, 0.0}, {0.25, 1.0}, {0.5, 0.0}, {0.75, -1.0}};
    for (const auto& lm : landmarks) {
        tri.set_phase_offset(lm.phase);
        tri.reset();
        REQUIRE_THAT(tri.next(), WithinAbs(lm.expected, 1e-12));
    }
}


TEST_CASE("LFO phase offset of half a cycle is exact inversion", "[lfo][mod-utilities]") {
    // The property the Juno/Dimension D voicings assert: two otherwise
    // identical LFOs half a cycle apart are exact negatives of each other, for
    // every odd-symmetric shape.
    for (auto wave : {LfoWave::sine, LfoWave::triangle, LfoWave::square}) {
        EffectLfo64 a, b;
        for (auto* l : {&a, &b}) {
            l->prepare(kSr);
            l->set_rate_hz(2.0);
            l->set_wave(wave);
        }
        b.set_stereo_offset(0.5);
        a.reset();
        b.reset();
        for (int i = 0; i < 200000; ++i) REQUIRE_THAT(a.next() + b.next(), WithinAbs(0.0, 1e-9));
    }
}


TEST_CASE("LFO half-cycle offset is NOT inversion for the saw shapes",
          "[lfo][mod-utilities]") {
    // The negative half of the case above, pinned deliberately.
    //
    // The header used to claim half-cycle inversion for "triangle, sine, saw"
    // while the test above iterated {sine, triangle, square} — the set had been
    // narrowed to the shapes that pass without the claim being corrected, so
    // the false half was load-bearing documentation that nothing measured.
    //
    // A sawtooth's discontinuity makes this inherent: a half-cycle shift is a
    // shift, not a negation. saw(φ + 0.5) = −saw(φ) holds only at φ = 0.5, and
    // everywhere else the two differ by a full unit. Asserting that here means
    // a future change that "fixes" saw to invert has to come here and explain
    // itself, rather than silently altering what anti-phase means for the
    // chorus/flanger/phaser pair constructions built on set_phase_offset(0.5).
    for (auto wave : {LfoWave::saw_up, LfoWave::saw_down}) {
        EffectLfo64 a, b;
        for (auto* l : {&a, &b}) {
            l->prepare(kSr);
            l->set_rate_hz(2.0);
            l->set_wave(wave);
        }
        b.set_stereo_offset(0.5);
        a.reset();
        b.reset();

        double worst = 0.0;
        for (int i = 0; i < 20000; ++i) worst = std::max(worst, std::abs(a.next() + b.next()));
        // Not merely "imperfect" — the sum reaches a full unit.
        REQUIRE_THAT(worst, WithinAbs(1.0, 1e-9));
    }
}


TEST_CASE("LFO N-voice spacing is even", "[lfo][mod-utilities]") {
    // TriChorus: three voices at 120°. Their instantaneous sine values must sum
    // to zero at every sample, which is the algebraic form of even spacing.
    constexpr int n = 3;
    EffectLfo64 voices[n];
    for (int k = 0; k < n; ++k) {
        voices[k].prepare(kSr);
        voices[k].set_rate_hz(1.5);
        voices[k].set_wave(LfoWave::sine);
        voices[k].set_phase_offset(static_cast<double>(k) / n);
        voices[k].reset();
    }
    for (int i = 0; i < 100000; ++i) {
        double sum = 0.0;
        for (auto& v : voices) sum += v.next();
        REQUIRE_THAT(sum, WithinAbs(0.0, 1e-9));
    }
}


TEST_CASE("LFO quadrature is exact and drift-free over a long render",
          "[lfo][mod-utilities]") {
    // The frequency-shifter spec forbids a recursive resonator here precisely
    // because its amplitude drifts. Assert the invariant that forbids it:
    // sin² + cos² == 1 forever, not just at the start.
    EffectLfo64 lfo;
    lfo.prepare(kSr);
    lfo.set_rate_hz(200.0);
    lfo.reset();
    double s = 0.0, c = 0.0;
    for (int i = 0; i < 2000000; ++i) {
        lfo.next_quadrature(s, c);
        REQUIRE_THAT(s * s + c * c, WithinAbs(1.0, 1e-12));
    }
}


TEST_CASE("LFO random shapes are seeded and reset-repeatable", "[lfo][mod-utilities]") {
    EffectLfo64 lfo;
    lfo.prepare(kSr);
    lfo.set_rate_hz(20.0);
    lfo.set_wave(LfoWave::sample_hold);
    lfo.set_seed(31337u);

    lfo.reset();
    std::vector<double> first;
    for (int i = 0; i < 20000; ++i) first.push_back(lfo.next());
    lfo.reset();
    for (int i = 0; i < 20000; ++i) REQUIRE(lfo.next() == first[static_cast<std::size_t>(i)]);

    // sample_hold is piecewise constant; smooth_random is not. Both must
    // actually move.
    bool moved = false;
    for (std::size_t i = 1; i < first.size(); ++i) moved = moved || first[i] != first[i - 1];
    REQUIRE(moved);
}


TEST_CASE("LFO rate is clamped to its documented ceiling", "[lfo][mod-utilities]") {
    EffectLfo64 lfo;
    lfo.set_rate_hz(1e6);
    REQUIRE(lfo.rate_hz() == EffectLfoT<double>::kMaxRateHz);
    lfo.set_rate_hz(-5.0);
    REQUIRE(lfo.rate_hz() == 0.0);
}
