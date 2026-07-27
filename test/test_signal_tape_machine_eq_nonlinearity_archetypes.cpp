#include "test_signal_tape_machine_support.hpp"

TEST_CASE("Tape machine: the NAB record network matches the analytic prototype",
          "[signal][tape-machine][eq]") {
    // R1. The prototype is evaluated from the SHIPPED time constants, so this
    // fails if the 3180/50 µs table row moves, rather than agreeing with a
    // hard-coded copy of it.
    for (const double fs : {kSr, kAltSr}) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::studer_a800);
        machine.set_eq_curve(TapeCurve::nab);
        machine.set_speed_ips(15.0);
        machine.prepare(fs);

        const tape::EqTimeConstants tc = tape::eq_time_constants(TapeCurve::nab, 15.0);
        REQUIRE(tc.has_bass_shelf());
        REQUIRE_THAT(tc.bass_s, WithinAbs(3180e-6, 1e-12));
        REQUIRE_THAT(tc.treble_s, WithinAbs(50e-6, 1e-12));

        for (const double hz : {50.0, 1000.0, 3183.0, 10000.0}) {
            const double realized = machine.record_eq().response_db(hz, fs);
            const double analytic = tape::eq_record_response_db(tc, hz);
            REQUIRE_THAT(realized, WithinAbs(analytic, 0.5));
            // The playback network is the record network's exact inverse, so
            // its response is the negation to numerical precision, not to a
            // fitted tolerance.
            REQUIRE_THAT(machine.playback_eq().response_db(hz, fs),
                         WithinAbs(-realized, 1e-9));
        }

        // Unity at the alignment frequency by construction, for every curve —
        // the property that makes switching EQ standards a tone change rather
        // than a level change.
        for (const TapeCurve curve : kAllCurves) {
            TapeMachine any;
            any.set_archetype(TapeArchetype::studer_a800);
            any.set_eq_curve(curve);
            any.prepare(fs);
            REQUIRE_THAT(any.record_eq().response_db(tape::kEqReferenceHz, fs),
                         WithinAbs(0.0, 1e-9));
            REQUIRE_THAT(any.playback_eq().response_db(tape::kEqReferenceHz, fs),
                         WithinAbs(0.0, 1e-9));
        }
    }
}

TEST_CASE("Tape machine: record and playback EQ cascade flat",
          "[signal][tape-machine][eq]") {
    // R2, at every curve rather than only the one R1 measures — the degenerate
    // no-bass-shelf curves are exactly the ones whose inverse needed damping,
    // so they are the ones a flatness claim has to cover.
    for (const double fs : {kSr, kAltSr}) {
        for (const TapeCurve curve : kAllCurves) {
            TapeMachine machine;
            machine.set_archetype(curve == TapeCurve::cassette_type1 ||
                                          curve == TapeCurve::cassette_type2
                                      ? TapeArchetype::cassette_deck
                                      : TapeArchetype::studer_a800);
            machine.set_eq_curve(curve);
            machine.prepare(fs);

            for (int i = 0; i < 20; ++i) {
                const double hz =
                    20.0 * std::pow(15000.0 / 20.0, static_cast<double>(i) / 19.0);
                const double cascade = machine.record_eq().response_db(hz, fs) +
                                       machine.playback_eq().response_db(hz, fs);
                REQUIRE_THAT(cascade, WithinAbs(0.0, 0.3));
            }
        }
    }
}

TEST_CASE("Tape machine: the IEC reel curves are the ones that needed damping",
          "[signal][tape-machine][eq]") {
    // Documenting the trap rather than only its fix. A curve with no bass shelf
    // has a record network whose bilinear numerator is `(1 + z⁻¹)` — a zero AT
    // Nyquist — so the exact inverse would place a pole there. Assert the
    // structure the damping protects: the playback network's gain stays finite
    // and bounded right up to Nyquist.
    TapeMachine machine;
    machine.set_archetype(TapeArchetype::studer_a800);
    machine.set_eq_curve(TapeCurve::iec_ccir);
    machine.set_speed_ips(15.0);
    machine.prepare(kSr);

    REQUIRE_FALSE(tape::eq_time_constants(TapeCurve::iec_ccir, 15.0).has_bass_shelf());

    const double at_nyquist = machine.playback_eq().response_db(0.5 * kSr, kSr);
    REQUIRE(std::isfinite(at_nyquist));
    // The undamped inverse would be +inf here. The damping's ceiling is
    // `1/(1 − kInversePoleDamping)` in the pole term; 60 dB is a generous bound
    // on it and a tight one on "unbounded".
    REQUIRE(at_nyquist < 60.0);

    // NAB is a proper first-order pair and is left exact — no damping applied.
    TapeMachine nab;
    nab.set_archetype(TapeArchetype::studer_a800);
    nab.set_eq_curve(TapeCurve::nab);
    nab.set_speed_ips(15.0);
    nab.prepare(kSr);
    for (int i = 0; i < 20; ++i) {
        const double hz = 20.0 * std::pow(15000.0 / 20.0, static_cast<double>(i) / 19.0);
        REQUIRE_THAT(nab.record_eq().response_db(hz, kSr) +
                         nab.playback_eq().response_db(hz, kSr),
                     WithinAbs(0.0, 1e-9));
    }
}

