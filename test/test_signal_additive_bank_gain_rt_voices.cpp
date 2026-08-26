#include "test_signal_additive_bank_support.hpp"

TEST_CASE("Coherent phases give a higher crest factor than scattered ones",
          "[signal][additive][retrigger]") {
    // Phase is a timbre control on the attack, not only a determinism detail:
    // aligned partials produce a percussive click, scattered ones a softer
    // entry at IDENTICAL partial amplitudes. Measured as peak-to-RMS, which is
    // the definition of crest factor — one of the two places in this file a
    // peak sample is the actual quantity under test.
    const auto crest = [](Bank::RetrigPhase policy, double stored_phase) {
        VoiceTable v = harmonic_voice(32, 1.0, stored_phase);
        Bank bank;
        configure_steady(bank, v, 32, 200.0);
        bank.set_retrig_phase(policy);
        bank.set_seed(0x1234u);
        bank.reset();
        bank.retrigger();
        const auto x = render(bank, 4800);

        double peak = 0.0, sq = 0.0;
        for (double s : x) {
            peak = std::max(peak, std::abs(s));
            sq += s * s;
        }
        return peak / std::sqrt(sq / static_cast<double>(x.size()));
    };

    const double aligned = crest(Bank::RetrigPhase::reset_stored, 0.25);
    const double scattered = crest(Bank::RetrigPhase::seeded_random, 0.25);
    REQUIRE(aligned > scattered);
    // A cosine-aligned bank of N equal partials has crest sqrt(2N) — every
    // partial peaks together while the RMS is the incoherent sum. For 32
    // partials that is 8.0.
    REQUIRE_THAT(aligned, WithinRel(std::sqrt(2.0 * 32.0), 0.02));
}

TEST_CASE("The coherent-sum crest bound equals the registry worst-case gain",
          "[signal][additive][gain]") {
    // The registry's number, and it is a proof rather than an estimate: the
    // normaliser divides by the sum of magnitudes, so |y| <= 1 by the triangle
    // inequality, and `master_gain_db` at its legal ceiling scales that.
    REQUIRE_THAT(Bank::worst_case_gain(),
                 WithinRel(units::db_to_linear(Bank::kMasterGainMaxDb), 1e-15));
    REQUIRE_THAT(db(Bank::worst_case_gain()),
                 WithinAbs(Bank::kMasterGainMaxDb, 1e-12));

    // The worst case is CONSTRUCTIBLE, not statistical: store phase 0.25 for
    // every partial so all of them sit at `sin = +1` together. f0 = 200 Hz
    // makes the organ's lowest ratio (0.5) repeat every 480 samples exactly, so
    // the realignment lands ON a sample and the peak is genuinely observable
    // rather than passing between two of them.
    VoiceTable organ = make_organ_voice(64);
    for (int p = 0; p < organ.count; ++p)
        organ.partials[static_cast<std::size_t>(p)].phase01 = 0.25;

    Bank bank;
    configure_steady(bank, organ, 64, 200.0);
    bank.set_master_gain_db(Bank::kMasterGainMaxDb);
    bank.reset();
    bank.retrigger();
    const auto x = render(bank, 4800);

    double peak = 0.0;
    for (double v : x) peak = std::max(peak, std::abs(v));
    REQUIRE(peak <= Bank::worst_case_gain() + kOneLsb);
    // Attained — so the bound is tight, and a future change that quietly
    // over-attenuates fails here instead of passing a one-sided inequality.
    REQUIRE(peak > Bank::worst_case_gain() - kOneLsb);
}

TEST_CASE("The crest bound holds across the whole legal parameter range",
          "[signal][additive][gain]") {
    // Series law 8 wants a bound the module's own tests assert across the
    // range, not at one flattering point. Every combination of voice, partial
    // count, morph, tilt, detune, inharmonicity and envelope mode below is
    // rendered with phases aligned and the master trim at its ceiling.
    const auto envelope_b = SpectralEnvelope::tilt(6.0, 200.0);

    for (bool bell : {false, true}) {
        for (int count : {1, 16, 64, 128}) {
            for (double morph : {0.0, 1.0}) {
                for (double tilt : {Bank::kSpectralTiltMinDbOct,
                                    Bank::kSpectralTiltMaxDbOct}) {
                    for (double detune : {0.0, Bank::kDetuneMaxCents}) {
                        VoiceTable v = bell ? make_bell_voice(count)
                                            : make_organ_voice(count);
                        for (int p = 0; p < v.count; ++p)
                            v.partials[static_cast<std::size_t>(p)].phase01 = 0.25;

                        Bank bank;
                        configure_steady(bank, v, count, 200.0);
                        bank.set_envelope_a(SpectralEnvelope::tilt(0.0, 200.0));
                        bank.set_envelope_b(envelope_b);
                        bank.set_morph(static_cast<float>(morph));
                        bank.set_spectral_tilt_db_oct(tilt);
                        bank.set_detune_cents(detune);
                        bank.set_inharmonicity_b(Bank::kInharmonicityMax);
                        bank.set_master_gain_db(Bank::kMasterGainMaxDb);
                        bank.set_envelope_mode(
                            Bank::EnvelopeMode::per_partial_decay);
                        bank.reset();
                        bank.retrigger();

                        const auto x = render(bank, 2400);
                        for (double s : x) {
                            REQUIRE(std::isfinite(s));
                            REQUIRE(std::abs(s) <=
                                    Bank::worst_case_gain() + kOneLsb);
                        }
                    }
                }
            }
        }
    }
}

