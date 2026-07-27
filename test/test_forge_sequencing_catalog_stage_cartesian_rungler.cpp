#include "test_forge_sequencing_catalog_support.hpp"

TEST_CASE("Forge sequencing stage_seq: the node bakes with two CV ports each way",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    const auto type = seqcat::stage_seq::make_stage_seq_node();
    REQUIRE(type.type_id == std::string("sequencing.stage_seq"));
    REQUIRE(type.lowerable);
    REQUIRE(type.num_input_ports == 2);   // clock, reset
    REQUIRE(type.num_output_ports == 2);  // pitch CV, gate
    REQUIRE(type.baked_params.size() == 5);

    StageFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    REQUIRE(inj.valid());
    stage_baseline(inj);

    const auto out = fx.render({clock_line(64), flat(0.0f)});
    require_finite(out[0]);
    require_finite(out[1]);

    // The default pattern is a rising major scale; the walk visits it in order.
    const auto pitches = at_clocks(out[0], 64);
    REQUIRE(pitches.size() == 8);
    for (std::size_t i = 1; i < pitches.size(); ++i) REQUIRE(pitches[i] > pitches[i - 1]);
    REQUIRE_THAT(pitches[0], WithinAbs(0.0, 1e-6));
    REQUIRE_THAT(pitches[7], WithinAbs(12.0 / 12.0, 1e-6));  // the octave
}

TEST_CASE("Forge sequencing stage_seq: non-finite stepped injections use declared defaults",
          "[host][baked][forge][forge-sequencing][stageseq][nan][security]") {
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(),
                            std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
        StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr,
                        kFrames);
        auto inj = fx.claim_injector();
        REQUIRE(inj.valid());
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRun, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages, bad)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection, bad)) == InjectStatus::Ok);

        const auto out = fx.render({clock_line(32), flat(0.0f)});
        require_finite(out[0]);
        require_finite(out[1]);

        // Both controls are decoded with lround in the catalog. Their declared
        // defaults are eight stages and forward, so the ladder must walk 0..7
        // and wrap for every form of non-finite host input.
        const auto pitches = at_clocks(out[0], 32);
        for (std::size_t i = 0; i < pitches.size(); ++i) {
            REQUIRE_THAT(static_cast<double>(pitches[i]),
                         WithinAbs(static_cast<double>(i % 8), 1e-6));
        }
    }
}

TEST_CASE("Forge sequencing stage_seq: run gates the transport",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);

    const auto running = fx.render({clock_line(64), flat(0.0f)});
    const auto run_pitches = at_clocks(running[0], 64);
    REQUIRE(run_pitches.back() > run_pitches.front());  // it advanced

    // run = 0: clocks are ignored, the gate is forced low, the pitch holds.
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRun, 0.0f)) == InjectStatus::Ok);
    const auto stopped = fx.render({clock_line(64), flat(0.0f)});
    const float held = stopped[0].front();
    for (std::size_t k = 0; k < stopped[0].size(); ++k) {
        REQUIRE_THAT(stopped[0][k], WithinAbs(held, 1e-6));
        REQUIRE_THAT(stopped[1][k], WithinAbs(0.0, 1e-9));  // no hung gate across a stop
    }

    // Raising run continues from where it stopped rather than restarting — the
    // ladder's pitch IS its stage index, so "one stage on, modulo the pattern"
    // is the whole assertion. The eight-stage walk had reached stage 7 when the
    // transport stopped, so continuing wraps to 0; a RESTART would also land on
    // 0, so the case above (position held while stopped) is what separates them.
    REQUIRE_THAT(static_cast<double>(held), WithinAbs(7.0, 1e-6));
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRun, 1.0f)) == InjectStatus::Ok);
    const auto resumed = fx.render({clock_line(64), flat(0.0f)});
    const auto resumed_pitches = at_clocks(resumed[0], 64);
    REQUIRE_THAT(static_cast<double>(resumed_pitches[0]),
                 WithinAbs(std::fmod(static_cast<double>(held) + 1.0, 8.0), 1e-6));
    REQUIRE_THAT(static_cast<double>(resumed_pitches[1]), WithinAbs(1.0, 1e-6));
}

