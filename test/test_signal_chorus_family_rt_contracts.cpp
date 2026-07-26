#include "test_signal_chorus_family_support.hpp"

TEST_CASE("chorus renders are bit-identical after reset", "[signal][chorus][chorus-family]") {
    struct Case {
        Voicing voicing;
        JunoMode mode;
        bool bbd;
    };
    const Case cases[] = {
        {Voicing::ce2, JunoMode::mode_I, false},
        {Voicing::juno_ensemble, JunoMode::mode_I, false},
        {Voicing::juno_ensemble, JunoMode::mode_II, false},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II, false},
        {Voicing::dimension_d, JunoMode::mode_I, false},
        {Voicing::tri_chorus, JunoMode::mode_I, false},
        {Voicing::ce2, JunoMode::mode_I, true},
        {Voicing::tri_chorus, JunoMode::mode_I, true},
    };
    const auto source = seeded_noise(10 * static_cast<std::size_t>(kSr), 0.5, 0x51F0u);

    for (const auto& c : cases) {
        Chorus engine;
        engine.prepare(kSr);
        Config cfg;
        cfg.voicing = c.voicing;
        cfg.mode = c.mode;
        cfg.width = 1.0;
        cfg.bbd = c.bbd;
        configure(engine, cfg);

        auto run = [&] {
            engine.reset();
            std::vector<double> left = source;
            std::vector<double> right = source;
            engine.process(left.data(), right.data(), static_cast<int>(left.size()));
            return std::pair{left, right};
        };
        const auto first = run();
        const auto second = run();
        INFO(voicing_name(c.voicing) << " bbd=" << c.bbd);
        REQUIRE(first.first == second.first);
        REQUIRE(first.second == second.second);
    }
}

TEST_CASE("chorus process and reset allocate nothing", "[signal][chorus][chorus-family]") {
    // The probe's silence only means something if the probe can speak, so every
    // engine below is first run through a KNOWN-allocating call — `prepare`,
    // which sizes the delay lines and is the one function in the class allowed
    // to allocate. That doubles as the positive half of the RT contract.
    //
    // A synthetic control (a local `std::vector` inside a probe scope) does NOT
    // work here and was tried first: at -O3 clang stack-promotes it under the
    // C++14 allocation-elision rule, and the probe correctly reports zero for an
    // allocation that no longer happens. Anchoring the data pointer in a
    // `volatile` did not stop it either. Using the real call avoids the whole
    // question.
    const std::pair<Voicing, JunoMode> configs[] = {
        {Voicing::ce2, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I},
        {Voicing::juno_ensemble, JunoMode::mode_I_plus_II},
        {Voicing::dimension_d, JunoMode::mode_I},
        {Voicing::tri_chorus, JunoMode::mode_I},
    };
    for (const auto& [voicing, mode] : configs) {
        for (bool bbd : {false, true}) {
            auto engine = std::make_unique<Chorus>();
            {
                pulp::test::RtAllocationProbe control;
                engine->prepare(kSr);
                INFO(voicing_name(voicing) << " bbd=" << bbd << ": prepare allocated "
                                           << control.allocated_bytes() << " bytes in "
                                           << control.allocation_count() << " calls");
                REQUIRE(control.allocation_count() > 0);
            }
            Config cfg;
            cfg.voicing = voicing;
            cfg.mode = mode;
            cfg.bbd = bbd;
            configure(*engine, cfg);

            std::vector<double> left(512, 0.25);
            std::vector<double> right(512, -0.25);
            engine->process(left.data(), right.data(), 512);  // warm any lazy state

            require_allocates_no_memory([&] {
                engine->process(left.data(), right.data(), 512);
                engine->reset();
                engine->set_rate_hz(2.0);
                engine->set_depth(0.75);
                engine->set_mix(0.4);
                engine->set_stereo_width(0.6);
                engine->set_voicing(voicing);
                engine->set_juno_mode(mode);
                engine->set_bbd_color(bbd);
                engine->process(left.data(), right.data(), 512);
            });
        }
    }
}

