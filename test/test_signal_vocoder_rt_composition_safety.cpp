#include "test_signal_vocoder_support.hpp"

TEST_CASE("vocoder process and reset allocate nothing", "[signal][vocoder]") {
    // Each engine is first run through `prepare`, which is the one call allowed
    // to do bounded work, so the probe is known to be able to speak before its
    // silence is taken as evidence. (A synthetic control — a local vector
    // inside a probe scope — does not work at -O3: clang stack-promotes it
    // under the C++14 allocation-elision rule and the probe correctly reports
    // zero for an allocation that no longer happens.)
    auto engine = std::make_unique<Voc>();
    // A positive control first. M10 could use `prepare` for this because its
    // delay lines are vectors; this class allocates NOTHING anywhere, so its
    // probe would otherwise be silent whether or not it was working. An
    // explicit `::operator new` of a runtime-sized block is the funnel every
    // heap allocation goes through and is not elidable the way a local
    // container is (clang stack-promotes those at -O3 under the C++14
    // allocation-elision rule, which makes a container control useless here).
    {
        pulp::test::RtAllocationProbe control;
        const std::size_t bytes = 64 + (Voc::kMaxBands * sizeof(double));
        void* block = ::operator new(bytes);
        const std::size_t seen = control.allocation_count();
        ::operator delete(block);
        REQUIRE(seen > 0);
    }

    std::size_t prepare_allocations = 0;
    std::size_t prepare_bytes = 0;
    {
        pulp::test::RtAllocationProbe control;
        engine->prepare(kSr);
        engine->set_band_count(20);
        // Read inside the scope but REPORTED outside it: Catch2's INFO builds a
        // string, and a string built inside a probe scope is an allocation the
        // probe counts. The first draft of this test measured its own message.
        prepare_allocations = control.allocation_count();
        prepare_bytes = control.allocated_bytes();
    }
    INFO("prepare + set_band_count allocated " << prepare_bytes << " bytes in "
                                               << prepare_allocations << " calls");
    // Every buffer is a fixed std::array, so even prepare must be silent here —
    // this class's RT contract is stronger than "process is clean".
    REQUIRE(prepare_allocations == 0);

    for (auto source : {Voc::CarrierSource::internal, Voc::CarrierSource::external}) {
        for (bool freeze : {false, true}) {
            engine->prepare(kSr);
            engine->set_carrier_source(source);
            engine->set_formant_freeze(freeze);
            engine->set_band_count(16);
            engine->reset();

            double out = 0.0;
            for (int i = 0; i < 512; ++i) engine->process(0.25, -0.25, out);  // warm any lazy state

            require_allocates_no_memory([&] {
                for (int i = 0; i < 512; ++i) engine->process(0.25, -0.25, out);
                engine->reset();
                engine->set_attack_ms(3.0);
                engine->set_release_ms(40.0);
                engine->set_noise_mix(0.4);
                engine->set_unvoiced_sensitivity(0.7);
                engine->set_sibilance_mix(0.2);
                engine->set_formant_shift_semitones(-7.0);
                engine->set_internal_pitch_hz(220.0);
                engine->set_internal_pulse_width(0.3);
                engine->set_output_trim_db(-6.0);
                engine->set_dry_wet(0.8);
                // The two structural changes, mid-stream: both only move the
                // active loop bound and recompute coefficients.
                engine->set_band_count(11);
                engine->set_band_range_hz(90.0, 9000.0);
                for (int i = 0; i < 512; ++i) engine->process(0.25, -0.25, out);
            });
        }
    }
}

