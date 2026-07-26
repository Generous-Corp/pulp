#include "harness/modular_sequencing_test_support.hpp"

// ── Test 3: StageSeq gate modes and directions ────────────────────────────

TEST_CASE("StageSeq gate modes over a three-pulse stage", "[signal][sequencing][stageseq]") {
    const auto pattern = [](StageGateMode mode) {
        StageSeq64 seq;
        seq.prepare(kSr);
        seq.set_num_stages(1);
        seq.set_stage_pulse_count(0, 3);
        seq.set_stage_gate_mode(0, mode);
        auto obs = run_stage_seq(seq, 3, 200);
        return std::vector<bool>{obs[0].gate, obs[1].gate, obs[2].gate};
    };

    REQUIRE(pattern(StageGateMode::hold) == std::vector<bool>{true, true, true});
    REQUIRE(pattern(StageGateMode::single) == std::vector<bool>{true, false, false});
    REQUIRE(pattern(StageGateMode::rest) == std::vector<bool>{false, false, false});
    // `repeat` re-pulses on every clock, so it too reads high AT each clock.
    REQUIRE(pattern(StageGateMode::repeat) == std::vector<bool>{true, true, true});
}

TEST_CASE("StageSeq rest consumes its clocks — silence with duration",
          "[signal][sequencing][stageseq]") {
    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_num_stages(2);
    seq.set_stage_pulse_count(0, 3);
    seq.set_stage_gate_mode(0, StageGateMode::rest);
    seq.set_stage_pulse_count(1, 1);
    seq.set_stage_gate_mode(1, StageGateMode::hold);

    auto obs = run_stage_seq(seq, 5, 64);
    REQUIRE(obs[0].stage == 0);
    REQUIRE(obs[1].stage == 0);
    REQUIRE(obs[2].stage == 0);
    REQUIRE(obs[3].stage == 1);  // three clocks of silence, then the next stage
    REQUIRE_FALSE(obs[0].gate);
    REQUIRE_FALSE(obs[2].gate);
    REQUIRE(obs[3].gate);
}

TEST_CASE("StageSeq repeat duty is exact once a period has been measured",
          "[signal][sequencing][stageseq]") {
    constexpr int kPeriod = 480;
    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_num_stages(1);
    seq.set_stage_pulse_count(0, 4);
    seq.set_stage_gate_mode(0, StageGateMode::repeat);

    ClockLine clk{kPeriod};
    // D5: the first pulse has no measured period, so the gate stays high for
    // the whole of it. Asserted rather than glossed.
    int first_high = 0;
    for (int i = 0; i < kPeriod; ++i)
        if (seq.process(true, false, clk.tick()).gate) ++first_high;
    REQUIRE(first_high == kPeriod);

    // From the second clock on the duty is exact: `since_clock < duty · period`
    // is high on offsets 0 .. ceil(duty·period) − 1.
    const int expected_high =
        static_cast<int>(std::ceil(StageSeq64::kRepeatDuty * static_cast<double>(kPeriod)));
    for (int pulse = 0; pulse < 3; ++pulse) {
        int high = 0;
        for (int i = 0; i < kPeriod; ++i)
            if (seq.process(true, false, clk.tick()).gate) ++high;
        REQUIRE(std::abs(high - expected_high) <= 1);
    }
}

