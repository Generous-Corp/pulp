#include "harness/modular_sequencing_test_support.hpp"

// ── Test 1: reset-matrix conformance ──────────────────────────────────────

TEST_CASE("Reset matrix: StageSeq reset edge zeroes position and holds pitch",
          "[signal][sequencing][reset]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);
    seq.set_stage_pitch(2, 0.5);

    // Walk to stage 2 so there is a position worth zeroing.
    auto obs = run_stage_seq(seq, 3, 100);
    REQUIRE(obs.back().stage == 2);
    REQUIRE(seq.gate());
    const double held = seq.pitch_v();
    REQUIRE_THAT(held, WithinAbs(0.5, 1e-12));

    seq.apply_reset_edge();

    // Position to the top of the pattern, gate low.
    REQUIRE(seq.stage() == 0);
    REQUIRE(seq.pulse() == 0);
    REQUIRE_FALSE(seq.gate());
    REQUIRE_FALSE(seq.started());

    // Pitch CV holds its last value — the reset edge must not click.
    REQUIRE_THAT(seq.pitch_v(), WithinAbs(held, 1e-12));

    // And it still holds through un-clocked samples after the reset.
    for (int i = 0; i < 64; ++i) {
        const auto f = seq.process(true, false, false);
        REQUIRE_FALSE(f.gate);
        REQUIRE_THAT(f.pitch_v, WithinAbs(held, 1e-12));
    }
}

TEST_CASE("Reset matrix: StageSeq reset edge does not advance or rewind the RNG",
          "[signal][sequencing][reset][determinism]") {
    constexpr int kStages = 4;

    // Ground truth: the stage index drawn on each ADVANCING clock is
    // `next_uint() % N`. A landing clock (the first after a reset) consumes no
    // draw, because it lands on stage 0 rather than drawing.
    RefXorshift ref(StageSeq64::kRandomSeed);

    StageSeq64 seq;
    configure_walk(seq, kStages, SeqDirection::random);

    auto first = run_stage_seq(seq, 11, 32);
    REQUIRE(first.front().stage == 0);  // landing clock
    for (std::size_t i = 1; i < first.size(); ++i)
        REQUIRE(first[i].stage == static_cast<int>(ref.next() % kStages));

    // A reset edge mid-pattern: the NEXT clock lands on stage 0 without a draw,
    // and the stream then continues from exactly where it was.
    seq.apply_reset_edge();
    auto second = run_stage_seq(seq, 11, 32);
    REQUIRE(second.front().stage == 0);
    for (std::size_t i = 1; i < second.size(); ++i)
        REQUIRE(second[i].stage == static_cast<int>(ref.next() % kStages));

    // `reset()` (verb 1), by contrast, rewinds the stream.
    seq.reset();
    RefXorshift rewound(StageSeq64::kRandomSeed);
    auto third = run_stage_seq(seq, 11, 32);
    for (std::size_t i = 1; i < third.size(); ++i)
        REQUIRE(third[i].stage == static_cast<int>(rewound.next() % kStages));
}

TEST_CASE("Reset matrix: CartesianWalk reset edge homes both counters and holds CV",
          "[signal][sequencing][reset]") {
    CartesianWalk64 walk;
    walk.prepare(kSr);
    walk.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) walk.set_value(x, y, 0.1 * (y * 4 + x));

    ClockLine xc{16};
    ClockLine yc{64};
    double last = 0.0;
    for (int i = 0; i < 16 * 6; ++i) {
        const auto f = walk.process(true, false, xc.tick(), yc.tick());
        last = f.cv;
    }
    REQUIRE(walk.cell_x() != 0);

    walk.apply_reset_edge();
    REQUIRE(walk.x() == 0);
    REQUIRE(walk.y() == 0);
    REQUIRE_FALSE(walk.gate());
    REQUIRE_THAT(walk.cv(), WithinAbs(last, 1e-12));

    // CV keeps holding across un-clocked samples.
    for (int i = 0; i < 32; ++i) {
        const auto f = walk.process(true, false, false, false);
        REQUIRE_FALSE(f.gate);
        REQUIRE_THAT(f.cv, WithinAbs(last, 1e-12));
    }
}

TEST_CASE("Reset matrix: Rungler reset edge restores the seed pattern and its DAC level",
          "[signal][sequencing][reset]") {
    // D1: this is the documented exception to "continuous outputs hold".
    Rungler64 r;
    r.prepare(kSr);

    RefRungler ref{Rungler64::kDefaultBits, Rungler64::kDefaultDacBits,
                   Rungler64::kFeedbackTap, Rungler64::kRangeV,
                   Rungler64::kSeedPattern};
    const double seed_level = ref.out();

    REQUIRE(r.register_bits() == Rungler64::kSeedPattern);
    REQUIRE_THAT(r.value(), WithinAbs(seed_level, 1e-12));

    for (int i = 0; i < 37; ++i) (void)r.process(true, false, true);
    REQUIRE(r.register_bits() != Rungler64::kSeedPattern);

    r.apply_reset_edge();
    REQUIRE(r.register_bits() == Rungler64::kSeedPattern);
    REQUIRE_THAT(r.value(), WithinAbs(seed_level, 1e-12));
}

