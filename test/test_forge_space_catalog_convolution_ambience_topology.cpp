#include "test_forge_space_catalog_support.hpp"

TEST_CASE("Forge space convolution: the node bakes and runs true stereo",
          "[host][baked][param-injection][forge][forge-space][convolution]") {
    const auto type = conv::make_convolution_reverb_node(room_ir());
    REQUIRE(std::string(type.type_id) == "space.convolution_reverb");
    REQUIRE(type.lowerable);
    REQUIRE(type.num_input_ports == 2);
    REQUIRE(type.num_output_ports == 2);
    REQUIRE(type.baked_params.size() == 7);

    // Every declared range is the DSP's canonical contract, not a second
    // opinion about it.
    using E = conv::Engine;
    auto param = [&](pulp::state::ParamID id) {
        for (const auto& p : type.baked_params)
            if (p.id == id) return p;
        FAIL("param not declared");
        return type.baked_params.front();
    };
    REQUIRE_THAT(param(conv::kIrGainDb).min_value, WithinAbs(E::kIrGainDbMin, 1e-6));
    REQUIRE_THAT(param(conv::kIrGainDb).max_value, WithinAbs(E::kIrGainDbMax, 1e-6));
    REQUIRE_THAT(param(conv::kPredelayMs).max_value, WithinAbs(E::kPredelayMsMax, 1e-6));
    REQUIRE_THAT(param(conv::kWidthPercent).max_value, WithinAbs(E::kWidthPercentMax, 1e-6));
    REQUIRE_THAT(param(conv::kLowcutHz).min_value, WithinAbs(E::kLowcutHzMin, 1e-6));
    REQUIRE_THAT(param(conv::kHighcutHz).max_value, WithinAbs(E::kHighcutHzMax, 1e-6));

    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_convolution_defaults(inj);
    const auto out = fx.settle(impulse_block());
    REQUIRE(std::isfinite(out[0][0]));
    REQUIRE(std::isfinite(out[1][0]));
}

TEST_CASE("Forge space convolution: zero latency survives the graph",
          "[host][baked][param-injection][forge][forge-space][convolution][latency]") {
    // The DSP promises a literal constant 0. A node wrapper that buffered one
    // block would still pass every level, spectrum and mix assertion in this
    // file, so the property is asserted here three separate ways.
    const auto type = conv::make_convolution_reverb_node(delta_ir());
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_convolution_defaults(inj);

    // 1. The baked processor reports zero.
    REQUIRE(fx.baked().latency_samples() == 0);

    // 2. A baked impulse comes out at sample 0. With a single-sample IR the
    //    convolution IS the input, so any wrapper delay appears directly here —
    //    a one-block buffer would put the onset at 128.
    const Ir ir = render_impulse_response(fx, 8);
    REQUIRE(first_nonzero(ir.left) == 0);
    REQUIRE(first_nonzero(ir.right) == 0);
    REQUIRE(peak(ir.left) > 0.1);

    // 3. And the strongest form: the node reproduces the BARE ENGINE
    //    sample-for-sample. Nothing that adds delay, gain, or filtering on the
    //    way through can satisfy this.
    pulp::signal::ZeroLatencyConvolver bare;
    bare.prepare(kSr, kFrames, 2);
    bare.set_normalize_mode(conv::IrPolicy{}.normalize);
    bare.set_tail_trim_db(conv::IrPolicy{}.tail_trim_db);
    bare.set_tail_fade_ms(conv::IrPolicy{}.tail_fade_ms);
    bare.set_resample_taps_per_phase(conv::IrPolicy{}.resample_taps_per_phase);
    bare.set_true_stereo(conv::IrPolicy{}.true_stereo);
    const auto reference_ir = delta_ir();
    std::vector<const float*> ir_ptrs{reference_ir.channels[0].data(),
                                      reference_ir.channels[1].data()};
    REQUIRE(bare.load_impulse_response(ir_ptrs.data(), 2,
                                       static_cast<int>(reference_ir.channels[0].size()),
                                       reference_ir.sample_rate));
    bare.set_ir_gain_db(0.0);
    bare.set_predelay_ms(0.0);
    bare.set_wet_percent(100.0);
    bare.set_dry_percent(0.0);
    bare.set_width_percent(100.0);
    bare.set_lowcut_hz(20.0);
    bare.set_highcut_hz(20000.0);

    const int blocks = 8;
    std::vector<float> ref_left, ref_right;
    for (int b = 0; b < blocks; ++b) {
        std::vector<float> l(kFrames, 0.0f), r(kFrames, 0.0f);
        std::vector<float> ol(kFrames, 0.0f), orr(kFrames, 0.0f);
        if (b == 0) {
            l[0] = 1.0f;
            r[0] = 1.0f;
        }
        const float* in_ptrs[2] = {l.data(), r.data()};
        float* out_ptrs[2] = {ol.data(), orr.data()};
        bare.process(in_ptrs, out_ptrs, kFrames);
        ref_left.insert(ref_left.end(), ol.begin(), ol.end());
        ref_right.insert(ref_right.end(), orr.begin(), orr.end());
    }
    REQUIRE(ref_left.size() == ir.left.size());
    int mismatches = 0, first_mismatch = -1;
    for (std::size_t n = 0; n < ref_left.size(); ++n)
        if (ir.left[n] != ref_left[n] || ir.right[n] != ref_right[n]) {
            ++mismatches;
            if (first_mismatch < 0) first_mismatch = static_cast<int>(n);
        }
    INFO(mismatches << " samples differ from the bare engine, first at " << first_mismatch);
    REQUIRE(mismatches == 0);
}

