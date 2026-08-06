#include "test_forge_sequencing_catalog_support.hpp"
#include <pulp/host/forge_eurorack_utility_catalog.hpp>

TEST_CASE("Eurorack clock divider emits one input-width pulse every N edges",
          "[host][baked][forge][eurorack][clock-divider]") {
    using Fixture = pulp::test::BakedNodeFixture<1>;
    const auto render = [](int division) {
        Fixture fx(pulp::host::eurorack::make_clock_divider_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        REQUIRE(inj.valid());
        REQUIRE(inj.inject(immediate(pulp::host::eurorack::kDivision,
                                     static_cast<float>(division))) == InjectStatus::Ok);
        return fx.render({clock_line(16)})[0];
    };

    const auto div1 = render(1);
    const auto div2 = render(2);
    for (int i = 0; i < kFrames; ++i) {
        const bool input_pulse = (i % 16) == 0;
        CHECK(high(div1[static_cast<std::size_t>(i)]) == input_pulse);
        const bool second_pulse = input_pulse && ((i / 16) % 2 == 1);
        CHECK(high(div2[static_cast<std::size_t>(i)]) == second_pulse);
    }
}

TEST_CASE("Forge sequencing quantizer: edo_n sets the step grid",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);

    for (int n : {12, 19, 24, 31, 48}) {
        // Every output must be an exact multiple of one step of THIS division.
        for (float cv : {0.07f, 0.23f, 0.41f, 0.77f}) {
            reinit(fx);
            quant_baseline(inj);
            REQUIRE(inj.inject(immediate(seqcat::quantize::kEdoN, static_cast<float>(n))) ==
                    InjectStatus::Ok);
            const float out = fx.render({flat(cv)})[0].front();
            const double steps = static_cast<double>(out) * n;
            REQUIRE_THAT(steps - std::round(steps), WithinAbs(0.0, 1e-4));
            REQUIRE(std::fabs(out - cv) <= 0.5 / n + 1e-5);  // nearest step
        }
    }
}

TEST_CASE("Forge sequencing quantizer: scale_mask and root_pc pick the allowed classes",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);

    // The mask and root are injected INSIDE the probe, after the re-init that
    // clears the hysteresis latch — `reinit` rewinds every param to its declared
    // default, so a probe that set them outside would be measuring the defaults.
    const auto pitch_class_of = [&](int mask, int root_pc, float cv) {
        reinit(fx);
        quant_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kScaleMask,
                                     static_cast<float>(mask))) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kRootPc,
                                     static_cast<float>(root_pc))) == InjectStatus::Ok);
        const float out = fx.render({flat(cv)})[0].front();
        int pc = static_cast<int>(std::lround(static_cast<double>(out) * 12.0)) % 12;
        return pc < 0 ? pc + 12 : pc;
    };

    SECTION("a mask restricts the output to its own pitch classes") {
        // A pentatonic mask: {0, 2, 4, 7, 9}.
        const int degrees[] = {0, 2, 4, 7, 9};
        int mask = 0;
        for (int d : degrees) mask |= 1 << d;
        for (int st = 0; st < 12; ++st) {
            const int pc = pitch_class_of(mask, 0, static_cast<float>(st) / 12.0f);
            REQUIRE(((mask >> pc) & 1) != 0);
        }
    }

    SECTION("the root rotates the mask") {
        const int major = static_cast<int>(sig::QuantizeScale::kMajorMask);
        // C major contains E (pc 4) but not D# (pc 3), so 3 snaps up to 4.
        REQUIRE(pitch_class_of(major, 0, 3.0f / 12.0f) == 4);
        // Rooted on D# the same mask contains pc 3 itself, so the input stands.
        REQUIRE(pitch_class_of(major, 3, 3.0f / 12.0f) == 3);
    }

    SECTION("an empty mask falls through to chromatic") {
        for (int st = 0; st < 12; ++st)
            REQUIRE(pitch_class_of(0, 0, static_cast<float>(st) / 12.0f) == st);
    }
}