TEST_CASE("Reset matrix: quantizer reset edge clears the hysteresis latch",
          "[signal][sequencing][reset]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(12);

    // Latch onto semitone 4, then move just inside the window so the latch holds.
    (void)q.process(4.0 / 12.0);
    REQUIRE(q.latched_step() == 4);
    const double inside = (4.0 - 0.6) / 12.0;  // rounds to 3, inside 0.5 + 0.2
    REQUIRE_THAT(q.process(inside), WithinAbs(4.0 / 12.0, kFloatCompareEps));
    REQUIRE(q.latched_step() == 4);

    // Cleared: the same input now quantizes on its own merits.
    q.apply_reset_edge();
    REQUIRE_THAT(q.process(inside), WithinAbs(3.0 / 12.0, kFloatCompareEps));
    REQUIRE(q.latched_step() == 3);
}

TEST_CASE("Reset matrix: GateLogic reset is a no-op on a combinational block",
          "[signal][sequencing][reset]") {
    GateLogic64 g;
    g.set_op(GateOp::logic_xor);
    const bool before = g.process(true, false);
    g.apply_reset_edge();
    REQUIRE(g.process(true, false) == before);
    g.reset();
    REQUIRE(g.process(true, false) == before);
    REQUIRE(g.op() == GateOp::logic_xor);  // configuration survives
}

TEST_CASE("Reset matrix: ProbGate reset edge does not rewind randomness",
          "[signal][sequencing][reset][determinism]") {
    const auto run = [](ProbGate64& p, int n) {
        std::vector<char> out;
        out.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) out.push_back(p.process_edge(true) ? 1 : 0);
        return out;
    };

    ProbGate64 a;
    const auto reference = run(a, 10);

    ProbGate64 b;
    auto head = run(b, 5);
    b.apply_reset_edge();  // verb 2: latch only
    const auto tail = run(b, 5);
    for (std::size_t i = 0; i < 5; ++i) REQUIRE(tail[i] == reference[5 + i]);
    REQUIRE(b.draw_count() == 10u);

    ProbGate64 c;
    (void)run(c, 5);
    c.reset();  // verb 1: rewind
    const auto rewound = run(c, 5);
    for (std::size_t i = 0; i < 5; ++i) REQUIRE(rewound[i] == reference[i]);
    REQUIRE(c.draw_count() == 5u);
}

TEST_CASE("Reset matrix: the trigger-kit rows this header composes",
          "[signal][sequencing][reset]") {
    // §3 also tabulates the kit blocks these sequencers are built from. Their
    // reset edge IS the kit's `reset()`; each row is asserted here because this
    // header depends on the behaviour.

    SECTION("HystereticTriggerDetectT never emits a trigger from a reset") {
        HystereticTriggerDetectT<double> d;
        REQUIRE(d.process(1.0));    // armed → edge
        REQUIRE_FALSE(d.process(1.0));
        d.reset();
        // The reset itself emits nothing; the next high sample is an edge, even
        // though the input never fell.
        REQUIRE(d.process(1.0));
    }

    SECTION("GateGenT reset forces a hung gate low") {
        GateGenT<double> g;
        g.prepare(kSr);
        g.set_length_ms(100.0);
        REQUIRE_THAT(g.process(1.0), WithinAbs(1.0, 1e-12));
        REQUIRE(g.open());
        g.reset();
        REQUIRE_FALSE(g.open());
        REQUIRE_THAT(g.process(0.0), WithinAbs(0.0, 1e-12));
    }

    SECTION("ClockDividerT reset makes the next clock the downbeat") {
        ClockDividerT<double> d;
        d.set_division(4);
        REQUIRE(d.process(1.0));  // the "1"
        (void)d.process(0.0);
        REQUIRE_FALSE(d.process(1.0));
        (void)d.process(0.0);
        d.reset();
        REQUIRE(d.process(1.0));  // downbeat again, not 2 edges later
    }

    SECTION("SignalClockMultT reset aborts the in-flight subdivision burst") {
        SignalClockMultT<double> m;
        m.prepare(kSr);
        m.set_multiple(4);
        // Two edges 400 samples apart establish a period.
        REQUIRE(m.process(1.0));
        for (int i = 0; i < 399; ++i) (void)m.process(0.0);
        REQUIRE(m.process(1.0));
        m.reset();
        int emitted = 0;
        for (int i = 0; i < 400; ++i)
            if (m.process(0.0)) ++emitted;
        REQUIRE(emitted == 0);  // no orphan ticks
    }

    SECTION("BurstGenT reset aborts the burst") {
        BurstGenT<double> b;
        b.prepare(kSr);
        b.set_count(8);
        b.set_interval_ms(1.0);
        REQUIRE(b.process(1.0));
        REQUIRE(b.busy());
        b.reset();
        REQUIRE_FALSE(b.busy());
        int emitted = 0;
        for (int i = 0; i < static_cast<int>(kSr / 100.0); ++i)
            if (b.process(0.0)) ++emitted;
        REQUIRE(emitted == 0);
    }

    SECTION("TrigDelayT reset flushes the queued trigger rather than firing it") {
        TrigDelayT<double> t;
        t.prepare(kSr);
        t.set_delay_ms(10.0);
        REQUIRE_FALSE(t.process(1.0));
        t.reset();
        int emitted = 0;
        for (int i = 0; i < static_cast<int>(kSr / 10.0); ++i)
            if (t.process(0.0)) ++emitted;
        REQUIRE(emitted == 0);
    }

    SECTION("SampleHoldT reset zeroes the held value") {
        SampleHold64 sh;
        REQUIRE_THAT(sh.process_signal(0.75, 1.0), WithinAbs(0.75, 1e-12));
        sh.reset();
        REQUIRE_THAT(sh.value(), WithinAbs(0.0, 1e-12));
    }
}