TEST_CASE("Forge sequencing stage_seq: num_stages sets the loop length",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);

    for (int stages : {2, 3, 5, 8}) {
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages,
                                     static_cast<float>(stages))) == InjectStatus::Ok);
        // A reset pulse on the PORT so each length starts from the top.
        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        const auto out = fx.render({clock_line(32), reset});
        const auto pitches = at_clocks(out[0], 32);

        // The ladder's pitch IS its stage index, so the walk order is readable
        // directly and the loop length is asserted rather than inferred.
        for (std::size_t i = 0; i < pitches.size(); ++i)
            REQUIRE_THAT(static_cast<double>(pitches[i]),
                         WithinAbs(static_cast<double>(i % static_cast<std::size_t>(stages)),
                                   1e-6));
    }
}

TEST_CASE("Forge sequencing stage_seq: direction picks the walk order",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages, 4.0f)) == InjectStatus::Ok);

    const auto walk = [&](int direction) {
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection,
                                     static_cast<float>(direction))) == InjectStatus::Ok);
        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        const auto out = fx.render({clock_line(32), reset});
        std::vector<int> order;
        for (float v : at_clocks(out[0], 32)) order.push_back(static_cast<int>(std::lround(v)));
        return order;
    };

    const auto forward = walk(0);
    const auto reverse = walk(1);
    const auto pingpong = walk(2);
    const auto random_walk = walk(3);

    REQUIRE(forward[0] == 0);
    REQUIRE(forward[1] == 1);
    REQUIRE(forward[2] == 2);
    REQUIRE(forward[3] == 3);
    REQUIRE(forward[4] == 0);

    REQUIRE(reverse[0] == 0);
    REQUIRE(reverse[1] == 3);
    REQUIRE(reverse[2] == 2);
    REQUIRE(reverse[3] == 1);

    // Reflects at the ends without repeating them: 0,1,2,3,2,1,0,…
    REQUIRE(pingpong[0] == 0);
    REQUIRE(pingpong[3] == 3);
    REQUIRE(pingpong[4] == 2);
    REQUIRE(pingpong[6] == 0);

    // The random walk is a walk, not a constant, and stays inside the pattern.
    for (int s : random_walk) {
        REQUIRE(s >= 0);
        REQUIRE(s <= 3);
    }
    REQUIRE(random_walk != forward);
}

TEST_CASE("Forge sequencing stage_seq: slide_ms sets the glide time",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    // A two-stage pattern with a slide onto stage 1, so the glide is the only
    // thing moving between the two clocks.
    auto pattern = ladder_pattern(sig::StageGateMode::hold);
    pattern[0].pitch_v = 0.0f;
    pattern[1].pitch_v = 1.0f;
    pattern[1].slide = true;

    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(pattern), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages, 2.0f)) == InjectStatus::Ok);

    // How far the glide has travelled 128 samples after entering stage 1.
    const auto travelled_after = [&](float slide_ms) {
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kSlideMs, slide_ms)) == InjectStatus::Ok);
        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        const auto out = fx.render({clock_line(256), reset});
        return out[0][static_cast<std::size_t>(256 + 128)];
    };

    // Constant TIME: a shorter slide has covered more of the same distance at
    // the same instant. Directional, and monotone across the declared range.
    const float fast = travelled_after(1.0f);
    const float mid = travelled_after(30.0f);
    const float slow = travelled_after(500.0f);
    REQUIRE(fast > mid);
    REQUIRE(mid > slow);
    REQUIRE_THAT(static_cast<double>(fast), WithinAbs(1.0, 1e-4));  // 1 ms is long done

    // 30 ms at 48 kHz is 1440 samples, so at 128 samples the glide is ~8.9 %
    // of the way. Computed from the injected value, not restated.
    const double expected_mid = 128.0 / (30.0 * kSr / 1000.0);
    REQUIRE_THAT(static_cast<double>(mid), WithinAbs(expected_mid, 0.02));
}

