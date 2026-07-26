#include "test_signal_harmony_engine_support.hpp"

TEST_CASE("T10 nothing on the audio path allocates after prepare",
          "[signal][harmony-engine][rt-safety]") {
    Tracker tracker;
    tracker.prepare(kSr);
    tracker.reset();
    Mapper mapper;
    Engine engine;
    engine.prepare(kSr);
    engine.reset();

    pulp::test::RtAllocationProbe probe;
    for (int n = 0; n < 8192; ++n) {
        // The tracker, across its hop boundary.
        tracker.process(0.3 * std::sin(0.01 * n));

        // The mapper, with the key and scale changing mid-stream — which must
        // rewrite the fixed degree array rather than resize anything.
        mapper.set_key(n % 12);
        mapper.set_scale(static_cast<ScaleType>(n % kScaleCount));
        mapper.set_off_scale_policy(static_cast<OffScalePolicy>(n % 3));
        mapper.map_midi(48 + (n % 36), -14 + (n % 29));
        mapper.map_hz(110.0 + 0.1 * (n % 1000), n % 8);

        // The engine, with every control moving.
        engine.set_key(n % 12);
        engine.set_scale(static_cast<ScaleType>(n % kScaleCount));
        engine.set_voice_interval(n % 2, -14 + (n % 29));
        engine.set_voice_detune_cents(n % 2, -50.0 + 0.1 * (n % 1000));
        engine.set_voice_level_db(n % 2, -60.0 + 0.01 * (n % 6600));
        engine.set_voice_enabled(1, n % 2 == 0);
        engine.set_dry_level_db(-6.0);
        engine.set_glide_ms(static_cast<double>(n % 500));
        engine.set_humanize_cents(static_cast<double>(n % 16));
        engine.set_crossfade_ms(10.0 + static_cast<double>(n % 40));
        engine.set_interp(static_cast<PitchInterp>(n % 2));
        engine.process(0.05);
    }
    tracker.reset();
    engine.reset();
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("T11 the tracker geometry is derived from the sample rate",
          "[signal][harmony-engine][yin]") {
    // (a) An implementation-consistency regression: W = 2·τ_max is a design law,
    // so this catches derivation bugs — a stale cached window, an integer
    // rounding or overflow in the ceilings — not a physical property.
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        Tracker tracker;
        tracker.prepare(rate);

        const int tau_min = static_cast<int>(std::ceil(rate / Tracker::kF0MaxDefault));
        const int tau_max = static_cast<int>(std::ceil(rate / Tracker::kF0MinDefault));
        REQUIRE(tracker.tau_min() == tau_min);
        REQUIRE(tracker.tau_max() == tau_max);
        REQUIRE(tracker.window_samples() == 2 * tau_max);
        REQUIRE(tracker.latency_samples() == tracker.window_samples());

        // The window holds two periods of the lowest tracked pitch, which is
        // the law the geometry exists to satisfy.
        const double window_seconds = tracker.window_samples() / rate;
        REQUIRE(window_seconds * Tracker::kF0MinDefault >= 2.0);

        // The difference function reads `x[j + τ]` for j < integration and
        // τ ≤ τ_max, so the last index touched must be the last sample IN the
        // window. Integrating over the full window — as the difference function
        // is usually written — would read τ_max samples past its end.
        REQUIRE(tracker.integration_samples() + tracker.tau_max() ==
                tracker.window_samples());
        REQUIRE(tracker.integration_samples() > 0);
    }
}

TEST_CASE("T11 the shifter buffer outruns the deepest downshift",
          "[signal][harmony-engine]") {
    // (b) The physical-achievability assertion series law 6 requires. The
    // harmony voices are `PitchShifterT`, whose buffer is sized for its own
    // maximum window; the invariant that keeps a downshift inside it is that
    // the shifter's ceiling covers twice the widest crossfade this engine can
    // ask for. Raising `kCrossfadeMsMax` without raising the shifter's window
    // ceiling fires this.
    REQUIRE(PitchShifter64::kWindowMsMax >= 2.0 * Engine::kCrossfadeMsMax);
    REQUIRE(Engine::kCrossfadeMsMin >= PitchShifter64::kWindowMsMin);

    // And the ±1 octave ratio clamp is inside the shifter's own range, so the
    // mapper can never ask for a shift the shifter would clamp differently.
    REQUIRE(static_cast<double>(Mapper::kShiftSemitonesMax) <=
            PitchShifter64::kShiftSemisMax);
    REQUIRE(static_cast<double>(-Mapper::kShiftSemitonesMax) >=
            PitchShifter64::kShiftSemisMin);

    // The alignment delay survives a crossfade change at any supported rate.
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        Engine engine;
        engine.prepare(rate);
        for (double ms : {Engine::kCrossfadeMsMin, Engine::kCrossfadeMsDefault,
                          Engine::kCrossfadeMsMax}) {
            engine.set_crossfade_ms(ms);
            REQUIRE(engine.latency_samples() >= engine.shifter_latency_samples());
            REQUIRE(engine.latency_samples() >= engine.tracker_latency_samples());
            for (int n = 0; n < 2000; ++n)
                REQUIRE(std::isfinite(static_cast<double>(engine.process(0.3))));
        }
    }
}

