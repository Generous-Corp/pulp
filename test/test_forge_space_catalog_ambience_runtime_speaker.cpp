#include "test_forge_space_catalog_support.hpp"

TEST_CASE("Forge space ambience: the continuous params move the audio per sample",
          "[host][baked][param-injection][forge][forge-space][ambience]") {
    const auto type = amb::make_nonlin_ambience_node();

    SECTION("output_gain_db scales by exactly the dB injected") {
        auto level = [&](float db) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kOutputGainDb, db)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            return peak(render_impulse_response(fx, 200).left);
        };
        const double unity = level(0.0f);
        const double boosted = level(6.0f);
        INFO("peak ratio " << boosted / unity);
        REQUIRE_THAT(boosted / unity, WithinRel(std::pow(10.0, 6.0 / 20.0), 0.02));
    }

    SECTION("mix_pct at 0 is the dry wire, sample-aligned") {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_ambience_defaults(inj);
        REQUIRE(inj.inject(immediate(amb::kMixPct, 0.0f)) == InjectStatus::Ok);
        settle_silent(fx, 8);
        const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.5f);
        const auto out = fx.render({tone, tone});
        for (int n = 0; n < kFrames; ++n) {
            INFO("sample " << n);
            REQUIRE_THAT(out[0][static_cast<std::size_t>(n)],
                         WithinAbs(tone[static_cast<std::size_t>(n)], 1e-6f));
        }
        REQUIRE(fx.baked().latency_samples() == 0);
    }

    SECTION("width_pct at 0 is exactly mono") {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_ambience_defaults(inj);
        REQUIRE(inj.inject(immediate(amb::kWidthPct, 0.0f)) == InjectStatus::Ok);
        settle_silent(fx, 32);
        const Ir ir = render_impulse_response(fx, 200);
        REQUIRE(peak(ir.left) > 1e-4);
        REQUIRE(ir.left == ir.right);

        Fixture wide(type, kSr, kFrames);
        ParamInjector wide_inj = wide.claim_injector();
        inject_ambience_defaults(wide_inj);
        settle_silent(wide, 32);
        const Ir wide_ir = render_impulse_response(wide, 200);
        REQUIRE(wide_ir.left != wide_ir.right);
    }

    SECTION("tone and hf_damp_hz steer the colour of the tail") {
        auto late_hf = [&](float tone_value, float damp_hz) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 800.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kGateHoldPct, 95.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kTone, tone_value)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kHfDampHz, damp_hz)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 340);
            return hf_fraction(ir.left, static_cast<int>(0.6 * 0.8 * kSr),
                               static_cast<int>(0.05 * kSr));
        };
        const double dark = late_hf(-1.0f, 6000.0f);
        const double neutral = late_hf(0.0f, 6000.0f);
        const double bright = late_hf(1.0f, 6000.0f);
        INFO("late HF fraction: tone -1 " << dark << ", 0 " << neutral << ", +1 " << bright);
        REQUIRE(bright > neutral);
        REQUIRE(neutral > dark);

        const double damped = late_hf(0.0f, 1000.0f);
        const double open = late_hf(0.0f, 16000.0f);
        INFO("late HF fraction: hf_damp 1 kHz " << damped << ", 16 kHz " << open);
        REQUIRE(open > damped);
    }

    SECTION("diffusion smears the discrete taps") {
        auto crest = [&](float diffusion) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, diffusion)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 200);
            const double rms = std::sqrt(window_power(ir.left, 0, static_cast<int>(ir.left.size())));
            return peak(ir.left) / std::max(rms, 1e-30);
        };
        const double naked = crest(0.0f);
        const double diffused = crest(0.85f);
        INFO("crest factor: diffusion 0 -> " << naked << ", 0.85 -> " << diffused);
        REQUIRE(diffused < naked);
    }

    SECTION("converter_amount engages the character stage") {
        auto render_at = [&](float amount) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kConverterAmount, amount)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            return render_impulse_response(fx, 200);
        };
        const Ir off = render_at(0.0f);
        const Ir on = render_at(1.0f);
        double difference = 0.0;
        for (std::size_t n = 0; n < off.left.size(); ++n)
            difference = std::max(difference, std::fabs(static_cast<double>(on.left[n] -
                                                                           off.left[n])));
        INFO("largest difference with the converter engaged: " << difference);
        REQUIRE(difference > 0.0);
        REQUIRE(peak(on.left) > 1e-5);
    }
}

