#include "harness/modular_sequencing_test_support.hpp"

// ── Test 7: quantizer ─────────────────────────────────────────────────────

TEST_CASE("Quantizer: 12-TET nearest-step rounding", "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(QuantizeScale64::kDefaultEdo);

    const double cv = 0.30;
    const int expected_step = round_half_up(cv * QuantizeScale64::kDefaultEdo);
    REQUIRE(expected_step == 4);  // the spec's worked example, re-derived
    REQUIRE_THAT(q.process(cv),
                 WithinAbs(static_cast<double>(expected_step) / QuantizeScale64::kDefaultEdo,
                           kFloatCompareEps));
}

TEST_CASE("Quantizer: a scale mask snaps to the nearest enabled pitch class",
          "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::scale_mask);
    q.set_scale_mask(QuantizeScale64::kMajorMask);
    q.set_root_pc(0);

    const double cv = 0.26;
    const int chromatic = round_half_up(cv * 12.0);
    REQUIRE(chromatic == 3);  // D#, not in C major
    const int snapped = ref_snap(chromatic, QuantizeScale64::kMajorMask, 0);
    REQUIRE(snapped == 4);  // tie between 2 and 4 resolves upward → E
    REQUIRE_THAT(q.process(cv), WithinAbs(snapped / 12.0, kFloatCompareEps));

    // The shipped mask really is the major scale, derived rather than asserted
    // as a magic number.
    const int major_degrees[] = {0, 2, 4, 5, 7, 9, 11};
    std::uint16_t rebuilt = 0;
    for (int d : major_degrees) rebuilt = static_cast<std::uint16_t>(rebuilt | (1u << d));
    REQUIRE(rebuilt == QuantizeScale64::kMajorMask);

    // Every chromatic input maps onto an enabled class.
    for (int st = -24; st <= 24; ++st) {
        const double out = q.process(st / 12.0);
        q.apply_reset_edge();
        const int out_step = round_half_up(out * 12.0);
        int pc = out_step % 12;
        if (pc < 0) pc += 12;
        REQUIRE(((QuantizeScale64::kMajorMask >> pc) & 1u) != 0u);
    }
}

TEST_CASE("Quantizer: the root rotates the mask", "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::scale_mask);
    q.set_scale_mask(QuantizeScale64::kMajorMask);
    q.set_root_pc(2);  // D major: F# and C# enabled, F and C not

    for (int st = 0; st < 12; ++st) {
        q.apply_reset_edge();
        const double out = q.process(st / 12.0);
        const int out_step = round_half_up(out * 12.0);
        REQUIRE(out_step == ref_snap(st, QuantizeScale64::kMajorMask, 2));
    }
}

TEST_CASE("Quantizer: hysteresis holds a step until the input crosses by the stated cents",
          "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(12);

    // Window derived from the shipped constants: cents → steps at 12 per
    // octave, capped at `kMaxHystSteps` (inactive here — see D7).
    const double window_steps =
        std::min(QuantizeScale64::kHystCents * 12.0 / 1200.0, QuantizeScale64::kMaxHystSteps);
    REQUIRE_THAT(window_steps, WithinAbs(0.2, 1e-12));

    // Latch onto semitone 4, then ramp down slowly.
    (void)q.process(4.0 / 12.0);
    const double boundary = (4.0 - 0.5 - window_steps) / 12.0;  // = 3.3/12 V
    REQUIRE_THAT(boundary, WithinAbs(3.3 / 12.0, 1e-12));

    double last_out = 4.0 / 12.0;
    int changed_at_or_below_boundary = 0;
    for (int i = 0; i <= 2000; ++i) {
        const double cv = 4.0 / 12.0 - (i / 2000.0) * (1.0 / 12.0);
        const double out = q.process(cv);
        if (out != last_out) {
            // The only step change on this ramp must happen at the boundary.
            REQUIRE(cv <= boundary + 1e-9);
            REQUIRE(cv >= boundary - (1.0 / 12.0) / 2000.0 - 1e-9);
            ++changed_at_or_below_boundary;
            last_out = out;
        }
    }
    REQUIRE(changed_at_or_below_boundary == 1);

    // With hysteresis disabled the same ramp changes at the plain boundary.
    QuantizeScale64 q0;
    q0.set_mode(QuantizeMode::edo);
    q0.set_edo(12);
    q0.set_hysteresis_cents(0.0);
    (void)q0.process(4.0 / 12.0);
    REQUIRE_THAT(q0.process(3.6 / 12.0), WithinAbs(4.0 / 12.0, kFloatCompareEps));
    REQUIRE_THAT(q0.process(3.4 / 12.0), WithinAbs(3.0 / 12.0, kFloatCompareEps));
}

