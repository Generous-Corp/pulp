#include "harness/leslie_test_support.hpp"

#include <numbers>

// ── 1. Doppler ────────────────────────────────────────────────────────────

TEST_CASE("the rotating source produces the Doppler depth its geometry implies",
          "[leslie][doppler]") {
    // The spec's compact figure is the FAR-FIELD limit `r*omega/c`. The shipped
    // model solves the exact law of cosines instead, so the two only have to
    // agree where the mic is far compared with the radius — which is where the
    // defaults sit. Checking them against each other is a check on the physics;
    // checking the render against the exact one is a check on the code.
    Leslie l = make_bare_leslie();
    const double far_field =
        Leslie::kHornRadiusM * kTwoPi * Leslie::kHornFastHz / 343.0;
    const double exact = geometric_doppler_ratio(l, Leslie::kHornFastHz, false);
    REQUIRE_THAT(exact, WithinRel(far_field, 0.01));

    // A 4 kHz carrier, not the 1 kHz the spec's recipe names. At 1 kHz an
    // 800 Hz LR4 crossover still passes the drum band at −11 dB, so a
    // "horn-only" measurement there is a tenth drum — and the drum runs at a
    // different rate and radius, which is exactly what would corrupt this. Two
    // octaves up the drum band is below −55 dB and the horn is alone.
    constexpr double kCarrierHz = 4000.0;
    const auto rendered = render_tone(l, kCarrierHz, 0.5, 3.0, Channel::left);
    const auto d = demodulate(rendered, kCarrierHz, kSr, 400.0, 48);
    const double measured = peak_deviation(d.freq_hz, kCarrierHz) / kCarrierHz;

    REQUIRE_THAT(measured, WithinRel(exact, 0.05));

    // The same radius at the chorale rate is an order of magnitude shallower —
    // the whole reason the two speeds are voiced by rate alone and never by a
    // second depth curve (series law 7).
    Leslie slow = make_bare_leslie(LeslieSpeed::chorale);
    const double slow_exact = geometric_doppler_ratio(slow, Leslie::kHornSlowHz, false);
    REQUIRE(slow_exact < exact * 0.2);
    REQUIRE_THAT(slow_exact / exact,
                 WithinRel(Leslie::kHornSlowHz / Leslie::kHornFastHz, 0.01));

    // The drum is deliberately shallower than the horn at the same speed
    // setting: smaller radius AND lower rate.
    const double drum_exact = geometric_doppler_ratio(l, Leslie::kDrumFastHz, true);
    REQUIRE(drum_exact < exact);
}

// ── 2. Rotor rate ─────────────────────────────────────────────────────────

TEST_CASE("the horn's tremolo lands exactly on the configured rate", "[leslie][rate]") {
    // The amplitude modulation is locked to the horn's phase, so measuring the
    // envelope's rate measures the phase accumulator through the whole audio
    // path. Resolution comes from the scan grid rather than from an FFT bin, so
    // 30 s is enough to resolve far below the 0.1 % the spec asks for.
    Leslie l = make_leslie();
    const auto rendered = render_tone(l, 4000.0, 0.5, 30.0, Channel::left);
    const auto d = demodulate(rendered, 4000.0, kSr, 400.0, 48);
    const auto env = remove_mean(d.envelope);

    const double measured = locate_peak(env, Leslie::kHornFastHz * 0.98,
                                        Leslie::kHornFastHz * 1.02, d.rate_hz, 4000);
    REQUIRE_THAT(measured, WithinRel(Leslie::kHornFastHz, 0.001));
}

