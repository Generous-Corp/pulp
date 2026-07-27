#include "harness/modular_sequencing_test_support.hpp"

// ── Test 10: determinism, block-size independence, alias parity ───────────

namespace {

/// A fixed pseudo-arbitrary transport drive, so every determinism assertion
/// exercises resets, stops and irregular clocks rather than a metronome.
struct DriveStep {
    bool run;
    bool reset_edge;
    bool clock;
    bool clock_b;
    bool data;
};

std::vector<DriveStep> make_drive(int samples) {
    std::vector<DriveStep> drive;
    drive.reserve(static_cast<std::size_t>(samples));
    RefXorshift r(0xBEEF01u);
    for (int i = 0; i < samples; ++i) {
        const std::uint32_t w = r.next();
        drive.push_back({(w & 0xFFu) > 24u, (i % 977) == 976, (i % 53) == 0, (i % 31) == 0,
                         (w & 0x10000u) != 0u});
    }
    return drive;
}

}  // namespace

TEST_CASE("Determinism: render, reset, re-render is bit-identical for every block",
          "[signal][sequencing][determinism]") {
    const auto drive = make_drive(20000);

    SECTION("StageSeqT") {
        StageSeq64 seq;
        configure_walk(seq, 6, SeqDirection::random);
        seq.set_stage_slide(3, true);
        seq.set_stage_gate_mode(2, StageGateMode::repeat);
        seq.set_stage_gate_mode(4, StageGateMode::rest);
        seq.set_stage_pulse_count(1, 3);

        const auto render = [&] {
            std::vector<double> out;
            out.reserve(drive.size() * 2);
            for (const auto& d : drive) {
                const auto f = seq.process(d.run, d.reset_edge, d.clock);
                out.push_back(f.pitch_v);
                out.push_back(f.gate ? 1.0 : 0.0);
            }
            return out;
        };
        seq.reset();
        const auto a = render();
        seq.reset();
        const auto b = render();
        REQUIRE(a == b);
    }

    SECTION("CartesianWalkT") {
        CartesianWalk64 w;
        w.prepare(kSr);
        w.set_size(4, 3);
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 4; ++x) w.set_value(x, y, 0.07 * (y * 4 + x));

        const auto render = [&] {
            std::vector<double> out;
            for (const auto& d : drive) {
                const auto f = w.process(d.run, d.reset_edge, d.clock, d.clock_b);
                out.push_back(f.cv);
                out.push_back(f.gate ? 1.0 : 0.0);
            }
            return out;
        };
        w.reset();
        const auto a = render();
        w.reset();
        const auto b = render();
        REQUIRE(a == b);
    }

    SECTION("RunglerT") {
        Rungler64 r;
        r.prepare(kSr);
        r.set_external_data(true);
        const auto render = [&] {
            std::vector<double> out;
            for (const auto& d : drive) out.push_back(r.process(d.run, d.reset_edge, d.clock, d.data));
            return out;
        };
        r.reset();
        const auto a = render();
        r.reset();
        const auto b = render();
        REQUIRE(a == b);
    }

    SECTION("QuantizeScaleT") {
        QuantizeScale64 q;
        const auto render = [&] {
            std::vector<double> out;
            RefXorshift src(0x5150u);
            for (const auto& d : drive) {
                if (d.reset_edge) q.apply_reset_edge();
                out.push_back(q.process(src.unit() * 4.0 - 2.0));
            }
            return out;
        };
        q.reset();
        const auto a = render();
        q.reset();
        const auto b = render();
        REQUIRE(a == b);
    }

    SECTION("ProbGateT") {
        ProbGate64 p;
        p.set_probability(0.37);
        const auto render = [&] {
            std::vector<double> out;
            for (const auto& d : drive) {
                if (d.reset_edge) p.apply_reset_edge();
                out.push_back(p.process_edge(d.clock) ? 1.0 : 0.0);
            }
            return out;
        };
        p.reset();
        const auto a = render();
        p.reset();
        const auto b = render();
        REQUIRE(a == b);
    }
}