TEST_CASE("Forge space convolution: pre-delay shifts the wet path and not the report",
          "[host][baked][param-injection][forge][forge-space][convolution][latency]") {
    // The DSP is explicit that pre-delay is signal delay and not I/O latency.
    // With a single-sample IR the shift is exact and countable, which is the
    // only way to tell "the wet path moved" from "the node started buffering".
    const auto type = conv::make_convolution_reverb_node(delta_ir());
    for (double predelay_ms : {0.0, 1.0, 5.0}) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_convolution_defaults(inj);
        REQUIRE(inj.inject(immediate(conv::kPredelayMs,
                                     static_cast<float>(predelay_ms))) == InjectStatus::Ok);
        const Ir ir = render_impulse_response(fx, 8);

        const int expected = static_cast<int>(std::lround(predelay_ms * kSr / 1000.0));
        INFO("pre-delay " << predelay_ms << " ms: onset at " << first_nonzero(ir.left)
                          << ", expected " << expected);
        REQUIRE(first_nonzero(ir.left) == expected);
        // The reported latency does not move with it — that is the distinction.
        REQUIRE(fx.baked().latency_samples() == 0);
    }
}

TEST_CASE("Forge space convolution: every gain param moves the audio the way it says",
          "[host][baked][param-injection][forge][forge-space][convolution]") {
    const auto type = conv::make_convolution_reverb_node(room_ir());

    SECTION("ir_gain_db scales the wet path by exactly the dB injected") {
        double previous = 0.0;
        for (float db : {0.0f, 6.0f, 12.0f}) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_convolution_defaults(inj);
            REQUIRE(inj.inject(immediate(conv::kIrGainDb, db)) == InjectStatus::Ok);
            const Ir ir = render_impulse_response(fx, 24);
            const double p = peak(ir.left);
            if (previous > 0.0) {
                INFO("ir_gain " << db << " dB: peak ratio " << p / previous);
                REQUIRE_THAT(p / previous, WithinRel(std::pow(10.0, 6.0 / 20.0), 0.02));
            }
            previous = p;
        }
    }

    SECTION("wet and dry are separate paths") {
        // Dry only: the output IS the input, sample for sample. Wet only: the
        // input is gone and the convolution is what is left.
        Fixture dry_fx(type, kSr, kFrames);
        ParamInjector dry_inj = dry_fx.claim_injector();
        inject_convolution_defaults(dry_inj, /*wet=*/0.0f, /*dry=*/100.0f);
        const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.5f);
        const auto dry_out = dry_fx.settle({tone, tone});
        for (int n = 0; n < kFrames; ++n) {
            INFO("sample " << n);
            REQUIRE_THAT(dry_out[0][static_cast<std::size_t>(n)],
                         WithinAbs(tone[static_cast<std::size_t>(n)], 1e-6f));
        }

        Fixture wet_fx(type, kSr, kFrames);
        ParamInjector wet_inj = wet_fx.claim_injector();
        inject_convolution_defaults(wet_inj, /*wet=*/100.0f, /*dry=*/0.0f);
        const auto wet_out = wet_fx.settle({tone, tone});
        double difference = 0.0;
        for (int n = 0; n < kFrames; ++n)
            difference = std::max(difference,
                                  std::fabs(static_cast<double>(
                                      wet_out[0][static_cast<std::size_t>(n)] -
                                      tone[static_cast<std::size_t>(n)])));
        INFO("wet-vs-input largest difference " << difference);
        REQUIRE(difference > 0.05);
    }

    SECTION("width collapses and widens the wet return") {
        auto side_energy = [&](float width_pct) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_convolution_defaults(inj);
            REQUIRE(inj.inject(immediate(conv::kWidthPercent, width_pct)) == InjectStatus::Ok);
            const Ir ir = render_impulse_response(fx, 24);
            double side = 0.0;
            for (std::size_t n = 0; n < ir.left.size(); ++n) {
                const double s = 0.5 * (ir.left[n] - ir.right[n]);
                side += s * s;
            }
            return side;
        };
        const double mono = side_energy(0.0f);
        const double normal = side_energy(100.0f);
        const double wide = side_energy(200.0f);
        INFO("side energy: mono " << mono << " normal " << normal << " wide " << wide);
        REQUIRE(mono < normal * 1e-9);   // 0 % is exactly mono
        REQUIRE(wide > normal * 3.0);    // 200 % doubles the side amplitude
        REQUIRE_THAT(wide / normal, WithinRel(4.0, 0.05));  // energy, so 2^2
    }
}

