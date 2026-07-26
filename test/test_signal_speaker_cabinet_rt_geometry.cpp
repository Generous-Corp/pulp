#include "test_signal_speaker_cabinet_support.hpp"

TEST_CASE("AT-9 latency is zero", "[signal][speaker][acceptance]") {
    SpeakerModel64 model;
    sealed_reference(model);
    REQUIRE(model.latency_samples() == 0);

    // A minimum-phase cascade with no block buffering: the first output sample
    // responds to the first input sample.
    std::vector<double> h(64, 0.0);
    h[0] = 1.0;
    model.process(h.data(), h.data(), 64);
    REQUIRE(h[0] != 0.0);

    // True at every setting, including the ones that add filters to the chain.
    SpeakerModel64 loaded;
    neutralise(loaded);
    loaded.set_box_type(SpeakerBoxType::open_back);
    loaded.set_cone_breakup_amount(100.0);
    loaded.set_diffraction_amount(100.0);
    loaded.set_compression_amount(100.0);
    loaded.set_drive_db(SpeakerModel64::kDriveDbMax);
    loaded.prepare(kFs);
    REQUIRE(loaded.latency_samples() == 0);
    std::vector<double> h2(64, 0.0);
    h2[0] = 1.0;
    loaded.process(h2.data(), h2.data(), 64);
    REQUIRE(h2[0] != 0.0);
}

TEST_CASE("AT-10 renders are bit-identical across reset", "[signal][speaker][acceptance]") {
    // Series law 2. There is no RNG in this module, so determinism is
    // structural — this guards against state that survives `reset()`.
    SpeakerModel model;  // float, the shipping type
    model.set_driver_archetype(2);
    model.set_box_type(SpeakerBoxType::open_back);
    model.set_compression_amount(100.0);
    model.set_drive_db(9.0);
    model.set_cone_breakup_amount(100.0);
    model.set_diffraction_amount(75.0);
    model.set_mic_distance_cm(4.0);
    model.set_mic_position_pct(20.0);
    model.set_mic_axis_deg(15.0);
    model.prepare(kFs);

    const int n = static_cast<int>(5.0 * kFs);  // 5 s of program, per AT-10
    std::vector<float> input(static_cast<std::size_t>(n));
    std::uint32_t state = 0x5EED;
    for (auto& v : input) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        v = static_cast<float>((static_cast<double>(state) / 4294967295.0) * 2.0 - 1.0) * 0.5f;
    }

    std::vector<float> first(static_cast<std::size_t>(n)), second(static_cast<std::size_t>(n));
    model.process(input.data(), first.data(), n);
    model.reset();
    model.process(input.data(), second.data(), n);
    REQUIRE(first == second);

    // And a freshly constructed instance with the same settings agrees, so
    // "zero-initialised is a valid fresh instance" is more than a comment.
    SpeakerModel fresh;
    fresh.set_driver_archetype(2);
    fresh.set_box_type(SpeakerBoxType::open_back);
    fresh.set_compression_amount(100.0);
    fresh.set_drive_db(9.0);
    fresh.set_cone_breakup_amount(100.0);
    fresh.set_diffraction_amount(75.0);
    fresh.set_mic_distance_cm(4.0);
    fresh.set_mic_position_pct(20.0);
    fresh.set_mic_axis_deg(15.0);
    fresh.prepare(kFs);
    std::vector<float> third(static_cast<std::size_t>(n));
    fresh.process(input.data(), third.data(), n);
    REQUIRE(first == third);
}