TEST_CASE("Determinism: a fresh instance renders identically to a reset one",
          "[signal][sequencing][determinism]") {
    const auto drive = make_drive(4000);

    const auto render_stage = [&](StageSeq64& seq) {
        std::vector<double> out;
        for (const auto& d : drive) {
            const auto f = seq.process(d.run, d.reset_edge, d.clock);
            out.push_back(f.pitch_v);
            out.push_back(f.gate ? 1.0 : 0.0);
        }
        return out;
    };

    StageSeq64 fresh;
    configure_walk(fresh, 5, SeqDirection::pingpong);
    fresh.set_stage_slide(2, true);
    const auto a = render_stage(fresh);

    StageSeq64 used;
    configure_walk(used, 5, SeqDirection::pingpong);
    used.set_stage_slide(2, true);
    (void)render_stage(used);
    used.reset();
    const auto b = render_stage(used);
    REQUIRE(a == b);
}

TEST_CASE("Determinism: output depends only on the drive sequence, not on how it is chunked",
          "[signal][sequencing][determinism]") {
    // These blocks have no block-scoped state and no block API to get wrong, so
    // this is a structural assertion: the same call sequence delivered in
    // different groupings must produce the same samples. It is cheap and it is
    // the thing a future `process_block` overload would break first.
    const auto drive = make_drive(6000);
    const auto render = [&](const std::vector<int>& chunks) {
        StageSeq64 seq;
        configure_walk(seq, 7, SeqDirection::forward);
        seq.set_stage_gate_mode(3, StageGateMode::repeat);
        seq.set_stage_slide(5, true);
        std::vector<double> out;
        std::size_t i = 0;
        std::size_t c = 0;
        while (i < drive.size()) {
            const std::size_t n =
                std::min(static_cast<std::size_t>(chunks[c % chunks.size()]), drive.size() - i);
            for (std::size_t k = 0; k < n; ++k, ++i) {
                const auto f = seq.process(drive[i].run, drive[i].reset_edge, drive[i].clock);
                out.push_back(f.pitch_v);
                out.push_back(f.gate ? 1.0 : 0.0);
            }
            ++c;
        }
        return out;
    };

    REQUIRE(render({1}) == render({97, 13, 512, 3}));
}

TEST_CASE("Double-alias parity: the float and double instantiations agree",
          "[signal][sequencing][determinism]") {
    const auto drive = make_drive(3000);

    StageSeq f32;
    StageSeq64 f64;
    f32.prepare(kSr);
    f64.prepare(kSr);
    f32.set_num_stages(5);
    f64.set_num_stages(5);
    for (int s = 0; s < 5; ++s) {
        f32.set_stage_pitch(s, static_cast<float>(s) * 0.25f);
        f64.set_stage_pitch(s, s * 0.25);
        f32.set_stage_pulse_count(s, 1 + (s % 3));
        f64.set_stage_pulse_count(s, 1 + (s % 3));
    }
    f32.set_stage_slide(2, true);
    f64.set_stage_slide(2, true);

    for (const auto& d : drive) {
        const auto a = f32.process(d.run, d.reset_edge, d.clock);
        const auto b = f64.process(d.run, d.reset_edge, d.clock);
        REQUIRE(a.gate == b.gate);
        REQUIRE_THAT(static_cast<double>(a.pitch_v), WithinAbs(b.pitch_v, 1e-6));
    }

    Rungler r32;
    Rungler64 r64;
    r32.prepare(kSr);
    r64.prepare(kSr);
    r32.set_external_data(true);
    r64.set_external_data(true);
    for (const auto& d : drive) {
        const double a = static_cast<double>(r32.process(d.run, d.reset_edge, d.clock, d.data));
        const double b = r64.process(d.run, d.reset_edge, d.clock, d.data);
        REQUIRE(r32.register_bits() == r64.register_bits());
        REQUIRE_THAT(a, WithinAbs(b, 1e-6));
    }
}