TEST_CASE("Forge sequencing quantizer: hyst_cents widens the step boundary",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);

    // Latch onto semitone 4, then present an input just below the plain
    // boundary. With no hysteresis it follows; with hysteresis it holds.
    const auto follows_at = [&](float hyst_cents, float probe_semitones) {
        reinit(fx);
        quant_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, hyst_cents)) ==
                InjectStatus::Ok);
        std::vector<float> ramp(static_cast<std::size_t>(kFrames), probe_semitones / 12.0f);
        for (std::size_t k = 0; k < 64; ++k) ramp[k] = 4.0f / 12.0f;  // establish the latch
        const auto out = fx.render({ramp});
        return static_cast<double>(out[0].back()) * 12.0;
    };

    // 3.4 semitones rounds to 3 on its own.
    REQUIRE_THAT(follows_at(0.0f, 3.4f), WithinAbs(3.0, 1e-4));
    // With a 20-cent window the release point moves to 3.3, so 3.4 still holds.
    REQUIRE_THAT(follows_at(20.0f, 3.4f), WithinAbs(4.0, 1e-4));
    // Past the widened boundary it follows again.
    REQUIRE_THAT(follows_at(20.0f, 3.2f), WithinAbs(3.0, 1e-4));
    // A wider window holds further still.
    REQUIRE_THAT(follows_at(50.0f, 3.2f), WithinAbs(4.0, 1e-4));
}

TEST_CASE("Forge sequencing quantizer: the reset param is an edge, not a level",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 20.0f)) == InjectStatus::Ok);

    // Latch on 4, then sit at 3.4 semitones — inside the window, so it holds.
    std::vector<float> input(static_cast<std::size_t>(kFrames), 3.4f / 12.0f);
    for (std::size_t k = 0; k < 64; ++k) input[k] = 4.0f / 12.0f;
    REQUIRE_THAT(static_cast<double>(fx.render({input})[0].back()) * 12.0,
                 WithinAbs(4.0, 1e-4));

    // A reset edge mid-block clears the latch, so the same input re-quantizes on
    // its own merits from that sample on.
    reinit(fx);
    // ONE queue carrying the whole operating point plus the two reset events:
    // the queue form of `inject` REPLACES the pending batch rather than merging
    // into it, so a baseline published as separate singles beforehand would be
    // silently discarded here and the node would run on its declared defaults.
    pulp::state::ParameterEventQueue q;
    q.push(immediate(seqcat::quantize::kMode, 0.0f, 0));
    q.push(immediate(seqcat::quantize::kEdoN, 12.0f, 0));
    q.push(immediate(seqcat::quantize::kScaleMask, 2741.0f, 0));
    q.push(immediate(seqcat::quantize::kRootPc, 0.0f, 0));
    q.push(immediate(seqcat::quantize::kHystCents, 20.0f, 0));
    q.push(immediate(seqcat::quantize::kReset, 0.0f, 0));
    q.push(immediate(seqcat::quantize::kReset, 1.0f, 256));
    REQUIRE(inj.inject(q) == InjectStatus::Ok);
    const auto out = fx.render({input});
    REQUIRE_THAT(static_cast<double>(out[0][200]) * 12.0, WithinAbs(4.0, 1e-4));
    REQUIRE_THAT(static_cast<double>(out[0][300]) * 12.0, WithinAbs(3.0, 1e-4));

    // Holding it high does NOT keep clearing the latch — it is an edge. If it
    // were read as a level the hysteresis would be disabled for the whole block
    // and the first assertion below would read 3.
    reinit(fx);
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 20.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kReset, 1.0f)) == InjectStatus::Ok);
    const auto held_high = fx.render({input});
    REQUIRE_THAT(static_cast<double>(held_high[0].back()) * 12.0, WithinAbs(4.0, 1e-4));
}