TEST_CASE("AT-11 process and reset are allocation-free", "[signal][speaker][acceptance][rt]") {
    SpeakerModel model;
    model.set_compression_amount(100.0);
    model.set_drive_db(6.0);
    model.prepare(kFs);

    constexpr int kBlock = 128;
    const int blocks = static_cast<int>(10.0 * kFs) / kBlock;  // 10 s, per AT-11
    std::vector<float> in(kBlock), out(kBlock);
    std::uint32_t state = 0xA11;

    std::size_t allocations = 0;
    {
        // Nothing but the module runs inside this scope: a Catch2 INFO here
        // would allocate its own message buffer and be counted.
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < blocks; ++b) {
            for (int i = 0; i < kBlock; ++i) {
                state ^= state << 13;
                state ^= state >> 17;
                state ^= state << 5;
                in[static_cast<std::size_t>(i)] =
                    static_cast<float>((static_cast<double>(state) / 4294967295.0) * 2.0 - 1.0) *
                    0.5f;
            }
            // Sweep every setter across the block, including the ones that
            // recompute coefficients.
            const double t = static_cast<double>(b) / blocks;
            model.set_drive_db(SpeakerModel::kDriveDbMin +
                               t * (SpeakerModel::kDriveDbMax - SpeakerModel::kDriveDbMin));
            model.set_treble_rolloff_hz(SpeakerModel::kTrebleRolloffHzMin +
                                        t * (SpeakerModel::kTrebleRolloffHzMax -
                                             SpeakerModel::kTrebleRolloffHzMin));
            model.set_mic_distance_cm(SpeakerModel::kMicDistanceCmMin +
                                      t * (SpeakerModel::kMicDistanceCmMax -
                                           SpeakerModel::kMicDistanceCmMin));
            model.set_mic_position_pct(100.0 * t);
            model.set_mic_axis_deg(90.0 * t);
            model.set_cone_breakup_amount(100.0 * t);
            model.set_diffraction_amount(100.0 * (1.0 - t));
            model.set_compression_amount(100.0 * t);
            model.set_q_resonance(SpeakerModel::kQResonanceMin +
                                  t * (SpeakerModel::kQResonanceMax - SpeakerModel::kQResonanceMin));
            model.set_box_volume_l(SpeakerModel::kBoxVolumeLMin +
                                   t * (SpeakerModel::kBoxVolumeLMax - SpeakerModel::kBoxVolumeLMin));
            model.set_box_type(b & 1 ? SpeakerBoxType::open_back : SpeakerBoxType::sealed);
            model.set_driver_archetype(b % SpeakerModel::kArchetypeCount);
            model.set_output_trim_db(-6.0 + 12.0 * t);
            model.set_resonance_trim_semitones(-12.0 + 24.0 * t);
            model.process(in.data(), out.data(), kBlock);
            if ((b % 500) == 0) model.reset();
        }
        allocations = probe.allocation_count();
    }
    INFO("allocations during process/reset: " << allocations);
    REQUIRE(allocations == 0);

    // The sweep must not have produced anything non-finite either.
    for (float v : out) REQUIRE(std::isfinite(v));
}

TEST_CASE("AT-12 parameter steps do not zipper", "[signal][speaker][acceptance]") {
    // A zipper is a DISCONTINUITY, so the measurement is the largest
    // sample-to-sample step, and it only means something against a matched
    // control where the parameter was never touched — see deviation 6.
    constexpr int kN = 16384;
    const int step_at = kN / 2;
    const double f0 = kFs * 64 / kN;

    auto render = [&](bool step) {
        SpeakerModel64 model;
        sealed_reference(model);
        model.set_treble_rolloff_hz(4000.0);
        model.prepare(kFs);
        std::vector<double> y(kN);
        for (int i = 0; i < kN; ++i) {
            if (step && i == step_at) model.set_treble_rolloff_hz(2000.0);
            y[static_cast<std::size_t>(i)] =
                model.process(0.5 * std::sin(2.0 * kPi * f0 * i / kFs));
        }
        return y;
    };

    const auto stepped = render(true);
    const auto control = render(false);

    // Window covering the smoothing ramp: kSmoothingMs at kFs, plus a margin.
    const int window = static_cast<int>(2.0 * SpeakerModel64::kSmoothingMs * kFs / 1000.0);
    auto largest_step = [&](const std::vector<double>& y) {
        double worst = 0.0;
        for (int i = step_at + 1; i < step_at + window; ++i)
            worst = std::max(worst, std::abs(y[static_cast<std::size_t>(i)] -
                                             y[static_cast<std::size_t>(i - 1)]));
        return worst;
    };

    const double excess = amplitude_db(largest_step(stepped)) - amplitude_db(largest_step(control));
    INFO("largest sample-to-sample step exceeds the no-step control by " << excess << " dB");
    REQUIRE(excess < 0.1);

    // The parameter did actually move, or the test proves nothing.
    SpeakerModel64 before, after;
    sealed_reference(before);
    sealed_reference(after);
    before.set_treble_rolloff_hz(4000.0);
    after.set_treble_rolloff_hz(2000.0);
    before.prepare(kFs);
    after.prepare(kFs);
    REQUIRE(before.inductance_magnitude_db(4000.0) - after.inductance_magnitude_db(4000.0) > 3.0);
}