TEST_CASE("Quantizer: the hysteresis cap keeps adjacent steps reachable at every EDO — D7",
          "[signal][sequencing][quantizer]") {
    // Ground truth, computed rather than asserted: an adjacent step is one step
    // of travel away, and the latch releases after 0.5 + window steps. Without a
    // cap the shipped 20-cent window exceeds that above EDO-30 (one EDO-31 step
    // is 1200/31 = 38.71 cents, the boundary is 19.35 cents, and 19.35 + 20 >
    // 38.71), so the next step becomes unreachable and the output lags forever.
    for (int n = 1; n <= QuantizeScale64::kMaxEdo; ++n) {
        const double uncapped = QuantizeScale64::kHystCents * n / 1200.0;
        const double window = std::min(uncapped, QuantizeScale64::kMaxHystSteps);
        REQUIRE(0.5 + window < 1.0);  // an adjacent step is always reachable
        if (n > 30) REQUIRE(uncapped > QuantizeScale64::kMaxHystSteps);  // cap engages
        if (n <= 24) REQUIRE(uncapped <= QuantizeScale64::kMaxHystSteps);  // and only there

        // Behavioural check: a monotone one-step-at-a-time ramp tracks exactly,
        // with no accumulated lag, at every division of the octave.
        QuantizeScale64 q;
        q.set_mode(QuantizeMode::edo);
        q.set_edo(n);
        for (int step = 0; step <= 3 * n; ++step) {
            const double v = static_cast<double>(step) / n;
            REQUIRE_THAT(q.process(v), WithinAbs(v, kFloatCompareEps));
        }
    }
}

TEST_CASE("Quantizer: exact step voltages round-trip to themselves",
          "[signal][sequencing][quantizer]") {
    for (int n : {12, 19, 24, 31, QuantizeScale64::kMaxEdo}) {
        QuantizeScale64 q;
        q.set_mode(QuantizeMode::edo);
        q.set_edo(n);
        for (int step = -2 * n; step <= 2 * n; ++step) {
            const double v = static_cast<double>(step) / n;
            REQUIRE_THAT(q.process(v), WithinAbs(v, kFloatCompareEps));
        }
    }
}

TEST_CASE("Quantizer: EDO-24 steps are quarter tones", "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(24);

    const double expected_spacing = 0.5 / 12.0;  // half a semitone
    for (int step = 0; step < 24; ++step) {
        q.apply_reset_edge();
        const double a = q.process(static_cast<double>(step) / 24.0);
        q.apply_reset_edge();
        const double b = q.process(static_cast<double>(step + 1) / 24.0);
        REQUIRE_THAT(b - a, WithinAbs(expected_spacing, kFloatCompareEps));
    }
}

TEST_CASE("Quantizer: an empty mask passes the chromatic step through",
          "[signal][sequencing][quantizer]") {
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::scale_mask);
    q.set_scale_mask(0);
    for (int st = -12; st <= 12; ++st) {
        q.apply_reset_edge();
        REQUIRE_THAT(q.process(st / 12.0), WithinAbs(st / 12.0, kFloatCompareEps));
    }
}

TEST_CASE("Quantizer survives the wild CV it exists to tame",
          "[signal][sequencing][quantizer]") {
    // A quantizer's whole job is to take a rungler or a runaway envelope, so it
    // is the block most likely to be handed a value no musician would send.
    // Casting an unbounded or non-finite double to int is undefined behaviour,
    // not a wrong note, so the bound is asserted rather than assumed.
    QuantizeScale64 q;
    q.set_mode(QuantizeMode::edo);
    q.set_edo(12);

    const double hostile[] = {1e30,
                              -1e30,
                              std::numeric_limits<double>::infinity(),
                              -std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN(),
                              1e300,
                              0.0};
    for (double cv : hostile) {
        q.apply_reset_edge();
        const double out = q.process(cv);
        REQUIRE(std::isfinite(out));
        REQUIRE(std::abs(out) <= QuantizeScale64::kMaxAbsSteps / 12.0);
        // Still a valid pitch: an exact multiple of one step.
        REQUIRE_THAT(out * 12.0 - std::floor(out * 12.0 + 0.5), WithinAbs(0.0, 1e-9));
    }

    // A hostile input does not poison the latch for the next real one.
    q.process(std::numeric_limits<double>::quiet_NaN());
    q.apply_reset_edge();
    REQUIRE_THAT(q.process(0.25), WithinAbs(3.0 / 12.0, kFloatCompareEps));

    // Non-finite parameter values are clamped rather than propagated.
    QuantizeScale64 p;
    p.set_hysteresis_cents(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(std::isfinite(p.hysteresis_cents()));
    REQUIRE(p.hysteresis_cents() >= 0.0);
}

TEST_CASE("Cartesian offsets stay in range under a runaway CV",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) w.set_value(x, y, y * 4 + x);

    for (int off : {2147483647, -2147483647 - 1, 999999999, -999999999}) {
        w.set_offsets(off, off);
        for (int i = 0; i < 8; ++i) {
            (void)w.process(true, false, true, true);
            REQUIRE(w.cell_x() >= 0);
            REQUIRE(w.cell_x() < 4);
            REQUIRE(w.cell_y() >= 0);
            REQUIRE(w.cell_y() < 4);
            REQUIRE(std::isfinite(w.cv()));
        }
    }
}