TEST_CASE("vocoder composes with the chorus ensemble tail", "[signal][vocoder]") {
    // §10 is normative but deliberately not implemented in `VocoderT`: the
    // catalog node feeds this module's mono output into a chorus instance and
    // crossfades by `ensemble_amt`. That leaves the composition unwritten and
    // untested unless someone writes it, so it is written here — the node
    // author gets an executable reference rather than a paragraph.
    //
    // Voicing choice: `juno_ensemble`, which is the chorus module's Roland BBD
    // ensemble — two taps, one per channel, modulators an exact half cycle
    // apart. The VP-330's bed is a Roland BBD ensemble of exactly that era and
    // topology, so `bbd_color` is on as well.
    using Chorus = ChorusEnsembleT<double>;

    auto v = make_bank(12);  // V2 uses 12 bands — softer, more vowel than consonant
    v.set_carrier_source(Voc::CarrierSource::internal);
    v.set_internal_wave(Voc::InternalWave::saw);
    v.set_internal_pitch_hz(110.0);
    v.set_noise_mix(0.15);
    v.set_sibilance_mix(0.35);

    Chorus chorus;
    chorus.prepare(kSr);
    chorus.set_voicing(Chorus::Voicing::juno_ensemble);
    chorus.set_juno_mode(Chorus::JunoMode::mode_I);
    chorus.set_bbd_color(true);
    chorus.set_mix(1.0);
    chorus.reset();

    const auto n = static_cast<std::size_t>(2.0 * kSr);
    const auto modulator = seeded_noise(n, 0.4, 0x7A7Au);
    const std::vector<double> no_carrier(n, 0.0);

    v.reset();
    const auto mono = render(v, modulator, no_carrier);

    auto wire = [&](double ensemble_amount) {
        chorus.reset();
        std::vector<double> left = mono;
        std::vector<double> right = mono;
        chorus.process(left.data(), right.data(), static_cast<int>(left.size()));
        std::vector<double> out_l(n);
        std::vector<double> out_r(n);
        for (std::size_t i = 0; i < n; ++i) {
            out_l[i] = (1.0 - ensemble_amount) * mono[i] + ensemble_amount * left[i];
            out_r[i] = (1.0 - ensemble_amount) * mono[i] + ensemble_amount * right[i];
        }
        return std::pair{out_l, out_r};
    };

    // ensemble_amt = 0 is dual-mono `out_dry`, exactly as §10 states.
    const auto dry = wire(0.0);
    REQUIRE(dry.first == mono);
    REQUIRE(dry.second == mono);

    // ensemble_amt > 0 decorrelates the channels — that is what the bed is.
    const auto wet = wire(0.6);
    const auto skip = static_cast<std::size_t>(0.5 * kSr);
    double cross = 0.0;
    double energy_l = 0.0;
    double energy_r = 0.0;
    for (std::size_t i = skip; i < n; ++i) {
        cross += wet.first[i] * wet.second[i];
        energy_l += wet.first[i] * wet.first[i];
        energy_r += wet.second[i] * wet.second[i];
    }
    const double correlation = cross / std::sqrt(energy_l * energy_r);
    INFO("L/R correlation with ensemble_amt = 0.6: " << correlation);
    REQUIRE(energy_l > 0.0);
    REQUIRE(correlation < 0.995);
    REQUIRE(correlation > 0.0);  // still a coherent bed, not an inverted pair
}

TEST_CASE("vocoder pre-emphasis is applied to the modulator only", "[signal][vocoder]") {
    // §3.4 says "never to the carrier or the output". Measured by driving the
    // carrier alone through the synthesis bank and checking the band's peak
    // gain is unity — a tilt on the carrier path would show up as a
    // frequency-dependent scaling on it.
    auto v = make_bank();
    v.set_carrier_source(Voc::CarrierSource::external);
    v.set_sibilance_mix(0.0);

    for (int k : {2, 8, 14}) {
        const double center = v.band_center_hz(k);
        // Analysis magnitude WITHOUT dividing the tilt out must show it...
        v.reset();
        const auto window = static_cast<std::size_t>(std::ceil(40.0 * kSr / center));
        std::vector<double> trace(window);
        double out = 0.0;
        for (std::size_t i = 0; i < 8192 + window; ++i) {
            const double m = std::sin(2.0 * kPi * center * static_cast<double>(i) / kSr);
            v.process(m, 0.0, out);
            if (i >= 8192) trace[i - 8192] = v.analysis_band(k);
        }
        const double raw = coherent_magnitude(trace, center);
        INFO("band " << k << " (f_c " << center << "): raw analysis gain " << raw
                     << ", pre-emphasis tilt " << pre_emphasis_gain(center));
        REQUIRE_THAT(raw, WithinRel(pre_emphasis_gain(center), 0.03));
    }
}