TEST_CASE("The bound survives a parameter ramp",
          "[signal][additive][gain]") {
    // The normaliser is applied to the PER-PARTIAL gains rather than to the
    // summed output, which is what keeps the bound valid mid-crossfade: a
    // convex blend of two gain sets that each sum to <= 1 also sums to <= 1.
    // A post-sum normaliser would be a different number on each side of a
    // control-block edge and could overshoot between them. Swept hard enough
    // that a block-edge discontinuity would show.
    VoiceTable organ = make_organ_voice(64);
    for (int p = 0; p < organ.count; ++p)
        organ.partials[static_cast<std::size_t>(p)].phase01 = 0.25;

    Bank bank;
    configure_steady(bank, organ, 64, 200.0);
    bank.set_envelope_a(SpectralEnvelope::tilt(0.0, 200.0));
    bank.set_envelope_b(SpectralEnvelope::tilt(6.0, 200.0));
    bank.set_master_gain_db(Bank::kMasterGainMaxDb);
    bank.reset();
    bank.retrigger();

    constexpr int kBlocks = 600;
    constexpr int kBlock = 16;   // finer than the 32-sample control cadence, so
                                 // parameter changes land mid-block too
    for (int b = 0; b < kBlocks; ++b) {
        const double t = static_cast<double>(b) / (kBlocks - 1);
        bank.set_morph(static_cast<float>(t));
        bank.set_spectral_tilt_db_oct(Bank::kSpectralTiltMinDbOct +
                                      t * (Bank::kSpectralTiltMaxDbOct -
                                           Bank::kSpectralTiltMinDbOct));
        bank.set_fundamental_hz(200.0 + t * 300.0);
        for (double s : render(bank, kBlock)) {
            REQUIRE(std::isfinite(s));
            REQUIRE(std::abs(s) <= Bank::worst_case_gain() + kOneLsb);
        }
    }
}

TEST_CASE("A five-second bell render is bit-identical across reset",
          "[signal][additive][determinism]") {
    const auto run = [] {
        Bank bank;
        configure_steady(bank, make_bell_voice(64), 64, 220.0);
        bank.set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
        bank.set_detune_cents(3.0);
        bank.set_retrig_phase(Bank::RetrigPhase::seeded_random);
        bank.set_attack_ms(2.0);
        bank.reset();

        std::vector<double> out;
        // A fixed trigger sequence, so the RNG advance and the envelope state
        // both have history by the end rather than being freshly reset.
        for (int strike = 0; strike < 4; ++strike) {
            bank.retrigger();
            const auto a = render(bank, 48000);
            out.insert(out.end(), a.begin(), a.end());
            bank.release();
            const auto b = render(bank, 12000);
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    };

    const auto a = run();
    const auto b = run();
    REQUIRE(a.size() == static_cast<std::size_t>(4 * 60000));
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("Reset rewinds a live instance rather than a fresh one",
          "[signal][additive][determinism]") {
    // AT-6 and AT-8 in their literal form: "render -> reset -> render is
    // bit-identical". The distinction from the tests above is the whole point —
    // those build a NEW bank each time, so the generator starts at its seed
    // whether or not `reset()` rewinds it. Only re-rendering the SAME instance
    // after a reset can catch a `reset()` that forgets the RNG, and every
    // seeded quantity here (initial phases under `seeded_random`, the doublet
    // detune jitter) depends on it.
    Bank bank;
    configure_steady(bank, make_bell_voice(48), 48, 261.0);
    bank.set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
    bank.set_retrig_phase(Bank::RetrigPhase::seeded_random);
    bank.set_detune_cents(9.0);
    bank.set_attack_ms(4.0);
    bank.set_seed(0x51EEDu);

    const auto pass = [&] {
        bank.reset();
        std::vector<double> out;
        for (int strike = 0; strike < 3; ++strike) {
            bank.retrigger();
            const auto a = render(bank, 6000);
            out.insert(out.end(), a.begin(), a.end());
            bank.release();
            const auto b = render(bank, 2000);
            out.insert(out.end(), b.begin(), b.end());
        }
        return out;
    };

    const auto first = pass();
    const auto second = pass();   // same object, only `reset()` between them
    REQUIRE(first.size() == second.size());
    for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);

    // Not vacuous: the render has to actually contain the seeded content. If
    // the phases were not being scattered at all, the test above would pass on
    // a bank with no randomness in it.
    double energy = 0.0;
    for (double v : first) energy += v * v;
    REQUIRE(energy > 0.0);

    // And re-seeding really does change the render, so the seed is reaching the
    // signal rather than being stored and ignored.
    bank.set_seed(0xD1FFu);
    const auto reseeded = pass();
    bool differs = false;
    for (std::size_t i = 0; i < first.size(); ++i)
        if (first[i] != reseeded[i]) differs = true;
    REQUIRE(differs);
}

TEST_CASE("The float and double instantiations agree on the physics",
          "[signal][additive][parity]") {
    // The accumulators, gains and sum are `double` in both instantiations; only
    // the output cast differs. So the two must agree to float rounding, and
    // each must be internally deterministic.
    constexpr int kWindow = 48000;
    constexpr double kF0 = 200.0;

    AdditiveBank f32;
    Bank f64;
    configure_steady(f32, harmonic_voice(16), 16, kF0);
    configure_steady(f64, harmonic_voice(16), 16, kF0);

    const auto a = render(f32, kWindow);
    const auto b = render(f64, kWindow);
    for (std::size_t i = 0; i < a.size(); ++i)
        REQUIRE_THAT(a[i], WithinAbs(b[i], 1e-6));

    for (int n = 1; n <= 16; ++n) {
        const double x = coherent_amplitude(a, static_cast<int>(kF0) * n, kWindow);
        const double y = coherent_amplitude(b, static_cast<int>(kF0) * n, kWindow);
        REQUIRE_THAT(x, WithinRel(y, 1e-5));
    }

    AdditiveBank again;
    configure_steady(again, harmonic_voice(16), 16, kF0);
    const auto c = render(again, kWindow);
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == c[i]);

    REQUIRE_THAT(AdditiveBank::worst_case_gain(),
                 WithinRel(Bank::worst_case_gain(), 1e-15));
    REQUIRE(AdditiveBank::latency_samples() == Bank::latency_samples());
}