TEST_CASE("StageSeq and Rungler clamp non-finite parameter values",
          "[signal][sequencing][stageseq][rungler]") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_slide_ms(nan);
    REQUIRE(std::isfinite(seq.slide_ms()));
    seq.set_slide_ms(inf);
    REQUIRE(std::isfinite(seq.slide_ms()));
    seq.set_repeat_duty(nan);
    REQUIRE(std::isfinite(seq.repeat_duty()));
    REQUIRE(seq.repeat_duty() >= 0.0);
    REQUIRE(seq.repeat_duty() <= 1.0);
    seq.set_num_stages(2);
    seq.set_stage_slide(1, true);
    for (int i = 0; i < 512; ++i) REQUIRE(std::isfinite(seq.process(true, false, i % 64 == 0).pitch_v));

    Rungler64 r;
    r.prepare(kSr);
    r.set_range_v(nan);
    REQUIRE(std::isfinite(r.range_v()));
    r.set_range_v(inf);
    REQUIRE(std::isfinite(r.range_v()));
    for (int i = 0; i < 64; ++i) REQUIRE(std::isfinite(r.process(true, false, true)));

    TransportEdge64 t;
    t.prepare(kSr);
    t.set_refractory_ms(inf);
    for (int i = 0; i < 64; ++i) (void)t.process(1.0, i % 8 == 0 ? 1.0 : 0.0, 0.0);
}

TEST_CASE("Sequencing pattern cells retain finite values on invalid configuration",
          "[signal][sequencing][nan-recovery]") {
    StageSeq64 seq;
    seq.set_stage_pitch(0, 0.75);
    seq.set_stage_pitch(0, std::numeric_limits<double>::quiet_NaN());
    REQUIRE(seq.stage_pitch(0) == 0.75);

    CartesianWalk64 walk;
    walk.set_value(0, 0, -0.5);
    walk.set_value(0, 0, std::numeric_limits<double>::infinity());
    REQUIRE(walk.value(0, 0) == -0.5);

    ProbGate64 gate;
    gate.set_probability(0.75);
    gate.set_probability(std::numeric_limits<double>::quiet_NaN());
    REQUIRE(gate.probability() == 0.75);
}

// ── Test 8: gate logic ────────────────────────────────────────────────────

TEST_CASE("GateLogic truth tables are exhaustive", "[signal][sequencing][gatelogic]") {
    struct Row {
        GateOp op;
        bool expect[4];  // (F,F) (F,T) (T,F) (T,T)
    };
    const Row rows[] = {
        {GateOp::logic_and, {false, false, false, true}},
        {GateOp::logic_or, {false, true, true, true}},
        {GateOp::logic_xor, {false, true, true, false}},
        {GateOp::logic_nand, {true, true, true, false}},
        {GateOp::logic_nor, {true, false, false, false}},
        {GateOp::logic_xnor, {true, false, false, true}},
    };

    GateLogic64 g;
    for (const auto& row : rows) {
        g.set_op(row.op);
        int i = 0;
        for (bool a : {false, true})
            for (bool b : {false, true}) {
                REQUIRE(g.process(a, b) == row.expect[i]);
                // The level-domain form agrees with the boolean form.
                const double la = a ? 1.0 : 0.0;
                const double lb = b ? 1.0 : 0.0;
                REQUIRE_THAT(g.process_levels(la, lb),
                             WithinAbs(row.expect[i] ? 1.0 : 0.0, 1e-12));
                ++i;
            }
    }
}