TEST_CASE("AT-13 breakup bank is scale-invariant", "[signal][speaker][acceptance]") {
    // Series law 7: never interpolate independently fitted coefficient sets;
    // find the dimensionless shape. All archetypes share ONE mode table and
    // differ only in the anchor frequency, so archetype 3's bank must be
    // archetype 0's shifted by the ratio of the anchors.
    const double anchor0 = SpeakerModel64::archetype(0).f_breakup_hz;
    const double anchor3 = SpeakerModel64::archetype(3).f_breakup_hz;
    const double scale = anchor3 / anchor0;

    // Isolated by ratio: with breakup at 100 % versus 0 %, only the bank's
    // coefficients differ, so everything else cancels exactly.
    auto bank_response = [](int archetype) {
        SpeakerModel64 on, off;
        neutralise(on);
        neutralise(off);
        on.set_driver_archetype(archetype);
        off.set_driver_archetype(archetype);
        on.set_box_type(SpeakerBoxType::sealed);
        off.set_box_type(SpeakerBoxType::sealed);
        on.set_cone_breakup_amount(100.0);
        off.set_cone_breakup_amount(0.0);
        on.prepare(kFs);
        off.prepare(kFs);
        const auto h_on = impulse_response(on);
        const auto h_off = impulse_response(off);
        return [h_on, h_off](double hz) { return response_db(h_on, hz) - response_db(h_off, hz); };
    };

    const auto bank0 = bank_response(0);
    const auto bank3 = bank_response(3);

    double worst = 0.0, worst_ratio = 0.0;
    // Across the whole modal region, from below the first mode to above the
    // last, in the DIMENSIONLESS coordinate the shape is defined in.
    for (double ratio = 0.5; ratio <= 3.6; ratio *= 1.02) {
        const double diff = bank0(anchor0 * ratio) - bank3(anchor3 * ratio);
        if (std::abs(diff) > worst) { worst = std::abs(diff); worst_ratio = ratio; }
    }
    INFO("worst deviation " << worst << " dB at ratio " << worst_ratio << " (scale " << scale
                            << ")");
    REQUIRE(worst < 0.5);

    // Each mode sits where the shared table says, anchored per archetype.
    for (int archetype : {0, 3}) {
        SpeakerModel64 model;
        neutralise(model);
        model.set_driver_archetype(archetype);
        model.prepare(kFs);
        for (int mode = 0; mode < SpeakerModel64::kBreakupModeCount; ++mode) {
            const double expected = SpeakerModel64::archetype(archetype).f_breakup_hz *
                                    SpeakerModel64::breakup_mode(mode).ratio;
            REQUIRE_THAT(model.breakup_mode_hz(mode), Catch::Matchers::WithinRel(expected, 1e-12));
        }
    }

    // And the bank really is doing something: the shipped shape has a +4 dB
    // peak at the anchor and a -4 dB dip at ratio 2.20, which is the dip a
    // parallel resonator bank could not produce (see the header's note on why
    // this is peaking biquads rather than ModalBankT).
    REQUIRE(bank0(anchor0) > 3.0);
    REQUIRE(bank0(anchor0 * SpeakerModel64::breakup_mode(2).ratio) < -3.0);
    REQUIRE(SpeakerModel64::breakup_mode(2).gain_db < 0.0);
}