TEST_CASE("Forge space convolution: the send EQ corners filter the send",
          "[host][baked][param-injection][forge][forge-space][convolution]") {
    const auto type = conv::make_convolution_reverb_node(delta_ir());

    auto wet_hf = [&](float lowcut, float highcut) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_convolution_defaults(inj);
        REQUIRE(inj.inject(immediate(conv::kLowcutHz, lowcut)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(conv::kHighcutHz, highcut)) == InjectStatus::Ok);
        // A tone that holds whole cycles in the analysis block.
        const auto tone = pulp::test::sine_block(kFrames, 3000.0, kSr, 0.5f);
        const auto out = fx.settle({tone, tone});
        return pulp::test::harmonic_magnitude(out[0], 1, 3000.0, kSr);
    };
    auto wet_lf = [&](float lowcut, float highcut) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        inject_convolution_defaults(inj);
        REQUIRE(inj.inject(immediate(conv::kLowcutHz, lowcut)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(conv::kHighcutHz, highcut)) == InjectStatus::Ok);
        const auto tone = pulp::test::sine_block(kFrames, 375.0, kSr, 0.5f);
        const auto out = fx.settle({tone, tone});
        return pulp::test::harmonic_magnitude(out[0], 1, 375.0, kSr);
    };

    // Both endpoints are documented as BYPASS, so the reference is the default
    // setting and each knob is measured as a departure from it.
    const double reference_hf = wet_hf(20.0f, 20000.0f);
    const double filtered_hf = wet_hf(20.0f, 1000.0f);
    INFO("3 kHz through the send: bypassed " << reference_hf << ", low-passed at 1 kHz "
                                             << filtered_hf);
    REQUIRE(filtered_hf < reference_hf * 0.7);

    const double reference_lf = wet_lf(20.0f, 20000.0f);
    const double filtered_lf = wet_lf(500.0f, 20000.0f);
    INFO("375 Hz through the send: bypassed " << reference_lf << ", high-passed at 500 Hz "
                                              << filtered_lf);
    REQUIRE(filtered_lf < reference_lf * 0.7);
}