TEST_CASE("Tape machine: the reproduce gap loss has a true extinction null",
          "[signal][tape-machine][gap]") {
    // R3. Predicted null and gap width both come from the shipped table.
    TapeMachine machine;
    machine.set_archetype(TapeArchetype::cassette_deck);
    machine.prepare(kGapSr);

    const double gap_m = machine.reproduce_gap_m();
    REQUIRE_THAT(gap_m, WithinAbs(0.8e-6, 1e-12));
    REQUIRE_THAT(machine.speed_ips(), WithinAbs(1.875, 1e-12));

    const double predicted_null = tape::gap_null_hz(gap_m, machine.speed_ips());
    // `v/g` with v = 1.875 ips: the spec's own worked example lands at 59.5 kHz.
    REQUIRE(predicted_null > 59000.0);
    REQUIRE(predicted_null < 60000.0);

    const auto& taps = machine.gap_fir();
    REQUIRE_FALSE(taps.empty());

    const double passband_db = fir_response_db(taps, 5000.0, kGapSr);
    double deepest = 1e9;
    double deepest_hz = 0.0;
    for (double hz = 0.6 * predicted_null; hz < 1.4 * predicted_null; hz += 25.0) {
        const double db = fir_response_db(taps, hz, kGapSr);
        if (db < deepest) {
            deepest = db;
            deepest_hz = hz;
        }
    }
    REQUIRE(passband_db - deepest >= 40.0);
    REQUIRE_THAT(deepest_hz, WithinAbs(predicted_null, 0.05 * predicted_null));

    // In-band, the realized minimum-phase FIR tracks Westmijze's closed form.
    // That is the part that is audible; the null is the part that proves the
    // filter is modelling extinction rather than a corner frequency.
    for (const double hz : {1000.0, 10000.0, 20000.0, 40000.0}) {
        const double realized = fir_response_db(taps, hz, kGapSr) - passband_db;
        const double analytic = tape::gap_loss_db(hz, gap_m, machine.speed_ips()) -
                                tape::gap_loss_db(5000.0, gap_m, machine.speed_ips());
        REQUIRE_THAT(realized, WithinAbs(analytic, 0.25));
    }

    // A studio head at 30 ips puts its null far ultrasonic, which is why gap
    // loss is a subtle darkening there and an extinction on cassette. Computed,
    // not asserted as a literal.
    const tape::HeadGapGeometry studio = tape::head_gap_geometry(30.0);
    REQUIRE(tape::gap_null_hz(studio.reproduce_m, 30.0) > 200000.0);
    REQUIRE(std::abs(tape::gap_loss_db(20000.0, studio.reproduce_m, 30.0)) < 0.2);
    // Record gaps run wider than reproduce gaps at every speed class.
    for (const double ips : {1.875, 7.5, 30.0}) {
        const tape::HeadGapGeometry g = tape::head_gap_geometry(ips);
        REQUIRE(g.record_m > g.reproduce_m);
    }
}