TEST_CASE("Nothing allocates after prepare",
          "[signal][additive][rt]") {
    auto bank = std::make_unique<Bank>();
    bank->prepare(kSr, Bank::kMaxPartialsDefault);

    const auto organ = make_organ_voice(128);
    const auto bell = make_bell_voice(128);
    const auto envelope = SpectralEnvelope::tilt(-6.0, 200.0);
    std::vector<double> out(512, 0.0);

    require_allocates_no_memory([&] {
        bank->load_voice(organ);
        bank->load_voice(bell);
        bank->set_partial_count(97);
        bank->set_fundamental_hz(311.0);
        bank->set_inharmonicity_b(Bank::kBUpperTreble);
        bank->set_spectral_tilt_db_oct(-9.0);
        bank->set_master_gain_db(-3.0);
        bank->set_envelope_a(envelope);
        bank->set_envelope_b(envelope);
        bank->set_morph(0.5f);
        bank->set_spectral_domain(SpectralDomain::relative_to_f0);
        bank->set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
        bank->set_attack_ms(3.0);
        bank->set_release_ms(500.0);
        bank->set_detune_cents(7.0);
        bank->set_pitch_glide(-120.0, 40.0);
        bank->set_retrig_phase(Bank::RetrigPhase::seeded_random);
        bank->set_seed(99u);
        bank->set_partial(3, 3.5, 0.4, 0.1, 250.0);
        bank->reset();
        bank->retrigger();
        bank->process(out.data(), 512);
        (void) bank->next();
        bank->release();
        bank->process(out.data(), 512);
        (void) bank->partial_frequency(3);
        (void) bank->envelope_db_at(1000.0);
    });

    // A silent tail flushes to exact zero rather than drizzling subnormals into
    // the accumulator for minutes after a long decay.
    bank->reset();
    const auto tail = render(*bank, 4096);
    for (double v : tail) {
        REQUIRE(std::fpclassify(v) != FP_SUBNORMAL);
        REQUIRE(v == 0.0);   // never retriggered, so the onset is idle
    }
}