TEST_CASE("the two rotors beat instead of locking together", "[leslie][rate]") {
    // The signature of the class. Both rotors modulate the same mic, at rates
    // that differ on purpose, so the mic sum's envelope carries their
    // difference frequency. A model that shared one LFO between the rotors
    // would show nothing here.
    Leslie l = make_leslie();
    const auto rendered = render_tone(l, 1000.0, 0.5, 40.0, Channel::sum);
    const auto d = demodulate(rendered, 1000.0, kSr, 400.0, 48);
    const auto env = remove_mean(d.envelope);

    const double expected = std::abs(Leslie::kHornFastHz - Leslie::kDrumFastHz);
    const double measured = locate_peak(env, expected * 0.7, expected * 1.3, d.rate_hz, 3000);
    REQUIRE_THAT(measured, WithinRel(expected, 0.05));

    // Not a vacuous find: the beat is a real component, not the scan settling
    // on noise at the edge of its window.
    REQUIRE(std::abs(coherent(env, measured, d.rate_hz, 0)) >
            10.0 * std::abs(coherent(env, expected * 0.55, d.rate_hz, 0)));
}

// ── 3. Inertia — the gesture ──────────────────────────────────────────────

TEST_CASE("the horn reaches a new speed before the drum does", "[leslie][inertia]") {
    // THE behaviour. A speed change is not one ramp, it is two of different
    // lengths, and the gap between them is what a listener hears as the swirl
    // rising and the low end smearing in behind it.
    //
    // Measured from the audio in each band separately: a 4 kHz carrier sits in
    // the horn band and a 150 Hz carrier in the drum band, so each render's
    // envelope carries exactly one rotor's rate. The settling time is then the
    // last moment that envelope's own cycle rate was still outside 5 % of its
    // new target.
    //
    // The directivity shelf is switched off so the envelope is pure amplitude
    // modulation, whose mean is `1 − depth/2` of the carrier regardless of the
    // rotor's rate — which is what lets one mean serve as the zero-crossing
    // reference across a sweep from rest to full speed.
    const auto settle_seconds = [](bool drum) {
        const double carrier = drum ? 150.0 : 4000.0;
        const double target = drum ? Leslie::kDrumFastHz : Leslie::kHornFastHz;

        Leslie l = make_leslie(LeslieSpeed::chorale);
        l.set_reflection_db(-60.0);
        l.set_dir_depth_db(0.0);
        l.set_drum_dir_depth_db(0.0);
        l.reset();

        const int pre = static_cast<int>(kSr * 2.0);
        const int post = static_cast<int>(kSr * 14.0);
        std::vector<double> rendered;
        rendered.reserve(static_cast<std::size_t>(post));
        for (int i = 0; i < pre + post; ++i) {
            if (i == pre) l.set_speed(LeslieSpeed::tremolo);
            double a = 0.0;
            double b = 0.0;
            l.process(0.5 * std::sin(kTwoPi * carrier * i / kSr), a, b);
            if (i >= pre) rendered.push_back(a);
        }

        // The demodulation corner is a fixed fraction of the carrier, not a
        // fixed frequency: the image sits at twice the carrier, so a corner that
        // is comfortable under a 4 kHz carrier sits ABOVE the image of a 150 Hz
        // one and fills the envelope with it. A tenth of the carrier clears the
        // image at both ends and still passes a 6 Hz modulation.
        const auto d = demodulate(rendered, carrier, kSr, carrier * 0.1, 48);
        const auto rates = cycle_rates(remove_mean(d.envelope), d.rate_hz);
        REQUIRE(rates.size() > 10);

        // Walk back from the end: the arrival is the last moment the rate was
        // still outside the band, so a rate that happens to sweep through the
        // band on its way up cannot be mistaken for having settled there.
        const double tolerance = 0.05 * target;
        std::size_t index = rates.size();
        while (index > 0 && std::abs(rates[index - 1].second - target) < tolerance) --index;
        return index == 0 ? 0.0 : rates[index - 1].first;
    };

    const double horn = settle_seconds(false);
    const double drum = settle_seconds(true);

    INFO("horn settled at " << horn << " s, drum at " << drum << " s");

    // Each rotor arrives on the schedule its own shipped constant sets. The
    // constants are "time to close `kSettleFraction` of the change", so this is
    // the number the header promises, not a derived one.
    REQUIRE_THAT(horn, WithinAbs(Leslie::kHornAccelS, 0.6));
    REQUIRE_THAT(drum, WithinAbs(Leslie::kDrumAccelS, 1.2));

    // And the asymmetry itself, which is the part that matters: the drum is
    // still catching up well after the horn has arrived.
    REQUIRE(drum > horn + 1.0);
    REQUIRE_THAT(drum / horn, WithinRel(Leslie::kDrumAccelS / Leslie::kHornAccelS, 0.5));
}