TEST_CASE("Tape machine: bias trades distortion against high-frequency output",
          "[signal][tape-machine][bias]") {
    // R4. The programme level is −20 dBFS rather than the −6 dBFS a "drive
    // test" invites, and that choice is load-bearing: at 0.5 peak the reused
    // Jiles-Atherton stage is already deep in saturation, the parabola's extra
    // drive buys almost no extra harmonic content, and the measured margin
    // between THD(0) and THD(+1) collapses to 1.1 %. At −20 dBFS the same
    // comparison has a 37 % margin. The criterion is about the SIGN of the
    // effect, so it is measured where the effect is legible.
    auto thd_at_bias = [](double bias) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::studer_a800);
        machine.prepare(kSr);
        quiesce(machine);
        machine.set_drive(0.5f);
        machine.set_bias(static_cast<float>(bias));
        const auto out = render_tone(machine, 1000.0, 0.1, static_cast<int>(kSr) * 2);
        return thd(out.left, 1000.0);
    };

    const double under = thd_at_bias(-1.0);
    const double optimum = thd_at_bias(0.0);
    const double over = thd_at_bias(+1.0);

    // A real, if shallow, minimum at optimum — the parabola of §4.2.
    REQUIRE(optimum < under);
    REQUIRE(optimum < over);

    // ...and the high-frequency side of the same trade, strictly monotone from
    // under-bias (brighter) to over-bias (darker).
    double previous = 1e9;
    for (const double bias : {-1.0, -0.5, 0.0, 0.5, 1.0}) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::studer_a800);
        machine.prepare(kSr);
        quiesce(machine);
        machine.set_bias(static_cast<float>(bias));
        const auto out = render_tone(machine, 10000.0, 0.05, static_cast<int>(kSr) * 2);
        const double db = 20.0 * std::log10(steady_rms(out.left) / (0.05 / std::sqrt(2.0)));
        REQUIRE(db < previous);
        previous = db;
    }
}

TEST_CASE("Tape machine: age drifts bias toward over-bias and raises print",
          "[signal][tape-machine][bias][age]") {
    // The two columns this module adds to the reused age table, asserted
    // against the shipped table rather than against remembered numbers.
    TapeMachine machine;
    machine.set_archetype(TapeArchetype::studer_a800);
    machine.prepare(kSr);
    machine.set_bias(0.0f);

    double previous_bias = -2.0;
    double previous_print = -200.0;
    for (const float age : {0.0f, 0.5f, 1.0f}) {
        machine.set_age(age);
        REQUIRE_THAT(machine.effective_bias(),
                     WithinAbs(tape::age_bias_drift(age), 1e-12));
        REQUIRE_THAT(machine.print_through_db(),
                     WithinAbs(tape::age_print_through_db(age), 1e-12));
        REQUIRE(machine.effective_bias() > previous_bias);
        REQUIRE(machine.print_through_db() > previous_print);
        previous_bias = machine.effective_bias();
        previous_print = machine.print_through_db();
    }

    // The drift ADDS to what the user asked for, and the sum is clamped rather
    // than allowed to run past the parameter's range.
    machine.set_age(1.0f);
    machine.set_bias(1.0f);
    REQUIRE_THAT(machine.effective_bias(), WithinAbs(TapeMachine::kBiasMax, 1e-12));
    machine.set_bias(-1.0f);
    REQUIRE_THAT(machine.effective_bias(),
                 WithinAbs(-1.0 + tape::age_bias_drift(1.0f), 1e-12));

    // The print-through parameter's declared bounds ARE the age table's
    // extremes, so no baked value can ask for a level the age axis cannot reach.
    REQUIRE_THAT(TapeMachine::kPrintThroughDbMin,
                 WithinAbs(tape::age_print_through_db(0.0), 1e-12));
    REQUIRE_THAT(TapeMachine::kPrintThroughDbMax,
                 WithinAbs(tape::age_print_through_db(1.0), 1e-12));
}