TEST_CASE("Double-alias parity: every float alias instantiates and behaves",
          "[signal][sequencing][determinism]") {
    // The `Foo` aliases are the ones a caller reaches for first, so each must be
    // exercised at least once — a template that only ever gets instantiated at
    // `double` can carry a `float`-only compile error indefinitely.
    CartesianWalk w;
    QuantizeScale q;
    GateLogic g;
    ProbGate p;
    TransportEdge t;

    w.prepare(static_cast<float>(kSr));
    q.prepare(static_cast<float>(kSr));
    g.prepare(static_cast<float>(kSr));
    p.prepare(static_cast<float>(kSr));
    t.prepare(static_cast<float>(kSr));

    w.set_size(2, 2);
    w.set_value(1, 0, 0.5f);
    (void)w.process(true, false, true, false);
    const auto wf = w.process(true, false, true, false);
    REQUIRE(wf.gate);
    REQUIRE_THAT(static_cast<double>(wf.cv), WithinAbs(0.5, 1e-6));

    q.set_mode(QuantizeMode::edo);
    q.set_edo(12);
    REQUIRE_THAT(static_cast<double>(q.process(0.30f)), WithinAbs(4.0 / 12.0, 1e-6));

    g.set_op(GateOp::logic_xor);
    REQUIRE(g.process(true, false));
    REQUIRE_THAT(static_cast<double>(g.process_levels(1.0f, 0.0f)), WithinAbs(1.0, 1e-6));

    p.set_probability(1.0);
    REQUIRE(p.process(1.0f));

    REQUIRE(t.process(1.0f, 1.0f, 1.0f).reset_edge);
}

TEST_CASE("GateLogic returns each operation's identity on an empty input list",
          "[signal][sequencing][gatelogic]") {
    // Documented behaviour, so a tree that loses a branch degrades predictably
    // rather than collapsing every op to false.
    GateLogic64 g;
    const bool* none = nullptr;
    g.set_op(GateOp::logic_and);
    REQUIRE(g.process(none, 0));
    g.set_op(GateOp::logic_or);
    REQUIRE_FALSE(g.process(none, 0));
    g.set_op(GateOp::logic_xor);
    REQUIRE_FALSE(g.process(none, 0));
    g.set_op(GateOp::logic_nand);
    REQUIRE_FALSE(g.process(none, 0));
    g.set_op(GateOp::logic_nor);
    REQUIRE(g.process(none, 0));
    g.set_op(GateOp::logic_xnor);
    REQUIRE(g.process(none, 0));
}

// ── Test 11: RT allocation probe roster ───────────────────────────────────

TEST_CASE("RT safety: no allocation in process, set_* or reset after prepare",
          "[signal][sequencing][rt]") {
    StageSeq64 seq;
    CartesianWalk64 walk;
    Rungler64 rungler;
    QuantizeScale64 quant;
    GateLogic64 logic;
    ProbGate64 prob;
    TransportEdge64 transport;

    seq.prepare(kSr);
    walk.prepare(kSr);
    rungler.prepare(kSr);
    quant.prepare(kSr);
    logic.prepare(kSr);
    prob.prepare(kSr);
    transport.prepare(kSr);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 4096; ++i) {
            const double t = i / 4096.0;
            const bool clock = (i % 7) == 0;
            const bool reset_edge = (i % 601) == 600;
            const bool run = (i % 997) < 900;

            // set_* on the audio thread must be as safe as process.
            seq.set_num_stages(1 + (i % StageSeq64::kMaxStages));
            seq.set_direction(static_cast<SeqDirection>(i % 4));
            seq.set_stage_pitch(i % StageSeq64::kMaxStages, t * 4.0 - 2.0);
            seq.set_stage_pulse_count(i % StageSeq64::kMaxStages, 1 + (i % 8));
            seq.set_stage_gate_mode(i % StageSeq64::kMaxStages,
                                    static_cast<StageGateMode>(i % 4));
            seq.set_stage_slide(i % StageSeq64::kMaxStages, (i % 5) == 0);
            seq.set_stage_skip(i % StageSeq64::kMaxStages, (i % 11) == 0);
            seq.set_slide_ms(1.0 + 400.0 * t);
            seq.set_repeat_duty(0.1 + 0.8 * t);
            (void)seq.process(run, reset_edge, clock);

            walk.set_size(1 + (i % CartesianWalk64::kMaxDim), 1 + ((i / 3) % CartesianWalk64::kMaxDim));
            walk.set_value(i % CartesianWalk64::kMaxDim, (i / 5) % CartesianWalk64::kMaxDim, t);
            walk.set_access(static_cast<CartesianAccess>(i % 2));
            walk.set_offsets(i % 9 - 4, i % 7 - 3);
            (void)walk.process(run, reset_edge, clock, (i % 5) == 0);

            rungler.set_reg_bits(Rungler64::kMinBits + (i % 13));
            rungler.set_dac_bits(Rungler64::kMinDacBits + (i % 4));
            rungler.set_feedback_tap(i % 15);
            rungler.set_range_v(0.5 + 4.5 * t);
            rungler.set_external_data((i % 2) == 0);
            rungler.set_seed_pattern(static_cast<std::uint32_t>(i * 2654435761u) & 0xFFFFu);
            (void)rungler.process(run, reset_edge, clock, (i % 3) == 0);

            quant.set_mode(static_cast<QuantizeMode>(i % 2));
            quant.set_edo(1 + (i % QuantizeScale64::kMaxEdo));
            quant.set_scale_mask(static_cast<std::uint16_t>(i & 0x0FFF));
            quant.set_root_pc(i % 12);
            quant.set_hysteresis_cents(50.0 * t);
            if (reset_edge) quant.apply_reset_edge();
            (void)quant.process(t * 6.0 - 3.0);

            logic.set_op(static_cast<GateOp>(i % 6));
            (void)logic.process((i % 2) == 0, (i % 3) == 0);
            const bool three[3] = {(i % 2) == 0, (i % 3) == 0, (i % 5) == 0};
            (void)logic.process(three, 3);
            (void)logic.process_levels(t, 1.0 - t);

            prob.set_probability(t);
            if (reset_edge) prob.apply_reset_edge();
            (void)prob.process_edge(clock);
            (void)prob.process(clock ? 1.0 : 0.0);

            transport.set_refractory_ms(0.1 + 4.9 * t);
            transport.set_thresholds(0.5, 0.25);
            (void)transport.process(run ? 1.0 : 0.0, reset_edge ? 1.0 : 0.0, clock ? 1.0 : 0.0);

            (void)seq.latency_samples();
            (void)walk.latency_samples();
            (void)rungler.latency_samples();
            (void)quant.latency_samples();
            (void)logic.latency_samples();
            (void)prob.latency_samples();
            (void)transport.latency_samples();
        }

        seq.reset();
        walk.reset();
        rungler.reset();
        quant.reset();
        logic.reset();
        prob.reset();
        transport.reset();
    });
}