TEST_CASE("Forge sequencing quantizer: the registry gain bound holds and is attained",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    // Law 8: the registry number is a bound this suite asserts, not an estimate.
    //
    // EDO mode is multiplicatively bounded. The output is an exact multiple of
    // one step; the latch holds a step only while the input is within
    // `0.5 + window` of it, and the window is capped at `kMaxHystSteps`. So for
    // an output of one step the input is at least `0.5 − 0.45 = 0.05` steps, and
    // the worst ratio is `1 / 0.05 = 20`. Below half a step the output is 0, so
    // there is no larger ratio anywhere.
    const double bound = 1.0 / (0.5 - sig::QuantizeScale::kMaxHystSteps);
    REQUIRE_THAT(static_cast<double>(seqcat::quantize::quantize_scale_worst_case_gain()),
                 WithinAbs(bound, 1e-9));

    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 50.0f)) == InjectStatus::Ok);

    // Construct the worst case: latch on step 1, then drop the input to just
    // inside the widened window.
    const double release = 1.0 - 0.5 - sig::QuantizeScale::kMaxHystSteps;  // 0.05 steps
    std::vector<float> input(static_cast<std::size_t>(kFrames),
                             static_cast<float>((release * 1.02) / 12.0));
    for (std::size_t k = 0; k < 64; ++k) input[k] = 1.0f / 12.0f;
    const auto out = fx.render({input});
    const double ratio = static_cast<double>(out[0].back()) / static_cast<double>(input.back());
    REQUIRE(ratio <= bound + 1e-6);   // the bound holds
    REQUIRE(ratio > 0.9 * bound);     // and it is nearly attained, so it is not slack

    // Scale-mask mode is NOT multiplicatively bounded — an input near 0 V can be
    // snapped up to six semitones by a one-note mask — so the invariant there is
    // ADDITIVE, and that is the one asserted.
    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kScaleMask,
                                 static_cast<float>(1 << 6))) == InjectStatus::Ok);
    const float offset_bound = seqcat::quantize::quantize_scale_mask_offset_bound_v();
    for (float cv : {0.0f, 0.001f, 0.2f, -0.35f, 1.7f, -2.4f}) {
        reinit(fx);
        quant_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::quantize::kScaleMask,
                                     static_cast<float>(1 << 6))) == InjectStatus::Ok);
        const float snapped = fx.render({flat(cv)})[0].front();
        REQUIRE(std::fabs(snapped) <= std::fabs(cv) + offset_bound + 1e-5f);
    }
}

TEST_CASE("Forge sequencing quantizer: the baked node is bit-reproducible across resets",
          "[host][baked][forge][forge-sequencing][quantizer][determinism]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 20.0f)) == InjectStatus::Ok);

    std::vector<float> sweep(static_cast<std::size_t>(kFrames));
    for (std::size_t k = 0; k < sweep.size(); ++k)
        sweep[k] = static_cast<float>(std::sin(k * 0.01) * 1.5);

    const auto first = fx.render({sweep});
    reinit(fx);
    quant_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::quantize::kHystCents, 20.0f)) == InjectStatus::Ok);
    REQUIRE(fx.render({sweep})[0] == first[0]);
}

TEST_CASE("Forge sequencing gate_logic: op selects the truth table, on both outputs",
          "[host][baked][forge][forge-sequencing][gatelogic]") {
    const auto type = seqcat::gate_logic::make_gate_logic_node();
    REQUIRE(type.type_id == std::string("sequencing.gate_logic"));
    REQUIRE(type.num_input_ports == 2);
    REQUIRE(type.num_output_ports == 2);
    REQUIRE(type.baked_params.size() == 1);

    pulp::test::BakedNodeFixture<2> fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();

    // (F,F) (F,T) (T,F) (T,T) for AND / OR / XOR / NAND / NOR / XNOR.
    const bool expect[6][4] = {
        {false, false, false, true},  // AND
        {false, true, true, true},    // OR
        {false, true, true, false},   // XOR
        {true, true, true, false},    // NAND
        {true, false, false, false},  // NOR
        {true, false, false, true},   // XNOR
    };

    for (int op = 0; op < 6; ++op) {
        REQUIRE(inj.inject(immediate(seqcat::gate_logic::kOp, static_cast<float>(op))) ==
                InjectStatus::Ok);
        int row = 0;
        for (float a : {0.0f, 1.0f})
            for (float b : {0.0f, 1.0f}) {
                const auto out = fx.render({flat(a), flat(b)});
                REQUIRE(high(out[0].front()) == expect[op][row]);
                // Output 1 is the complement, always — that is what makes it
                // worth having rather than a second node kept in sync by hand.
                REQUIRE(high(out[1].front()) == !expect[op][row]);
                REQUIRE((out[0].front() == 0.0f || out[0].front() == 1.0f));
                ++row;
            }
    }
}