TEST_CASE("The precise additive trig profile is explicit and float-only",
          "[signal][additive][fast-trig]") {
    AdditiveBank bank;
    REQUIRE(bank.trig_profile() == FastTrigProfile::reference);
    REQUIRE(bank.set_trig_profile(FastTrigProfile::realtime_precise));
    REQUIRE(bank.trig_profile() == FastTrigProfile::realtime_precise);

    AdditiveBank64 analysis;
    REQUIRE_FALSE(analysis.set_trig_profile(FastTrigProfile::realtime_precise));
    REQUIRE(analysis.trig_profile() == FastTrigProfile::reference);

#if defined(__APPLE__) && defined(__clang__) && defined(__aarch64__)
    REQUIRE(AdditiveBank::trig_profile_has_vector_path(
        FastTrigProfile::realtime_precise));
#else
    REQUIRE_FALSE(AdditiveBank::trig_profile_has_vector_path(
        FastTrigProfile::realtime_precise));
#endif
    REQUIRE_FALSE(AdditiveBank::trig_profile_has_vector_path(
        FastTrigProfile::reference));
    REQUIRE_FALSE(AdditiveBank::trig_profile_has_vector_path(
        FastTrigProfile::realtime_efficient));
    REQUIRE_FALSE(bank.set_trig_profile(FastTrigProfile::realtime_efficient));
    REQUIRE(bank.trig_profile() == FastTrigProfile::realtime_precise);

    const auto invalid =
        static_cast<FastTrigProfile>(std::numeric_limits<std::uint8_t>::max());
    REQUIRE_FALSE(bank.set_trig_profile(invalid));
    REQUIRE(bank.trig_profile() == FastTrigProfile::realtime_precise);
}

TEST_CASE("The precise additive profile stays within two float LSBs",
          "[signal][additive][fast-trig][quality]") {
    for (bool bell : {false, true}) {
        for (int count : {1, 63, 64, 128}) {
            const auto voice = bell ? make_bell_voice(count)
                                    : make_organ_voice(count);
            AdditiveBank reference;
            AdditiveBank candidate;
            configure_steady(reference, voice, count, bell ? 180.0 : 110.0);
            configure_steady(candidate, voice, count, bell ? 180.0 : 110.0);
            reference.set_detune_cents(bell ? 7.0 : 0.0);
            candidate.set_detune_cents(bell ? 7.0 : 0.0);
            reference.reset();
            candidate.reset();
            reference.retrigger();
            candidate.retrigger();
            REQUIRE(candidate.set_trig_profile(
                FastTrigProfile::realtime_precise));

            const auto expected = render(reference, 48000);
            const auto actual = render(candidate, 48000);
            double maximum = 0.0;
            double squared = 0.0;
            for (std::size_t i = 0; i < actual.size(); ++i) {
                const double error = actual[i] - expected[i];
                maximum = std::max(maximum, std::abs(error));
                squared += error * error;
            }
            INFO("bell=" << bell << " count=" << count);
            REQUIRE(maximum <= 2.0 * kOneLsb);
            REQUIRE(std::sqrt(squared / static_cast<double>(actual.size())) <=
                    0.5 * kOneLsb);
        }
    }
}

TEST_CASE("The precise additive profile remains allocation-free",
          "[signal][additive][fast-trig][rt]") {
    AdditiveBank bank;
    configure_steady(bank, make_bell_voice(63), 63, 180.0);
    bank.set_detune_cents(9.0);
    REQUIRE(bank.set_trig_profile(FastTrigProfile::realtime_precise));
    std::array<float, 512> output{};
    require_allocates_no_memory(
        [&] { bank.process(output.data(), static_cast<int>(output.size())); });
}

TEST_CASE("Additive trig profile changes preserve oscillator state",
          "[signal][additive][fast-trig][state]") {
    AdditiveBank reference;
    AdditiveBank switched;
    const auto voice = make_organ_voice(64);
    configure_steady(reference, voice, 64, 110.0);
    configure_steady(switched, voice, 64, 110.0);

    std::array<float, 257> prefix_reference{};
    std::array<float, 257> prefix_switched{};
    reference.process(prefix_reference.data(), 257);
    switched.process(prefix_switched.data(), 257);
    REQUIRE(prefix_reference == prefix_switched);

    REQUIRE(switched.set_trig_profile(FastTrigProfile::realtime_precise));
    std::array<float, 513> middle_reference{};
    std::array<float, 513> middle_switched{};
    reference.process(middle_reference.data(), 513);
    switched.process(middle_switched.data(), 513);
    for (std::size_t i = 0; i < middle_reference.size(); ++i)
        REQUIRE(std::abs(middle_reference[i] - middle_switched[i]) <=
                2.0f * static_cast<float>(kOneLsb));

    REQUIRE(switched.set_trig_profile(FastTrigProfile::reference));
    std::array<float, 512> suffix_reference{};
    std::array<float, 512> suffix_switched{};
    reference.process(suffix_reference.data(), 512);
    switched.process(suffix_switched.data(), 512);
    REQUIRE(suffix_reference == suffix_switched);
}

TEST_CASE("Latency is zero and output begins at sample zero",
          "[signal][additive][latency]") {
    REQUIRE(Bank::latency_samples() == 0);
    REQUIRE(AdditiveBank::latency_samples() == 0);

    // A quarter-cycle offset puts the partial at its crest on the first sample, so a
    // group delay of even one sample would show as a zero at index 0.
    Bank bank;
    configure_steady(bank, harmonic_voice(1, 1.0, 0.25), 1, 440.0);
    const auto x = render(bank, 16);
    REQUIRE(x[0] != 0.0);
    REQUIRE_THAT(x[0], WithinRel(1.0, 1e-9));

    // The same with a full 128-partial bank: no buffering appears at scale.
    Bank big;
    configure_steady(big, harmonic_voice(128, 1.0, 0.25), 128, 100.0);
    const auto y = render(big, 16);
    REQUIRE(y[0] != 0.0);
}