TEST_CASE("Forge sequencing stage_seq: repeat_duty_pct sets the gate's high fraction",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    constexpr int kPeriod = 64;
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(
                        ladder_pattern(sig::StageGateMode::repeat)),
                    kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);

    const auto high_fraction = [&](float duty_pct) {
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kRepeatDutyPct, duty_pct)) ==
                InjectStatus::Ok);
        const auto out = fx.render({clock_line(kPeriod), flat(0.0f)});
        // Count over ONE pulse well past the first — the first clock after a
        // reset has no measured period yet (the DSP module's D5), so its gate
        // is high for the whole pulse by design.
        int highs = 0;
        for (int k = kPeriod * 4; k < kPeriod * 5; ++k)
            if (high(out[1][static_cast<std::size_t>(k)])) ++highs;
        return static_cast<double>(highs) / kPeriod;
    };

    for (float duty : {10.0f, 25.0f, 50.0f, 75.0f, 90.0f})
        REQUIRE_THAT(high_fraction(duty), WithinAbs(duty / 100.0, 1.5 / kPeriod));
}

TEST_CASE("Forge sequencing stage_seq: the reset port returns the walk to the top",
          "[host][baked][forge][forge-sequencing][stageseq]") {
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);

    // Clock at 32; fire a reset midway, spaced clear of the 0.5 ms refractory.
    auto reset = flat(0.0f);
    reset[200] = 1.0f;
    const auto out = fx.render({clock_line(32), reset});
    const auto pitches = at_clocks(out[0], 32);

    // Clocks land at 0,32,…; the reset at 200 falls between the clocks at 192
    // and 224, so the clock at 224 (index 7) is the first after it.
    REQUIRE_THAT(static_cast<double>(pitches[6]), WithinAbs(6.0, 1e-6));
    REQUIRE_THAT(static_cast<double>(pitches[7]), WithinAbs(0.0, 1e-6));  // back to the top
    REQUIRE_THAT(static_cast<double>(pitches[8]), WithinAbs(1.0, 1e-6));
}

TEST_CASE("Forge sequencing stage_seq: the seed is a realization, not a param",
          "[host][baked][forge][forge-sequencing][stageseq][determinism]") {
    // Series law 2: a seed is which performance the artifact plays, so it is
    // frozen at registration. Two nodes, two seeds, two different random walks —
    // and each is reproducible.
    const auto walk_with_seed = [](std::uint32_t seed) {
        StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern(), seed), kSr,
                        kFrames);
        auto inj = fx.claim_injector();
        stage_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kNumStages, 8.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection, 3.0f)) == InjectStatus::Ok);
        return at_clocks(fx.render({clock_line(16), flat(0.0f)})[0], 16);
    };

    const auto a = walk_with_seed(0x2A3Bu);
    const auto b = walk_with_seed(0x2A3Bu);
    const auto c = walk_with_seed(0xBEEFu);
    REQUIRE(a == b);  // same seed, same performance
    REQUIRE(a != c);  // a different seed really is a different one
}

TEST_CASE("Forge sequencing stage_seq: the baked node is bit-reproducible across resets",
          "[host][baked][forge][forge-sequencing][stageseq][determinism]") {
    // The module's headline claim, asserted at the NODE level rather than only
    // in the DSP suite: a baked artifact renders the same sequence every time.
    StageFixture fx(seqcat::stage_seq::make_stage_seq_node(ladder_pattern()), kSr, kFrames);
    auto inj = fx.claim_injector();
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection, 3.0f)) == InjectStatus::Ok);

    const auto clock = clock_line(16);
    const auto first = fx.render({clock, flat(0.0f)});
    const auto second = fx.render({clock, flat(0.0f)});

    reinit(fx);
    stage_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::stage_seq::kDirection, 3.0f)) == InjectStatus::Ok);
    const auto after_reset = fx.render({clock, flat(0.0f)});
    const auto after_reset_2 = fx.render({clock, flat(0.0f)});

    // Bit-identical, not "close": these are the same samples or the claim is
    // false. The second block after a reset must match the second block before
    // it too, so the reset restores the RNG rather than only the position.
    REQUIRE(after_reset[0] == first[0]);
    REQUIRE(after_reset[1] == first[1]);
    REQUIRE(after_reset_2[0] == second[0]);
    REQUIRE(after_reset_2[1] == second[1]);
}