TEST_CASE("StageSeq walk orders are exact in every direction",
          "[signal][sequencing][stageseq]") {
    SECTION("forward") {
        StageSeq64 seq;
        configure_walk(seq, 4, SeqDirection::forward);
        auto obs = run_stage_seq(seq, 9, 8);
        const std::vector<int> want{0, 1, 2, 3, 0, 1, 2, 3, 0};
        for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(obs[i].stage == want[i]);
    }

    SECTION("reverse") {
        StageSeq64 seq;
        configure_walk(seq, 4, SeqDirection::reverse);
        auto obs = run_stage_seq(seq, 9, 8);
        const std::vector<int> want{0, 3, 2, 1, 0, 3, 2, 1, 0};
        for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(obs[i].stage == want[i]);
    }

    SECTION("pingpong reflects without repeating the endpoints, period 2N−2") {
        for (int n = 2; n <= 6; ++n) {
            StageSeq64 seq;
            configure_walk(seq, n, SeqDirection::pingpong);
            const int period = 2 * n - 2;
            auto obs = run_stage_seq(seq, 3 * period + 1, 4);

            // Period computed from N, not restated.
            for (std::size_t i = 0; i + static_cast<std::size_t>(period) < obs.size(); ++i)
                REQUIRE(obs[i].stage == obs[i + static_cast<std::size_t>(period)].stage);

            // Endpoints appear exactly once per period — the reflection does not
            // sit on them for two clocks.
            int at_low = 0;
            int at_high = 0;
            for (int i = 0; i < period; ++i) {
                if (obs[static_cast<std::size_t>(i)].stage == 0) ++at_low;
                if (obs[static_cast<std::size_t>(i)].stage == n - 1) ++at_high;
            }
            REQUIRE(at_low == 1);
            REQUIRE(at_high == 1);
        }
    }

    SECTION("pingpong at N = 1 is guarded — D4") {
        // 2N − 2 = 0 is not a period. A one-stage pattern stays put.
        StageSeq64 seq;
        configure_walk(seq, 1, SeqDirection::pingpong);
        auto obs = run_stage_seq(seq, 8, 4);
        for (const auto& o : obs) REQUIRE(o.stage == 0);
    }

    SECTION("random is reproducible and matches the reference stream") {
        StageSeq64 seq;
        configure_walk(seq, 8, SeqDirection::random);
        auto first = run_stage_seq(seq, 33, 4);

        RefXorshift ref(StageSeq64::kRandomSeed);
        for (std::size_t i = 1; i < first.size(); ++i)
            REQUIRE(first[i].stage == static_cast<int>(ref.next() % 8u));

        seq.reset();
        auto second = run_stage_seq(seq, 33, 4);
        for (std::size_t i = 0; i < first.size(); ++i)
            REQUIRE(first[i].stage == second[i].stage);
    }
}

TEST_CASE("StageSeq steps over skipped stages in every direction",
          "[signal][sequencing][stageseq]") {
    SECTION("forward") {
        StageSeq64 seq;
        configure_walk(seq, 4, SeqDirection::forward);
        seq.set_stage_skip(1, true);
        seq.set_stage_skip(2, true);
        auto obs = run_stage_seq(seq, 5, 8);
        const std::vector<int> want{0, 3, 0, 3, 0};
        for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(obs[i].stage == want[i]);
    }

    SECTION("a skipped stage 0 moves the reset landing forward") {
        StageSeq64 seq;
        configure_walk(seq, 4, SeqDirection::forward);
        seq.set_stage_skip(0, true);
        auto obs = run_stage_seq(seq, 4, 8);
        const std::vector<int> want{1, 2, 3, 1};
        for (std::size_t i = 0; i < want.size(); ++i) REQUIRE(obs[i].stage == want[i]);
    }

    SECTION("random rejection-samples past skips") {
        StageSeq64 seq;
        configure_walk(seq, 8, SeqDirection::random);
        for (int s = 0; s < 8; ++s) seq.set_stage_skip(s, s % 2 == 1);
        auto obs = run_stage_seq(seq, 64, 4);
        for (const auto& o : obs) REQUIRE(o.stage % 2 == 0);
    }
}

TEST_CASE("StageSeq all-skip guard: advance is a no-op and the gate stays low",
          "[signal][sequencing][stageseq]") {
    StageSeq64 seq;
    configure_walk(seq, 4, SeqDirection::forward);
    for (int s = 0; s < 4; ++s) {
        seq.set_stage_skip(s, true);
        seq.set_stage_pitch(s, 1.0 + s);
    }

    auto obs = run_stage_seq(seq, 200, 4);
    for (const auto& o : obs) {
        REQUIRE(o.stage == 0);
        REQUIRE_FALSE(o.gate);
        REQUIRE_THAT(o.pitch, WithinAbs(0.0, 1e-12));  // never entered a stage
    }
    REQUIRE_FALSE(seq.started());

    // The same guard holds in every direction, including random (which would
    // otherwise reject-sample forever).
    for (auto dir : {SeqDirection::reverse, SeqDirection::pingpong, SeqDirection::random}) {
        StageSeq64 s2;
        configure_walk(s2, 4, dir);
        for (int s = 0; s < 4; ++s) s2.set_stage_skip(s, true);
        auto o2 = run_stage_seq(s2, 100, 4);
        for (const auto& o : o2) REQUIRE_FALSE(o.gate);
    }
}