TEST_CASE("Forge sequencing gate_logic: the outputs are gates whatever the input amplitude",
          "[host][baked][forge][forge-sequencing][gatelogic]") {
    // The registry's worst_case_gain of 1.0 is not about a signal path — there
    // isn't one. A 100 V input still produces a 1 V gate.
    pulp::test::BakedNodeFixture<2> fx(seqcat::gate_logic::make_gate_logic_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(seqcat::gate_logic::kOp, 0.0f)) == InjectStatus::Ok);

    for (float level : {1.0f, 5.0f, 100.0f, -100.0f}) {
        const auto out = fx.render({flat(level), flat(level)});
        for (float v : out[0]) REQUIRE(std::fabs(v) <= 1.0f);
        for (float v : out[1]) REQUIRE(std::fabs(v) <= 1.0f);
    }
    REQUIRE_THAT(static_cast<double>(seqcat::gate_logic::gate_logic_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));
}

TEST_CASE("Forge sequencing prob_gate: probability sets the pass density",
          "[host][baked][forge][forge-sequencing][probgate]") {
    const auto type = seqcat::prob_gate::make_prob_gate_node();
    REQUIRE(type.type_id == std::string("sequencing.prob_gate"));
    REQUIRE(type.num_input_ports == 1);
    REQUIRE(type.num_output_ports == 1);
    REQUIRE(type.baked_params.size() == 2);

    ProbFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kReset, 0.0f)) == InjectStatus::Ok);

    constexpr int kPeriod = 4;  // 128 triggers per block
    const auto trig = clock_line(kPeriod);
    const int triggers = static_cast<int>(clock_indices(kPeriod).size());

    const auto density = [&](float pct) {
        reinit(fx);
        REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, pct)) ==
                InjectStatus::Ok);
        int passed = 0;
        for (int b = 0; b < 16; ++b) passed += count_passes(fx.render({trig})[0]);
        return static_cast<double>(passed) / (triggers * 16);
    };

    REQUIRE_THAT(density(0.0f), WithinAbs(0.0, 1e-12));    // blocks everything
    REQUIRE_THAT(density(100.0f), WithinAbs(1.0, 1e-12));  // passes everything
    // And it is monotone in between, which is the directional claim.
    const double at25 = density(25.0f);
    const double at50 = density(50.0f);
    const double at75 = density(75.0f);
    REQUIRE(at25 < at50);
    REQUIRE(at50 < at75);
    REQUIRE_THAT(at50, WithinAbs(0.5, 0.05));
}

TEST_CASE("Forge sequencing prob_gate: the seed is a realization and the reset does not rewind it",
          "[host][baked][forge][forge-sequencing][probgate][determinism]") {
    const auto pattern_for = [](std::uint32_t seed) {
        ProbFixture fx(seqcat::prob_gate::make_prob_gate_node(seed), kSr, kFrames);
        auto inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
                InjectStatus::Ok);
        return fx.render({clock_line(4)})[0];
    };

    // A seeded chance gate renders the same performance every time — audition,
    // bounce, reload, same groove. Changing the seed is how you roll again.
    REQUIRE(pattern_for(0x1234567u) == pattern_for(0x1234567u));
    REQUIRE(pattern_for(0x1234567u) != pattern_for(0x7654321u));

    // A transport reset clears the edge latch but must NOT rewind the stream: a
    // live reset that rewound randomness would make every reset sound identical.
    ProbFixture fx(seqcat::prob_gate::make_prob_gate_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
            InjectStatus::Ok);
    const auto trig = clock_line(4);
    const auto block1 = fx.render({trig});
    const auto block2 = fx.render({trig});

    reinit(fx);
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
            InjectStatus::Ok);
    (void)fx.render({trig});
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kReset, 1.0f)) == InjectStatus::Ok);
    const auto after_reset_edge = fx.render({trig});
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kReset, 0.0f)) == InjectStatus::Ok);
    // The stream carried on: block 2 of the run is unchanged by the reset edge.
    REQUIRE(after_reset_edge[0] == block2[0]);

    // Whereas the lifecycle reset DOES rewind (series law 2).
    reinit(fx);
    REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
            InjectStatus::Ok);
    REQUIRE(fx.render({trig})[0] == block1[0]);
}