TEST_CASE("Tape machine: crosstalk leaks at the configured level",
          "[signal][tape-machine][crosstalk]") {
    // R5. One channel driven, the other silent; the leak is read at 1 kHz,
    // below the tilt corner.
    //
    // The tolerance is ±0.25 dB, not the ±1 dB the criterion allows, and that
    // is deliberate. The tilt shelf is normalized to unity at 1 kHz against the
    // REALIZED digital filter, so the measurement lands on the configured
    // figure exactly — and at ±1 dB, deleting that normalization entirely still
    // passes, because the un-normalized shelf's skirt at 1 kHz is only 0.47 dB.
    // A tolerance that admits the bug it exists to catch is not a test.
    for (const double configured : {-40.0, -30.0, -22.0}) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::studer_a800);
        machine.prepare(kSr);
        quiesce(machine);
        machine.set_crosstalk_db(static_cast<float>(configured));

        const int n = static_cast<int>(kSr) * 2;
        std::vector<float> in_l(static_cast<std::size_t>(n)),
            in_r(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> out_l(static_cast<std::size_t>(n)),
            out_r(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k)
            in_l[static_cast<std::size_t>(k)] =
                static_cast<float>(0.1 * std::sin(2.0 * M_PI * 1000.0 * k / kSr));
        machine.process(in_l.data(), in_r.data(), out_l.data(), out_r.data(), n);

        const double leak_db = 20.0 * std::log10(bin_magnitude(out_r, 1000.0) /
                                                 bin_magnitude(out_l, 1000.0));
        REQUIRE_THAT(leak_db, WithinAbs(configured, 0.25));
    }

    // The tilt rises with frequency and is shelf-limited, so the leak can never
    // exceed the configured figure by more than the declared ceiling at ANY
    // frequency — asserted against the realized digital shelf, evaluated on a
    // grid to Nyquist rather than at its asymptote.
    const double tilt = units::db_to_linear(TapeMachine::kCrosstalkTiltMaxDb);
    const double at_reference = tape::first_order_shelf_db(
        TapeMachine::kCrosstalkTiltHz, tilt, tape::kEqReferenceHz, kSr);
    double worst = -1e9;
    for (double hz = 20.0; hz < 0.5 * kSr; hz *= 1.05)
        worst = std::max(worst, tape::first_order_shelf_db(TapeMachine::kCrosstalkTiltHz,
                                                           tilt, hz, kSr) -
                                    at_reference);
    REQUIRE(worst <= TapeMachine::kCrosstalkTiltMaxDb);
    REQUIRE(worst > 0.0);  // it really does tilt up
}

TEST_CASE("Tape machine: a companded round trip is transparent",
          "[signal][tape-machine][compander]") {
    // R6, on the pair — see the header note on why it cannot be end-to-end.
    // Checked at the reference AND 20 dB either side, because "unity at the
    // reference" is trivially true of any gain law that happens to be 1 there;
    // what has to hold is that the 2:1 and 1:2 slopes are exact inverses
    // everywhere.
    for (const double dbr : {-20.0, 0.0, +20.0}) {
        TapeCompanderT<double> compander;
        compander.prepare(kSr);
        const double amplitude =
            units::db_to_linear(TapeCompanderT<double>::kRefLevelDbfs + dbr);

        const int n = static_cast<int>(kSr);
        double out_peak = 0.0, in_peak = 0.0;
        for (int k = 0; k < n; ++k) {
            const double x = amplitude * std::sin(2.0 * M_PI * 1000.0 * k / kSr);
            const double y = compander.decode(compander.encode(x));
            if (k > n / 2) {
                out_peak = std::max(out_peak, std::abs(y));
                in_peak = std::max(in_peak, std::abs(x));
            }
        }
        REQUIRE_THAT(20.0 * std::log10(out_peak / in_peak), WithinAbs(0.0, 0.1));
    }
}

TEST_CASE("Tape machine: companding buys signal-to-noise over the tape floor",
          "[signal][tape-machine][compander]") {
    // R7. The reused hiss generator at age 0.5 is a fixed, measurable floor;
    // the expander should push it down along with a quiet programme.
    auto snr_with = [](bool companding) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::cassette_deck);
        machine.prepare(kSr);
        quiesce(machine);
        machine.set_age(0.5f);
        machine.set_companding_enabled(companding);

        // −30 dBr, with 0 dBr = the compander's reference level.
        const double amplitude =
            units::db_to_linear(TapeCompanderT<float>::kRefLevelDbfs - 30.0);
        const auto out = render_tone(machine, kProbeHz, amplitude, static_cast<int>(kSr) * 3);
        const double signal = smeared_magnitude(out.left, kProbeHz);
        const double noise = band_noise(out.left, 3200.0, 3800.0, 150.0);
        return 20.0 * std::log10(signal / noise);
    };

    const double without = snr_with(false);
    const double with = snr_with(true);
    REQUIRE(with - without >= 6.0);
}

TEST_CASE("Tape machine: the post-echo print tap lands at the wrap offset",
          "[signal][tape-machine][print-through]") {
    // R8. Post-echo is the default and costs no latency beyond the module's
    // constant — nothing is delayed to make room for it.
    constexpr double kLevelDb = -40.0;
    constexpr double kOffsetMs = 300.0;
    const PrintProbe probe = probe_print_through(kLevelDb, kOffsetMs, /*pre_echo=*/false);

    TapeMachine reference;
    reference.set_archetype(TapeArchetype::studer_a800);
    reference.prepare(kSr);
    quiesce(reference);
    REQUIRE(probe.latency == reference.latency_samples());  // pre-echo adds nothing

    const auto [main_peak, main_at] = peak_of(probe.baseline);
    const auto [tap_peak, tap_at] = peak_of(probe.difference);

    REQUIRE(tap_at - main_at == probe.offset_samples);
    REQUIRE_THAT(20.0 * std::log10(tap_peak / main_peak), WithinAbs(kLevelDb, 0.5));
}