TEST_CASE("StageSeq: the first clock after a reset is stage 0's first pulse",
          "[signal][sequencing][stageseq]") {
    for (auto dir : {SeqDirection::forward, SeqDirection::reverse, SeqDirection::pingpong,
                     SeqDirection::random}) {
        StageSeq64 seq;
        configure_walk(seq, 5, dir);
        (void)run_stage_seq(seq, 7, 4);
        seq.apply_reset_edge();
        auto obs = run_stage_seq(seq, 1, 4);
        REQUIRE(obs.front().stage == 0);
        REQUIRE(obs.front().pulse == 0);
    }
}

// ── Test 4: StageSeq slide ────────────────────────────────────────────────

namespace {

/// Drives a two-stage sequencer whose second stage slides to `target_v`, and
/// returns the number of samples after the entering clock at which the pitch
/// first arrives.
int slide_arrival_samples(double target_v, bool slide_on) {
    constexpr int kPeriod = 8000;  // longer than any slide under test
    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_num_stages(2);
    for (int s = 0; s < 2; ++s) {
        seq.set_stage_pulse_count(s, 1);
        seq.set_stage_gate_mode(s, StageGateMode::hold);
    }
    seq.set_stage_pitch(0, 0.0);
    seq.set_stage_pitch(1, target_v);
    seq.set_stage_slide(1, slide_on);

    ClockLine clk{kPeriod};
    for (int i = 0; i < kPeriod; ++i) (void)seq.process(true, false, clk.tick());

    int arrival = -1;
    for (int i = 0; i < kPeriod; ++i) {
        const auto f = seq.process(true, false, clk.tick());
        if (arrival < 0 && std::abs(f.pitch_v - target_v) <= 1e-12) arrival = i;
    }
    return arrival;
}

}  // namespace

TEST_CASE("StageSeq slide is constant-time and interval-independent",
          "[signal][sequencing][stageseq][slide]") {
    // Expected duration computed from the shipped constant, not restated.
    const double slide_samples = units::ms_to_samples(StageSeq64::kSlideMs, kSr);
    const int expected = static_cast<int>(std::llround(slide_samples)) - 1;

    const int one_semitone = slide_arrival_samples(1.0 / 12.0, true);
    const int one_octave = slide_arrival_samples(1.0, true);

    REQUIRE(std::abs(one_semitone - expected) <= 1);
    REQUIRE(std::abs(one_octave - expected) <= 1);
    // The whole point of constant TIME: the two take the same time.
    REQUIRE(std::abs(one_semitone - one_octave) <= 1);

    // A non-slide stage steps within one sample.
    REQUIRE(slide_arrival_samples(1.0, false) == 0);
}

TEST_CASE("StageSeq slide freezes while stopped and resumes on run",
          "[signal][sequencing][stageseq][slide]") {
    StageSeq64 seq;
    seq.prepare(kSr);
    seq.set_num_stages(2);
    for (int s = 0; s < 2; ++s) {
        seq.set_stage_pulse_count(s, 1);
        seq.set_stage_gate_mode(s, StageGateMode::hold);
    }
    seq.set_stage_pitch(0, 0.0);
    seq.set_stage_pitch(1, 1.0);
    seq.set_stage_slide(1, true);

    (void)seq.process(true, false, true);   // land on stage 0
    (void)seq.process(true, false, true);   // enter stage 1, slide begins
    for (int i = 0; i < 200; ++i) (void)seq.process(true, false, false);
    const double mid = seq.pitch_v();
    REQUIRE(mid > 0.0);
    REQUIRE(mid < 1.0);

    // Order of operations rule (b) returns before the smoother updates, so a
    // stopped transport freezes the glide where it stood.
    for (int i = 0; i < 500; ++i) {
        const auto f = seq.process(false, false, false);
        REQUIRE_THAT(f.pitch_v, WithinAbs(mid, 1e-12));
    }
    const auto resumed = seq.process(true, false, false);
    REQUIRE(resumed.pitch_v > mid);
}