TEST_CASE("Forge sequencing cartesian: the node bakes with three CV ports each way",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    const auto type = seqcat::cartesian::make_cartesian_walk_node();
    REQUIRE(type.type_id == std::string("sequencing.cartesian_walk"));
    REQUIRE(type.num_input_ports == 3);   // X clock, Y clock, reset
    REQUIRE(type.num_output_ports == 3);  // CV, gate, end of cycle
    REQUIRE(type.baked_params.size() == 5);

    CartFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    // X only: the bottom row of four values, looping.
    const auto out = fx.render({clock_line(32), flat(0.0f), flat(0.0f)});
    require_finite(out[0]);
    // The DEFAULT grid is a chromatic ramp in VOLTS, so cell (x, y) holds
    // `index/12` — the other cases below bake an index grid instead, where the
    // cell value is the index itself and a walk order reads off directly.
    const auto cv = at_clocks(out[0], 32);
    for (std::size_t i = 0; i < cv.size(); ++i)
        REQUIRE_THAT(static_cast<double>(cv[i]),
                     WithinAbs(cell_index(static_cast<int>(i % 4), 0) / 12.0, 1e-6));
}

TEST_CASE("Forge sequencing cartesian: the two clocks are independent",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    // Y at four times the X period: the closed-form 16-cell super-cycle.
    const auto out = fx.render({clock_line(32), clock_line(128), flat(0.0f)});
    const auto cv = at_clocks(out[0], 32);
    for (std::size_t i = 0; i < cv.size(); ++i) {
        const int k = static_cast<int>(i);
        REQUIRE_THAT(static_cast<double>(cv[i]),
                     WithinAbs(cell_index(k % 4, (k / 4) % 4), 1e-6));
    }
}

TEST_CASE("Forge sequencing cartesian: grid width and height set the walk's extent",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    SECTION("grid_w sets the X loop length") {
        for (int w : {2, 3, 5, 8}) {
            REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridW,
                                         static_cast<float>(w))) == InjectStatus::Ok);
            auto reset = flat(0.0f);
            reset[0] = 1.0f;
            const auto cv = at_clocks(fx.render({clock_line(32), flat(0.0f), reset})[0], 32);
            for (std::size_t i = 0; i < cv.size(); ++i)
                REQUIRE_THAT(static_cast<double>(cv[i]),
                             WithinAbs(cell_index(static_cast<int>(i) % w, 0), 1e-6));
        }
    }

    SECTION("grid_h sets the Y loop length") {
        REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridW, 1.0f)) == InjectStatus::Ok);
        for (int h : {2, 4, 7}) {
            REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridH,
                                         static_cast<float>(h))) == InjectStatus::Ok);
            auto reset = flat(0.0f);
            reset[0] = 1.0f;
            // Clock Y only; X is one cell wide so the walk is a pure column.
            const auto cv = at_clocks(fx.render({flat(0.0f), clock_line(32), reset})[0], 32);
            for (std::size_t i = 0; i < cv.size(); ++i)
                REQUIRE_THAT(static_cast<double>(cv[i]),
                             WithinAbs(cell_index(0, static_cast<int>(i) % h), 1e-6));
        }
    }
}

TEST_CASE("Forge sequencing cartesian: the CV offsets shift which cell is read",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    const auto walk_with = [&](int x_off, int y_off) {
        REQUIRE(inj.inject(immediate(seqcat::cartesian::kXOffset,
                                     static_cast<float>(x_off))) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::cartesian::kYOffset,
                                     static_cast<float>(y_off))) == InjectStatus::Ok);
        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        return at_clocks(fx.render({clock_line(32), flat(0.0f), reset})[0], 32);
    };

    // The offset is added to the counter modulo the axis length, so it rotates
    // the sequence rather than transposing the values.
    for (auto [x_off, y_off] : {std::pair{1, 0}, std::pair{2, 1}, std::pair{-1, 0}}) {
        const auto cv = walk_with(x_off, y_off);
        for (std::size_t i = 0; i < cv.size(); ++i) {
            const int x = ((static_cast<int>(i) + x_off) % 4 + 4) % 4;
            const int y = ((y_off % 4) + 4) % 4;
            REQUIRE_THAT(static_cast<double>(cv[i]), WithinAbs(cell_index(x, y), 1e-6));
        }
    }
}

