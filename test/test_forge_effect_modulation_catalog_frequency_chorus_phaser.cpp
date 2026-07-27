#include "test_forge_effect_modulation_catalog_support.hpp"

TEST_CASE("Forge modulation: the recipe's frequencies are all on analysis bins",
          "[host][baked][forge][forge-modulation]") {
    // Guards the measurement rather than the code. A frequency off a bin makes
    // every magnitude read in this file leaky, and the failure would look like
    // a DSP bug rather than a recipe bug.
    for (double hz : {kToneHz, kShiftHz, kToneHz + kShiftHz, kToneHz - kShiftHz,
                      kToneHz + 2.0 * kShiftHz})
        REQUIRE(on_bin(hz));
}

TEST_CASE("Forge modulation: the frequency shifter bakes and runs",
          "[host][baked][forge][forge-modulation]") {
    auto fx = make_fixture();
    const auto t = tone();
    const auto out = fx.settle({t, t}, kSettleBlocks);
    for (int ch = 0; ch < 2; ++ch)
        for (float v : out[static_cast<std::size_t>(ch)]) REQUIRE(std::isfinite(v));
    REQUIRE(peak(out[0]) > 0.0f);
}

TEST_CASE("Forge modulation: injecting shift_hz relocates the tone",
          "[host][baked][param-injection][forge][forge-modulation]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    const auto t = tone();

    const auto out = fx.settle({t, t}, kSettleBlocks)[0];
    // All of it arrives at f + shift, and the input frequency is emptied.
    REQUIRE_THAT(magnitude_at(out, kToneHz + kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE(magnitude_at(out, kToneHz) < kAmplitude * 0.05);

    // Zero shift passes the signal through: allpass, so magnitude survives.
    REQUIRE(inj.inject(immediate(mod::kShiftHz, 0.0f)) == InjectStatus::Ok);
    const auto through = fx.settle({t, t}, kSettleBlocks)[0];
    REQUIRE_THAT(magnitude_at(through, kToneHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
}

TEST_CASE("Forge modulation: the mode param selects the sideband",
          "[host][baked][param-injection][forge][forge-modulation]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    const auto t = tone();

    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeDown)) == InjectStatus::Ok);
    const auto down = fx.settle({t, t}, kSettleBlocks)[0];
    REQUIRE_THAT(magnitude_at(down, kToneHz - kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE(magnitude_at(down, kToneHz + kShiftHz) < kAmplitude * 0.05);

    // `dual_mono` is the up combine under a name that says "deliberately no
    // stereo differentiation"; asserted so the enum cannot quietly drift.
    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeDualMono)) == InjectStatus::Ok);
    const auto dual = fx.settle({t, t}, kSettleBlocks);
    REQUIRE_THAT(magnitude_at(dual[0], kToneHz + kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    for (std::size_t n = 0; n < dual[0].size(); ++n) REQUIRE(dual[0][n] == dual[1][n]);
}

TEST_CASE("Forge modulation: stereo split survives the graph",
          "[host][baked][param-injection][forge][forge-modulation]") {
    // The true-stereo wiring assertion. `stereo_split` drives the left channel
    // up and the right down from ONE shared carrier, so the two rails have to
    // be processed in the same call. A node wired dual-mono would pass every
    // other test in this file and fail here, with both channels shifted up.
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeStereoSplit)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(mod::kStereoSpread, 100.0f)) == InjectStatus::Ok);

    const auto t = tone();
    const auto out = fx.settle({t, t}, kSettleBlocks);
    REQUIRE_THAT(magnitude_at(out[0], kToneHz + kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE_THAT(magnitude_at(out[1], kToneHz - kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE(magnitude_at(out[0], kToneHz - kShiftHz) < kAmplitude * 0.05);
    REQUIRE(magnitude_at(out[1], kToneHz + kShiftHz) < kAmplitude * 0.05);
}

TEST_CASE("Forge modulation: stereo spread scales the split",
          "[host][baked][param-injection][forge][forge-modulation]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    REQUIRE(inj.inject(immediate(mod::kShiftMode, mod::kModeStereoSplit)) == InjectStatus::Ok);
    const auto t = tone();

    // The spread scales the CARRIER, so half spread is a half-size shift on
    // each side rather than a blend of both sidebands. 750 Hz is two bins.
    REQUIRE(inj.inject(immediate(mod::kStereoSpread, 50.0f)) == InjectStatus::Ok);
    const auto half = fx.settle({t, t}, kSettleBlocks);
    REQUIRE(on_bin(kToneHz + 0.5 * kShiftHz));
    REQUIRE_THAT(magnitude_at(half[0], kToneHz + 0.5 * kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE_THAT(magnitude_at(half[1], kToneHz - 0.5 * kShiftHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));

    // At zero spread the carrier stops and both rails carry the input at full
    // magnitude — not a ring-modulated pair of half-amplitude sidebands.
    REQUIRE(inj.inject(immediate(mod::kStereoSpread, 0.0f)) == InjectStatus::Ok);
    const auto none = fx.settle({t, t}, kSettleBlocks);
    REQUIRE_THAT(magnitude_at(none[0], kToneHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
    REQUIRE_THAT(magnitude_at(none[1], kToneHz),
                 WithinRel(static_cast<double>(kAmplitude), 0.05));
}

TEST_CASE("Forge modulation: feedback recirculates through the shifter again",
          "[host][baked][param-injection][forge][forge-modulation]") {
    // The barberpole signature, made measurable: each pass adds ANOTHER shift,
    // so energy appears at f + 2*shift that a single pass cannot produce. A
    // feedback path wired around the shifter rather than through it would put
    // the recirculated energy back at f + shift and fail this.
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    REQUIRE(inj.inject(immediate(mod::kFeedbackDelayMs, 1.0f)) == InjectStatus::Ok);
    const auto t = tone();

    const auto dry = fx.settle({t, t}, kSettleBlocks)[0];
    const double without = magnitude_at(dry, kToneHz + 2.0 * kShiftHz);

    REQUIRE(inj.inject(immediate(mod::kFeedback, 0.8f)) == InjectStatus::Ok);
    const auto wet = fx.settle({t, t}, kSettleBlocks)[0];
    const double with = magnitude_at(wet, kToneHz + 2.0 * kShiftHz);

    INFO("second pass " << without << " -> " << with);
    REQUIRE(with > without * 10.0);
    REQUIRE(with > 0.05 * kAmplitude);
}

TEST_CASE("Forge modulation: mix blends against the whole wet chain",
          "[host][baked][param-injection][forge][forge-modulation]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    const auto t = tone();

    REQUIRE(inj.inject(immediate(mod::kMix, 0.0f)) == InjectStatus::Ok);
    const auto dry = fx.settle({t, t}, kSettleBlocks)[0];
    // Fully dry is the input, unaltered — not merely close to it.
    for (int n = 0; n < kFrames; ++n)
        REQUIRE_THAT(static_cast<double>(dry[static_cast<std::size_t>(n)]),
                     WithinAbs(static_cast<double>(t[static_cast<std::size_t>(n)]), 1e-6));

    REQUIRE(inj.inject(immediate(mod::kMix, 50.0f)) == InjectStatus::Ok);
    const auto half = fx.settle({t, t}, kSettleBlocks)[0];
    // Half the dry tone and half the shifted one, both present at once.
    REQUIRE_THAT(magnitude_at(half, kToneHz), WithinRel(0.5 * kAmplitude, 0.1));
    REQUIRE_THAT(magnitude_at(half, kToneHz + kShiftHz), WithinRel(0.5 * kAmplitude, 0.1));
}

TEST_CASE("Forge modulation: the baked table is the DSP block's own contract",
          "[host][baked][forge][forge-modulation]") {
    // Ranges are declared once, in the DSP header, and the node's table quotes
    // them. Asserted rather than eyeballed so the two cannot drift.
    using Shifter = pulp::signal::SsbFrequencyShifter;
    const auto type = mod::make_frequency_shifter_node();
    REQUIRE(type.num_input_ports == 2);
    REQUIRE(type.num_output_ports == 2);
    REQUIRE(type.lowerable);
    REQUIRE(type.baked_params.size() == 6);

    auto find = [&type](pulp::state::ParamID id) {
        for (const auto& p : type.baked_params)
            if (p.id == id) return p;
        FAIL("missing baked param");
        return type.baked_params.front();
    };

    const auto shift = find(mod::kShiftHz);
    REQUIRE_THAT(shift.min_value, WithinRel(-static_cast<float>(Shifter::kMaxShiftHz), 1e-6f));
    REQUIRE_THAT(shift.max_value, WithinRel(static_cast<float>(Shifter::kMaxShiftHz), 1e-6f));
    REQUIRE(shift.default_value == 0.0f);

    const auto feedback = find(mod::kFeedback);
    REQUIRE(feedback.min_value == 0.0f);
    REQUIRE_THAT(feedback.max_value, WithinRel(static_cast<float>(Shifter::kMaxFeedback), 1e-6f));
    REQUIRE(feedback.default_value == 0.0f);

    const auto delay = find(mod::kFeedbackDelayMs);
    REQUIRE_THAT(delay.min_value, WithinRel(static_cast<float>(Shifter::kMinDelayMs), 1e-6f));
    REQUIRE_THAT(delay.max_value, WithinRel(static_cast<float>(Shifter::kMaxLoopMs), 1e-6f));

    const auto mode = find(mod::kShiftMode);
    REQUIRE(mode.min_value == mod::kModeUp);
    REQUIRE(mode.max_value == mod::kModeStereoSplit);
    REQUIRE(mode.default_value == mod::kModeUp);
}

TEST_CASE("Forge modulation: the stepped mode param rounds to the nearest step",
          "[host][baked][forge][forge-modulation]") {
    using pulp::signal::FrequencyShiftMode;
    REQUIRE(mod::mode_from_param(0.0f) == FrequencyShiftMode::up);
    REQUIRE(mod::mode_from_param(0.49f) == FrequencyShiftMode::up);
    // A host ramping toward a stepped value must land on the nearest step, not
    // sit on the one below it for the whole ramp.
    REQUIRE(mod::mode_from_param(0.51f) == FrequencyShiftMode::down);
    REQUIRE(mod::mode_from_param(2.0f) == FrequencyShiftMode::dual_mono);
    REQUIRE(mod::mode_from_param(3.0f) == FrequencyShiftMode::stereo_split);
    // Out of range clamps to a valid mode rather than reading past the enum.
    REQUIRE(mod::mode_from_param(-7.0f) == FrequencyShiftMode::up);
    REQUIRE(mod::mode_from_param(99.0f) == FrequencyShiftMode::up);
}

TEST_CASE("Forge modulation: the registry's worst-case gain is the loop envelope",
          "[host][baked][forge][forge-modulation]") {
    // Series law 8: a tested invariant, not an estimate. The DSP suite measures
    // both factors of it; this asserts the registry quotes the same number.
    using Shifter = pulp::signal::SsbFrequencyShifter;
    const double expected = 1.0 / (1.0 - Shifter::kMaxFeedback * Shifter::kGshiftBudget);
    REQUIRE_THAT(static_cast<double>(mod::ssb_frequency_shifter_worst_case_gain()),
                 WithinRel(expected, 1e-6));
}

TEST_CASE("Forge modulation: the reported latency is zero through the graph",
          "[host][baked][param-injection][forge][forge-modulation]") {
    // At zero shift and full wet the node is an allpass network with no bulk
    // delay, so an impulse's FIRST output sample is already non-zero. A node
    // that had acquired latency somewhere in the bake would show a run of
    // zeros first.
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    set_baseline(inj);
    fx.settle({silence(), silence()}, 8);

    auto impulse = silence();
    impulse[0] = 0.5f;
    const auto first = fx.render({impulse, impulse});
    REQUIRE(std::fabs(first[0][0]) > 1e-4f);
}

TEST_CASE("Forge modulation: the node's process path allocates nothing",
          "[host][baked][forge][forge-modulation][rt-safety]") {
    auto fx = make_fixture();
    ParamInjector inj = fx.claim_injector();
    const auto t = tone();
    fx.settle({t, t}, 8);

    // Buffers and views built outside the probe, which is what
    // `ReusableRenderer` exists for. The fixture's convenience `render()`
    // constructs its own output vectors, so driving it from inside a probe
    // would report the harness's allocations as the node's.
    pulp::test::ReusableRenderer<2> renderer(fx, {t, t});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(mod::kShiftHz, -2000.0f + 125.0f * static_cast<float>(b)));
        inj.inject(immediate(mod::kFeedback, 0.02f * static_cast<float>(b)));
        inj.inject(immediate(mod::kFeedbackDelayMs, 0.1f + 1.5f * static_cast<float>(b % 32)));
        inj.inject(immediate(mod::kMix, 100.0f - 2.0f * static_cast<float>(b)));
        inj.inject(immediate(mod::kStereoSpread, 3.0f * static_cast<float>(b)));
        inj.inject(immediate(mod::kShiftMode, static_cast<float>(b % 4)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("Forge modulation: the chorus voicings are distinct registrations",
          "[host][baked][forge][forge-modulation][chorus]") {
    // The realization axis, asserted as an axis. Four voicings, three Juno
    // positions on one of them, and the colour stage on any — every combination
    // has to be a distinct type id, because a registry that gave two
    // differently-behaving nodes one id would load a session and sound wrong.
    using Voicing = chorus_ns::Voicing;
    using JunoMode = chorus_ns::JunoMode;
    std::vector<std::string> ids;
    for (auto v : {Voicing::ce2, Voicing::juno_ensemble, Voicing::dimension_d,
                   Voicing::tri_chorus})
        for (auto m : {JunoMode::mode_I, JunoMode::mode_II, JunoMode::mode_I_plus_II})
            for (bool bbd : {false, true}) {
                const auto type = chorus_ns::make_chorus_node(v, m, bbd);
                REQUIRE(type.lowerable);
                REQUIRE(type.num_input_ports == 2);
                REQUIRE(type.num_output_ports == 2);
                REQUIRE(type.baked_params.size() == 4);
                if (v == Voicing::juno_ensemble || m == JunoMode::mode_I)
                    ids.push_back(type.type_id);
            }
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // And the reason they are realizations rather than a knob: the voicings do
    // not share a voice count, so switching one is a topology change.
    using Engine = chorus_ns::Engine;
    REQUIRE(Engine::calibration(Voicing::ce2).voices == 1);
    REQUIRE(Engine::calibration(Voicing::dimension_d).voices == 2);
    REQUIRE(Engine::calibration(Voicing::tri_chorus).voices == 3);
}

TEST_CASE("Forge modulation: the chorus mix param reaches the engine",
          "[host][baked][param-injection][forge][forge-modulation][chorus]") {
    // At mix 0 the engine is a wire, so this is a bit-exact assertion rather
    // than a tolerance — and it fails on a node that wired `mix` to any other
    // parameter, which a "the output changed" test would not.
    auto fx = Stereo(chorus_ns::make_chorus_node(), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(chorus_ns::kMix, 0.0f)) == InjectStatus::Ok);
    const auto t = tone();
    const auto out = fx.settle({t, t}, 32);
    for (int k = 0; k < kFrames; ++k) {
        REQUIRE(out[0][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
        REQUIRE(out[1][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
    }

    // ...and at full mix it is emphatically not a wire.
    REQUIRE(inj.inject(immediate(chorus_ns::kMix, 100.0f)) == InjectStatus::Ok);
    const auto wet = fx.settle({t, t}, 32);
    double difference = 0.0;
    for (int k = 0; k < kFrames; ++k)
        difference = std::max(difference, std::abs(static_cast<double>(
                                              wet[0][static_cast<std::size_t>(k)] -
                                              t[static_cast<std::size_t>(k)])));
    REQUIRE(difference > 0.05);
}

TEST_CASE("Forge modulation: the chorus rate param sets the measured modulation speed",
          "[host][baked][param-injection][forge][forge-modulation][chorus]") {
    // Measured out of the audio, not read back off the setter. The CE-2 voicing
    // is used because the Juno's three modes run at their own fixed rates and
    // ignore this control by design — testing the rate there would assert the
    // opposite of the documented behaviour.
    //
    // What is asserted is the RATIO between two injected rates rather than the
    // rate itself, and that is not a weaker claim dressed up — it is the only
    // correct one for this effect. A chorus combs a dry copy against a delayed
    // one, and the CE-2's delay sweeps across many comb periods per LFO cycle
    // (±10 ms at 3 kHz is ±377 radians), so the envelope's fundamental is a
    // HARMONIC of the LFO whose order depends on the tone, the centre delay and
    // the depth. Asserting the envelope peak equals the injected rate would be
    // asserting something false; the DSP measured 6 Hz for a 1 Hz LFO here. The
    // comb geometry is identical between the two renders below because only the
    // rate differs, so the harmonic order cancels out of the ratio exactly.
    // A low tone and a shallow depth keep the comb's harmonic order down to
    // two, and the scan band is SCALED BY THE RATE so both renders can see the
    // same set of orders. With a fixed band the faster render's higher harmonics
    // fall outside it, the two peaks land on different orders, and the ratio
    // reads 0.4 instead of 2 — a measurement artefact that looks exactly like
    // the rate knob being wired backwards.
    constexpr double kLowToneHz = 375.0;  // one whole period per block
    const auto envelope_speed = [](double rate) {
        auto fx = Stereo(chorus_ns::make_chorus_node(chorus_ns::Voicing::ce2), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(chorus_ns::kRateHz, static_cast<float>(rate))) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(chorus_ns::kDepth, 5.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(chorus_ns::kMix, 100.0f)) == InjectStatus::Ok);

        const auto t = tone_at(kLowToneHz);
        fx.settle({t, t}, 64);
        const auto trace = capture(fx, {t, t}, 768);  // ~2 s
        const auto d = demodulate(trace, kLowToneHz, 100.0, 48);
        return locate_rate(d.envelope, 0.3 * rate, 12.0 * rate, d.rate_hz, 6000);
    };

    const double slow = envelope_speed(1.0);
    const double fast = envelope_speed(2.0);
    INFO("envelope speed at 1 Hz = " << slow << ", at 2 Hz = " << fast);
    REQUIRE(slow > 0.5);  // the modulation is present at all
    // The order is small and identical for both, which is what makes the ratio
    // meaningful — a large or differing order would mean the two renders were
    // not being compared on the same feature.
    REQUIRE(slow / 1.0 < 8.0);
    REQUIRE_THAT(fast / slow, WithinRel(2.0, 0.05));
}

TEST_CASE("Forge modulation: the chorus depth and width params reach the engine",
          "[host][baked][param-injection][forge][forge-modulation][chorus]") {
    const auto t = tone();

    // Depth 0 freezes the tap at its centre delay, so the whole node becomes a
    // time-INVARIANT comb: two consecutive settled blocks are then identical.
    // At full depth they cannot be. This is a stronger statement than "the
    // output changed" — it names what depth does.
    {
        auto fx = Stereo(chorus_ns::make_chorus_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(chorus_ns::kDepth, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(chorus_ns::kMix, 100.0f)) == InjectStatus::Ok);
        fx.settle({t, t}, 64);
        const auto a = fx.render({t, t});
        const auto b = fx.render({t, t});
        for (int k = 0; k < kFrames; ++k)
            REQUIRE_THAT(a[0][static_cast<std::size_t>(k)],
                         WithinAbs(b[0][static_cast<std::size_t>(k)], 1e-6f));

        REQUIRE(inj.inject(immediate(chorus_ns::kDepth, 100.0f)) == InjectStatus::Ok);
        fx.settle({t, t}, 64);
        const auto c = fx.render({t, t});
        const auto e = fx.render({t, t});
        double moved = 0.0;
        for (int k = 0; k < kFrames; ++k)
            moved = std::max(moved, std::abs(static_cast<double>(
                                        c[0][static_cast<std::size_t>(k)] -
                                        e[0][static_cast<std::size_t>(k)])));
        REQUIRE(moved > 1e-3);
    }

    // Width drives the Dimension D's cross-feed, so it is measured on that
    // voicing — on the CE-2 it is a documented no-op, and asserting it there
    // would be asserting nothing.
    {
        // Averaged over WHOLE LFO CYCLES rather than one block. A single 2.7 ms
        // block is one instant of a 2 Hz sweep, and the instantaneous side
        // energy at one arbitrary LFO phase is not the quantity width scales —
        // measured that way the two settings read within 0.2 % of each other
        // while the engine itself separates them by a factor of two.
        const auto side_energy = [&t](float width) {
            auto fx = Stereo(chorus_ns::make_chorus_node(chorus_ns::Voicing::dimension_d), kSr,
                             kFrames);
            ParamInjector inj = fx.claim_injector();
            REQUIRE(inj.inject(immediate(chorus_ns::kStereoWidth, width)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(chorus_ns::kMix, 100.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(chorus_ns::kRateHz, 2.0f)) == InjectStatus::Ok);
            fx.settle({t, t}, 64);
            const auto pair = capture_pair(fx, {t, t}, 768);  // ~2 s, four cycles
            double energy = 0.0;
            for (std::size_t k = 0; k < pair.first.size(); ++k) {
                const double side =
                    static_cast<double>(pair.first[k]) - static_cast<double>(pair.second[k]);
                energy += side * side;
            }
            return std::sqrt(energy / static_cast<double>(pair.first.size()));
        };
        REQUIRE(side_energy(100.0f) > 1.7 * side_energy(0.0f));
    }
}

TEST_CASE("Forge modulation: the chorus registry gain is the DSP's own L1 bound",
          "[host][baked][forge][forge-modulation][chorus]") {
    // Series law 8. Delegated rather than restated, because two of its terms
    // are counterintuitive — each modulated tap carries the Lagrange kernel's
    // 1.25, and the Dimension D's cross-feed high-pass carries nearly 2.
    using Engine = chorus_ns::Engine;
    for (auto v : {chorus_ns::Voicing::ce2, chorus_ns::Voicing::dimension_d,
                   chorus_ns::Voicing::tri_chorus}) {
        Engine reference;
        reference.prepare(kSr);
        reference.set_voicing(v);
        reference.set_stereo_width(1.0f);
        REQUIRE_THAT(static_cast<double>(chorus_ns::chorus_worst_case_gain(v, kSr)),
                     WithinRel(reference.worst_case_gain(), 1e-6));
    }
    // Not the naive "one dry plus one tap": the tap alone is 1.25.
    REQUIRE(chorus_ns::chorus_worst_case_gain(chorus_ns::Voicing::ce2, kSr) > 2.0f);
}

TEST_CASE("Forge modulation: the chorus node's process path allocates nothing",
          "[host][baked][forge][forge-modulation][chorus][rt-safety]") {
    auto fx = Stereo(chorus_ns::make_chorus_node(chorus_ns::Voicing::tri_chorus,
                                                 chorus_ns::JunoMode::mode_I, true),
                     kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    const auto t = tone();
    fx.settle({t, t}, 8);
    pulp::test::ReusableRenderer<2> renderer(fx, {t, t});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(chorus_ns::kRateHz, 0.05f + 0.3f * static_cast<float>(b)));
        inj.inject(immediate(chorus_ns::kDepth, 3.0f * static_cast<float>(b)));
        inj.inject(immediate(chorus_ns::kMix, 100.0f - 3.0f * static_cast<float>(b)));
        inj.inject(immediate(chorus_ns::kStereoWidth, 3.0f * static_cast<float>(b)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("Forge modulation: the phaser stage counts are distinct registrations",
          "[host][baked][forge][forge-modulation][phaser]") {
    using Engine = phaser_ns::Engine;
    std::vector<std::string> ids;
    for (int stages : {4, 6, 8, 10, 12}) {
        const auto type = phaser_ns::make_phaser_node(stages);
        REQUIRE(type.lowerable);
        REQUIRE(type.num_input_ports == 2);
        REQUIRE(type.num_output_ports == 2);
        REQUIRE(type.baked_params.size() == 8);
        ids.push_back(type.type_id);
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Odd and out-of-range requests normalise the same way the DSP normalises
    // them, so the registered id can never claim a count the engine will not
    // run. Registering "7 stages" and running 6 would be a session that reloads
    // into a different effect.
    REQUIRE(phaser_ns::make_phaser_node(7).type_id == phaser_ns::make_phaser_node(6).type_id);
    REQUIRE(phaser_ns::make_phaser_node(99).type_id ==
            phaser_ns::make_phaser_node(Engine::kMaxStages).type_id);

    // And the reason it is a realization: the notch COUNT and the notch
    // FREQUENCIES are both functions of it, so two counts are two different
    // response functions rather than two values of one.
    REQUIRE(Engine::notch_count(4) == 2);
    REQUIRE(Engine::notch_count(12) == 6);
    REQUIRE(Engine::notch_frequency_hz(1, 4, 800.0, kSr) !=
            Engine::notch_frequency_hz(1, 12, 800.0, kSr));
}

TEST_CASE("Forge modulation: the phaser centre param puts the notch where the law says",
          "[host][baked][param-injection][forge][forge-modulation][phaser]") {
    // The strongest available statement about this knob: not "the output
    // changed" but "the null landed at the frequency the shipped notch law
    // predicts for the value injected". A node that wired `center_hz` to any
    // other parameter fails this; a difference test would not.
    for (int stages : {4, 8}) {
        // The probe tone must sit on a block DFT bin, because the capture
        // repeats one input block and an off-bin tone would step in phase at
        // every boundary. So the NOTCH target is chosen from the bin grid and
        // the centre is solved for — the lowest bin whose required centre lands
        // inside the registered range, which differs per stage count because a
        // longer cascade puts its first notch proportionally lower.
        double notch_hz = 0.0;
        double center = 0.0;
        for (int bin = 1; bin <= 32; ++bin) {
            const double candidate = bin * kBinHz;
            const double required = center_for_notch(candidate, stages);
            if (required > phaser_ns::kCenterMinHz * 1.05 &&
                required < phaser_ns::kCenterMaxHz * 0.95) {
                notch_hz = candidate;
                center = required;
                break;
            }
        }
        REQUIRE(notch_hz > 0.0);
        REQUIRE(on_bin(notch_hz));

        const auto probe = tone_at(notch_hz);
        const auto measure = [&](float centre_hz, float mix_percent) {
            auto fx = Stereo(phaser_ns::make_phaser_node(stages), kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            // Depth 0 parks the sweep at the centre, which is what makes the
            // notch stand still long enough to be located.
            REQUIRE(inj.inject(immediate(phaser_ns::kDepth, 0.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(phaser_ns::kFeedback, 0.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(phaser_ns::kMix, mix_percent)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(phaser_ns::kCenterHz, centre_hz)) == InjectStatus::Ok);
            fx.settle({probe, probe}, 96);
            return trace_magnitude_at(capture(fx, {probe, probe}, 32), notch_hz);
        };

        // The control is the SAME tone through the same node at mix 0, so the
        // comparison needs no passband frequency to be found and no assumption
        // about where the cascade happens to be flat.
        const double dry = measure(static_cast<float>(center), 0.0f);
        const double on_notch = measure(static_cast<float>(center), 50.0f);
        INFO("stages=" << stages << " centre=" << center << " notch=" << notch_hz
                       << " dry=" << dry << " notched=" << on_notch);
        REQUIRE_THAT(dry, WithinRel(static_cast<double>(kAmplitude), 0.02));
        REQUIRE(on_notch < 0.1 * dry);

        // And the null follows the CENTRE rather than being a fixed hole in the
        // node: moved away, the same tone passes.
        const double moved = measure(static_cast<float>(center * 0.5), 50.0f);
        REQUIRE(moved > 5.0 * on_notch);
    }
}

TEST_CASE("Forge modulation: the phaser mix and spread params reach the engine",
          "[host][baked][param-injection][forge][forge-modulation][phaser]") {
    const auto t = tone();

    // Mix 0 is a wire — bit-exact, so this cannot pass on a mis-wired knob.
    {
        auto fx = Stereo(phaser_ns::make_phaser_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(phaser_ns::kMix, 0.0f)) == InjectStatus::Ok);
        const auto out = fx.settle({t, t}, 96);
        for (int k = 0; k < kFrames; ++k)
            REQUIRE(out[0][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
    }

    // Spread 0 puts both channels' LFOs on one phase, so the two rails are
    // bit-identical; quadrature makes them differ. Also bit-exact in the zero
    // direction, which is what makes it a test of the spread rather than of
    // some incidental decorrelation.
    {
        auto fx = Stereo(phaser_ns::make_phaser_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(phaser_ns::kStereoSpread, 0.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(phaser_ns::kMix, 50.0f)) == InjectStatus::Ok);
        const auto mono = fx.settle({t, t}, 96);
        for (int k = 0; k < kFrames; ++k)
            REQUIRE(mono[0][static_cast<std::size_t>(k)] == mono[1][static_cast<std::size_t>(k)]);

        REQUIRE(inj.inject(immediate(phaser_ns::kStereoSpread, 0.25f)) == InjectStatus::Ok);
        fx.settle({t, t}, 96);
        const auto wide = capture_pair(fx, {t, t}, 128);
        double side = 0.0;
        for (std::size_t k = 0; k < wide.first.size(); ++k)
            side = std::max(side, std::abs(static_cast<double>(wide.first[k]) -
                                           static_cast<double>(wide.second[k])));
        REQUIRE(side > 0.01);
    }
}

TEST_CASE("Forge modulation: the phaser depth, feedback, rate, wave and stagger all move the audio",
          "[host][baked][param-injection][forge][forge-modulation][phaser]") {
    const auto t = tone();
    // A baseline every variant is compared against, so each assertion isolates
    // one knob.
    const auto render_with = [&t](pulp::state::ParamID id, float value) {
        auto fx = Stereo(phaser_ns::make_phaser_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(phaser_ns::kMix, 50.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(phaser_ns::kCenterHz, 1200.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(phaser_ns::kRateHz, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(phaser_ns::kDepth, 60.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(id, value)) == InjectStatus::Ok);
        fx.settle({t, t}, 96);
        return capture(fx, {t, t}, 128);
    };
    const auto difference = [](const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0.0;
        for (std::size_t k = 0; k < a.size(); ++k)
            d = std::max(d, std::abs(static_cast<double>(a[k]) - static_cast<double>(b[k])));
        return d;
    };

    const auto baseline = render_with(phaser_ns::kDepth, 60.0f);
    REQUIRE(difference(baseline, render_with(phaser_ns::kDepth, 0.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(phaser_ns::kFeedback,
                                             static_cast<float>(
                                                 phaser_ns::Engine::kColorOnFeedback))) > 0.01);
    REQUIRE(difference(baseline, render_with(phaser_ns::kRateHz, 5.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(phaser_ns::kWave, phaser_ns::kWaveSquare)) > 0.01);
    REQUIRE(difference(baseline, render_with(phaser_ns::kStaggerRatio,
                                             static_cast<float>(
                                                 phaser_ns::Engine::kStaggerMax))) > 0.01);
}

TEST_CASE("Forge modulation: the stepped wave param rounds to the nearest shape",
          "[host][baked][forge][forge-modulation][phaser]") {
    using pulp::signal::LfoWave;
    REQUIRE(phaser_ns::wave_from_param(0.0f) == LfoWave::sine);
    REQUIRE(phaser_ns::wave_from_param(0.49f) == LfoWave::sine);
    REQUIRE(phaser_ns::wave_from_param(0.51f) == LfoWave::triangle);
    REQUIRE(phaser_ns::wave_from_param(4.0f) == LfoWave::square);
    REQUIRE(phaser_ns::wave_from_param(6.0f) == LfoWave::smooth_random);
    REQUIRE(phaser_ns::wave_from_param(-3.0f) == LfoWave::sine);
    REQUIRE(phaser_ns::wave_from_param(42.0f) == LfoWave::sine);
}

TEST_CASE("Forge modulation: the phaser registry gain is the DSP's loop bound",
          "[host][baked][forge][forge-modulation][phaser]") {
    // Series law 8. This lineage HAS a feedback path, so the bound is a loop
    // envelope rather than a constructive sum, and the DSP publishes it as a
    // constexpr its own suite asserts against.
    using Engine = phaser_ns::Engine;
    REQUIRE_THAT(static_cast<double>(phaser_ns::phaser_worst_case_gain()),
                 WithinRel(1.0 / (1.0 - Engine::kFeedbackMax), 1e-9));
    REQUIRE_THAT(static_cast<double>(phaser_ns::phaser_worst_case_gain()),
                 WithinRel(Engine::worst_case_gain(), 1e-12));
}

TEST_CASE("Forge modulation: the phaser node's process path allocates nothing",
          "[host][baked][forge][forge-modulation][phaser][rt-safety]") {
    auto fx = Stereo(phaser_ns::make_phaser_node(12), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    const auto t = tone();
    fx.settle({t, t}, 8);
    pulp::test::ReusableRenderer<2> renderer(fx, {t, t});

    pulp::test::RtAllocationProbe probe;
    for (int b = 0; b < 32; ++b) {
        inj.inject(immediate(phaser_ns::kRateHz, 0.02f + 0.3f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kDepth, 3.0f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kCenterHz, 100.0f + 150.0f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kFeedback, -0.9f + 0.056f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kMix, 3.0f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kStereoSpread, 0.015f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kStaggerRatio, 0.85f + 0.009f * static_cast<float>(b)));
        inj.inject(immediate(phaser_ns::kWave, static_cast<float>(b % 7)));
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}

TEST_CASE("Forge modulation: the delay vibrato shifts pitch by the cents it was given",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    // The one engine here that really moves pitch, so `depth_cents` has a
    // closed-form consequence: a peak fractional shift of `2^(cents/1200) − 1`.
    // Measured out of the rendered audio by complex demodulation, and compared
    // against that formula rather than against another render.
    const auto measured_ratio = [](float cents, float rate_hz) {
        auto fx = Mono(vib_delay::make_delay_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_delay::kRateHz, rate_hz)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_delay::kDepthCents, cents)) == InjectStatus::Ok);
        const auto t = tone();
        fx.settle({t}, 64);
        const auto trace = capture(fx, {t}, 512);  // ~1.4 s
        const auto d = demodulate(trace, kToneHz, 200.0, 48);
        return peak_deviation(d.freq_hz, kToneHz) / kToneHz;
    };

    for (float cents : {25.0f, 50.0f}) {
        const double expected = std::exp2(static_cast<double>(cents) / 1200.0) - 1.0;
        const double got = measured_ratio(cents, 6.0f);
        INFO("cents=" << cents << " expected=" << expected << " measured=" << got);
        REQUIRE_THAT(got, WithinRel(expected, 0.10));
    }

    // Zero depth is a static tap: no pitch movement at all.
    REQUIRE(measured_ratio(0.0f, 6.0f) < 1e-4);
}

TEST_CASE("Forge modulation: the delay vibrato rate param sets the measured rate",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    // The modulator here is a clean sine on the delay, so unlike the chorus the
    // instantaneous-frequency trace carries the LFO's own fundamental and can be
    // compared against the injected rate directly.
    for (double rate : {5.0, 9.0}) {
        auto fx = Mono(vib_delay::make_delay_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_delay::kRateHz, static_cast<float>(rate))) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_delay::kDepthCents, 50.0f)) == InjectStatus::Ok);
        const auto t = tone();
        fx.settle({t}, 64);
        const auto trace = capture(fx, {t}, 512);
        const auto d = demodulate(trace, kToneHz, 200.0, 48);
        const double measured = locate_rate(d.freq_hz, rate * 0.5, rate * 1.5, d.rate_hz, 3000);
        REQUIRE_THAT(measured, WithinRel(rate, 0.03));
    }
}

TEST_CASE("Forge modulation: the delay vibrato's onset controls do not stall the modulation",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    // A REGRESSION TEST for the trap the node exists to avoid. Both onset
    // setters re-arm the lifecycle envelope and zero its depth scale, so a node
    // that wrote them on every sample — which is what the rest of this family's
    // params do — would hold that envelope at zero forever. The node would then
    // pass audio with NO VIBRATO while every parameter read back exactly the
    // value that was set, and the only symptom would be an effect that seemed
    // not to work. Deleting the change detection in the node makes this fail.
    auto fx = Mono(vib_delay::make_delay_vibrato_node(), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(vib_delay::kRateHz, 6.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(vib_delay::kDepthCents, 50.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(vib_delay::kFadeInMs, 300.0f)) == InjectStatus::Ok);

    const auto t = tone();
    const auto trace = capture(fx, {t}, 750);  // ~2 s, well past the 300 ms fade
    const auto d = demodulate(trace, kToneHz, 200.0, 48);

    // The early window starts at 30 ms, not at zero. The demodulator's own
    // four-pole low-pass is still charging for the first few milliseconds and
    // its startup transient reads as a larger frequency excursion than anything
    // the vibrato produces — measured from sample zero it reports 115 Hz of
    // "deviation" against a full-depth 88, which would look like the fade
    // running backwards.
    const double trace_rate = d.rate_hz;
    const auto early_begin = static_cast<std::size_t>(0.030 * trace_rate);
    const auto early_end = static_cast<std::size_t>(0.120 * trace_rate);
    REQUIRE(early_end < d.freq_hz.size());
    double early = 0.0;
    for (std::size_t i = early_begin; i < early_end; ++i)
        early = std::max(early, std::abs(d.freq_hz[i] - kToneHz));
    double late = 0.0;
    for (std::size_t i = d.freq_hz.size() / 2; i < d.freq_hz.size(); ++i)
        late = std::max(late, std::abs(d.freq_hz[i] - kToneHz));

    const double expected = (std::exp2(50.0 / 1200.0) - 1.0) * kToneHz;
    INFO("early=" << early << " late=" << late << " expected full depth=" << expected);
    REQUIRE(late > 0.8 * expected);  // the envelope DID open — the trap's assertion
    REQUIRE(early < 0.5 * late);     // ...and it opened gradually, so the fade ran
}
