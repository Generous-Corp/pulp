#include "test_forge_effect_modulation_catalog_support.hpp"

TEST_CASE("Forge modulation: delay vibrato does not claim fixed PDC latency",
          "[host][baked][forge][forge-modulation][vibrato]") {
    // Rate and depth move the tap continuously, so a worst-case upper bound is
    // not an exact intrinsic latency and must not feed graph PDC.
    const auto type = vib_delay::make_delay_vibrato_node(4.0f);
    REQUIRE_FALSE(type.latency_samples);

    // The floor still changes the registered parameter contract and therefore
    // remains a stable realization identity.
    REQUIRE(vib_delay::make_delay_vibrato_node(4.0f).type_id !=
            vib_delay::make_delay_vibrato_node(8.0f).type_id);

    for (const auto& p : type.baked_params)
        if (p.id == vib_delay::kRateHz) REQUIRE_THAT(p.min_value, WithinAbs(4.0f, 1e-6f));
}

TEST_CASE("Forge modulation: the phase vibrato's params reach the engine",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    const auto t = tone();

    // Mix 0 is a wire, bit-exact.
    {
        auto fx = Mono(vib_phase::make_phase_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_phase::kMix, 0.0f)) == InjectStatus::Ok);
        const auto out = fx.settle({t}, 96);
        for (int k = 0; k < kFrames; ++k)
            REQUIRE(out[0][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
    }

    const auto render_with = [&t](pulp::state::ParamID id, float value) {
        auto fx = Mono(vib_phase::make_phase_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_phase::kMix, 100.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_phase::kRateHz, 1.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_phase::kDepth, 60.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_phase::kCenterHz, 500.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(id, value)) == InjectStatus::Ok);
        fx.settle({t}, 96);
        return capture(fx, {t}, 128);
    };
    const auto difference = [](const std::vector<float>& a, const std::vector<float>& b) {
        double d = 0.0;
        for (std::size_t k = 0; k < a.size(); ++k)
            d = std::max(d, std::abs(static_cast<double>(a[k]) - static_cast<double>(b[k])));
        return d;
    };

    const auto baseline = render_with(vib_phase::kDepth, 60.0f);
    REQUIRE(difference(baseline, render_with(vib_phase::kDepth, 0.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(vib_phase::kCenterHz, 1800.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(vib_phase::kRateHz, 6.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(vib_phase::kMix, 50.0f)) > 0.01);
}

TEST_CASE("Forge modulation: the phase vibrato stage counts are distinct registrations",
          "[host][baked][forge][forge-modulation][vibrato]") {
    using Engine = vib_phase::Engine;
    std::vector<std::string> ids;
    for (int stages = 1; stages <= Engine::kMaxStages; ++stages)
        ids.push_back(vib_phase::make_phase_vibrato_node(stages).type_id);
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());

    // Different cascade lengths are audibly different, which is what makes the
    // realization axis a real one rather than a naming convention.
    const auto t = tone();
    const auto render = [&t](int stages) {
        auto fx = Mono(vib_phase::make_phase_vibrato_node(stages), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_phase::kMix, 100.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_phase::kDepth, 80.0f)) == InjectStatus::Ok);
        fx.settle({t}, 96);
        return capture(fx, {t}, 64);
    };
    const auto two = render(2);
    const auto four = render(4);
    double d = 0.0;
    for (std::size_t k = 0; k < two.size(); ++k)
        d = std::max(d, std::abs(static_cast<double>(two[k]) - static_cast<double>(four[k])));
    REQUIRE(d > 0.01);
}