TEST_CASE("the inertia ramp is a time constant, not a fixed slide time",
          "[leslie][inertia]") {
    // Mechanical inertia has a constant TIME CONSTANT: a bigger speed change
    // takes proportionally longer. The other slew law available — constant
    // total time — would cover any distance in the same wall clock, which is a
    // portamento, not a flywheel. Asserted by comparing a small change against
    // a large one: under a constant time constant the two reach the SAME
    // FRACTION of their own change at the same moment.
    const auto fraction_after = [](double from_hz, double to_hz, double seconds) {
        Leslie l = make_leslie(LeslieSpeed::stop);
        l.set_horn_slow_hz(from_hz);
        l.set_horn_fast_hz(to_hz);
        l.set_speed(LeslieSpeed::chorale);
        l.reset();
        l.set_speed(LeslieSpeed::tremolo);
        double a = 0.0;
        double b = 0.0;
        for (int i = 0; i < static_cast<int>(kSr * seconds); ++i) l.process(0.0, a, b);
        return (l.horn_rate_hz() - from_hz) / (to_hz - from_hz);
    };

    const double small = fraction_after(6.0, 6.5, 0.5);
    const double large = fraction_after(0.6, 7.0, 0.5);
    REQUIRE_THAT(small, WithinRel(large, 0.02));

    // ...and that shared fraction is the one the one-pole predicts at that
    // instant, computed from the shipped accel time and settle fraction.
    const double tau = Leslie::kHornAccelS / -std::log(1.0 - Leslie::kSettleFraction);
    REQUIRE_THAT(small, WithinRel(1.0 - std::exp(-0.5 / tau), 0.02));
}

TEST_CASE("braking freezes the modulation rather than muting it", "[leslie][inertia]") {
    // `stop` is a third colour, not silence: the rotors coast to rest and the
    // horn parks facing wherever it stopped, so the tone holds bright or dark
    // depending on phase. Nothing crossfades to a bypass — a stopped rotor IS
    // the stationary path, because a phase that stops advancing freezes the
    // delay, the beam gain and the shelf together.
    Leslie l = make_leslie(LeslieSpeed::tremolo);
    double a = 0.0;
    double b = 0.0;
    for (int i = 0; i < static_cast<int>(kSr * 1.0); ++i)
        l.process(0.5 * std::sin(kTwoPi * 440.0 * i / kSr), a, b);

    l.set_speed(LeslieSpeed::stop);
    for (int i = 0; i < static_cast<int>(kSr * 20.0); ++i)
        l.process(0.5 * std::sin(kTwoPi * 440.0 * i / kSr), a, b);

    REQUIRE_THAT(l.horn_rate_hz(), WithinAbs(0.0, 1e-3));
    REQUIRE_THAT(l.drum_rate_hz(), WithinAbs(0.0, 1e-3));

    const double parked = l.horn_phase();
    const auto rendered = render_tone(l, 4000.0, 0.5, 2.0, Channel::left);
    REQUIRE_THAT(l.horn_phase(), WithinAbs(parked, 1e-12));

    // Frozen, not muted: the cabinet still passes signal at a level set by
    // wherever the horn happens to be pointing.
    double energy = 0.0;
    for (double v : rendered) energy += v * v;
    REQUIRE(std::sqrt(energy / static_cast<double>(rendered.size())) > 0.05);
}

// ── 4. Crossover ──────────────────────────────────────────────────────────