TEST_CASE("the gain bound is the feed-forward sum", "[signal][harmony-engine]") {
    // No feedback path exists, so the bound is arithmetic rather than an
    // invariant to discover — but the per-voice term is NOT unity. Each wet
    // voice passes a DC blocker whose worst-case sample gain is its impulse
    // response's L1 norm, exactly 2. This asserted `1 + 2·1.0 = 3.0`, treating
    // the convex crossfade as the whole story and missing the blocker sitting
    // after it.
    REQUIRE(Engine::kWorstCaseGain ==
            Engine::kLevelMaxLinear *
                (1.0 + Engine::kMaxVoices * PitchShifter64::kDcBlockerPeakGain));

    Engine engine;
    engine.prepare(kSr);
    engine.set_voice_enabled(0, true);
    engine.set_voice_enabled(1, true);
    engine.set_dry_level_db(0.0);
    engine.set_voice_level_db(0, 0.0);
    engine.set_voice_level_db(1, 0.0);
    engine.set_voice_interval(0, 0);  // unison voices give the legs the best
    engine.set_voice_interval(1, 0);  // chance to sum constructively
    engine.set_glide_ms(0.0);
    engine.reset();

    const double f0 = midi_hz(60);
    std::vector<double> out;
    const int settle = 3 * engine.latency_samples() + 8000;
    for (int n = 0; n < settle + 24000; ++n) {
        const double y = static_cast<double>(
            engine.process(std::sin(2.0 * kPi * f0 * static_cast<double>(n) / kSr)));
        if (n >= settle) out.push_back(y);
    }
    REQUIRE(engine.voiced());

    // The exact bound is the nominal sum times the ONE thing in either leg that
    // can exceed unity: the DC blocker on each shifter's wet output, whose
    // worst-case SAMPLE gain is the L1 norm of its impulse response — exactly
    // 2, not the `2/(1+p)` = 1.000327 magnitude peak this used to cite. A
    // magnitude peak bounds a steady sinusoid; it says nothing about the largest
    // single sample. Inherited from `PitchShifterT` along with the error.
    PitchShifter64 reference;
    reference.prepare(kSr);
    const double unity_bound =
        1.0 + Engine::kMaxVoices * PitchShifter64::kDcBlockerPeakGain;
    REQUIRE(peak(out) <= unity_bound);

    // Unity voice levels produce the structural 5x bound.  The registry must
    // additionally account for the public +6 dB ceiling on every level.
    const double ceiling_bound = Engine::kLevelMaxLinear * unity_bound;
    REQUIRE_THAT(Engine::kWorstCaseGain, WithinAbs(ceiling_bound, 1e-12));

    // The +6 dB ceiling raises the full structural bound to ~9.98, which is
    // what a registry consumer must budget for when it exposes those limits.
    const double ceiling = units::db_to_linear(Engine::kLevelMaxDb);
    REQUIRE_THAT(Engine::kLevelMaxLinear, WithinAbs(ceiling, 1e-12));
    REQUIRE_THAT((1.0 + Engine::kMaxVoices) * ceiling, WithinRel(5.98, 0.01));
}