TEST_CASE("Cabinet geometry follows the baffle", "[signal][speaker][cabinet]") {
    // The baffle step is derived from the archetype's width, not tuned: a
    // narrower baffle steps higher.
    SpeakerModel64 wide, narrow;
    neutralise(wide);
    neutralise(narrow);
    wide.set_driver_archetype(4);  // Bass-15, 0.60 m
    narrow.set_driver_archetype(3);  // Brit-10, 0.42 m
    wide.prepare(kFs);
    narrow.prepare(kFs);

    const double expected_wide = SpeakerModel64::kSpeedOfSoundMs /
                                 (kPi * SpeakerModel64::archetype(4).baffle_width_m);
    REQUIRE_THAT(wide.baffle_step_hz(), Catch::Matchers::WithinRel(expected_wide, 1e-12));
    REQUIRE(narrow.baffle_step_hz() > wide.baffle_step_hz());

    // The step is a +6 dB transition scaled by `diffraction_amount`, so the
    // parameter has to actually scale it.
    SpeakerModel64 full, none;
    neutralise(full);
    neutralise(none);
    full.set_diffraction_amount(100.0);
    none.set_diffraction_amount(0.0);
    full.prepare(kFs);
    none.prepare(kFs);
    const auto h_full = impulse_response(full);
    const auto h_none = impulse_response(none);
    // Well above the step, where the shelf is on its plateau and the ripple
    // sections have rolled off, the difference approaches the full +6 dB.
    const double step = response_db(h_full, 8000.0) - response_db(h_none, 8000.0);
    INFO("baffle step at the plateau = " << step << " dB against a nominal "
                                         << SpeakerModel64::kBaffleStepDb);
    REQUIRE(step > 0.9 * SpeakerModel64::kBaffleStepDb);
    REQUIRE(step <= SpeakerModel64::kBaffleStepDb + 1e-9);
    // And well below it there is no step.
    REQUIRE(std::abs(response_db(h_full, 30.0) - response_db(h_none, 30.0)) < 0.5);
}

TEST_CASE("Box volume moves the resonance the way the physics says",
          "[signal][speaker][cabinet]") {
    // A smaller box traps stiffer air: alpha rises, so fc and Qtc both rise as
    // sqrt(1 + alpha). This is the whole reason the box is a parameter.
    double previous_fc = 0.0;
    for (double volume : {SpeakerModel64::kBoxVolumeLMax, 90.0, 28.0,
                          SpeakerModel64::kBoxVolumeLMin}) {
        SpeakerModel64 model;
        neutralise(model);
        model.set_box_type(SpeakerBoxType::sealed);
        model.set_box_volume_l(volume);
        model.prepare(kFs);
        const double alpha = SpeakerModel64::archetype(0).vas_litres / volume;
        const double expected_fc = SpeakerModel64::archetype(0).fs_hz * std::sqrt(1.0 + alpha);
        REQUIRE_THAT(model.resonance_fc_hz(), Catch::Matchers::WithinRel(expected_fc, 1e-12));
        REQUIRE(model.resonance_fc_hz() > previous_fc);
        previous_fc = model.resonance_fc_hz();
    }

    // The voicing trim shifts fc by whole semitones without touching Q.
    SpeakerModel64 model;
    sealed_reference(model);
    const double base_fc = model.resonance_fc_hz();
    const double base_q = model.resonance_q();
    model.set_resonance_trim_semitones(12.0);
    REQUIRE_THAT(model.resonance_fc_hz(), Catch::Matchers::WithinRel(2.0 * base_fc, 1e-12));
    REQUIRE_THAT(model.resonance_q(), Catch::Matchers::WithinRel(base_q, 1e-12));
    model.set_resonance_trim_semitones(-12.0);
    REQUIRE_THAT(model.resonance_fc_hz(), Catch::Matchers::WithinRel(0.5 * base_fc, 1e-12));

    // The Q override replaces the computed value and is clamped to its range.
    model.set_resonance_trim_semitones(0.0);
    model.set_q_resonance(SpeakerModel64::kQResonanceMax);
    REQUIRE_THAT(model.resonance_q(),
                 Catch::Matchers::WithinRel(SpeakerModel64::kQResonanceMax, 1e-12));
    model.set_q_resonance(10.0);
    REQUIRE_THAT(model.resonance_q(),
                 Catch::Matchers::WithinRel(SpeakerModel64::kQResonanceMax, 1e-12));
    model.set_q_resonance(0.0);  // back to computed
    REQUIRE_THAT(model.resonance_q(), Catch::Matchers::WithinRel(base_q, 1e-12));
}