// ── Test 2: transport order of operations ─────────────────────────────────

TEST_CASE("Transport: reset and clock in the same sample fire the downbeat on that clock",
          "[signal][sequencing][transport]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);
    seq.set_stage_pitch(0, -0.25);

    // Walk away from the top first, so landing on stage 0 is a real assertion.
    (void)run_stage_seq(seq, 3, 8);
    REQUIRE(seq.stage() == 2);

    // Rule 4: reset wins, then the clock advances from the top — so the very
    // sample carrying both is stage 0's first pulse.
    const auto f = seq.process(true, true, true);
    REQUIRE(seq.stage() == 0);
    REQUIRE(seq.pulse() == 0);
    REQUIRE(f.gate);
    REQUIRE_THAT(f.pitch_v, WithinAbs(-0.25, 1e-12));
}

TEST_CASE("Transport: run is a level — clocks are ignored while stopped, gate forced low",
          "[signal][sequencing][transport]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);

    auto obs = run_stage_seq(seq, 3, 8);
    REQUIRE(obs.back().stage == 2);
    REQUIRE(seq.gate());
    const double held = seq.pitch_v();

    // Stopped: clock edges do nothing, the gate is low, position survives.
    for (int i = 0; i < 40; ++i) {
        const auto f = seq.process(false, false, true);
        REQUIRE_FALSE(f.gate);
        REQUIRE_THAT(f.pitch_v, WithinAbs(held, 1e-12));
    }
    REQUIRE(seq.stage() == 2);
    REQUIRE(seq.pulse() == 0);

    // Continue (not restart): the next clock resumes from stage 2.
    const auto resumed = seq.process(true, false, true);
    REQUIRE(seq.stage() == 3);
    REQUIRE(resumed.gate);
}

TEST_CASE("Transport: a reset while stopped arms the pattern for the next run",
          "[signal][sequencing][transport]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);
    (void)run_stage_seq(seq, 3, 8);

    (void)seq.process(false, true, false);  // reset while stopped
    REQUIRE(seq.stage() == 0);
    REQUIRE_FALSE(seq.started());

    const auto f = seq.process(true, false, true);
    REQUIRE(seq.stage() == 0);
    REQUIRE(seq.pulse() == 0);
    REQUIRE(f.gate);
}

TEST_CASE("Transport: TransportEdgeT decodes a level pair into run and a debounced edge",
          "[signal][sequencing][transport]") {
    TransportEdge64 t;
    t.prepare(kSr);

    // Run is a level.
    REQUIRE_FALSE(t.process(0.0, 0.0, 0.0).run_high);
    REQUIRE(t.process(1.0, 0.0, 0.0).run_high);
    REQUIRE(t.process(1.0, 0.0, 0.0).run_high);
    REQUIRE_FALSE(t.process(0.0, 0.0, 0.0).run_high);

    // Reset is an edge, and only one per crossing.
    REQUIRE(t.process(1.0, 1.0, 0.0).reset_edge);
    REQUIRE_FALSE(t.process(1.0, 1.0, 0.0).reset_edge);

    // D3: the refractory is this block's, not the kit detector's. Inside the
    // window a fresh crossing is swallowed; outside it, it is honoured.
    const int window =
        static_cast<int>(std::llround(units::ms_to_samples(TransportEdge64::kRefractoryMs, kSr)));
    REQUIRE(window > 1);
    (void)t.process(1.0, 0.0, 0.0);
    REQUIRE_FALSE(t.process(1.0, 1.0, 0.0).reset_edge);

    t.reset();
    REQUIRE(t.process(1.0, 1.0, 0.0).reset_edge);
    for (int i = 0; i < window + 2; ++i) (void)t.process(1.0, 0.0, 0.0);
    REQUIRE(t.process(1.0, 1.0, 0.0).reset_edge);

    // The clock is NOT debounced: consecutive crossings all pass.
    t.reset();
    int clocks = 0;
    for (int i = 0; i < 8; ++i) {
        if (t.process(1.0, 0.0, 1.0).clock_edge) ++clocks;
        if (t.process(1.0, 0.0, 0.0).clock_edge) ++clocks;
    }
    REQUIRE(clocks == 8);
}