TEST_CASE("Forge sequencing cartesian: run and the reset port work as on the stage sequencer",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    (void)fx.render({clock_line(32), flat(0.0f), flat(0.0f)});

    REQUIRE(inj.inject(immediate(seqcat::cartesian::kRun, 0.0f)) == InjectStatus::Ok);
    const auto stopped = fx.render({clock_line(32), flat(0.0f), flat(0.0f)});
    const float held = stopped[0].front();
    for (std::size_t k = 0; k < stopped[0].size(); ++k) {
        REQUIRE_THAT(stopped[0][k], WithinAbs(held, 1e-6));
        REQUIRE_THAT(stopped[1][k], WithinAbs(0.0, 1e-9));
        REQUIRE_THAT(stopped[2][k], WithinAbs(0.0, 1e-9));
    }

    REQUIRE(inj.inject(immediate(seqcat::cartesian::kRun, 1.0f)) == InjectStatus::Ok);
    auto reset = flat(0.0f);
    reset[200] = 1.0f;
    const auto out = fx.render({clock_line(32), flat(0.0f), reset});
    const auto cv = at_clocks(out[0], 32);
    // The clock at 224 is the first after the reset at 200, and it lands home.
    REQUIRE_THAT(static_cast<double>(cv[7]), WithinAbs(cell_index(0, 0), 1e-6));
    REQUIRE_THAT(static_cast<double>(cv[8]), WithinAbs(cell_index(1, 0), 1e-6));
}

TEST_CASE("Forge sequencing cartesian: the end-of-cycle output fires only at the home cell",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kGridH, 1.0f)) == InjectStatus::Ok);

    const auto out = fx.render({clock_line(32), flat(0.0f), flat(0.0f)});
    const auto eoc = at_clocks(out[2], 32);
    const auto cv = at_clocks(out[0], 32);
    for (std::size_t i = 0; i < eoc.size(); ++i) {
        const bool at_home = std::lround(static_cast<double>(cv[i])) == cell_index(0, 0);
        REQUIRE(high(eoc[i]) == at_home);  // exactly the cycle boundary, no more
    }
    // On a 4-wide row that is one pulse in four.
    int fired = 0;
    for (float v : eoc)
        if (high(v)) ++fired;
    REQUIRE(fired == static_cast<int>(eoc.size()) / 4);

    // The EOC reads the COUNTERS, so a CV offset changes which notes play
    // without moving the cycle boundary.
    REQUIRE(inj.inject(immediate(seqcat::cartesian::kXOffset, 2.0f)) == InjectStatus::Ok);
    auto reset = flat(0.0f);
    reset[0] = 1.0f;
    const auto shifted = fx.render({clock_line(32), flat(0.0f), reset});
    const auto shifted_eoc = at_clocks(shifted[2], 32);
    REQUIRE(high(shifted_eoc[0]));
    REQUIRE_FALSE(high(shifted_eoc[1]));
    REQUIRE(high(shifted_eoc[4]));  // still every fourth clock
    // …but the cell it plays there has moved.
    REQUIRE_THAT(static_cast<double>(at_clocks(shifted[0], 32)[0]),
                 WithinAbs(cell_index(2, 0), 1e-6));
}

TEST_CASE("Forge sequencing cartesian: access mode is a realization, and row-major ignores Y",
          "[host][baked][forge][forge-sequencing][cartesian]") {
    // Two registered type ids, exactly as the diode-bridge member splits on
    // detection topology — because in row-major, INPUT PORT 1 IS NOT READ.
    const auto independent = seqcat::cartesian::make_cartesian_walk_node(index_grid(), false);
    const auto row_major = seqcat::cartesian::make_cartesian_walk_node(index_grid(), true);
    REQUIRE(independent.type_id != row_major.type_id);
    REQUIRE(row_major.type_id == std::string("sequencing.cartesian_walk_row_major"));

    SECTION("independent: a Y clock alone moves the walk") {
        CartFixture fx(independent, kSr, kFrames);
        auto inj = fx.claim_injector();
        cart_baseline(inj);
        const auto cv = at_clocks(fx.render({flat(0.0f), clock_line(32), flat(0.0f)})[0], 32);
        REQUIRE_THAT(static_cast<double>(cv[1]), WithinAbs(cell_index(0, 1), 1e-6));
    }

    SECTION("row-major: a Y clock alone moves nothing, and an X wrap carries") {
        CartFixture fx(row_major, kSr, kFrames);
        auto inj = fx.claim_injector();
        cart_baseline(inj);

        const auto y_only = fx.render({flat(0.0f), clock_line(32), flat(0.0f)});
        for (float v : y_only[0]) REQUIRE_THAT(static_cast<double>(v), WithinAbs(0.0, 1e-9));

        auto reset = flat(0.0f);
        reset[0] = 1.0f;
        const auto cv = at_clocks(fx.render({clock_line(32), clock_line(32), reset})[0], 32);
        // 0,1,2,3 then the wrap carries into the next row: (0,1) = index 8.
        REQUIRE_THAT(static_cast<double>(cv[3]), WithinAbs(cell_index(3, 0), 1e-6));
        REQUIRE_THAT(static_cast<double>(cv[4]), WithinAbs(cell_index(0, 1), 1e-6));
        REQUIRE_THAT(static_cast<double>(cv[5]), WithinAbs(cell_index(1, 1), 1e-6));
    }
}