TEST_CASE("Forge modulation: the Univibe's stepped mode param reaches the engine",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    // The decisive test for this knob, and it is available only because the DSP
    // guarantees something exact: in the chorus position the LEFT output is the
    // untouched input. So the stepped param is checked bit-for-bit rather than
    // by a level difference.
    const auto t = tone();
    auto fx = Stereo(vib_univibe::make_univibe_node(), kSr, kFrames);
    ParamInjector inj = fx.claim_injector();

    REQUIRE(inj.inject(immediate(vib_univibe::kMode, vib_univibe::kModeChorus)) ==
            InjectStatus::Ok);
    const auto chorus_out = fx.settle({t, t}, 96);
    for (int k = 0; k < kFrames; ++k)
        REQUIRE(chorus_out[0][static_cast<std::size_t>(k)] == t[static_cast<std::size_t>(k)]);
    // ...while the right rail is genuinely phase-shifted.
    double side = 0.0;
    for (int k = 0; k < kFrames; ++k)
        side = std::max(side, std::abs(static_cast<double>(
                                  chorus_out[0][static_cast<std::size_t>(k)] -
                                  chorus_out[1][static_cast<std::size_t>(k)])));
    REQUIRE(side > 0.01);

    // In the vibrato position both rails carry the same wet signal, and neither
    // is the input.
    REQUIRE(inj.inject(immediate(vib_univibe::kMode, vib_univibe::kModeVibrato)) ==
            InjectStatus::Ok);
    const auto vibrato_out = fx.settle({t, t}, 96);
    for (int k = 0; k < kFrames; ++k)
        REQUIRE(vibrato_out[0][static_cast<std::size_t>(k)] ==
                vibrato_out[1][static_cast<std::size_t>(k)]);
    double moved = 0.0;
    for (int k = 0; k < kFrames; ++k)
        moved = std::max(moved, std::abs(static_cast<double>(
                                    vibrato_out[0][static_cast<std::size_t>(k)] -
                                    t[static_cast<std::size_t>(k)])));
    REQUIRE(moved > 0.01);

    REQUIRE(vib_univibe::mode_from_param(0.0f) == vib_univibe::Mode::vibrato);
    REQUIRE(vib_univibe::mode_from_param(0.49f) == vib_univibe::Mode::vibrato);
    REQUIRE(vib_univibe::mode_from_param(0.51f) == vib_univibe::Mode::chorus);
    REQUIRE(vib_univibe::mode_from_param(7.0f) == vib_univibe::Mode::chorus);
}