// ── Test 5: Cartesian walk ────────────────────────────────────────────────

TEST_CASE("Cartesian: an X-only clock loops row 0 in exact cell order",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 walk;
    walk.prepare(kSr);
    walk.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) walk.set_value(x, y, y + 0.1 * x);

    ClockLine xc{10};
    std::vector<int> cells;
    int gates = 0;
    for (int i = 0; i < 10 * 9; ++i) {
        const bool edge = xc.tick();
        const auto f = walk.process(true, false, edge, false);
        if (edge) {
            cells.push_back(walk.cell_x());
            REQUIRE(walk.cell_y() == 0);
            REQUIRE_THAT(f.cv, WithinAbs(0.1 * walk.cell_x(), 1e-12));
        }
        if (f.gate) ++gates;
    }
    const std::vector<int> want{0, 1, 2, 3, 0, 1, 2, 3, 0};
    REQUIRE(cells == want);
    REQUIRE(gates == 9);  // one per position-changing clock, including the downbeat
}

TEST_CASE("Cartesian: independent X and Y produce the closed-form counter walk",
          "[signal][sequencing][cartesian]") {
    const auto walk_cells = [](int x_period, int y_period, int clocks) {
        CartesianWalk64 w;
        w.prepare(kSr);
        w.set_size(4, 4);
        ClockLine xc{x_period};
        ClockLine yc{y_period};
        std::vector<std::pair<int, int>> cells;
        for (int i = 0; i < x_period * clocks; ++i) {
            const bool xe = xc.tick();
            const bool ye = yc.tick();
            (void)w.process(true, false, xe, ye);
            if (xe) cells.emplace_back(w.cell_x(), w.cell_y());
        }
        return cells;
    };

    SECTION("Y at four times the X period gives a 16-cell super-cycle") {
        const auto cells = walk_cells(10, 40, 33);
        for (std::size_t k = 0; k < cells.size(); ++k) {
            const int kk = static_cast<int>(k);
            REQUIRE(cells[k].first == kk % 4);
            REQUIRE(cells[k].second == (kk / 4) % 4);
        }
        // Period 16 and all 16 cells visited.
        for (std::size_t k = 0; k + 16 < cells.size(); ++k) REQUIRE(cells[k] == cells[k + 16]);
        bool seen[4][4] = {};
        for (int k = 0; k < 16; ++k)
            seen[cells[static_cast<std::size_t>(k)].second][cells[static_cast<std::size_t>(k)].first] =
                true;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x) REQUIRE(seen[y][x]);
    }

    SECTION("coprime periods give an lcm super-cycle in a non-obvious order") {
        const auto cells = walk_cells(10, 30, 37);
        for (std::size_t k = 0; k < cells.size(); ++k) {
            const int kk = static_cast<int>(k);
            REQUIRE(cells[k].first == kk % 4);
            REQUIRE(cells[k].second == (kk / 3) % 4);
        }
        // X repeats every 4 clocks, Y every 12 → lcm(4, 12) = 12.
        for (std::size_t k = 0; k + 12 < cells.size(); ++k) REQUIRE(cells[k] == cells[k + 12]);
        REQUIRE(cells[0] != cells[4]);  // not simply the X loop
    }
}

TEST_CASE("Cartesian: a simultaneous X and Y edge is one gate",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) w.set_value(x, y, y * 4 + x);

    (void)w.process(true, false, true, true);  // downbeat, home cell
    REQUIRE(w.cell_x() == 0);
    REQUIRE(w.cell_y() == 0);

    int gates = 0;
    for (int i = 0; i < 8; ++i) {
        if (w.process(true, false, true, true).gate) ++gates;
        (void)w.process(true, false, false, false);
    }
    REQUIRE(gates == 8);           // eight clocks, eight gates — not sixteen
    REQUIRE(w.cell_x() == 8 % 4);  // both counters advanced once per clock
    REQUIRE(w.cell_y() == 8 % 4);
}