TEST_CASE("Forge space convolution: params are block-rate, and that is the contract",
          "[host][baked][param-injection][forge][forge-space][convolution]") {
    // Not a limitation being papered over — the engine hoists its mix gains out
    // of its own sample loop, so sub-block injection could not reach the audio.
    // The node reads at offset 0 and this asserts that, so a future change that
    // moved to per-sample reads without changing the DSP would be caught rather
    // than quietly claiming a resolution it does not have.
    const auto type = conv::make_convolution_reverb_node(delta_ir());
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_convolution_defaults(inj);
    fx.settle(silence(), 4);

    const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.5f);
    // A dry-only reference block, so the level is exactly the input's.
    REQUIRE(inj.inject(immediate(conv::kWetPercent, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kDryPercent, 100.0f)) == InjectStatus::Ok);
    const auto reference = fx.render({tone, tone});

    // Inject a halving of the dry level, HALFWAY through the next block.
    REQUIRE(inj.inject(immediate(conv::kDryPercent, 50.0f, /*offset=*/kFrames / 2)) ==
            InjectStatus::Ok);
    const auto during = fx.render({tone, tone});
    // Block rate: the whole block still carries the pre-injection value.
    for (int n = 0; n < kFrames; ++n) {
        INFO("sample " << n);
        REQUIRE_THAT(during[0][static_cast<std::size_t>(n)],
                     WithinAbs(reference[0][static_cast<std::size_t>(n)], 1e-6f));
    }
    // And the block after it carries the new one, in full.
    const auto after = fx.render({tone, tone});
    for (int n = 0; n < kFrames; ++n) {
        INFO("sample " << n);
        REQUIRE_THAT(after[0][static_cast<std::size_t>(n)],
                     WithinAbs(0.5f * reference[0][static_cast<std::size_t>(n)], 1e-6f));
    }
}

TEST_CASE("Forge space convolution: the registry gain bound holds over the real path",
          "[host][baked][param-injection][forge][forge-space][convolution][gain]") {
    const auto ir = room_ir();
    const conv::IrPolicy policy{};
    const float bound = conv::convolution_reverb_worst_case_gain(ir, policy, kSr, kFrames);
    REQUIRE(bound > 0.0f);

    // The bound is composed from the DSP's own measured-at-load L1 norm, and
    // the composition is re-derived here rather than restated.
    conv::Engine probe;
    probe.prepare(kSr, kFrames, 2);
    probe.set_normalize_mode(policy.normalize);
    std::vector<const float*> ptrs{ir.channels[0].data(), ir.channels[1].data()};
    REQUIRE(probe.load_impulse_response(ptrs.data(), 2,
                                        static_cast<int>(ir.channels[0].size()),
                                        ir.sample_rate));
    probe.set_ir_gain_db(conv::Engine::kIrGainDbMax);
    const double expected =
        1.0 + 1.0 * (conv::Engine::kWidthPercentMax / 100.0) * probe.worst_case_gain();
    INFO("bound " << bound << ", recomposed " << expected);
    REQUIRE_THAT(static_cast<double>(bound), WithinRel(expected, 1e-6));

    // And it really does bound the node: a full-scale input at every ceiling
    // cannot exceed it.
    Fixture fx(conv::make_convolution_reverb_node(ir, policy), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    REQUIRE(inj.inject(immediate(conv::kIrGainDb, conv::Engine::kIrGainDbMax)) ==
            InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kPredelayMs, 0.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kWetPercent, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kDryPercent, 100.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kWidthPercent, 200.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kLowcutHz, 20.0f)) == InjectStatus::Ok);
    REQUIRE(inj.inject(immediate(conv::kHighcutHz, 20000.0f)) == InjectStatus::Ok);
    const Ir rendered = render_impulse_response(fx, 24, 1.0f);
    INFO("peak " << peak(rendered.left) << " against bound " << bound);
    REQUIRE(peak(rendered.left) <= bound);
    REQUIRE(peak(rendered.right) <= bound);
}