TEST_CASE("Tape machine: pre-echo arrives early and pays for it in latency",
          "[signal][tape-machine][print-through][latency]") {
    // R9. Both taps exist in this mode — post-echo never switches off — so the
    // pre tap is searched before the programme and the post tap after it.
    constexpr double kLevelDb = -40.0;
    constexpr double kOffsetMs = 300.0;
    const PrintProbe probe = probe_print_through(kLevelDb, kOffsetMs, /*pre_echo=*/true);

    TapeMachine baseline;
    baseline.set_archetype(TapeArchetype::studer_a800);
    baseline.prepare(kSr);
    quiesce(baseline);
    REQUIRE(probe.latency == baseline.latency_samples() + probe.offset_samples);

    const auto [main_peak, main_at] = peak_of(probe.baseline);
    const auto [pre_peak, pre_at] =
        peak_of(probe.difference, 0, main_at - probe.offset_samples / 2);
    const auto [post_peak, post_at] =
        peak_of(probe.difference, main_at + probe.offset_samples / 2);

    REQUIRE(main_at - pre_at == probe.offset_samples);
    REQUIRE(post_at - main_at == probe.offset_samples);
    REQUIRE_THAT(20.0 * std::log10(post_peak / main_peak), WithinAbs(kLevelDb, 0.5));
    // Pre-echo is documented as the weaker of the two in tails-out storage.
    REQUIRE_THAT(20.0 * std::log10(pre_peak / main_peak),
                 WithinAbs(kLevelDb - TapeMachine::kPreEchoAsymmetryDb, 0.5));
    REQUIRE(pre_peak < post_peak);
}

TEST_CASE("Tape machine: over-bias erases print-through",
          "[signal][tape-machine][print-through][bias]") {
    // §4.2's third consequence of the bias control, and the only one not
    // covered by R4: the erasure term is subtracted from the print level, and
    // only for over-bias.
    auto tap_level_db = [](double bias) {
        TapeMachine hot;
        hot.set_archetype(TapeArchetype::studer_a800);
        hot.prepare(kSr);
        quiesce(hot);
        hot.set_print_through(-40.0f, 300.0f, false);
        hot.set_bias(static_cast<float>(bias));
        const auto hot_out = render_impulse(hot, static_cast<int>(kSr));

        TapeMachine floor;
        floor.set_archetype(TapeArchetype::studer_a800);
        floor.prepare(kSr);
        quiesce(floor);
        floor.set_print_through(TapeMachine::kPrintThroughDbMin, 300.0f, false);
        floor.set_bias(static_cast<float>(bias));
        const auto floor_out = render_impulse(floor, static_cast<int>(kSr));

        std::vector<float> difference(hot_out.left.size());
        for (std::size_t k = 0; k < difference.size(); ++k)
            difference[k] = hot_out.left[k] - floor_out.left[k];
        return 20.0 * std::log10(peak_of(difference).first / peak_of(floor_out.left).first);
    };

    const double at_optimum = tap_level_db(0.0);
    const double at_over = tap_level_db(1.0);
    const double at_under = tap_level_db(-1.0);

    REQUIRE_THAT(at_over, WithinAbs(at_optimum - TapeMachine::kBiasErasureDb, 0.5));
    // Under-bias leaves the print un-erased: the term is `max(0, bias)`.
    REQUIRE_THAT(at_under, WithinAbs(at_optimum, 0.5));
}