TEST_CASE("Cartesian: a 1x1 grid never changes position, so it never gates",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(1, 1);
    w.set_value(0, 0, 0.5);

    REQUIRE(w.process(true, false, true, true).gate);  // the downbeat still fires
    int gates = 0;
    for (int i = 0; i < 16; ++i) {
        if (w.process(true, false, true, true).gate) ++gates;
        (void)w.process(true, false, false, false);
    }
    REQUIRE(gates == 0);
    REQUIRE_THAT(w.cv(), WithinAbs(0.5, 1e-12));
}

TEST_CASE("Cartesian: CV offsets are read at clock time, not continuously",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(4, 4);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) w.set_value(x, y, y * 4 + x);

    (void)w.process(true, false, true, false);
    REQUIRE_THAT(w.cv(), WithinAbs(0.0, 1e-12));

    w.set_offsets(2, 1);
    // No clock yet: the output must not zipper to the new cell.
    for (int i = 0; i < 16; ++i) {
        const auto f = w.process(true, false, false, false);
        REQUIRE_THAT(f.cv, WithinAbs(0.0, 1e-12));
    }

    // The next clock reads the offset: counters (1,0) plus offset (2,1) = (3,1).
    const auto f = w.process(true, false, true, false);
    REQUIRE(w.cell_x() == 3);
    REQUIRE(w.cell_y() == 1);
    REQUIRE_THAT(f.cv, WithinAbs(7.0, 1e-12));

    // Negative offsets wrap rather than index out of range.
    w.set_offsets(-1, -1);
    (void)w.process(true, false, true, false);
    REQUIRE(w.cell_x() >= 0);
    REQUIRE(w.cell_x() < 4);
    REQUIRE(w.cell_y() >= 0);
    REQUIRE(w.cell_y() < 4);
}

TEST_CASE("Cartesian: row-major carries an X wrap into Y and ignores the Y clock — D6",
          "[signal][sequencing][cartesian]") {
    CartesianWalk64 w;
    w.prepare(kSr);
    w.set_size(4, 4);
    w.set_access(CartesianAccess::row_major);

    std::vector<std::pair<int, int>> cells;
    for (int k = 0; k < 9; ++k) {
        // A Y clock on every edge too: in row-major it must have no effect.
        (void)w.process(true, false, true, true);
        cells.emplace_back(w.cell_x(), w.cell_y());
        (void)w.process(true, false, false, false);
    }
    const std::vector<std::pair<int, int>> want{{0, 0}, {1, 0}, {2, 0}, {3, 0}, {0, 1},
                                                {1, 1}, {2, 1}, {3, 1}, {0, 2}};
    REQUIRE(cells == want);
}

// ── Test 6: Rungler determinism and bound ─────────────────────────────────

TEST_CASE("Rungler reproduces the worked shift sequence bit-exactly",
          "[signal][sequencing][rungler]") {
    // First: the reference in this file is checked against the spec's worked
    // example, so the header is then compared with independently-verified
    // ground truth rather than with a copy of itself.
    RefRungler ref{8, 3, 0, 2.0, 0b10110100u};
    REQUIRE(ref.code() == 4);
    REQUIRE_THAT(ref.out(), WithinAbs(2.0 * (2.0 * 4.0 / 7.0 - 1.0), 1e-12));
    ref.clock(false);
    REQUIRE(ref.reg == 0b01101001u);
    REQUIRE(ref.code() == 1);
    REQUIRE_THAT(ref.out(), WithinAbs(2.0 * (2.0 * 1.0 / 7.0 - 1.0), 1e-12));

    // Now the header, configured from its own shipped constants.
    Rungler64 r;
    r.prepare(kSr);
    RefRungler mirror{Rungler64::kDefaultBits, Rungler64::kDefaultDacBits,
                      Rungler64::kFeedbackTap, Rungler64::kRangeV, Rungler64::kSeedPattern};

    REQUIRE(r.register_bits() == mirror.reg);
    REQUIRE(r.dac_code() == mirror.code());
    REQUIRE_THAT(r.value(), WithinAbs(mirror.out(), 1e-12));

    for (int i = 0; i < 512; ++i) {
        const double y = r.process(true, false, true);
        mirror.clock(false);
        REQUIRE(r.register_bits() == mirror.reg);
        REQUIRE_THAT(y, WithinAbs(mirror.out(), 1e-12));
    }
}