TEST_CASE("Forge sequencing cartesian: the baked node is bit-reproducible across resets",
          "[host][baked][forge][forge-sequencing][cartesian][determinism]") {
    CartFixture fx(seqcat::cartesian::make_cartesian_walk_node(index_grid()), kSr, kFrames);
    auto inj = fx.claim_injector();
    cart_baseline(inj);

    const auto x = clock_line(32);
    const auto y = clock_line(96);
    const auto first = fx.render({x, y, flat(0.0f)});
    const auto second = fx.render({x, y, flat(0.0f)});
    reinit(fx);
    cart_baseline(inj);
    REQUIRE(fx.render({x, y, flat(0.0f)})[0] == first[0]);
    REQUIRE(fx.render({x, y, flat(0.0f)})[0] == second[0]);
}

TEST_CASE("Forge sequencing rungler: the node bakes and reproduces the DSP's worked sequence",
          "[host][baked][forge][forge-sequencing][rungler]") {
    const auto type = seqcat::rungler::make_rungler_node();
    REQUIRE(type.type_id == std::string("sequencing.rungler"));
    REQUIRE(type.num_input_ports == 2);   // clock, reset
    REQUIRE(type.num_output_ports == 2);  // CV, serial bit
    REQUIRE(type.baked_params.size() == 6);

    RunglerFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    const auto out = fx.render({clock_line(32), flat(0.0f)});
    const auto cv = at_clocks(out[0], 32);

    // The module's worked example, re-derived from the shipped constants rather
    // than restated: seed 0b10110100 has low three bits 100 → code 4 → the first
    // clock shifts to 0b01101001 → code 1.
    const double range = seqcat::rungler::Rung::kRangeV;
    const double levels = 7.0;  // 2^3 − 1
    REQUIRE_THAT(static_cast<double>(cv[0]),
                 WithinAbs(range * (2.0 * 1.0 / levels - 1.0), 1e-5));
    // Eight DAC levels, and the line really moves through them.
    REQUIRE(distinct_levels(cv) > 3);
}

TEST_CASE("Forge sequencing rungler: feedback-tap range follows the registered length",
          "[host][baked][forge][forge-sequencing][rungler]") {
    const auto type = seqcat::rungler::make_rungler_node(4);
    const auto it = std::find_if(type.baked_params.begin(), type.baked_params.end(),
                                 [](const auto& p) {
                                     return p.id == seqcat::rungler::kFeedbackTap;
                                 });
    REQUIRE(it != type.baked_params.end());
    REQUIRE(it->min_value == 0.0f);
    REQUIRE(it->max_value == 2.0f);
}

TEST_CASE("Forge sequencing rungler: dac_bits sets the number of output levels",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    // A long enough run for the orbit to visit its levels.
    const auto levels_for = [&](int dac_bits) {
        reinit(fx);
        rungler_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kDacBits,
                                     static_cast<float>(dac_bits))) == InjectStatus::Ok);
        std::vector<float> all;
        for (int b = 0; b < 8; ++b) {
            const auto cv = at_clocks(fx.render({clock_line(8), flat(0.0f)})[0], 8);
            all.insert(all.end(), cv.begin(), cv.end());
        }
        return distinct_levels(all);
    };

    // A D-bit DAC can emit at most 2^D levels, and the count grows with D.
    const auto one = levels_for(1);
    const auto two = levels_for(2);
    const auto three = levels_for(3);
    const auto four = levels_for(4);
    REQUIRE(one <= 2u);
    REQUIRE(two <= 4u);
    REQUIRE(three <= 8u);
    REQUIRE(four <= 16u);
    REQUIRE(one < three);
    REQUIRE(three <= four);
}