TEST_CASE("Per-partial decay follows each partial's own time constant",
          "[signal][additive][envelope]") {
    // The whole reason `per_partial_decay` exists: a struck body's top dies
    // while its bottom rings. Measured by fitting the decay of two partials
    // with a 4:1 ratio in their table time constants.
    constexpr double kF0 = 200.0;
    constexpr double kSlowMs = 4000.0;
    constexpr double kFastMs = 1000.0;

    VoiceTable v;
    v.harmonic = true;
    v.add({1.0, 1.0, 0.0, kSlowMs});
    v.add({4.0, 1.0, 0.0, kFastMs});

    Bank bank;
    configure_steady(bank, v, 2, kF0);
    bank.set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
    bank.reset();
    bank.retrigger();
    const auto x = render(bank, 96000);   // 2 s

    const auto tau_ms_of = [&](int cycles) {
        // Two coherent windows a known distance apart; the ratio of amplitudes
        // gives the time constant directly, with no curve fitting.
        constexpr int kWin = 4800;        // 0.1 s, whole cycles of every partial
        const int gap = 48000;            // 1.0 s between window starts
        const double a0 = coherent_amplitude(x, cycles / 10, kWin, 0);
        const double a1 = coherent_amplitude(x, cycles / 10, kWin, gap);
        return 1000.0 * (static_cast<double>(gap) / kSr) / std::log(a0 / a1);
    };

    REQUIRE_THAT(tau_ms_of(static_cast<int>(kF0)), WithinRel(kSlowMs, 0.02));
    REQUIRE_THAT(tau_ms_of(static_cast<int>(kF0) * 4), WithinRel(kFastMs, 0.02));

    // `shared_ar` ignores the table's time constants outright — both partials
    // hold. If the mode leaked, the 4:1 spread above would still be visible.
    Bank shared;
    configure_steady(shared, v, 2, kF0);
    shared.set_envelope_mode(Bank::EnvelopeMode::shared_ar);
    shared.reset();
    shared.retrigger();
    const auto held = render(shared, 96000);
    for (int cycles : {static_cast<int>(kF0), static_cast<int>(kF0) * 4}) {
        const double a0 = coherent_amplitude(held, cycles / 10, 4800, 0);
        const double a1 = coherent_amplitude(held, cycles / 10, 4800, 48000);
        REQUIRE_THAT(a1, WithinRel(a0, 1e-9));
    }
}

TEST_CASE("The shared onset holds at unity while gated",
          "[signal][additive][envelope]") {
    // A dependency on `envelope.hpp` worth pinning, because getting it wrong is
    // silent and costs exactly 3.1 dB.
    //
    // A shape with no decay or sustain segment holds at its peak. Both an
    // explicitly configured instance and a defaulted instance must stay at
    // unity until the gate closes.
    ArT<double> onset;
    onset.prepare(kSr);
    onset.set_attack_ms(1.0);
    onset.gate_on();
    for (int i = 0; i < 4800; ++i) onset.next();
    REQUIRE_THAT(onset.next(), WithinAbs(1.0, 1e-12));

    // The one that matters: a caller who never touches sustain must get the
    // peak too, because that is what the shape promises.
    ArT<double> defaulted;
    defaulted.prepare(kSr);
    defaulted.set_attack_ms(1.0);
    defaulted.gate_on();
    for (int i = 0; i < 4800; ++i) defaulted.next();
    REQUIRE_THAT(defaulted.next(), WithinAbs(1.0, 1e-12));

    // Which the bank does not inherit: a sustained partial holds its full
    // table amplitude for as long as the gate is up.
    Bank bank;
    configure_steady(bank, harmonic_voice(1), 1, 200.0);
    bank.set_attack_ms(1.0);
    bank.reset();
    bank.retrigger();
    const auto x = render(bank, 96000);
    REQUIRE_THAT(coherent_amplitude(x, 20, 4800, 48000),
                 WithinRel(coherent_amplitude(x, 20, 4800, 24000), 1e-9));
    REQUIRE_THAT(coherent_amplitude(x, 20, 4800, 48000), WithinRel(1.0, 1e-6));

    // And a release actually falls.
    bank.release();
    const auto tail = render(bank, 96000);
    REQUIRE(std::abs(tail.back()) < 1e-9);
}