TEST_CASE("Forge space ambience: topology is block-rate, continuous is sample-rate",
          "[host][baked][param-injection][forge][forge-space][ambience]") {
    // The two-tier contract, asserted rather than described. A future change
    // that moved a topology param to per-sample reads would put a full tap-table
    // rebuild in the sample loop, and this is what would catch it.
    const auto type = amb::make_nonlin_ambience_node();
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_ambience_defaults(inj);
    REQUIRE(inj.inject(immediate(amb::kMixPct, 0.0f)) == InjectStatus::Ok);
    settle_silent(fx, 8);

    const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.5f);

    // Continuous: a mid-block injection takes effect MID-BLOCK. Asserted as a
    // divergence point rather than as a level, because the continuous params
    // are smoothed over 20 ms — "the value arrives at sample 64" shows up as
    // "samples 0..63 are identical to a run that never saw the change, and
    // samples after 64 are not", which is true regardless of the ramp shape.
    // A block-rate read would leave the WHOLE block identical.
    Fixture unchanged(type, kSr, kFrames);
    ParamInjector unchanged_inj = unchanged.claim_injector();
    inject_ambience_defaults(unchanged_inj);
    REQUIRE(unchanged_inj.inject(immediate(amb::kMixPct, 0.0f)) == InjectStatus::Ok);
    settle_silent(unchanged, 8);
    const auto reference = unchanged.render({tone, tone});

    REQUIRE(inj.inject(immediate(amb::kMixPct, 100.0f, /*offset=*/kFrames / 2)) ==
            InjectStatus::Ok);
    const auto split = fx.render({tone, tone});
    for (int n = 0; n < kFrames / 2; ++n) {
        INFO("sample " << n << " (before the injection offset)");
        REQUIRE_THAT(split[0][static_cast<std::size_t>(n)],
                     WithinAbs(reference[0][static_cast<std::size_t>(n)], 1e-7f));
    }
    double late_divergence = 0.0;
    for (int n = kFrames / 2; n < kFrames; ++n)
        late_divergence =
            std::max(late_divergence,
                     std::fabs(static_cast<double>(split[0][static_cast<std::size_t>(n)] -
                                                   reference[0][static_cast<std::size_t>(n)])));
    INFO("divergence after the injection offset " << late_divergence);
    REQUIRE(late_divergence > 1e-6);

    // Topology: a mid-block injection does NOT take effect until the next
    // block. Measured on the tap count's audible proxy — the program — by
    // asserting the whole block still matches a run that never saw the change.
    Fixture a(type, kSr, kFrames), b(type, kSr, kFrames);
    ParamInjector a_inj = a.claim_injector(), b_inj = b.claim_injector();
    inject_ambience_defaults(a_inj);
    inject_ambience_defaults(b_inj);
    a.render(impulse_block());
    b.render(impulse_block());
    // `b` asks for a different program halfway through the next block.
    REQUIRE(b_inj.inject(immediate(amb::kProgram, 2.0f, /*offset=*/kFrames / 2)) ==
            InjectStatus::Ok);
    const auto a_out = a.render(silence());
    const auto b_out = b.render(silence());
    REQUIRE(a_out[0] == b_out[0]);  // block rate: the change waits
}