TEST_CASE("Forge space convolution: process allocates nothing",
          "[host][baked][param-injection][forge][forge-space][convolution][rt]") {
    const auto type = conv::make_convolution_reverb_node(room_ir());
    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_convolution_defaults(inj);

    const auto tone = pulp::test::sine_block(kFrames, 750.0, kSr, 0.4f);
    pulp::test::ReusableRenderer<2> renderer(fx, {tone, tone});
    renderer.render();  // warm the first block outside the probe

    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 64; ++block) {
            const float u = static_cast<float>(block) / 64.0f;
            REQUIRE(inj.inject(immediate(conv::kIrGainDb, -12.0f + 24.0f * u)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kPredelayMs, 50.0f * u)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kWetPercent, 100.0f * u)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kDryPercent, 100.0f * (1.0f - u))) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kWidthPercent, 200.0f * u)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kLowcutHz, 20.0f + 480.0f * u)) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(conv::kHighcutHz, 20000.0f - 19000.0f * u)) ==
                    InjectStatus::Ok);
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Forge space convolution rejects malformed registration IRs",
          "[host][baked][forge][forge-space][convolution]") {
    auto ragged = delta_ir();
    ragged.channels[1].resize(ragged.channels[0].size() - 1);
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(ragged)),
                      std::invalid_argument);

    auto nonfinite = delta_ir();
    nonfinite.channels[0][7] = std::numeric_limits<float>::quiet_NaN();
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(nonfinite)),
                      std::invalid_argument);

    auto bad_count = delta_ir(3);
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(bad_count)),
                      std::invalid_argument);

    auto tiny_rate = delta_ir();
    tiny_rate.sample_rate = std::numeric_limits<double>::denorm_min();
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(tiny_rate)),
                      std::invalid_argument);
    auto huge_rate = delta_ir();
    huge_rate.sample_rate = std::numeric_limits<double>::max();
    REQUIRE_THROWS_AS(conv::make_convolution_reverb_node(std::move(huge_rate)),
                      std::invalid_argument);

    for (double valid_rate : {8000.0, 768000.0}) {
        auto boundary = delta_ir();
        boundary.sample_rate = valid_rate;
        REQUIRE_NOTHROW(conv::make_convolution_reverb_node(std::move(boundary)));
    }
}

TEST_CASE("Forge space ambience: the node bakes and runs true stereo",
          "[host][baked][param-injection][forge][forge-space][ambience]") {
    const auto type = amb::make_nonlin_ambience_node();
    REQUIRE(std::string(type.type_id) == "space.nonlin_ambience");
    REQUIRE(type.lowerable);
    REQUIRE(type.num_input_ports == 2);
    REQUIRE(type.num_output_ports == 2);
    REQUIRE(type.baked_params.size() == 14);

    // The seed is NOT among them — series law 2, and the file note's item 4.
    for (const auto& p : type.baked_params) REQUIRE(p.id != 0);
    auto declared = [&](pulp::state::ParamID id) {
        for (const auto& p : type.baked_params)
            if (p.id == id) return true;
        return false;
    };
    REQUIRE(declared(amb::kProgram));
    REQUIRE(declared(amb::kMixPct));

    Fixture fx(type, kSr, kFrames);
    ParamInjector inj = fx.claim_injector();
    inject_ambience_defaults(inj);
    const auto out = fx.settle(impulse_block());
    REQUIRE(std::isfinite(out[0][0]));
    REQUIRE(std::isfinite(out[1][0]));
}