TEST_CASE("The doublet splits a partial into a beating pair",
          "[signal][additive][doublet]") {
    // A real bell's casting asymmetry gives each mode two close frequencies.
    // With the detune engaged the bank renders two oscillators per partial, so
    // the test looks for two peaks where one used to be.
    constexpr double kF0 = 440.0;
    constexpr double kCents = 20.0;   // wide enough to resolve in 2.7 s

    Bank bank;
    configure_steady(bank, harmonic_voice(1), 1, kF0);
    bank.set_detune_cents(kCents);
    REQUIRE(bank.doublet_active());
    bank.set_seed(0x2468u);
    bank.reset();
    bank.retrigger();
    const auto x = render(bank, 1 << 17);

    // The pair straddles the nominal frequency, each within half the detune
    // times the seeded jitter — so the bracket is the widest the jitter allows.
    const double widest = 0.5 * kCents * (1.0 + Bank::kDoubletJitterSpread);
    const double lo_edge = kF0 * units::cents_to_ratio(-widest * 1.05);
    const double hi_edge = kF0 * units::cents_to_ratio(widest * 1.05);

    const double lower = refine_peak(x, lo_edge, kF0);
    const double upper = refine_peak(x, kF0, hi_edge);
    REQUIRE(lower < kF0);
    REQUIRE(upper > kF0);

    // Symmetric about the nominal in cents, because the pair is +/- the same
    // jittered offset rather than two independent draws.
    REQUIRE_THAT(units::ratio_to_cents(lower / kF0),
                 WithinAbs(-units::ratio_to_cents(upper / kF0), 1e-3));

    // Zero detune renders ONE oscillator, not two coincident ones at half
    // amplitude — the amplitude at the nominal is the full table value.
    Bank mono;
    configure_steady(mono, harmonic_voice(1), 1, 200.0);
    REQUIRE_FALSE(mono.doublet_active());
    const auto m = render(mono, 48000);
    REQUIRE_THAT(coherent_amplitude(m, 200, 48000), WithinRel(1.0, 1e-9));

    // And the pair conserves level: two half-amplitude oscillators, so the
    // sum's magnitude at the nominal is unchanged when they are coincident in
    // the limit of zero detune.
    Bank pair;
    configure_steady(pair, harmonic_voice(1), 1, 200.0);
    pair.set_detune_cents(1e-6);
    pair.reset();
    pair.retrigger();
    const auto p = render(pair, 48000);
    REQUIRE_THAT(coherent_amplitude(p, 200, 48000), WithinRel(1.0, 1e-4));
}

TEST_CASE("The organ voice is the documented drawbar registration",
          "[signal][additive][voice]") {
    // The footage-to-harmonic mapping is documented behaviour (Hammond, US
    // 1,956,350): 16' -> 0.5, 5 1/3' -> 1.5, 8' -> 1, 4' -> 2, 2 2/3' -> 3,
    // 2' -> 4, 1 3/5' -> 5, 1 1/3' -> 6, 1' -> 8.
    const double expected_ratio[9] = {0.5, 1.0, 1.5, 2.0, 3.0, 4.0, 5.0, 6.0, 8.0};
    const auto organ = make_organ_voice(64);

    REQUIRE(organ.harmonic);
    REQUIRE(organ.count == 64);
    for (int p = 0; p < 9; ++p) {
        REQUIRE_THAT(organ.partials[static_cast<std::size_t>(p)].ratio,
                     WithinRel(expected_ratio[p], 1e-15));
        REQUIRE(organ.partials[static_cast<std::size_t>(p)].amp > 0.0);
    }

    // Every partial is sustained — a tonewheel is switched, not struck.
    for (int p = 0; p < organ.count; ++p)
        REQUIRE(organ.partials[static_cast<std::size_t>(p)].decay_ms <= 0.0);

    // Documented tonewheel voices are phase-coherent.
    for (int p = 0; p < organ.count; ++p)
        REQUIRE(organ.partials[static_cast<std::size_t>(p)].phase01 == 0.0);

    // The tail past the ninth drawbar is -12 dB/oct, i.e. amplitude
    // proportional to 1/ratio^2. Checked as a ratio between two partials an
    // octave apart, so the assertion is about the LAW rather than the level.
    //
    // The expectation is COMPUTED from that exponent, not written as "-12":
    // "12 dB per octave" is the trade name for a factor of four, and
    // 20*log10(4) is 12.0412 dB. Asserting the round number to two decimals
    // fails a correct implementation by 0.04 dB.
    const auto amp_at = [&](int p) {
        return organ.partials[static_cast<std::size_t>(p)].amp;
    };
    constexpr double kTailExponent = 2.0;
    const double per_octave_db = -20.0 * std::log10(std::pow(2.0, kTailExponent));
    REQUIRE_THAT(per_octave_db, WithinAbs(-12.0412, 1e-3));
    REQUIRE_THAT(db(amp_at(31) / amp_at(15)), WithinAbs(per_octave_db, 1e-9));
    REQUIRE_THAT(db(amp_at(63) / amp_at(31)), WithinAbs(per_octave_db, 1e-9));
}