TEST_CASE("GateLogic N-input form is the N-input gate, not a pairwise fold",
          "[signal][sequencing][gatelogic]") {
    GateLogic64 g;
    const bool three[3] = {true, false, true};

    g.set_op(GateOp::logic_and);
    REQUIRE_FALSE(g.process(three, 3));
    g.set_op(GateOp::logic_or);
    REQUIRE(g.process(three, 3));
    g.set_op(GateOp::logic_xor);
    REQUIRE_FALSE(g.process(three, 3));  // parity of two trues
    g.set_op(GateOp::logic_nand);
    REQUIRE(g.process(three, 3));
    g.set_op(GateOp::logic_nor);
    REQUIRE_FALSE(g.process(three, 3));
    g.set_op(GateOp::logic_xnor);
    REQUIRE(g.process(three, 3));

    // A pairwise fold of NAND would give NAND(NAND(T,F),T) = NAND(T,T) = F,
    // which is NOT the 3-input NAND. This is the case that distinguishes them.
    g.set_op(GateOp::logic_nand);
    const bool fold = g.process(g.process(three[0], three[1]), three[2]);
    REQUIRE(fold != g.process(three, 3));

    // The two-input path and the vector path agree on two inputs.
    for (auto op : {GateOp::logic_and, GateOp::logic_or, GateOp::logic_xor, GateOp::logic_nand,
                    GateOp::logic_nor, GateOp::logic_xnor}) {
        g.set_op(op);
        for (bool a : {false, true})
            for (bool b : {false, true}) {
                const bool pair[2] = {a, b};
                REQUIRE(g.process(pair, 2) == g.process(a, b));
            }
    }
}

// ── Test 9: ProbGate ──────────────────────────────────────────────────────

TEST_CASE("ProbGate decisions match the reference xorshift stream",
          "[signal][sequencing][probgate][determinism]") {
    ProbGate64 p;
    p.set_probability(ProbGate64::kDefaultProbability);
    RefXorshift ref(ProbGate64::kProbSeed);

    for (int i = 0; i < 4096; ++i) {
        const bool got = p.process_edge(true);
        REQUIRE(got == (ref.unit() < ProbGate64::kDefaultProbability));
    }
    REQUIRE(p.draw_count() == 4096u);
}

TEST_CASE("ProbGate pass rate matches its probability", "[signal][sequencing][probgate]") {
    constexpr int kTriggers = 1000000;
    ProbGate64 p;
    p.set_probability(0.5);
    int passed = 0;
    for (int i = 0; i < kTriggers; ++i)
        if (p.process_edge(true)) ++passed;

    const double rate = static_cast<double>(passed) / kTriggers;
    // Acceptance tolerance: ±0.002 absolute, which is 4σ of the binomial
    // standard error (sqrt(0.25/1e6) = 5e-4) for a uniform source.
    REQUIRE_THAT(rate, WithinAbs(0.5, 0.002));
}

TEST_CASE("ProbGate extremes and stream discipline", "[signal][sequencing][probgate]") {
    SECTION("p = 0 blocks everything, p = 1 passes everything") {
        ProbGate64 zero;
        zero.set_probability(0.0);
        ProbGate64 one;
        one.set_probability(1.0);
        for (int i = 0; i < 1000; ++i) {
            REQUIRE_FALSE(zero.process_edge(true));
            REQUIRE(one.process_edge(true));
        }
    }

    SECTION("a draw is consumed per trigger regardless of p") {
        // Two instances see the same triggers with different probabilities; the
        // stream position must be identical, so switching p mid-take cannot
        // shift every later decision.
        ProbGate64 a;
        ProbGate64 b;
        a.set_probability(0.0);
        b.set_probability(1.0);
        for (int i = 0; i < 500; ++i) {
            (void)a.process_edge(true);
            (void)b.process_edge(true);
        }
        REQUIRE(a.draw_count() == b.draw_count());
        REQUIRE(a.draw_count() == 500u);

        // And a non-trigger sample consumes nothing.
        (void)a.process_edge(false);
        REQUIRE(a.draw_count() == 500u);
    }

    SECTION("the signal-domain path detects its own edges") {
        ProbGate64 p;
        p.set_probability(1.0);
        int passes = 0;
        for (int i = 0; i < 100; ++i) {
            if (p.process(1.0)) ++passes;
            if (p.process(1.0)) ++passes;  // still high: not a second edge
            if (p.process(0.0)) ++passes;
        }
        REQUIRE(passes == 100);
        REQUIRE(p.draw_count() == 100u);
    }
}