TEST_CASE("Tape machine: cassette Type I and Type II diverge by their treble constant",
          "[signal][tape-machine][eq]") {
    // R10, with a correction worth stating. Measuring "the module's 8 kHz gain"
    // for each curve and differencing them yields ZERO, because the record and
    // playback networks are exact reciprocals and the linear path through the
    // insert is EQ-independent by construction. That is correct behaviour — it
    // is what "complementary networks" means — so the gain difference is
    // asserted where it exists, on the record network.
    const tape::EqTimeConstants type1 =
        tape::eq_time_constants(TapeCurve::cassette_type1, 1.875);
    const tape::EqTimeConstants type2 =
        tape::eq_time_constants(TapeCurve::cassette_type2, 1.875);
    REQUIRE_THAT(type1.treble_s, WithinAbs(120e-6, 1e-12));
    REQUIRE_THAT(type2.treble_s, WithinAbs(70e-6, 1e-12));
    // Both formulations keep the bass turnover; only the treble constant
    // switches with tape type.
    REQUIRE_THAT(type1.bass_s, WithinAbs(type2.bass_s, 1e-12));

    auto record_eq_db = [](TapeCurve curve, double hz) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::cassette_deck);
        machine.set_eq_curve(curve);
        machine.prepare(kSr);
        return machine.record_eq().response_db(hz, kSr);
    };

    const double analytic = tape::eq_record_response_db(type2, 8000.0) -
                            tape::eq_record_response_db(type1, 8000.0);
    const double realized = record_eq_db(TapeCurve::cassette_type2, 8000.0) -
                            record_eq_db(TapeCurve::cassette_type1, 8000.0);
    REQUIRE(analytic > 1.0);  // the curves really do differ at 8 kHz
    REQUIRE_THAT(realized, WithinAbs(analytic, 0.5));

    // ...and the divergence IS audible through the whole module, by the route
    // that actually carries it: the reproduce network shapes the tape's own
    // noise floor, and the two types shape it differently.
    auto noise_floor_db = [](TapeCurve curve) {
        TapeMachine machine;
        machine.set_archetype(TapeArchetype::cassette_deck);
        machine.set_eq_curve(curve);
        machine.prepare(kSr);
        quiesce(machine);
        machine.set_age(0.5f);
        const int n = static_cast<int>(kSr) * 2;
        std::vector<float> silence(static_cast<std::size_t>(n), 0.0f);
        std::vector<float> out_l(static_cast<std::size_t>(n)),
            out_r(static_cast<std::size_t>(n));
        machine.process(silence.data(), silence.data(), out_l.data(), out_r.data(), n);
        return 20.0 * std::log10(steady_rms(out_l));
    };
    REQUIRE(noise_floor_db(TapeCurve::cassette_type2) >
            noise_floor_db(TapeCurve::cassette_type1) + 0.25);
}

TEST_CASE("Tape machine: every archetype bakes its documented preset",
          "[signal][tape-machine][archetype]") {
    // R11, field by field against `tape::archetype_preset` — the table the DSP
    // reads, so a preset edit fails here rather than silently shipping.
    for (const TapeArchetype archetype : kAllArchetypes) {
        const tape::ArchetypePreset preset = tape::archetype_preset(archetype);
        TapeMachine machine;
        machine.set_archetype(archetype);
        machine.prepare(kSr);

        REQUIRE(machine.archetype() == archetype);
        REQUIRE_THAT(machine.speed_ips(), WithinAbs(preset.default_speed_ips, 1e-12));
        REQUIRE(machine.eq_curve() == preset.default_curve);
        REQUIRE_THAT(machine.crosstalk_db(), WithinAbs(preset.crosstalk_db, 1e-12));
        REQUIRE(machine.companding_enabled() == preset.companding);
        REQUIRE_THAT(machine.age(), WithinAbs(preset.age01, 1e-12));
        REQUIRE_THAT(machine.print_through_db(),
                     WithinAbs(tape::age_print_through_db(preset.age01), 1e-12));

        // Every legal speed is on the published ladder, and every one of them
        // is reachable.
        for (int i = 0; i < preset.legal_speed_count; ++i) {
            const double legal = preset.legal_speeds_ips[static_cast<std::size_t>(i)];
            REQUIRE(std::find(tape::kSpeedsIps.begin(), tape::kSpeedsIps.end(), legal) !=
                    tape::kSpeedsIps.end());
            machine.set_speed_ips(legal);
            REQUIRE_THAT(machine.speed_ips(), WithinAbs(legal, 1e-12));
        }
    }

    // A cassette deck never reports 30 ips no matter what it is asked for.
    TapeMachine cassette;
    cassette.set_archetype(TapeArchetype::cassette_deck);
    cassette.prepare(kSr);
    for (const double asked : {30.0, 15.0, 7.5, 3.75, 0.5}) {
        cassette.set_speed_ips(asked);
        REQUIRE_THAT(cassette.speed_ips(), WithinAbs(1.875, 1e-12));
    }

    // The EQ table is a per-speed lookup, not one fixed constant: a Studer told
    // to run IEC gets 70 / 35 / 17.5 µs at 7.5 / 15 / 30 ips.
    TapeMachine studer;
    studer.set_archetype(TapeArchetype::studer_a800);
    studer.set_eq_curve(TapeCurve::iec_ccir);
    studer.prepare(kSr);
    const double expected_us[] = {70.0, 35.0, 17.5};
    const double speeds[] = {7.5, 15.0, 30.0};
    for (int i = 0; i < 3; ++i) {
        studer.set_speed_ips(speeds[i]);
        const tape::EqTimeConstants tc =
            tape::eq_time_constants(TapeCurve::iec_ccir, studer.speed_ips());
        REQUIRE_THAT(tc.treble_s * 1e6, WithinAbs(expected_us[i], 1e-9));
        REQUIRE_FALSE(tc.has_bass_shelf());
    }
    // The corner the 15 ips row implies, stated the way §5's worked example
    // does. Computed from the table, not restated.
    REQUIRE_THAT(tape::eq_corner_hz(tape::eq_time_constants(TapeCurve::iec_ccir, 15.0).treble_s),
                 WithinAbs(4547.0, 1.0));

    // NAB drops its bass shelf at 30 ips and keeps it below — the structural
    // difference the "thin European tape" folklore actually comes from.
    REQUIRE(tape::eq_time_constants(TapeCurve::nab, 15.0).has_bass_shelf());
    REQUIRE_FALSE(tape::eq_time_constants(TapeCurve::nab, 30.0).has_bass_shelf());
    REQUIRE_FALSE(tape::eq_time_constants(TapeCurve::iec_ccir, 15.0).has_bass_shelf());
}