// ── Test 12: latency ──────────────────────────────────────────────────────

TEST_CASE("Latency is zero and effects land on the clock sample",
          "[signal][sequencing][latency]") {
    static_assert(StageSeq64::latency_samples() == 0);
    static_assert(CartesianWalk64::latency_samples() == 0);
    static_assert(Rungler64::latency_samples() == 0);
    static_assert(QuantizeScale64::latency_samples() == 0);
    static_assert(GateLogic64::latency_samples() == 0);
    static_assert(ProbGate64::latency_samples() == 0);
    static_assert(TransportEdge64::latency_samples() == 0);

    SECTION("StageSeq gate and CV appear on the clock sample") {
        StageSeq64 seq;
        seq.prepare(kSr);
        seq.set_num_stages(1);
        seq.set_stage_pitch(0, 0.75);
        seq.set_stage_gate_mode(0, StageGateMode::hold);

        for (int i = 0; i < 100; ++i) {
            const auto f = seq.process(true, false, false);
            REQUIRE_FALSE(f.gate);
        }
        const auto f = seq.process(true, false, true);
        REQUIRE(f.gate);                                    // same sample
        REQUIRE_THAT(f.pitch_v, WithinAbs(0.75, 1e-12));    // no slide, no delay
    }

    SECTION("Cartesian CV appears on the clock sample") {
        CartesianWalk64 w;
        w.prepare(kSr);
        w.set_size(2, 1);
        w.set_value(0, 0, 0.25);
        const auto f = w.process(true, false, true, false);
        REQUIRE(f.gate);
        REQUIRE_THAT(f.cv, WithinAbs(0.25, 1e-12));
    }

    SECTION("Rungler steps on the clock sample") {
        Rungler64 r;
        r.prepare(kSr);
        RefRungler ref{Rungler64::kDefaultBits, Rungler64::kDefaultDacBits,
                       Rungler64::kFeedbackTap, Rungler64::kRangeV, Rungler64::kSeedPattern};
        ref.clock(false);
        REQUIRE_THAT(r.process(true, false, true), WithinAbs(ref.out(), 1e-12));
    }
}