TEST_CASE("Forge space ambience: the registry gain bound composes the DSP's own",
          "[host][baked][param-injection][forge][forge-space][ambience][gain]") {
    namespace cal = pulp::signal::nonlin_ambience;
    const float bound = amb::nonlin_ambience_worst_case_gain();

    // Recomposed from the shipped constants rather than restated: the DSP's
    // closed-form bound at maximum diffusion with the converter engaged, times
    // the node's output-trim ceiling.
    const double expected = cal::worst_case_gain(cal::kDiffusionMax, true) *
                            std::pow(10.0, amb::kOutputGainDbMax / 20.0);
    INFO("bound " << bound << ", recomposed " << expected);
    REQUIRE_THAT(static_cast<double>(bound), WithinRel(expected, 1e-6));
    // The decomposition, each factor asserted so a constant change re-derives
    // rather than inherits: (1 + 2*0.85)^2 * 4 * 2 * 10^(24/20).
    REQUIRE_THAT(cal::worst_case_gain(cal::kDiffusionMax, false),
                 WithinRel(std::pow(1.0 + 2.0 * cal::kDiffusionMax, cal::kNumAllpass) *
                               cal::kL1Budget,
                           1e-12));
    REQUIRE_THAT(cal::worst_case_gain(cal::kDiffusionMax, true),
                 WithinRel(2.0 * cal::worst_case_gain(cal::kDiffusionMax, false), 1e-12));

    // And it bounds the node over the real path, at every ceiling at once.
    const auto type = amb::make_nonlin_ambience_node();
    for (int program = 0; program < 4; ++program) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_ambience_defaults(inj);
        REQUIRE(inj.inject(immediate(amb::kProgram, static_cast<float>(program))) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(amb::kLengthMs, 200.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(amb::kDiffusion,
                                     static_cast<float>(cal::kDiffusionMax))) ==
                InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(amb::kConverterAmount, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(amb::kOutputGainDb, amb::kOutputGainDbMax)) ==
                InjectStatus::Ok);
        settle_silent(fx, 32);
        const Ir ir = render_impulse_response(fx, 200, 1.0f);
        INFO("program " << program << " peak " << peak(ir.left) << " against bound " << bound);
        REQUIRE(peak(ir.left) <= bound);
        REQUIRE(peak(ir.right) <= bound);
    }
}

TEST_CASE("Forge space ambience: process allocates nothing, including across a swap",
          "[host][baked][param-injection][forge][forge-space][ambience][rt]") {
    const auto type = amb::make_nonlin_ambience_node();
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_ambience_defaults(inj);
    REQUIRE(inj.inject(immediate(amb::kLengthMs, 200.0f)) == InjectStatus::Ok);

    const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.4f);
    pulp::test::ReusableRenderer<2> renderer(fx, {tone, tone});
    renderer.render();  // warm the first block outside the probe

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 64; ++block) {
            const float u = static_cast<float>(block) / 64.0f;
            // Continuous, every block.
            REQUIRE(inj.inject(immediate(amb::kTone, 2.0f * u - 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kHfDampHz, 1000.0f + 17000.0f * u)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kWidthPct, 100.0f * u)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kMixPct, 100.0f * (1.0f - u))) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kOutputGainDb, 6.0f * u - 3.0f)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kConverterAmount, u)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.85f * u)) == InjectStatus::Ok);
            // And the topology path, which is the one that regenerates a tap
            // table into the pre-sized back bank and crossfades it in.
            if (block == 16) REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) ==
                                     InjectStatus::Ok);
            if (block == 32) REQUIRE(inj.inject(immediate(amb::kLengthMs, 260.0f)) ==
                                     InjectStatus::Ok);
            if (block == 48)
                REQUIRE(inj.inject(immediate(amb::kDensityPct, 90.0f)) == InjectStatus::Ok);
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Forge speaker cabinet declares and renders its complete mono node",
          "[host][baked][param-injection][forge][forge-space][cabinet]") {
    const auto type = cabinet::make_speaker_cabinet_node();
    CHECK(type.num_input_ports == 1);
    CHECK(type.num_output_ports == 1);
    CHECK(type.baked_params.size() == 14);
    CHECK(type.lowerable);

    const auto tone = pulp::test::sine_block(kFrames, 220.0, kSr, 0.35f);
    auto render = [&](float drive_db, float output_trim_db = 0.0f) {
        pulp::test::BakedNodeFixture<1> fx(type, kSr, kFrames);
        auto injector = fx.claim_injector();
        REQUIRE(injector.inject(immediate(cabinet::kDriveDb, drive_db)) == InjectStatus::Ok);
        REQUIRE(injector.inject(immediate(cabinet::kOutputTrimDb, output_trim_db)) ==
                InjectStatus::Ok);
        return fx.settle({tone}, 24)[0];
    };
    const auto clean = render(0.0f);
    const auto driven = render(static_cast<float>(pulp::signal::SpeakerModel::kDriveDbMax),
                               static_cast<float>(pulp::signal::SpeakerModel::kOutputTrimDbMax));
    REQUIRE(std::all_of(driven.begin(), driven.end(), [](float v) { return std::isfinite(v); }));
    CHECK(clean != driven);
    CHECK(peak(driven) <= cabinet::speaker_cabinet_worst_case_gain());
    CHECK(cabinet::speaker_cabinet_worst_case_gain() ==
          static_cast<float>(pulp::signal::SpeakerModel{}.worst_case_gain() *
                             pulp::signal::units::db_to_linear(
                                 pulp::signal::SpeakerModel::kOutputTrimDbMax)));
}