TEST_CASE("Tape machine: two renders from a reset are bit-identical",
          "[signal][tape-machine][determinism]") {
    // R12, series law 2. The chain under test carries every stochastic stage
    // the module has: the reused hiss generator, the chew state machine, the
    // wow drift's Ornstein-Uhlenbeck walk, and the compander's followers.
    auto render_twice = [](TapeMachine& machine, const std::vector<float>& in_l,
                           const std::vector<float>& in_r) {
        const auto n = static_cast<int>(in_l.size());
        std::vector<float> a(in_l.size()), b(in_l.size()), c(in_l.size()), d(in_l.size());
        machine.reset();
        machine.process(in_l.data(), in_r.data(), a.data(), b.data(), n);
        machine.reset();
        machine.process(in_l.data(), in_r.data(), c.data(), d.data(), n);
        for (std::size_t k = 0; k < a.size(); ++k) {
            REQUIRE(a[k] == c[k]);
            REQUIRE(b[k] == d[k]);
            REQUIRE(std::isfinite(a[k]));
            REQUIRE(std::isfinite(b[k]));
        }
    };

    const int n = 8192;
    chardelay::Xorshift32 rng(20260725u);
    std::vector<float> in_l(static_cast<std::size_t>(n)), in_r(static_cast<std::size_t>(n));
    for (int k = 0; k < n; ++k) {
        in_l[static_cast<std::size_t>(k)] = static_cast<float>(0.3 * rng.bipolar());
        in_r[static_cast<std::size_t>(k)] = static_cast<float>(0.3 * rng.bipolar());
    }

    for (const TapeArchetype archetype : kAllArchetypes) {
        TapeMachine machine;
        machine.set_archetype(archetype);
        machine.prepare(kSr);
        machine.set_age(0.5f);
        machine.set_companding_enabled(true);
        render_twice(machine, in_l, in_r);
    }

    // Two independently constructed instances agree too, which is the stronger
    // claim: no state survives from a previous render through a static.
    TapeMachine first, second;
    for (auto* machine : {&first, &second}) {
        machine->set_archetype(TapeArchetype::cassette_deck);
        machine->prepare(kSr);
        machine->set_age(0.5f);
    }
    std::vector<float> a(in_l.size()), b(in_l.size()), c(in_l.size()), d(in_l.size());
    first.process(in_l.data(), in_r.data(), a.data(), b.data(), n);
    second.process(in_l.data(), in_r.data(), c.data(), d.data(), n);
    for (std::size_t k = 0; k < a.size(); ++k) REQUIRE(a[k] == c[k]);

    // The two channels are seeded differently, so they must NOT be identical —
    // the failure mode a determinism test alone would happily pass.
    bool channels_differ = false;
    for (std::size_t k = 0; k < a.size(); ++k)
        if (a[k] != b[k]) channels_differ = true;
    REQUIRE(channels_differ);
}