TEST_CASE("Rungler output is bounded by construction over adversarial data",
          "[signal][sequencing][rungler][bound]") {
    // Law 8: the Forge `worst_case_gain` cites THIS assertion. |y| <= range_v
    // holds for any clock/data sequence because the output is a D-bit DAC code
    // mapped affinely onto [-range_v, +range_v] — there is no accumulating
    // state that could exceed it.
    constexpr int kClocks = 1000000;

    struct Config {
        int bits;
        int dac;
        int tap;
        double range;
    };
    const Config configs[] = {
        {Rungler64::kDefaultBits, Rungler64::kDefaultDacBits, Rungler64::kFeedbackTap,
         Rungler64::kRangeV},
        {Rungler64::kMinBits, Rungler64::kMinDacBits, 0, 0.5},
        {Rungler64::kMaxBits, Rungler64::kMaxDacBits, Rungler64::kMaxBits - 2, 5.0},
    };

    for (const auto& cfg : configs) {
        Rungler64 r;
        r.prepare(kSr);
        r.set_reg_bits(cfg.bits);
        r.set_dac_bits(cfg.dac);
        r.set_feedback_tap(cfg.tap);
        r.set_range_v(cfg.range);
        r.set_external_data(true);

        RefXorshift noise(0xC0FFEEu);
        double peak = 0.0;
        bool saw_positive_extreme = false;
        bool saw_negative_extreme = false;

        for (int i = 0; i < kClocks; ++i) {
            // Three adversarial data streams interleaved: all-ones, alternating,
            // and xorshift.
            const int which = i % 3;
            const bool data = which == 0 ? true : (which == 1 ? (i & 1) != 0 : noise.next() & 1u);
            const double y = r.process(true, false, true, data);
            peak = std::max(peak, std::abs(y));
            if (y >= cfg.range - 1e-12) saw_positive_extreme = true;
            if (y <= -cfg.range + 1e-12) saw_negative_extreme = true;
            REQUIRE(std::abs(y) <= cfg.range);
        }

        // The bound is TIGHT, not merely satisfied: both DAC extremes are hit,
        // so a passing test is not passing because the output stayed small.
        REQUIRE(saw_positive_extreme);
        REQUIRE(saw_negative_extreme);
        REQUIRE_THAT(peak, WithinAbs(cfg.range, 1e-12));
    }
}

TEST_CASE("Rungler: a zero seed is an absorbing state, by documented design",
          "[signal][sequencing][rungler]") {
    Rungler64 r;
    r.prepare(kSr);
    r.set_seed_pattern(0);
    const double floor_v = -Rungler64::kRangeV;
    for (int i = 0; i < 256; ++i)
        REQUIRE_THAT(r.process(true, false, true), WithinAbs(floor_v, 1e-12));

    // External data kicks it out — the documented escape.
    r.set_external_data(true);
    REQUIRE_THAT(r.process(true, false, true, true), WithinAbs(floor_v + 2.0 * Rungler64::kRangeV /
                                                                             7.0,
                                                              1e-12));
}

TEST_CASE("Rungler holds its output while the transport is stopped",
          "[signal][sequencing][rungler]") {
    Rungler64 r;
    r.prepare(kSr);
    for (int i = 0; i < 5; ++i) (void)r.process(true, false, true);
    const std::uint32_t reg = r.register_bits();
    const double held = r.value();
    for (int i = 0; i < 64; ++i) REQUIRE_THAT(r.process(false, false, true), WithinAbs(held, 1e-12));
    REQUIRE(r.register_bits() == reg);
}