TEST_CASE("DC blocker sits below every archetype resonance", "[signal][speaker][cabinet]") {
    // A DC blocker's job is removing DC, not shaping the low end. `DcBlocker`'s
    // own default pole of 0.995 is a ~38 Hz corner at 48 kHz, which would sit
    // directly on the Bass-15's 45 Hz free-air resonance and steepen the
    // low-end asymptote that AT-1 measures. The module sets the pole
    // explicitly; this asserts the separation it buys.
    double lowest_fs = 1e9;
    for (int i = 0; i < SpeakerModel64::kArchetypeCount; ++i)
        lowest_fs = std::min(lowest_fs, SpeakerModel64::archetype(i).fs_hz);
    INFO("lowest archetype fs " << lowest_fs << " Hz vs DC corner "
                                << SpeakerModel64::kDcBlockerHz << " Hz");
    REQUIRE(SpeakerModel64::kDcBlockerHz < lowest_fs / 4.0);

    // Measured: the open-back Bass-15 keeps the driver's own +12 dB/oct
    // asymptote an octave below its resonance, which it could not do if the
    // blocker were adding a third pole there.
    SpeakerModel64 model;
    neutralise(model);
    model.set_driver_archetype(4);
    model.set_box_type(SpeakerBoxType::open_back);
    model.prepare(kFs);
    const auto h = impulse_response(model);
    const double fc = model.resonance_fc_hz();
    // Below the dipole corner the open-back adds its own first-order high-pass,
    // so the expected asymptote there is 12 + 6 = 18 dB/oct.
    const double slope = response_db(h, 0.25 * fc) - response_db(h, 0.125 * fc);
    INFO("Bass-15 open-back slope below fc = " << slope << " dB/oct");
    REQUIRE(slope > 16.0);
    REQUIRE(slope < 20.0);
}

TEST_CASE("Degenerate and default states are safe", "[signal][speaker]") {
    // A default-constructed, never-prepared instance must not emit NaN: the
    // spec's "zero-initialised state is a valid, silent-safe fresh instance".
    SpeakerModel64 raw;
    std::vector<double> buffer(256, 0.5);
    raw.process(buffer.data(), buffer.data(), 256);
    for (double v : buffer) REQUIRE(std::isfinite(v));
    REQUIRE(raw.latency_samples() == 0);

    // Every parameter clamps rather than propagating a nonsense value.
    SpeakerModel64 model;
    model.prepare(kFs);
    model.set_driver_archetype(-5);
    REQUIRE(model.archetype_index() == 0);
    model.set_driver_archetype(99);
    REQUIRE(model.archetype_index() == SpeakerModel64::kArchetypeCount - 1);
    model.set_mic_distance_cm(-1.0);
    REQUIRE(model.proximity_gain_db() == SpeakerModel64::kProximityCeilingDb);
    model.set_mic_axis_deg(1000.0);
    REQUIRE(model.offaxis_corner_hz() > 0.0);
    // Volume only has meaning in a sealed box: open-back vents the rear wave,
    // so alpha is 0 there whatever the volume says.
    model.set_box_type(SpeakerBoxType::sealed);
    model.set_box_volume_l(0.0);
    const double clamped =
        SpeakerModel64::archetype(model.archetype_index()).vas_litres /
        SpeakerModel64::kBoxVolumeLMin;
    REQUIRE_THAT(model.compliance_ratio(), Catch::Matchers::WithinRel(clamped, 1e-12));
    model.set_box_type(SpeakerBoxType::open_back);
    REQUIRE(model.compliance_ratio() == 0.0);

    // Hard drive into a hot input stays finite and bounded.
    model.set_driver_archetype(4);
    model.set_box_type(SpeakerBoxType::sealed);
    model.set_drive_db(SpeakerModel64::kDriveDbMax);
    model.set_compression_amount(100.0);
    model.set_output_trim_db(SpeakerModel64::kOutputTrimDbMax);
    model.prepare(kFs);
    std::vector<double> hot(4096);
    for (std::size_t i = 0; i < hot.size(); ++i)
        hot[i] = (i % 2 == 0) ? 1.0 : -1.0;  // full-scale square, the worst case
    model.process(hot.data(), hot.data(), static_cast<int>(hot.size()));
    for (double v : hot) REQUIRE(std::isfinite(v));
}