TEST_CASE("Tape machine: the double instantiation renders and stays finite",
          "[signal][tape-machine][determinism]") {
    TapeMachine64 machine;
    machine.set_archetype(TapeArchetype::studer_a800);
    machine.prepare(kSr);
    machine.set_age(0.5f);
    machine.set_companding_enabled(true);

    const int n = 4096;
    std::vector<double> in_l(static_cast<std::size_t>(n)),
        in_r(static_cast<std::size_t>(n)), a(static_cast<std::size_t>(n)),
        b(static_cast<std::size_t>(n)), c(static_cast<std::size_t>(n)),
        d(static_cast<std::size_t>(n));
    chardelay::Xorshift32 rng(20260725u);
    for (int k = 0; k < n; ++k) {
        in_l[static_cast<std::size_t>(k)] = 0.3 * rng.bipolar();
        in_r[static_cast<std::size_t>(k)] = 0.3 * rng.bipolar();
    }

    machine.reset();
    machine.process(in_l.data(), in_r.data(), a.data(), b.data(), n);
    machine.reset();
    machine.process(in_l.data(), in_r.data(), c.data(), d.data(), n);
    for (std::size_t k = 0; k < a.size(); ++k) {
        REQUIRE(a[k] == c[k]);
        REQUIRE(std::isfinite(a[k]));
    }
}

TEST_CASE("Tape machine: process and reset allocate nothing",
          "[signal][tape-machine][rt-safety]") {
    // R13, across every archetype, both print-through modes, and both sample
    // types. The buffers and the machine are built before the probe opens, and
    // the machine is warmed first so no lazily-touched path allocates inside it.
    for (const TapeArchetype archetype : kAllArchetypes) {
        for (const bool pre_echo : {false, true}) {
            TapeMachine machine;
            machine.set_archetype(archetype);
            machine.prepare(kSr);
            machine.set_print_through(-50.0f, 700.0f, pre_echo);
            machine.set_companding_enabled(true);
            Driver<TapeMachine, float> driver(512);
            driver.run(machine);

            pulp::test::RtAllocationProbe probe;
            for (int block = 0; block < 8; ++block) {
                machine.set_bias(0.1f * static_cast<float>(block) - 0.4f);
                machine.set_drive(0.05f * static_cast<float>(block));
                machine.set_age(0.05f * static_cast<float>(block));
                machine.set_crosstalk_db(-40.0f + static_cast<float>(block));
                machine.set_eq_curve(block % 2 ? TapeCurve::nab : TapeCurve::iec_ccir);
                machine.set_print_through(-50.0f + static_cast<float>(block), 700.0f,
                                          pre_echo);
                machine.set_companding_enabled(block % 2 == 0);
                machine.set_mix(0.1f * static_cast<float>(block));
                driver.run(machine);
                machine.reset();
            }
            REQUIRE(probe.allocation_count() == 0);
        }
    }

    TapeMachine64 wide;
    wide.set_archetype(TapeArchetype::cassette_deck);
    wide.prepare(kSr);
    Driver<TapeMachine64, double> wide_driver(512);
    wide_driver.run(wide);
    {
        pulp::test::RtAllocationProbe probe;
        for (int block = 0; block < 8; ++block) {
            wide.set_bias(0.1f * static_cast<float>(block));
            wide_driver.run(wide);
            wide.reset();
        }
        REQUIRE(probe.allocation_count() == 0);
    }
}

TEST_CASE("Tape machine: a speed change is control-thread work, and says so",
          "[signal][tape-machine][rt-safety]") {
    // The one documented exception to the RT contract, asserted rather than
    // assumed. The reused physical tier redesigns its speed-dependent
    // minimum-phase loss FIR when the speed moves, and that allocates. Pinning
    // it here means a future change that quietly makes `set_speed_ips`
    // RT-clean, or quietly makes another setter dirty, shows up as a failing
    // test instead of as a dropout in somebody's session.
    TapeMachine machine;
    machine.set_archetype(TapeArchetype::studer_a800);
    machine.prepare(kSr);
    machine.set_speed_ips(15.0);
    Driver<TapeMachine, float> driver(256);
    driver.run(machine);

    {
        pulp::test::RtAllocationProbe probe;
        machine.set_speed_ips(30.0);
        REQUIRE(probe.allocation_count() > 0);
    }
    // ...and a speed change that is a no-op does not.
    {
        pulp::test::RtAllocationProbe probe;
        machine.set_speed_ips(30.0);
        REQUIRE(probe.allocation_count() == 0);
    }
}