TEST_CASE("Forge sequencing: every node reports zero latency",
          "[host][baked][forge][forge-sequencing]") {
    // The series' latency lever is INERT in this module — no setting on any
    // block can move it — which is why no realization here is forced by latency.
    // Asserted so that stops being true loudly rather than quietly.
    REQUIRE(sig::StageSeq::latency_samples() == 0);
    REQUIRE(sig::CartesianWalk::latency_samples() == 0);
    REQUIRE(sig::Rungler::latency_samples() == 0);
    REQUIRE(sig::QuantizeScale::latency_samples() == 0);
    REQUIRE(sig::GateLogic::latency_samples() == 0);
    REQUIRE(sig::ProbGate::latency_samples() == 0);

    StageFixture stage(seqcat::stage_seq::make_stage_seq_node(), kSr, kFrames);
    REQUIRE(stage.baked().latency_samples() == 0);
    QuantFixture quant(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    REQUIRE(quant.baked().latency_samples() == 0);
}

TEST_CASE("Forge sequencing: the registry gain rows are the asserted invariants",
          "[host][baked][forge][forge-sequencing]") {
    // Five of the six have no input-to-output amplitude path at all: the clock
    // and reset ports are consumed as edges and the outputs are synthesised.
    REQUIRE_THAT(static_cast<double>(seqcat::stage_seq::stage_seq_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(static_cast<double>(seqcat::cartesian::cartesian_walk_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(static_cast<double>(seqcat::rungler::rungler_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));
    REQUIRE_THAT(static_cast<double>(seqcat::prob_gate::prob_gate_worst_case_gain()),
                 WithinAbs(1.0, 1e-9));

    // The pattern bounds are computed from the baked pattern, so a registry row
    // cannot drift from the artifact it describes.
    auto pattern = seqcat::stage_seq::default_pattern();
    // The bound covers ALL sixteen slots, not only the eight that play at the
    // default `num_stages` — that param is injectable up to the full capacity,
    // so a bound that only looked at the first eight would be wrong the moment
    // someone automated it. The default fills the upper eight an octave up.
    REQUIRE_THAT(static_cast<double>(seqcat::stage_seq::stage_seq_pitch_bound_v(pattern)),
                 WithinAbs(2.0, 1e-6));
    pattern[3].pitch_v = -4.5f;
    REQUIRE_THAT(static_cast<double>(seqcat::stage_seq::stage_seq_pitch_bound_v(pattern)),
                 WithinAbs(4.5, 1e-6));

    const auto grid = seqcat::cartesian::default_grid();
    REQUIRE_THAT(static_cast<double>(seqcat::cartesian::cartesian_walk_cv_bound_v(grid)),
                 WithinAbs(63.0 / 12.0, 1e-6));
}

TEST_CASE("Forge sequencing: the pitch bound really bounds the baked pitch output",
          "[host][baked][forge][forge-sequencing]") {
    // The registry row is only worth having if the artifact obeys it, including
    // through the slide — which is a `SlewLimiterT` between two pattern pitches
    // and therefore cannot overshoot either.
    auto pattern = ladder_pattern(sig::StageGateMode::hold);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i].pitch_v = (i % 2 == 0) ? 3.0f : -3.0f;  // maximal jumps
        pattern[i].slide = true;
    }
    const float bound = seqcat::stage_seq::stage_seq_pitch_bound_v(pattern);
    REQUIRE_THAT(static_cast<double>(bound), WithinAbs(3.0, 1e-6));

    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(pattern), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kSlideMs, 1.0f)) == InjectStatus::Ok);
    for (int b = 0; b < 4; ++b) {
        const auto out = fx.render({clock_line(16), flat(0.0f)});
        for (float v : out[0]) REQUIRE(std::fabs(v) <= bound + 1e-6f);
    }
}