TEST_CASE("the crossover splits at its stated corner", "[leslie][crossover]") {
    // Located from OUTSIDE the box, using the rotors themselves as band labels:
    // the horn band's content is amplitude-modulated at the horn rate and the
    // drum band's at the drum rate, so the frequency where the envelope carries
    // equal energy at the two rates is the frequency where the two band gains
    // are equal — which for an LR4 is the crossover.
    //
    // The Doppler is switched off for the measurement (radius 0) on purpose.
    // With it on, the two bands reach the mic through delays that differ by
    // their differing radii, their relative phase is modulated, and that
    // interference adds an amplitude term at BOTH rotor rates which biases the
    // crossing — it reads 8.9 % low. Zero radius removes the difference without
    // touching the split being measured.
    const auto locate_crossover = [](double asked_hz) {
        double lo = asked_hz * 0.4;
        double hi = asked_hz * 2.5;
        for (int iteration = 0; iteration < 16; ++iteration) {
            const double mid = std::sqrt(lo * hi);
            Leslie l = make_leslie();
            l.set_crossover_hz(asked_hz);
            l.set_dir_depth_db(0.0);
            l.set_drum_dir_depth_db(0.0);
            l.set_reflection_db(-60.0);
            l.set_horn_radius_m(0.0);
            l.set_drum_radius_m(0.0);
            l.reset();

            const auto rendered = render_tone(l, mid, 0.5, 10.0, Channel::left);
            const auto d = demodulate(rendered, mid, kSr, 200.0, 48);
            const auto env = remove_mean(d.envelope);
            const std::size_t skip = env.size() / 5;
            const double horn = std::abs(coherent(env, Leslie::kHornFastHz, d.rate_hz, skip));
            const double drum = std::abs(coherent(env, Leslie::kDrumFastHz, d.rate_hz, skip));
            if (horn > drum) hi = mid; else lo = mid;
        }
        return std::sqrt(lo * hi);
    };

    REQUIRE_THAT(locate_crossover(Leslie::kCrossoverHz), WithinRel(Leslie::kCrossoverHz, 0.03));
    // It is the parameter that moves it, not a coincidence at one value.
    REQUIRE_THAT(locate_crossover(500.0), WithinRel(500.0, 0.03));
}

TEST_CASE("the crossover itself is inaudible", "[leslie][crossover]") {
    // §3.4's claim: an LR4 pair recombines flat, so with the rotors stopped the
    // split leaves no trace. Asserted two ways, because the absolute version
    // runs into a limit that has nothing to do with the crossover.
    const auto neutral = [](double crossover_hz) {
        Leslie l = make_leslie(LeslieSpeed::stop);
        l.set_crossover_hz(crossover_hz);
        l.set_am_depth(0.0);
        l.set_dir_depth_db(0.0);
        l.set_drum_dir_depth_db(0.0);
        l.set_reflection_db(-60.0);
        l.set_mix(1.0);
        l.reset();
        return l;
    };

    // (a) The crossover's OWN contribution, across the full audio band. Moving
    // the corner from one end of its range to the other leaves every other
    // stage — the fractional-delay read, the shelves, the DC blocker —
    // bit-identical, so the ratio of the two responses isolates the crossover
    // and nothing else. A flat ratio is exactly the claim "the split is
    // inaudible", and it holds where the absolute version cannot.
    {
        Leslie low = neutral(700.0);
        Leslie high = neutral(900.0);
        double worst_db = 0.0;
        for (double hz = 20.0; hz <= 20000.0; hz *= 1.15) {
            const double ratio_db =
                units::linear_to_db(response_at(high, hz) / response_at(low, hz));
            worst_db = std::max(worst_db, std::abs(ratio_db));
        }
        INFO("worst crossover-dependent deviation: " << worst_db << " dB");
        REQUIRE(worst_db < 0.5);
    }

    // (b) The absolute flatness, up to the ceiling the SIGNAL PATH allows.
    //
    // The spec asks for +/-0.5 dB from 20 Hz to 20 kHz. That is not reachable
    // by any implementation of this chain, and the crossover is not the reason:
    // every rotor read is a fractional delay, and the 4-point Lagrange kernel
    // the catalog uses for modulated delays is −0.23 dB at 8 kHz, −0.53 dB at
    // 10 kHz and −3.3 dB at 16 kHz at its worst fractional offset. The
    // interpolator alone spends the whole budget before 10 kHz. The band
    // asserted here is therefore the one the claim can be true over, and the
    // ceiling is stated rather than quietly widened tolerance.
    {
        constexpr double kFlatnessCeilingHz = 8000.0;
        Leslie l = neutral(Leslie::kCrossoverHz);
        double worst_db = 0.0;
        double worst_hz = 0.0;
        for (double hz = 20.0; hz <= kFlatnessCeilingHz; hz *= 1.15) {
            const double db = units::linear_to_db(response_at(l, hz));
            if (std::abs(db) > std::abs(worst_db)) {
                worst_db = db;
                worst_hz = hz;
            }
        }
        INFO("worst absolute deviation " << worst_db << " dB at " << worst_hz << " Hz");
        REQUIRE(std::abs(worst_db) < 0.5);
    }
}