TEST_CASE("chorus juno ignores the rate parameter", "[signal][chorus][chorus-family]") {
    // Closed decision, §4.2: the Juno's fixed per-mode rates ARE the Juno
    // sound, so the shared rate control is deliberately inert there. A test
    // that only checked "rate_hz() changed" would pass for a voicing that
    // silently honoured it, so this measures the delay trace.
    Config cfg;
    cfg.voicing = Voicing::juno_ensemble;
    cfg.mode = JunoMode::mode_II;
    const auto before = delay_trace(cfg, 0, 4096);

    Chorus c;
    c.prepare(kSr);
    configure(c, cfg);
    c.set_rate_hz(7.5);
    REQUIRE_THAT(c.rate_hz(), WithinRel(7.5, 1e-12));
    std::vector<double> after(4096);
    for (std::size_t i = 0; i < after.size(); ++i) {
        double l = 0.0;
        double r = 0.0;
        c.process(&l, &r, 1);
        after[i] = c.current_delay_ms(0);
    }
    REQUIRE(before == after);

    // ... while every other voicing does honour it.
    Config ce2 = cfg;
    ce2.voicing = Voicing::ce2;
    const auto default_trace = delay_trace(ce2, 0, 4096);
    Chorus fast;
    fast.prepare(kSr);
    configure(fast, ce2);
    fast.set_rate_hz(5.0);
    double l = 0.0;
    double r = 0.0;
    for (int i = 0; i < 4096; ++i) fast.process(&l, &r, 1);
    REQUIRE(fast.current_delay_ms(0) != default_trace.back());
}

TEST_CASE("chorus mix crossfades bypass against the whole circuit",
          "[signal][chorus][chorus-family]") {
    // §5: the blend is applied AFTER the voicing matrix, and every matrix
    // carries its own dry term, so mix = 0 must be an exact bypass.
    const auto source = seeded_noise(4096, 0.5, 0x0C0Fu);
    for (auto voicing : {Voicing::ce2, Voicing::juno_ensemble, Voicing::dimension_d,
                         Voicing::tri_chorus}) {
        Config cfg;
        cfg.voicing = voicing;
        cfg.mix = 0.0;
        cfg.width = 1.0;
        const auto out = render(cfg, source, source);
        INFO(voicing_name(voicing));
        REQUIRE(out.left == source);
        REQUIRE(out.right == source);
    }
}

TEST_CASE("chorus ce2 is mono on both outputs", "[signal][chorus][chorus-family]") {
    // §4.1: the real pedal is mono in and out on both jacks.
    const auto left_in = seeded_noise(4096, 0.5, 0x11u);
    const auto right_in = seeded_noise(4096, 0.5, 0x22u);
    Config cfg;
    cfg.voicing = Voicing::ce2;
    cfg.mix = 1.0;
    const auto out = render(cfg, left_in, right_in);
    REQUIRE(out.left == out.right);
}

TEST_CASE("chorus float and double instantiations agree", "[signal][chorus][chorus-family]") {
    // The read position and phase accumulators are `double` in both
    // instantiations by construction; only the delay-line storage narrows. This
    // pins that the narrowing costs precision and nothing else.
    const auto source = seeded_noise(8192, 0.5, 0x2B1Du);
    ChorusEnsembleT<float> narrow;
    narrow.prepare(kSr);
    narrow.set_voicing(ChorusEnsembleT<float>::Voicing::tri_chorus);
    narrow.set_depth(1.0f);
    narrow.set_mix(1.0f);
    narrow.set_stereo_width(1.0f);
    narrow.reset();

    std::vector<float> fl(source.size());
    std::vector<float> fr(source.size());
    for (std::size_t i = 0; i < source.size(); ++i) {
        fl[i] = static_cast<float>(source[i]);
        fr[i] = fl[i];
    }
    narrow.process(fl.data(), fr.data(), static_cast<int>(fl.size()));

    Config cfg;
    cfg.voicing = Voicing::tri_chorus;
    cfg.mix = 1.0;
    cfg.width = 1.0;
    const auto wide = render(cfg, source, source);

    double worst = 0.0;
    for (std::size_t i = 0; i < source.size(); ++i)
        worst = std::max(worst, std::abs(static_cast<double>(fl[i]) - wide.left[i]));
    INFO("largest float/double divergence " << worst);
    REQUIRE(worst < 1e-4);
}