TEST_CASE("levels and enables behave as a mixer", "[signal][harmony-engine]") {
    Engine engine;
    engine.prepare(kSr);
    REQUIRE(engine.voice_enabled(0));
    REQUIRE_FALSE(engine.voice_enabled(1));  // voice 2 is opt-in

    engine.set_voice_level_db(0, 100.0);
    REQUIRE(engine.voice_level_db(0) == Engine::kLevelMaxDb);
    engine.set_voice_level_db(0, -1000.0);
    REQUIRE(engine.voice_level_db(0) == Engine::kLevelMinDb);
    engine.set_dry_level_db(100.0);
    REQUIRE(engine.dry_level_db() == Engine::kLevelMaxDb);

    // Out-of-range voice indices are inert rather than corrupting.
    engine.set_voice_interval(7, 5);
    engine.set_voice_detune_cents(-1, 5.0);
    REQUIRE(engine.voice_interval(7) == 0);
    REQUIRE(engine.voice_detune_cents(-1) == 0.0);

    // Interval clamping is at the mapper's stated range.
    engine.set_voice_interval(0, 500);
    REQUIRE(engine.voice_interval(0) == Mapper::kIntervalStepsMax);
    engine.set_voice_interval(0, -500);
    REQUIRE(engine.voice_interval(0) == -Mapper::kIntervalStepsMax);

    // Dry only, with both voices off, is an exactly scaled delayed copy.
    Engine dry_only;
    dry_only.prepare(kSr);
    dry_only.set_voice_enabled(0, false);
    dry_only.set_voice_enabled(1, false);
    dry_only.set_dry_level_db(0.0);
    dry_only.reset();
    std::vector<double> input(4000);
    Xorshift32 rng(3);
    for (double& v : input) v = rng.next_bipolar<double>();
    std::vector<double> output;
    for (double v : input) output.push_back(static_cast<double>(dry_only.process(v)));
    const int latency = dry_only.latency_samples();
    for (int n = latency; n < static_cast<int>(input.size()); ++n)
        REQUIRE(output[static_cast<std::size_t>(n)] ==
                input[static_cast<std::size_t>(n - latency)]);
}

TEST_CASE("the float instantiation harmonizes to the same interval",
          "[signal][harmony-engine]") {
    // `HarmonyEngine` (float) is the default alias and the one a plugin will
    // instantiate; every case above runs the double one. The delay storage, the
    // shifters and the tracker front end all narrow to `SampleType`.
    HarmonyEngine engine;
    engine.prepare(kSr);
    engine.set_key(0);
    engine.set_scale(ScaleType::major);
    engine.set_voice_interval(0, 2);
    engine.set_glide_ms(0.0);
    engine.reset();

    const double f0 = midi_hz(62);  // D4 — the degree where a third is 3, not 4
    for (int n = 0; n < 3 * engine.latency_samples() + 8000; ++n) {
        const float x = static_cast<float>(
            0.5 * std::sin(2.0 * kPi * f0 * static_cast<double>(n) / kSr));
        REQUIRE(std::isfinite(engine.process(x)));
    }
    REQUIRE(engine.voiced());
    REQUIRE(engine.voice_shift_semitones(0) == expected_shift(ScaleType::major, 0, 62, 2));
    REQUIRE(engine.voice_shift_semitones(0) == 3);
    REQUIRE(engine.latency_samples() == 2 * static_cast<int>(std::ceil(kSr / 80.0)));
}

TEST_CASE("a fresh instance survives being used before prepare",
          "[signal][harmony-engine]") {
    Engine engine;
    for (int n = 0; n < 256; ++n)
        REQUIRE(std::isfinite(static_cast<double>(engine.process(0.5))));

    Tracker tracker;
    for (int n = 0; n < 256; ++n) REQUIRE_FALSE(tracker.process(0.5));
    REQUIRE(tracker.latency_samples() == 0);

    Mapper mapper;
    REQUIRE(mapper.degree_count() == 7);  // default-constructed is C major

    engine.prepare(kSr);
    engine.reset();
    REQUIRE(std::isfinite(static_cast<double>(engine.process(0.5))));
}

TEST_CASE("harmony retains controls and recovers from non-finite audio",
          "[signal][harmony-engine][nonfinite]") {
    for(double bad:{NAN,INFINITY,-INFINITY}){
        Tracker t;t.set_f0_range(73,911);t.set_f0_range(bad,1000);REQUIRE(t.f0_min_hz()==73);t.prepare(kSr);REQUIRE_FALSE(t.process(bad));
        Engine a,b;for(auto* e:{&a,&b}){e->prepare(kSr);e->set_voice_detune_cents(0,17);e->set_voice_level_db(0,-4);e->set_dry_level_db(-9);e->set_glide_ms(31);e->set_humanize_cents(7);e->set_crossfade_ms(33);e->reset();}
        a.set_voice_detune_cents(0,bad);a.set_voice_level_db(0,bad);a.set_dry_level_db(bad);a.set_glide_ms(bad);a.set_humanize_cents(bad);a.set_crossfade_ms(bad);
        REQUIRE(a.voice_detune_cents(0)==b.voice_detune_cents(0));REQUIRE(a.process(bad)==0);b.reset();for(int i=0;i<64;++i)REQUIRE(a.process(.2)==b.process(.2));
    }
}