TEST_CASE("The bell voice has its named modes and its decay spread",
          "[signal][additive][voice]") {
    // The five tuned partials of the modern English church bell are documented
    // (Perrin, Charnley & de Pont 1983): hum 0.5, prime 1.0, tierce ~1.2,
    // quint 1.5, nominal 2.0.
    const auto bell = make_bell_voice(64);
    REQUIRE_FALSE(bell.harmonic);

    REQUIRE_THAT(bell.partials[0].ratio, WithinAbs(0.50, 1e-12));
    REQUIRE_THAT(bell.partials[1].ratio, WithinAbs(1.00, 1e-12));
    REQUIRE_THAT(bell.partials[2].ratio, WithinAbs(1.20, 0.02));   // tierce
    REQUIRE_THAT(bell.partials[3].ratio, WithinAbs(1.50, 1e-12));
    REQUIRE_THAT(bell.partials[4].ratio, WithinAbs(2.00, 1e-12));

    // Non-integer ratios are the point — a bell is not a harmonic series.
    for (int p = 0; p < 5; ++p) {
        const double r = bell.partials[static_cast<std::size_t>(p)].ratio;
        if (p == 2) REQUIRE(std::abs(r - std::round(r)) > 0.1);
    }

    // The decay spread the voice exists for: the nominal rings 2.5 s while the
    // p=10 cluster mode dies in about 526 ms. The spec's worked check.
    const double nominal_ms = bell.partials[4].decay_ms;
    const double cluster_ms = bell.partials[10].decay_ms;
    REQUIRE_THAT(nominal_ms / cluster_ms, WithinRel(4.75, 0.02));
    REQUIRE_THAT(cluster_ms, WithinAbs(526.0, 2.0));
    REQUIRE_THAT(bell.partials[10].ratio, WithinAbs(5.7, 0.01));

    // Monotone: higher modes die first, all the way up the cluster.
    for (int p = 11; p < bell.count; ++p) {
        REQUIRE(bell.partials[static_cast<std::size_t>(p)].ratio >
                bell.partials[static_cast<std::size_t>(p - 1)].ratio);
        REQUIRE(bell.partials[static_cast<std::size_t>(p)].decay_ms <
                bell.partials[static_cast<std::size_t>(p - 1)].decay_ms);
    }

    // And it is audibly a bell: with the prime at 440 the nominal sits at 880.
    Bank bank;
    configure_steady(bank, bell, 16, 440.0);
    REQUIRE_THAT(bank.partial_frequency(4), WithinRel(880.0, 1e-9));
    REQUIRE_THAT(bank.partial_frequency(0), WithinRel(220.0, 1e-9));
}

TEST_CASE("Partial count and max partials are clamped and reported",
          "[signal][additive][limits]") {
    Bank bank;
    bank.prepare(kSr, 32);
    REQUIRE(bank.max_partials() == 32);
    bank.load_voice(make_organ_voice(128));

    bank.set_partial_count(1000);
    REQUIRE(bank.partial_count() == 32);   // capped by prepare, not by the table
    bank.set_partial_count(-4);
    REQUIRE(bank.partial_count() == 1);
    bank.set_partial_count(17);
    REQUIRE(bank.partial_count() == 17);

    // `prepare` bounds `max_partials` to the array ceiling.
    bank.prepare(kSr, 10000);
    REQUIRE(bank.max_partials() == Bank::kMaxPartialsCeiling);
    bank.prepare(kSr, 0);
    REQUIRE(bank.max_partials() == 1);

    // A table shorter than the requested count caps the count too, so the bank
    // never reads a row that was never written.
    Bank small;
    small.prepare(kSr, 128);
    VoiceTable v = harmonic_voice(5);
    small.load_voice(v);
    small.set_partial_count(64);
    REQUIRE(small.partial_count() == 5);

    // Parameter clamps report what actually took effect.
    Bank p;
    p.prepare(kSr, 64);
    p.set_fundamental_hz(1e9);
    REQUIRE_THAT(p.fundamental_hz(), WithinRel(Bank::kFundamentalMaxHz, 1e-12));
    p.set_fundamental_hz(0.0);
    REQUIRE_THAT(p.fundamental_hz(), WithinRel(Bank::kFundamentalMinHz, 1e-12));
    p.set_inharmonicity_b(5.0);
    REQUIRE_THAT(p.inharmonicity_b(), WithinRel(Bank::kInharmonicityMax, 1e-12));
    p.set_inharmonicity_b(-1.0);
    REQUIRE_THAT(p.inharmonicity_b(), WithinAbs(0.0, 1e-15));
    p.set_master_gain_db(100.0);
    REQUIRE_THAT(p.master_gain_db(), WithinRel(Bank::kMasterGainMaxDb, 1e-12));
    p.set_detune_cents(1000.0);
    REQUIRE_THAT(p.detune_cents(), WithinRel(Bank::kDetuneMaxCents, 1e-12));
    p.set_morph(9.0f);
    REQUIRE_THAT(p.morph(), WithinAbs(1.0, 1e-12));
}