TEST_CASE("Forge speaker compatibility and baked-control contracts stay stable",
          "[host][baked][param-injection][forge][forge-space][cabinet][rt-safety]") {
    const auto type = cabinet::make_speaker_cabinet_node();
    const auto compatibility = cabinet::make_speaker_emulation_node();
    REQUIRE(compatibility.type_id == type.type_id);
    REQUIRE(compatibility.baked_params.size() == type.baked_params.size());
    REQUIRE(type.lowerable);

    const std::array<pulp::state::ParamID, 14> ids{{
        cabinet::kDriver, cabinet::kBox, cabinet::kVolumeL,
        cabinet::kResonanceTrimSt, cabinet::kQ, cabinet::kBreakupPct,
        cabinet::kTrebleHz, cabinet::kDriveDb, cabinet::kCompressionPct,
        cabinet::kMicDistanceCm, cabinet::kMicPositionPct, cabinet::kMicAxisDeg,
        cabinet::kDiffractionPct, cabinet::kOutputTrimDb,
    }};
    REQUIRE(type.baked_params.size() == ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        CHECK(type.baked_params[i].id == ids[i]);
        CHECK(type.baked_params[i].min_value <= type.baked_params[i].default_value);
        CHECK(type.baked_params[i].default_value <= type.baked_params[i].max_value);
        CHECK(compatibility.baked_params[i].id == type.baked_params[i].id);
        CHECK(compatibility.baked_params[i].min_value == type.baked_params[i].min_value);
        CHECK(compatibility.baked_params[i].max_value == type.baked_params[i].max_value);
        CHECK(compatibility.baked_params[i].default_value == type.baked_params[i].default_value);
    }

    auto render = [&](bool hostile) {
        pulp::test::BakedNodeFixture<1> fx(type, kSr, kFrames);
        auto injector = fx.claim_injector();
        pulp::state::ParameterEventQueue q;
        for (const auto& p : type.baked_params)
            REQUIRE(q.push(immediate(p.id, hostile ? std::numeric_limits<float>::quiet_NaN()
                                                   : p.max_value)));
        REQUIRE(injector.inject(q) == InjectStatus::Ok);
        auto input = pulp::test::sine_block(kFrames, 375.0, kSr, 0.2f);
        if (hostile) input[31] = std::numeric_limits<float>::infinity();
        std::vector<float> last;
        for (int block = 0; block < 80; ++block) last = fx.render({input})[0];
        REQUIRE(std::all_of(last.begin(), last.end(), [](float x) { return std::isfinite(x); }));
        return last;
    };
    CHECK(render(false) == render(false));
    render(true);

    pulp::test::BakedNodeFixture<1> fx(type, kSr, kFrames);
    auto injector = fx.claim_injector();
    const auto input = pulp::test::sine_block(kFrames, 375.0, kSr, 0.2f);
    pulp::test::ReusableRenderer<1> renderer(fx, {input});
    std::vector<pulp::state::ParameterEventQueue> queues(type.baked_params.size());
    for (std::size_t i = 0; i < type.baked_params.size(); ++i)
        REQUIRE(queues[i].push(immediate(type.baked_params[i].id,
                                         type.baked_params[i].max_value)));
    pulp::test::RtAllocationProbe probe;
    for (auto& q : queues) {
        REQUIRE(injector.inject(q) == InjectStatus::Ok);
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}