TEST_CASE("Forge sequencing rungler: feedback_tap picks a different orbit",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    const auto orbit_for = [&](int tap) {
        reinit(fx);
        rungler_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kFeedbackTap,
                                     static_cast<float>(tap))) == InjectStatus::Ok);
        return at_clocks(fx.render({clock_line(8), flat(0.0f)})[0], 8);
    };

    // The tap IS the recurrence, so a different tap is a different sequence —
    // and each one is still reproducible.
    const auto tap0 = orbit_for(0);
    const auto tap3 = orbit_for(3);
    const auto tap0_again = orbit_for(0);
    REQUIRE(tap0 != tap3);
    REQUIRE(tap0 == tap0_again);
}

TEST_CASE("Forge sequencing rungler: range_v scales the output and bounds it",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    for (float range : {0.5f, 1.0f, 2.0f, 5.0f}) {
        reinit(fx);
        rungler_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kRangeV, range)) == InjectStatus::Ok);
        const auto out = fx.render({clock_line(8), flat(0.0f)});
        // The registry's invariant, over the production path: |y| <= range_v by
        // construction, because the output is a D-bit DAC code mapped affinely
        // onto [-range, +range]. Not "close to" — never above.
        for (float v : out[0]) REQUIRE(std::fabs(v) <= range + 1e-6f);
        // And the bound is TIGHT: the orbit reaches an extreme, so a passing
        // test is not passing because the output stayed small.
        REQUIRE_THAT(static_cast<double>(peak_abs(out[0])),
                     WithinAbs(static_cast<double>(range), 1e-5));
    }
    REQUIRE_THAT(static_cast<double>(seqcat::rungler::rungler_output_bound_v()),
                 WithinAbs(5.0, 1e-9));
}

TEST_CASE("Forge sequencing rungler: external_data and data_in steer the chaos",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    const auto run_with = [&](float external, float data) {
        reinit(fx);
        rungler_baseline(inj);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kExternalData, external)) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(seqcat::rungler::kDataIn, data)) == InjectStatus::Ok);
        return at_clocks(fx.render({clock_line(8), flat(0.0f)})[0], 8);
    };

    const auto plain = run_with(0.0f, 0.0f);
    const auto data_ignored = run_with(0.0f, 1.0f);
    const auto steered = run_with(1.0f, 1.0f);

    // The switch is what makes the data bit matter: with it off, the data bit
    // is ignored entirely.
    REQUIRE(plain == data_ignored);
    // With it on, the bit perturbs the state every clock.
    REQUIRE(steered != plain);
}

TEST_CASE("Forge sequencing rungler: run holds, and the reset port re-pins to the seed",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);

    (void)fx.render({clock_line(8), flat(0.0f)});
    REQUIRE(inj.inject(immediate(seqcat::rungler::kRun, 0.0f)) == InjectStatus::Ok);
    const auto stopped = fx.render({clock_line(8), flat(0.0f)});
    const float held = stopped[0].front();
    for (float v : stopped[0]) REQUIRE_THAT(static_cast<double>(v),
                                            WithinAbs(static_cast<double>(held), 1e-9));

    // This is the module's one documented exception to "a reset edge holds the
    // continuous output": the rungler's reset re-pins the register AND the DAC
    // level immediately, so a wandering line can be caught live.
    reinit(fx);
    rungler_baseline(inj);
    const auto fresh = at_clocks(fx.render({clock_line(32), flat(0.0f)})[0], 32);

    reinit(fx);
    rungler_baseline(inj);
    auto reset = flat(0.0f);
    reset[200] = 1.0f;
    const auto repinned = at_clocks(fx.render({clock_line(32), reset})[0], 32);
    // The clock at 224 is the first after the reset, and it plays what the first
    // clock after a fresh start plays.
    REQUIRE_THAT(static_cast<double>(repinned[7]),
                 WithinAbs(static_cast<double>(fresh[0]), 1e-6));
}