TEST_CASE("A pitch glide scales every partial together",
          "[signal][additive][trajectory]") {
    // The strike chiff: a shared initial glide, so the whole spectrum arrives
    // sharp and settles. Scaling all partials by one ratio is what keeps it a
    // pitch move rather than an inharmonicity move.
    constexpr double kF0 = 400.0;
    constexpr double kStartCents = 200.0;
    constexpr double kGlideMs = 100.0;

    Bank bank;
    configure_steady(bank, harmonic_voice(4), 4, kF0);
    bank.set_pitch_glide(kStartCents, kGlideMs);
    bank.reset();
    bank.retrigger();

    // At the strike every partial is sharp by exactly the same ratio.
    const double expected = units::cents_to_ratio(kStartCents);
    for (int p = 0; p < 4; ++p)
        REQUIRE_THAT(bank.partial_frequency(p),
                     WithinRel(kF0 * static_cast<double>(p + 1) * expected, 1e-9));

    // After the glide time it has settled to the nominal.
    (void) render(bank, static_cast<int>(kSr * kGlideMs / 1000.0) + 256);
    for (int p = 0; p < 4; ++p)
        REQUIRE_THAT(bank.partial_frequency(p),
                     WithinRel(kF0 * static_cast<double>(p + 1), 1e-6));
}

TEST_CASE("Block partitioning does not disturb additive phase or control cadence",
          "[signal][additive][phase][determinism]") {
    Bank bank;
    bank.prepare(kSr, 64);
    bank.load_voice(make_bell_voice(32));
    bank.set_partial_count(32);
    bank.set_retrig_phase(Bank::RetrigPhase::seeded_random);
    bank.set_detune_cents(7.0);
    bank.set_pitch_glide(120.0, 37.0);

    constexpr int kSamples = 4097;
    std::vector<double> whole(kSamples), partitioned(kSamples);

    bank.reset();
    bank.retrigger();
    bank.process(whole.data(), kSamples);

    bank.reset();
    bank.retrigger();
    constexpr int kChunks[] = {1, 31, 2, 63, 7, 128, 3, 17};
    int position = 0;
    int chunk = 0;
    while (position < kSamples) {
        const int count = std::min(kChunks[chunk % 8], kSamples - position);
        bank.process(partitioned.data() + position, count);
        position += count;
        ++chunk;
    }

    REQUIRE(whole == partitioned);
}

TEST_CASE("Additive declared time minima and non-finite controls cannot poison the voice",
          "[signal][additive][limits][nan-recovery]") {
    const auto render = [](double attack_ms, double release_ms) {
        Bank bank;
        bank.prepare(kSr, 1);
        bank.load_voice(harmonic_voice(1, 1.0, 0.25));
        bank.set_partial_count(1);
        bank.set_envelope_mode(Bank::EnvelopeMode::shared_ar);
        bank.set_attack_ms(attack_ms);
        bank.set_release_ms(release_ms);
        bank.reset();
        bank.retrigger();

        std::vector<double> out(1024);
        bank.process(out.data(), 256);
        bank.release();
        bank.process(out.data() + 256, 768);
        return out;
    };

    REQUIRE(render(-100.0, -100.0) ==
            render(Bank::kAttackMinMs, Bank::kReleaseMinMs));

    Bank bank;
    bank.prepare(kSr, 8);
    bank.set_fundamental_hz(440.0);
    bank.set_inharmonicity_b(0.001);
    bank.set_spectral_tilt_db_oct(-3.0);
    bank.set_master_gain_db(-6.0);
    bank.set_morph(0.5f);
    bank.set_attack_ms(10.0);
    bank.set_release_ms(100.0);
    bank.set_detune_cents(3.0);
    bank.set_pitch_glide(100.0, 20.0);

    const double nan = std::numeric_limits<double>::quiet_NaN();
    bank.set_fundamental_hz(nan);
    bank.set_inharmonicity_b(nan);
    bank.set_spectral_tilt_db_oct(nan);
    bank.set_master_gain_db(nan);
    bank.set_morph(std::numeric_limits<float>::quiet_NaN());
    bank.set_attack_ms(nan);
    bank.set_release_ms(nan);
    bank.set_detune_cents(nan);
    bank.set_pitch_glide(nan, nan);

    REQUIRE(bank.fundamental_hz() == 440.0);
    REQUIRE(bank.inharmonicity_b() == 0.001);
    REQUIRE(bank.spectral_tilt_db_oct() == -3.0);
    REQUIRE(bank.master_gain_db() == -6.0);
    REQUIRE(bank.morph() == 0.5);
    bank.reset();
    bank.retrigger();
    for (int i = 0; i < 512; ++i) REQUIRE(std::isfinite(bank.next()));
}