TEST_CASE("Forge modulation: the Univibe's rate and depth params reach the engine",
          "[host][baked][param-injection][forge][forge-modulation][vibrato]") {
    const auto t = tone();
    const auto render_with = [&t](pulp::state::ParamID id, float value) {
        auto fx = Stereo(vib_univibe::make_univibe_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        REQUIRE(inj.inject(immediate(vib_univibe::kRateHz, 3.0f)) == InjectStatus::Ok);
        REQUIRE(inj.inject(immediate(vib_univibe::kDepth, 70.0f)) == InjectStatus::Ok);
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
    const auto baseline = render_with(vib_univibe::kDepth, 70.0f);
    REQUIRE(difference(baseline, render_with(vib_univibe::kDepth, 0.0f)) > 0.01);
    REQUIRE(difference(baseline, render_with(vib_univibe::kRateHz, 8.0f)) > 0.01);
}

TEST_CASE("Forge modulation: the vibrato registry gains cite the bounds their suite asserts",
          "[host][baked][forge][forge-modulation][vibrato]") {
    // Series law 8, and the place in this family where the obvious number is the
    // wrong one.
    //
    // The delay engine is one unit-gain tap and still is not 0 dB, because the
    // tap is read through the Lagrange kernel whose L1 norm is 1.25.
    REQUIRE_THAT(static_cast<double>(vib_delay::delay_vibrato_worst_case_gain()),
                 WithinRel(static_cast<double>(vib_delay::Engine::kInterpolatorPeakGain), 1e-9));
    REQUIRE(vib_delay::delay_vibrato_worst_case_gain() > 1.0f);

    // The two PHASE engines are allpass cascades, and an allpass is unity
    // MAGNITUDE — which bounds steady-state sinusoids and says nothing about
    // sample gain. Its impulse response changes sign, so a sign-matched bounded
    // input accumulates to the cascade's L1 norm, and the DSP suite measures
    // both engines above a factor of two. Citing the sinusoidal bound of 1 would
    // put a number in the registry that the DSP's own suite disproves, so these
    // assertions pin the L1 and pin it ABOVE the sinusoidal bound — a later
    // "simplification" back to 1.0 fails here.
    for (int stages : {2, 4}) {
        const double lowest = vib_phase::Engine::kMinCenterHz *
                              std::exp2(-vib_phase::Engine::kSweepOctaves);
        const double reference = vib::allpass_cascade_l1(
            std::vector<double>(static_cast<std::size_t>(stages), lowest), kSr);
        // Compared at float precision: the node returns a float for the
        // registry, so a double-precision tolerance would be asserting that a
        // float can hold sixteen digits.
        REQUIRE_THAT(static_cast<double>(vib_phase::phase_vibrato_worst_case_gain(stages, kSr)),
                     WithinRel(reference, 1e-6));
        REQUIRE(vib_phase::phase_vibrato_worst_case_gain(stages, kSr) >
                static_cast<float>(vib_phase::Engine::kSinusoidalGainBound));
    }
    REQUIRE(vib_univibe::univibe_worst_case_gain(kSr) >
            static_cast<float>(vib_univibe::Engine::kSinusoidalGainBound));
    REQUIRE(vib_univibe::univibe_worst_case_gain(kSr) > 2.0f);

    // A longer cascade cannot have a smaller worst case, which is the sanity
    // check that the number tracks the realization rather than being a
    // stage-count-blind constant.
    REQUIRE(vib_phase::phase_vibrato_worst_case_gain(4, kSr) >
            vib_phase::phase_vibrato_worst_case_gain(2, kSr));
}

TEST_CASE("Forge modulation: the vibrato nodes' process paths allocate nothing",
          "[host][baked][forge][forge-modulation][vibrato][rt-safety]") {
    const auto t = tone();
    {
        auto fx = Mono(vib_delay::make_delay_vibrato_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        fx.settle({t}, 8);
        pulp::test::ReusableRenderer<1> renderer(fx, {t});
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 32; ++b) {
            inj.inject(immediate(vib_delay::kRateHz, 4.0f + 0.5f * static_cast<float>(b)));
            inj.inject(immediate(vib_delay::kDepthCents, 3.0f * static_cast<float>(b)));
            // Including the onset controls, whose change-detected path must be
            // allocation-free on both the taken and the untaken branch.
            inj.inject(immediate(vib_delay::kDelayMs, 10.0f * static_cast<float>(b % 4)));
            inj.inject(immediate(vib_delay::kFadeInMs, 20.0f * static_cast<float>(b % 3)));
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
    {
        auto fx = Mono(vib_phase::make_phase_vibrato_node(4), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        fx.settle({t}, 8);
        pulp::test::ReusableRenderer<1> renderer(fx, {t});
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 32; ++b) {
            inj.inject(immediate(vib_phase::kRateHz, 0.05f + 0.3f * static_cast<float>(b)));
            inj.inject(immediate(vib_phase::kDepth, 3.0f * static_cast<float>(b)));
            inj.inject(immediate(vib_phase::kCenterHz, 200.0f + 55.0f * static_cast<float>(b)));
            inj.inject(immediate(vib_phase::kMix, 3.0f * static_cast<float>(b)));
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
    {
        auto fx = Stereo(vib_univibe::make_univibe_node(), kSr, kFrames);
        ParamInjector inj = fx.claim_injector();
        fx.settle({t, t}, 8);
        pulp::test::ReusableRenderer<2> renderer(fx, {t, t});
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 32; ++b) {
            inj.inject(immediate(vib_univibe::kRateHz, 0.3f + 0.24f * static_cast<float>(b)));
            inj.inject(immediate(vib_univibe::kDepth, 3.0f * static_cast<float>(b)));
            inj.inject(immediate(vib_univibe::kMode, static_cast<float>(b % 2)));
            renderer.render();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Forge modulation: every node in the family has a distinct type id",
          "[host][baked][forge][forge-modulation]") {
    // One registry, one namespace of ids. Two nodes sharing an id would load a
    // session into the wrong effect, and it is exactly the kind of thing that
    // only shows up once a fifth lineage is added.
    std::vector<std::string> ids{mod::kSsbFrequencyShifterTypeId,
                                 vib_univibe::kTypeId};
    using Voicing = chorus_ns::Voicing;
    using JunoMode = chorus_ns::JunoMode;
    for (auto v : {Voicing::ce2, Voicing::juno_ensemble, Voicing::dimension_d,
                   Voicing::tri_chorus})
        for (auto m : {JunoMode::mode_I, JunoMode::mode_II, JunoMode::mode_I_plus_II})
            for (bool bbd : {false, true}) {
                if (v != Voicing::juno_ensemble && m != JunoMode::mode_I) continue;
                ids.push_back(chorus_ns::make_chorus_node(v, m, bbd).type_id);
            }
    for (int stages : {4, 6, 8, 10, 12}) ids.push_back(phaser_ns::make_phaser_node(stages).type_id);
    for (float floor_hz : {4.0f, 8.0f})
        ids.push_back(vib_delay::make_delay_vibrato_node(floor_hz).type_id);
    for (int stages = 1; stages <= vib_phase::Engine::kMaxStages; ++stages)
        ids.push_back(vib_phase::make_phase_vibrato_node(stages).type_id);

    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
    // Every one of them is namespaced to the family, so a future dynamics or
    // reverb id cannot collide with these either.
    for (const auto& id : ids) REQUIRE(id.rfind("modulation.", 0) == 0);
}

TEST_CASE("Forge modulation: flanger, Leslie and scanner factories expose canonical contracts",
          "[host][baked][forge][forge-modulation][catalog]") {
    auto flanger = mod::flanger::make_flanger_node();
    auto leslie = mod::leslie::make_leslie_node();
    auto scanner = mod::leslie::make_scanner_vibrato_node();
    REQUIRE(flanger.type_id == mod::flanger::kTypeId);
    REQUIRE(flanger.baked_params.size() == 9);
    REQUIRE(flanger.num_input_ports == 2);
    REQUIRE(flanger.num_output_ports == 2);
    REQUIRE(leslie.type_id == mod::leslie::kTypeId);
    REQUIRE(leslie.baked_params.size() == 24);
    REQUIRE(leslie.num_input_ports == 2);
    REQUIRE(leslie.num_output_ports == 2);
    REQUIRE(scanner.type_id == mod::leslie::kScannerTypeId);
    REQUIRE(scanner.baked_params.size() == 7);
    REQUIRE(scanner.num_input_ports == 1);
    REQUIRE(scanner.num_output_ports == 1);
    REQUIRE(mod::flanger::worst_case_gain() ==
            static_cast<float>(pulp::signal::Flanger::worst_case_gain()));
    REQUIRE(mod::leslie::leslie_worst_case_gain() ==
            static_cast<float>(pulp::signal::LeslieRotary::kWorstCaseGain));
    REQUIRE(mod::leslie::scanner_worst_case_gain() ==
            static_cast<float>(pulp::signal::ScannerVibrato::kWorstCaseGain));

    Fixture flanger_fx(std::move(flanger), kSr, kFrames);
    const auto block = tone();
    auto out = flanger_fx.render({block, block});
    for (const auto& channel : out)
        for (float x : channel) REQUIRE(std::isfinite(x));
    Fixture leslie_fx(std::move(leslie), kSr, kFrames);
    out = leslie_fx.render({block, block});
    for (const auto& channel : out)
        for (float x : channel) REQUIRE(std::isfinite(x));
    pulp::test::BakedNodeFixture<1> scanner_fx(std::move(scanner), kSr, kFrames);
    const auto mono = scanner_fx.render({block});
    for (float x : mono[0]) REQUIRE(std::isfinite(x));
}

TEST_CASE("Forge modulation: flanger latency controls are frozen realizations",
          "[host][baked][forge][forge-modulation][flanger][latency]") {
    using Mode = pulp::signal::FlangerMode;
    const auto classic = mod::flanger::make_flanger_node(Mode::classic, 4.0);
    const auto through_zero = mod::flanger::make_flanger_node(Mode::through_zero, 4.0);
    REQUIRE(classic.type_id != through_zero.type_id);
    REQUIRE(mod::flanger::latency_samples(Mode::classic, 4.0, kSr) == 0);
    REQUIRE(mod::flanger::latency_samples(Mode::through_zero, 4.0, kSr) == 192);
    REQUIRE(classic.latency_samples(kSr) == 0);
    REQUIRE(through_zero.latency_samples(kSr) == 192);

    for (const auto& row : classic.baked_params) {
        REQUIRE(row.id != mod::flanger::kMode);
        REQUIRE(row.id != mod::flanger::kOffset);
    }
}