TEST_CASE("Forge space ambience: registration cannot invert the length range",
          "[host][baked][forge][forge-space][ambience]") {
    const auto type = amb::make_nonlin_ambience_node(amb::cal::kDefaultSeed, 1.0);
    const auto it = std::find_if(type.baked_params.begin(), type.baked_params.end(),
                                 [](const auto& p) { return p.id == amb::kLengthMs; });
    REQUIRE(it != type.baked_params.end());
    REQUIRE(it->min_value == static_cast<float>(amb::cal::kMinLengthMs));
    REQUIRE(it->max_value == static_cast<float>(amb::cal::kMinLengthMs));
    REQUIRE(it->default_value == static_cast<float>(amb::cal::kMinLengthMs));
}

TEST_CASE("Forge space ambience: the program param changes the envelope SHAPE",
          "[host][baked][param-injection][forge][forge-space][ambience][envelope]") {
    // The headline. Not "the output differs" — each program's rendered
    // impulse-response envelope has to have the shape that program is named
    // for, and the four shapes are mutually exclusive, so a program that
    // silently failed to switch fails three of the four.
    const auto type = amb::make_nonlin_ambience_node();
    constexpr double kLengthMs = 400.0;
    const int window_samples = static_cast<int>(kLengthMs * kSr / 1000.0);
    const int blocks = (window_samples + 8000) / kFrames + 2;
    const int win = static_cast<int>(0.020 * kSr);
    const int hop = static_cast<int>(0.010 * kSr);

    struct Rendered {
        std::vector<double> env;
        Ir ir;
    };
    auto measure = [&](int program) {
        Fixture fx(type, kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        Rendered r;
        r.ir = render_program_ir(fx, inj, program, kLengthMs, blocks);
        r.env = envelope_db(r.ir.left, win, hop);
        REQUIRE(peak(r.ir.left) > 1e-4);
        return r;
    };
    auto tau_of = [&](std::size_t index) {
        return (static_cast<double>(index) * hop + win * 0.5) / window_samples;
    };

    SECTION("gated: a flat body and then a cut") {
        const auto r = measure(1);
        // Flat across the body — the shape a decaying tank cannot make.
        double lo = 1e9, hi = -1e9;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            const double tau = tau_of(i);
            if (tau > 0.10 && tau < 0.60) {
                lo = std::min(lo, r.env[i]);
                hi = std::max(hi, r.env[i]);
            }
        }
        INFO("gated body spread " << (hi - lo) << " dB");
        REQUIRE(hi - lo < 4.0);

        // And then gone. The measurement is taken after the diffuser's own
        // 60 dB ring time, which is what actually bounds the cut — the DSP's
        // pre-diffusion allpasses are recursive and that is disclosed in its
        // header rather than wished away.
        double body = 0.0;
        int count = 0;
        for (std::size_t i = 0; i < r.env.size(); ++i)
            if (tau_of(i) > 0.10 && tau_of(i) < 0.60) {
                body += std::pow(10.0, r.env[i] / 10.0);
                ++count;
            }
        body /= count;
        const double late = std::pow(
            10.0, r.env[static_cast<std::size_t>(1.30 * window_samples / hop)] / 10.0);
        INFO("gated late level " << to_db(late / body) << " dB below the body");
        REQUIRE(to_db(late / body) < -50.0);
    }

    SECTION("reverse: the envelope rises") {
        const auto r = measure(2);
        // Monotone rising through the swell. No feedback reverb can do this,
        // which is why it is the assertion worth making at the node level.
        double previous = -1e9;
        int checked = 0;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            const double tau = tau_of(i);
            if (tau < 0.15 || tau > 0.75) continue;
            INFO("tau " << tau << " level " << r.env[i] << " previous " << previous);
            REQUIRE(r.env[i] > previous - 1.0);
            previous = r.env[i];
            ++checked;
        }
        REQUIRE(checked > 8);

        // Late is louder than early — the defining comparison.
        double early = 0.0, late = 0.0;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            if (std::fabs(tau_of(i) - 0.20) < 0.03) early = r.env[i];
            if (std::fabs(tau_of(i) - 0.80) < 0.03) late = r.env[i];
        }
        INFO("reverse early " << early << " dB, late " << late << " dB");
        REQUIRE(late > early + 6.0);
    }

    SECTION("ambience: the envelope falls") {
        const auto r = measure(0);
        double early = 0.0, late = 0.0;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            if (std::fabs(tau_of(i) - 0.20) < 0.03) early = r.env[i];
            if (std::fabs(tau_of(i) - 0.80) < 0.03) late = r.env[i];
        }
        INFO("ambience early " << early << " dB, late " << late << " dB");
        REQUIRE(late < early - 20.0);

        // Monotone, which separates it from the humped NonLin2 program.
        double previous = 1e9;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            const double tau = tau_of(i);
            if (tau < 0.15 || tau > 0.90) continue;
            INFO("tau " << tau);
            REQUIRE(r.env[i] < previous + 1.0);
            previous = r.env[i];
        }
    }

    SECTION("nonlin2: the body ripples and then gates") {
        const auto r = measure(3);
        // Neither monotone falling (Ambience) nor monotone rising (Reverse):
        // the body goes up, down and up again. Measured as three probes
        // straddling the designed two humps at tau = 0.25 and 0.75.
        double first_hump = 0.0, trough = 0.0, second_hump = 0.0;
        for (std::size_t i = 0; i < r.env.size(); ++i) {
            if (std::fabs(tau_of(i) - 0.25) < 0.03) first_hump = r.env[i];
            if (std::fabs(tau_of(i) - 0.50) < 0.03) trough = r.env[i];
            if (std::fabs(tau_of(i) - 0.75) < 0.03) second_hump = r.env[i];
        }
        INFO("nonlin2 humps " << first_hump << " / " << trough << " / " << second_hump
                              << " dB");
        REQUIRE(first_hump > trough + 1.5);
        REQUIRE(second_hump > trough + 1.5);
    }

    SECTION("the four programs are four different renders") {
        // Belt and braces: the shapes above are what matter, but two programs
        // that produced identical audio would be a wiring bug the shape
        // assertions might individually tolerate.
        std::vector<Ir> renders;
        for (int program = 0; program < 4; ++program) renders.push_back(measure(program).ir);
        for (std::size_t a = 0; a < renders.size(); ++a)
            for (std::size_t b = a + 1; b < renders.size(); ++b) {
                INFO("programs " << a << " and " << b);
                REQUIRE(renders[a].left != renders[b].left);
            }
    }
}