// ── 5. The stereo mic pair ────────────────────────────────────────────────

TEST_CASE("the mic pair produces a real stereo image, not a doubled mono",
          "[leslie][stereo]") {
    // Two mics at different angles see the horn face them at different phases,
    // so their tremolos run at the same rate with a phase offset — and that
    // offset IS the included angle, converted from degrees to a fraction of a
    // rotor revolution. Asserting the angle rather than merely "L differs from
    // R" is what makes this a test of the geometry instead of a test that some
    // decorrelation happened.
    const auto measured_offset_deg = [](double angle_deg) {
        Leslie l = make_leslie();
        l.set_mic_angle_deg(angle_deg);
        l.set_reflection_db(-60.0);
        l.reset();

        const int n = static_cast<int>(kSr * 10.0);
        std::vector<double> left;
        std::vector<double> right;
        left.reserve(static_cast<std::size_t>(n));
        right.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i) {
            double a = 0.0;
            double b = 0.0;
            l.process(0.5 * std::sin(kTwoPi * 4000.0 * i / kSr), a, b);
            left.push_back(a);
            right.push_back(b);
        }

        const auto dl = demodulate(left, 4000.0, kSr, 400.0, 48);
        const auto dr = demodulate(right, 4000.0, kSr, 400.0, 48);
        const auto el = remove_mean(dl.envelope);
        const auto er = remove_mean(dr.envelope);
        const std::size_t skip = el.size() / 5;
        double phase = std::arg(coherent(er, Leslie::kHornFastHz, dr.rate_hz, skip)) -
                       std::arg(coherent(el, Leslie::kHornFastHz, dl.rate_hz, skip));
        while (phase > std::numbers::pi)
            phase -= kTwoPi;
        while (phase < -std::numbers::pi)
            phase += kTwoPi;
        return std::abs(phase) * 180.0 / std::numbers::pi;
    };

    // The default: the two tremolos are the mic angle apart in rotor phase.
    REQUIRE_THAT(measured_offset_deg(Leslie::kMicAngleDeg),
                 WithinAbs(Leslie::kMicAngleDeg, 2.0));
    // Quadrature at the wide end — the "two mics on the cabinet" setup.
    REQUIRE_THAT(measured_offset_deg(90.0), WithinAbs(90.0, 2.0));
    // And it collapses when the mics are told to share an angle, which proves
    // the width comes from the geometry rather than from a fixed widener.
    REQUIRE_THAT(measured_offset_deg(0.0), WithinAbs(0.0, 2.0));

    // The collapse is total: at 0° the two mics are the same signal.
    Leslie mono = make_leslie();
    mono.set_mic_angle_deg(0.0);
    mono.reset();
    for (int i = 0; i < 4800; ++i) {
        double a = 0.0;
        double b = 0.0;
        mono.process(0.5 * std::sin(kTwoPi * 440.0 * i / kSr), a, b);
        REQUIRE_THAT(a, WithinAbs(b, 1e-12));
    }
}