TEST_CASE("Speaker rejects non-finite controls and audio without poisoning state",
          "[signal][speaker][nan-recovery][rt-safety]") {
    SpeakerModel64 poisoned;
    SpeakerModel64 fresh;
    poisoned.prepare(kFs);
    fresh.prepare(kFs);
    poisoned.set_drive_db(6.0);
    fresh.set_drive_db(6.0);
    poisoned.set_drive_db(std::numeric_limits<double>::quiet_NaN());
    poisoned.set_box_volume_l(std::numeric_limits<double>::infinity());

    double rejected = 1.0;
    {
        pulp::test::RtAllocationProbe probe;
        rejected = poisoned.process(std::numeric_limits<double>::quiet_NaN());
        REQUIRE(probe.allocation_count() == 0);
    }
    REQUIRE(rejected == 0.0);
    fresh.reset();

    std::vector<double> in(2048), a(in.size()), b(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) in[i] = 0.2 * std::sin(0.01 * i);
    poisoned.process(in.data(), a.data(), static_cast<int>(a.size()));
    fresh.process(in.data(), b.data(), static_cast<int>(b.size()));
    REQUIRE(a == b);
}

TEST_CASE("Float and double instantiations agree", "[signal][speaker]") {
    // The shipping type is float; the measurements above run in double. They
    // have to describe the same filter or the acceptance numbers do not apply
    // to what ships.
    SpeakerModel single;
    SpeakerModel64 twin;
    for (auto* configure : {+[](SpeakerModel& m) { m.set_compression_amount(0.0); }}) {
        (void)configure;
    }
    single.set_compression_amount(0.0);
    twin.set_compression_amount(0.0);
    single.set_box_type(SpeakerBoxType::sealed);
    twin.set_box_type(SpeakerBoxType::sealed);
    single.prepare(kFs);
    twin.prepare(kFs);

    REQUIRE_THAT(static_cast<double>(single.resonance_fc_hz()),
                 Catch::Matchers::WithinRel(twin.resonance_fc_hz(), 1e-12));

    constexpr int kN = 8192;
    std::vector<float> in_f(kN), out_f(kN);
    std::vector<double> in_d(kN), out_d(kN);
    std::uint32_t state = 0xFAB;
    for (int i = 0; i < kN; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const double v = ((static_cast<double>(state) / 4294967295.0) * 2.0 - 1.0) * 0.25;
        in_f[static_cast<std::size_t>(i)] = static_cast<float>(v);
        in_d[static_cast<std::size_t>(i)] = static_cast<double>(in_f[static_cast<std::size_t>(i)]);
    }
    single.process(in_f.data(), out_f.data(), kN);
    twin.process(in_d.data(), out_d.data(), kN);

    double worst = 0.0;
    for (int i = 0; i < kN; ++i)
        worst = std::max(worst, std::abs(static_cast<double>(out_f[static_cast<std::size_t>(i)]) -
                                         out_d[static_cast<std::size_t>(i)]));
    INFO("worst float-vs-double sample difference " << amplitude_db(worst) << " dBFS");
    REQUIRE(amplitude_db(worst) < -100.0);
}