TEST_CASE("Forge sequencing: invalid pattern cells are ignored by nodes and bounds",
          "[host][baked][forge][forge-sequencing][nan-recovery]") {
    auto pattern = seqcat::stage_seq::default_pattern();
    pattern[0].pitch_v = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(std::isfinite(seqcat::stage_seq::stage_seq_pitch_bound_v(pattern)));

    auto grid = seqcat::cartesian::default_grid();
    grid[0] = std::numeric_limits<float>::infinity();
    REQUIRE(std::isfinite(seqcat::cartesian::cartesian_walk_cv_bound_v(grid)));
}

TEST_CASE("Forge sequencing: no allocation in the baked render path",
          "[host][baked][forge][forge-sequencing][rt]") {
    // `ReusableRenderer` rather than the convenience `render()`, whose output
    // vectors would be attributed to the node under test.
    SECTION("stage_seq") {
        StageFixture fx(seqcat::stage_seq::make_stage_seq_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        stage_baseline(inj);
        pulp::test::ReusableRenderer<2> r(fx, {clock_line(8), flat(0.0f)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("cartesian_walk") {
        CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        cart_baseline(inj);
        pulp::test::ReusableRenderer<3> r(fx, {clock_line(8), clock_line(24), flat(0.0f)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("rungler") {
        RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        rungler_baseline(inj);
        pulp::test::ReusableRenderer<2> r(fx, {clock_line(8), flat(0.0f)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("quantize_scale") {
        QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        quant_baseline(inj);
        pulp::test::ReusableRenderer<1> r(fx, {flat(0.37f)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("gate_logic") {
        pulp::test::BakedNodeFixture<2> fx(seqcat::gate_logic::make_gate_logic_node(), kSr,
                                           kFrames);
        auto inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(seqcat::gate_logic::kOp, 2.0f)) == InjectStatus::Ok);
        pulp::test::ReusableRenderer<2> r(fx, {clock_line(8), clock_line(12)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }

    SECTION("prob_gate") {
        ProbFixture fx(seqcat::prob_gate::make_prob_gate_node(), kSr, kFrames);
        auto inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(seqcat::prob_gate::kProbabilityPct, 50.0f)) ==
                InjectStatus::Ok);
        pulp::test::ReusableRenderer<1> r(fx, {clock_line(4)});
        require_allocates_no_memory([&] {
            for (int b = 0; b < 8; ++b) r.render();
        });
    }
}

TEST_CASE("Forge sequencing: every node's params are declared with sane ranges",
          "[host][baked][forge][forge-sequencing]") {
    // A declared range is the module's canonical contract, and a default outside
    // its own range is a bug that only shows up when someone automates the knob.
    const CustomNodeType types[] = {
        seqcat::stage_seq::make_stage_seq_node(),
        seqcat::cartesian::make_cartesian_walk_node(),
        seqcat::cartesian::make_cartesian_walk_node(seqcat::cartesian::default_grid(), true),
        seqcat::rungler::make_rungler_node(),
        seqcat::quantize::make_quantize_scale_node(),
        seqcat::gate_logic::make_gate_logic_node(),
        seqcat::prob_gate::make_prob_gate_node(),
    };

    std::set<std::string> ids;
    for (const auto& t : types) {
        REQUIRE(ids.insert(t.type_id).second);  // no duplicate registrations
        REQUIRE(t.lowerable);
        REQUIRE(t.num_output_ports >= 1);
        REQUIRE_FALSE(t.baked_params.empty());
        REQUIRE(t.process_instance_baked_param);
        REQUIRE(t.create);
        REQUIRE(t.destroy);
        REQUIRE(t.prepare);
        REQUIRE(t.reset);

        std::set<pulp::state::ParamID> param_ids;
        for (const auto& p : t.baked_params) {
            REQUIRE(param_ids.insert(p.id).second);  // node-local ids are unique
            REQUIRE(p.min_value < p.max_value);
            REQUIRE(p.default_value >= p.min_value);
            REQUIRE(p.default_value <= p.max_value);
        }
    }
}