TEST_CASE("vocoder external carrier keeps its low bands intact", "[signal][vocoder]") {
    // §5's closed decision: under an external carrier the unvoiced decision
    // substitutes noise only ABOVE the sibilance corner, so the caller's pad is
    // not smeared in the bands where it carries pitch.
    //
    // Measured by freezing the bank first, so the synthesis gains are identical
    // in both conditions and the ONLY difference between them is the carrier
    // substitution. Comparing a voiced and an unvoiced render without freezing
    // compares two different spectral envelopes and says nothing about the
    // carrier — which is what the first draft of this test did.
    const auto n = static_cast<std::size_t>(1.0 * kSr);
    const auto hiss = seeded_noise(n, 0.5, 0x5151u);
    const auto buzz = sawtooth(n, 150.0, 0.8);

    auto tone_amplitude = [&](int band, bool unvoiced) {
        auto v = make_bank();
        v.set_carrier_source(Voc::CarrierSource::external);
        v.set_sibilance_mix(0.0);
        v.set_noise_mix(0.0);

        // Prime with a TONE at the band under test, so the frozen bank is
        // essentially that band alone. A broadband prime leaves every band
        // open, and the substituted high bands then drop a random noise
        // residual into the low band's measurement bin — 4.5 % of it, which is
        // what the first draft of this test measured and mistook for leakage.
        const double hz = v.band_center_hz(band);
        const auto prime = sine(static_cast<std::size_t>(0.3 * kSr), hz, 1.0);
        const std::vector<double> quiet(prime.size(), 0.0);
        v.reset();
        render(v, prime, quiet);
        v.set_formant_freeze(true);
        double scratch = 0.0;
        v.process(0.0, 0.0, scratch);

        const auto carrier = sine(n, hz, 1.0);
        const auto out = render(v, unvoiced ? hiss : buzz, carrier);
        REQUIRE_THAT(v.unvoiced(), WithinAbs(unvoiced ? 1.0 : 0.0, 0.05));
        const std::vector<double> tail(out.begin() + static_cast<std::ptrdiff_t>(kSr / 2),
                                       out.end());
        return coherent_magnitude(tail, hz);
    };

    // Below the corner the carrier passes untouched: the ratio is 1.
    const int low_band = 3;
    const double low_ratio = tone_amplitude(low_band, true) / tone_amplitude(low_band, false);
    INFO("band " << low_band << " (below " << Voc::kSibilanceCornerHz
                 << " Hz): unvoiced/voiced carrier tone = " << low_ratio);
    REQUIRE_THAT(low_ratio, WithinAbs(1.0, 0.02));

    // Above it the tone is displaced by exactly kUnvoicedNoise of noise, so the
    // coherent part is scaled by (1 − kUnvoicedNoise) — computed, not guessed.
    const int high_band = kBands - 1;
    auto probe = make_bank();
    // Its immediate neighbours must be above the corner as well, or an
    // un-substituted neighbour would put an unsubstituted tone in the same bin.
    REQUIRE(probe.band_center_hz(high_band) > Voc::kSibilanceCornerHz);
    REQUIRE(probe.band_center_hz(high_band - 1) > Voc::kSibilanceCornerHz);
    const double high_ratio = tone_amplitude(high_band, true) / tone_amplitude(high_band, false);
    INFO("band " << high_band << " (above the corner): " << high_ratio << " against the predicted "
                 << 1.0 - Voc::kUnvoicedNoise);
    REQUIRE_THAT(high_ratio, WithinAbs(1.0 - Voc::kUnvoicedNoise, 0.03));
}