TEST_CASE("Forge sequencing rungler: the serial-bit output tracks the register",
          "[host][baked][forge][forge-sequencing][rungler]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kDacBits, 1.0f)) == InjectStatus::Ok);

    const auto out = fx.render({clock_line(16), flat(0.0f)});
    // At D = 1 the DAC code IS bit 0, so the CV and the bit output must agree
    // sample for sample: CV at +range means bit 0 is set.
    for (std::size_t k = 0; k < out[0].size(); ++k) {
        const bool bit = high(out[1][k]);
        REQUIRE(bit == (out[0][k] > 0.0f));
        REQUIRE((out[1][k] == 0.0f || out[1][k] == 1.0f));  // a gate, not a level
    }
}

TEST_CASE("Forge sequencing rungler: register length and seed are realizations",
          "[host][baked][forge][forge-sequencing][rungler][determinism]") {
    const auto run_node = [](int reg_bits, std::uint32_t seed) {
        RunglerFixture fx(seqcat::rungler::make_rungler_node(reg_bits, seed), kSr, kFrames);
        auto inj = fx.claim_injector();
        rungler_baseline(inj);
        return at_clocks(fx.render({clock_line(8), flat(0.0f)})[0], 8);
    };

    // The seed is which performance the artifact plays (law 2).
    REQUIRE(run_node(8, 0b10110100u) == run_node(8, 0b10110100u));
    REQUIRE(run_node(8, 0b10110100u) != run_node(8, 0b11001010u));

    // The register length is a realization because the DSP reloads the seed when
    // it changes: as a param it would re-pin the sequence on every sample and
    // the node would emit one frozen level instead of a line. Different lengths
    // are different orbits, and each is a line rather than a constant.
    const auto n8 = run_node(8, 0b10110100u);
    const auto n16 = run_node(16, 0b10110100u);
    REQUIRE(n8 != n16);
    REQUIRE(distinct_levels(n8) > 1);
    REQUIRE(distinct_levels(n16) > 1);
}

TEST_CASE("Forge sequencing rungler: the baked node is bit-reproducible across resets",
          "[host][baked][forge][forge-sequencing][rungler][determinism]") {
    RunglerFixture fx(seqcat::rungler::make_rungler_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    rungler_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kExternalData, 1.0f)) == InjectStatus::Ok);

    const auto clock = clock_line(8);
    const auto first = fx.render({clock, flat(0.0f)});
    const auto second = fx.render({clock, flat(0.0f)});
    reinit(fx);
    rungler_baseline(inj);
    REQUIRE(inj.inject(immediate(seqcat::rungler::kExternalData, 1.0f)) == InjectStatus::Ok);
    REQUIRE(fx.render({clock, flat(0.0f)})[0] == first[0]);
    REQUIRE(fx.render({clock, flat(0.0f)})[0] == second[0]);
}

TEST_CASE("Forge sequencing quantizer: the node bakes and snaps to 12-TET",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    const auto type = seqcat::quantize::make_quantize_scale_node();
    REQUIRE(type.type_id == std::string("sequencing.quantize_scale"));
    REQUIRE(type.num_input_ports == 1);
    REQUIRE(type.num_output_ports == 1);
    REQUIRE(type.baked_params.size() == 6);

    QuantFixture fx(type, kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);

    const auto out = fx.render({flat(0.30f)});
    // The module's worked example over the production path: 0.30 V → step 4.
    for (float v : out[0]) REQUIRE_THAT(static_cast<double>(v), WithinAbs(4.0 / 12.0, 1e-6));
}

TEST_CASE("Forge sequencing quantizer: mode picks between EDO and the scale mask",
          "[host][baked][forge][forge-sequencing][quantizer]") {
    QuantFixture fx(seqcat::quantize::make_quantize_scale_node(), kSr, kFrames);
    auto inj = fx.claim_injector();
    quant_baseline(inj);

    // 0.26 V is chromatic step 3 (D#), which C major does not contain.
    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 0.0f)) == InjectStatus::Ok);
    REQUIRE_THAT(static_cast<double>(fx.render({flat(0.26f)})[0].front()),
                 WithinAbs(3.0 / 12.0, 1e-6));

    REQUIRE(inj.inject(immediate(seqcat::quantize::kMode, 1.0f)) == InjectStatus::Ok);
    REQUIRE_THAT(static_cast<double>(fx.render({flat(0.26f)})[0].front()),
                 WithinAbs(4.0 / 12.0, 1e-6));  // snapped up to E
}