TEST_CASE("Forge space ambience: the topology params reshape the field",
          "[host][baked][param-injection][forge][forge-space][ambience]") {
    const auto type = amb::make_nonlin_ambience_node();

    SECTION("length_ms sets how long the field lasts") {
        // Measured with the diffuser bypassed, so the field's end is the last
        // tap rather than the last tap plus the allpasses' own ~118 ms ring.
        // With the ring in, a 150 ms field and a 600 ms field are 230 ms and
        // 568 ms of audible tail — still ordered, but the ratio is compressed
        // by a fixed additive term that has nothing to do with the knob.
        auto tail_end = [&](float length_ms) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, length_ms)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const int blocks =
                static_cast<int>((length_ms * kSr / 1000.0 + 8000) / kFrames) + 2;
            const Ir ir = render_impulse_response(fx, blocks);
            const double p = peak(ir.left);
            int last = 0;
            for (std::size_t n = 0; n < ir.left.size(); ++n)
                if (std::fabs(static_cast<double>(ir.left[n])) > p * 1e-3)
                    last = static_cast<int>(n);
            return last;
        };
        const int shortish = tail_end(150.0f);
        const int longish = tail_end(600.0f);
        // The gate closes at (h + w) of the window, so the ratio is the length
        // ratio: 600/150 = 4.
        INFO("field ends at " << shortish << " vs " << longish << " samples");
        REQUIRE_THAT(static_cast<double>(longish) / shortish, WithinRel(4.0, 0.15));
    }

    SECTION("predelay_ms delays the onset by the samples it names") {
        for (double ms : {0.0, 10.0}) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kPredelayMs, static_cast<float>(ms))) ==
                    InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 40);
            const int onset = first_nonzero(ir.left, peak(ir.left) * 1e-3);
            const int expected = static_cast<int>(std::lround(ms * kSr / 1000.0));
            INFO("pre-delay " << ms << " ms: onset " << onset << ", at least " << expected);
            REQUIRE(onset >= expected);
        }
    }

    SECTION("density_pct makes the early field denser") {
        auto early_density = [&](float pct) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 800.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDensityPct, pct)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kHfDampHz, 16000.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 340);
            return active_samples(ir.left, 2000, static_cast<int>(0.02 * kSr));
        };
        const int sparse = early_density(10.0f);
        const int dense = early_density(100.0f);
        INFO("active samples early: sparse " << sparse << ", dense " << dense);
        REQUIRE(dense > sparse);
    }

    SECTION("density_growth decides whether the field densifies over time") {
        auto growth_ratio = [&](float gamma) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 800.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kGateHoldPct, 95.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDensityGrowth, gamma)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kHfDampHz, 16000.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 340);
            const int win = static_cast<int>(0.02 * kSr);
            const double early = active_samples(ir.left, 2000, win);
            const double late = active_samples(ir.left, static_cast<int>(0.7 * 0.8 * kSr), win);
            return late / std::max(early, 1.0);
        };
        const double flat = growth_ratio(0.0f);
        const double physical = growth_ratio(2.0f);
        INFO("late/early density ratio: gamma 0 -> " << flat << ", gamma 2 -> " << physical);
        REQUIRE_THAT(flat, WithinRel(1.0, 0.25));
        REQUIRE(physical > flat * 1.5);
    }

    SECTION("gate_hold_pct moves the gate and attack_pct moves the swell") {
        auto gate_end = [&](float hold_pct) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 1.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 400.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kGateHoldPct, hold_pct)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 220);
            const double p = peak(ir.left);
            int last = 0;
            for (std::size_t n = 0; n < ir.left.size(); ++n)
                if (std::fabs(static_cast<double>(ir.left[n])) > p * 1e-3)
                    last = static_cast<int>(n);
            return last;
        };
        const int early_gate = gate_end(20.0f);
        const int late_gate = gate_end(90.0f);
        INFO("gate closes at " << early_gate << " vs " << late_gate << " samples");
        REQUIRE(late_gate > early_gate * 2);

        auto swell_peak = [&](float attack_pct) {
            Fixture fx(type, kSr, kFrames);
            ParamInjector inj = fx.claim_injector();
            inject_ambience_defaults(inj);
            REQUIRE(inj.inject(immediate(amb::kProgram, 2.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kLengthMs, 400.0f)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kAttackPct, attack_pct)) == InjectStatus::Ok);
            REQUIRE(inj.inject(immediate(amb::kDiffusion, 0.0f)) == InjectStatus::Ok);
            settle_silent(fx, 32);
            const Ir ir = render_impulse_response(fx, 220);
            const auto env = envelope_db(ir.left, static_cast<int>(0.02 * kSr),
                                         static_cast<int>(0.01 * kSr));
            // The FIRST window that reaches within 3 dB of the maximum — not
            // the argmax. Reverse holds a PLATEAU from `r` to 1, and across it
            // the program's reversed segment mapping keeps brightening, so the
            // absolute maximum sits near the very end of the window for EVERY
            // attack setting: measured, both 30 % and 95 % peaked at the same
            // window index 39. Where the swell ARRIVES is the thing the knob
            // moves, and that is what this finds.
            double max_db = -1e9;
            for (double v : env) max_db = std::max(max_db, v);
            for (std::size_t i = 0; i < env.size(); ++i)
                if (env[i] > max_db - 3.0) return i;
            return env.size();
        };
        const std::size_t short_rise = swell_peak(30.0f);
        const std::size_t long_rise = swell_peak(95.0f);
        INFO("swell arrives at window " << short_rise << " vs " << long_rise);
        REQUIRE(long_rise > short_rise);
    }
}