TEST_CASE("vocoder mix and trim behave as declared", "[signal][vocoder]") {
    auto v = make_bank();
    v.set_carrier_source(Voc::CarrierSource::external);

    const auto n = static_cast<std::size_t>(0.5 * kSr);
    const auto modulator = seeded_noise(n, 0.4, 0x2B2Bu);
    const auto carrier = seeded_noise(n, 0.4, 0x6D6Du);

    // dry_wet = 0 passes the DC-BLOCKED modulator — the blocker is in the dry
    // path by design and by name. Compared against a reference blocker built
    // from the shipped corner rather than against the raw modulator, which
    // would only measure the blocker.
    v.set_dry_wet(0.0);
    v.reset();
    const auto dry = render(v, modulator, carrier);

    const double pole = std::exp(-2.0 * kPi * Voc::kDcBlockHz / kSr);
    double last_in = 0.0;
    double last_out = 0.0;
    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        last_out = modulator[i] - last_in + pole * last_out;
        last_in = modulator[i];
        worst = std::max(worst, std::abs(dry[i] - last_out));
    }
    INFO("largest deviation from a reference DC blocker at " << Voc::kDcBlockHz << " Hz: "
                                                             << worst);
    REQUIRE(worst < 1e-9);

    // Output trim is a plain gain on the wet path.
    v.set_dry_wet(1.0);
    v.set_output_trim_db(0.0);
    v.reset();
    const auto unity = render(v, modulator, carrier);
    v.set_output_trim_db(-6.0);
    v.reset();
    const auto trimmed = render(v, modulator, carrier);
    const double expected = units::db_to_linear(-6.0);
    for (std::size_t i = static_cast<std::size_t>(0.1 * kSr); i < n; ++i)
        REQUIRE_THAT(trimmed[i], WithinAbs(unity[i] * expected, 1e-12));
}

TEST_CASE("vocoder float and double instantiations agree", "[signal][vocoder]") {
    const auto n = static_cast<std::size_t>(0.25 * kSr);
    const auto modulator = seeded_noise(n, 0.4, 0x4F4Fu);
    const auto carrier = seeded_noise(n, 0.4, 0x1E1Eu);

    VocoderT<float> narrow;
    narrow.prepare(kSr);
    narrow.set_band_count(kBands);
    narrow.set_band_range_hz(kLoHz, kHiHz);
    narrow.set_carrier_source(VocoderT<float>::CarrierSource::external);
    narrow.reset();

    auto wide = make_bank();
    wide.set_carrier_source(Voc::CarrierSource::external);
    wide.reset();

    double worst = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        float narrow_out = 0.0f;
        double wide_out = 0.0;
        narrow.process(static_cast<float>(modulator[i]), static_cast<float>(carrier[i]),
                       narrow_out);
        wide.process(modulator[i], carrier[i], wide_out);
        worst = std::max(worst, std::abs(static_cast<double>(narrow_out) - wide_out));
    }
    INFO("largest float/double divergence " << worst);
    REQUIRE(worst < 1e-4);
}

TEST_CASE("vocoder rejects non-finite audio before recursive state and recovers exactly",
          "[signal][vocoder][nan-recovery][rt-safety]") {
    Voc poisoned = make_bank();
    Voc fresh = make_bank();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    double output = 1.0;

    {
        pulp::test::RtAllocationProbe probe;
        poisoned.process(nan, 0.25, output);
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(output == 0.0);

    for (int i = 0; i < 4096; ++i) {
        const double modulator = 0.4 * std::sin(2.0 * kPi * 220.0 * i / kSr);
        const double carrier = 0.3 * std::sin(2.0 * kPi * 110.0 * i / kSr);
        double recovered = 0.0;
        double reference = 0.0;
        poisoned.process(modulator, carrier, recovered);
        fresh.process(modulator, carrier, reference);
        REQUIRE(std::isfinite(recovered));
        REQUIRE(recovered == reference);
    }
}
