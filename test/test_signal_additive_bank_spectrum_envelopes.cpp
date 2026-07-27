#include "test_signal_additive_bank_support.hpp"

TEST_CASE("A single partial sits at exactly the requested frequency",
          "[signal][additive][frequency]") {
    // 440 Hz at 48 kHz is 11 cycles per 1200 samples exactly, so a window of
    // 1200·k samples holds a whole number of cycles and the coherent DFT is
    // exact. 40 such blocks is 1 s of analysis.
    constexpr int kWindow = 48000;
    constexpr int kCycles = 440;

    Bank bank;
    configure_steady(bank, harmonic_voice(1), 1, 440.0);
    // One sample past the analysis window, so the zero-crossing count below
    // sees the crossing that spans the wrap. Counting sign changes over N
    // samples examines N−1 adjacent pairs, which misses exactly one of the 2·k
    // crossings a k-cycle window contains — an off-by-one in the RULER, not in
    // the signal. The DFT still reads exactly `kWindow` samples.
    const auto x = render(bank, kWindow + 1);

    // Every joule of the signal is at 440 Hz. A far sharper frequency
    // statement than reading a peak: an error of 0.01 % would leave the fitted
    // 440 Hz component unable to account for the energy.
    const double amp = coherent_amplitude(x, kCycles, kWindow);
    double total = 0.0;
    for (int i = 0; i < kWindow; ++i) {
        const double v = x[static_cast<std::size_t>(i)];
        total += v * v;
    }
    const double explained = 0.5 * amp * amp * static_cast<double>(kWindow);
    REQUIRE_THAT(explained / total, WithinAbs(1.0, 1e-9));

    // The spec's corroboration: a zero-crossing count over the same window.
    //
    // Asserted to +/-1, and that is the exact right tolerance rather than
    // slack. A 440-cycle window contains 880 crossings, but its endpoint sits
    // exactly on one of them: 81 of these samples land on a zero of the sine,
    // and at those samples the sign is decided by the last bit of the
    // accumulated phase rather than by the frequency. Choosing a phase offset
    // does not escape it — any rational offset still puts samples on zeros,
    // because the period divides the window by construction. So a crossing
    // count over a finite window is a +/-1 measurement, full stop. The exact
    // frequency statement is the energy test above, which is already tight to
    // 1e-9; this is the independent corroboration the spec asks for.
    int crossings = 0;
    for (std::size_t i = 1; i < x.size(); ++i)
        if ((x[i - 1] < 0.0) != (x[i] < 0.0)) ++crossings;
    REQUIRE(std::abs(crossings - 2 * kCycles) <= 1);

    // And no harmonic content: the bank is a sine, not a shape.
    for (int h = 2; h <= 6; ++h)
        REQUIRE(db(coherent_amplitude(x, kCycles * h, kWindow) / amp) < -100.0);
}

TEST_CASE("One partial is bit-identical to a catalog sine oscillator",
          "[signal][additive][frequency]") {
    // The header claims a single partial IS the catalog's sine rather than a
    // lookalike. That is a bit-exactness claim, so it gets a bit-exact test
    // against `osc::VaOscillator` — including the phase CONVENTION, which is
    // the part that is easy to get wrong and invisible in a spectrum: both
    // evaluate at the entry phase and advance afterwards, so sample 0 is the
    // stored phase itself and not one increment past it.
    constexpr double kF0 = 440.0;
    constexpr double kPhase = 0.25;
    constexpr int kN = 4096;

    Bank bank;
    // Unity all the way through: one partial at amplitude 1 leaves the
    // normaliser inert (its sum is exactly 1), and 0 dB master, 0 tilt and a
    // flat envelope leave nothing else to scale it.
    configure_steady(bank, harmonic_voice(1, 1.0, kPhase), 1, kF0);
    const auto x = render(bank, kN);

    osc::VaOscillator reference;
    reference.set_shape(osc::VaShape::sine);
    reference.reset(kPhase);
    const double increment = kF0 / kSr;

    for (int i = 0; i < kN; ++i)
        REQUIRE(x[static_cast<std::size_t>(i)] == reference.next(increment));
}

TEST_CASE("The catalog sine meets the THD requirement and FastMath does not",
          "[signal][additive][frequency][spec-defect]") {
    // SPEC DEFECT, with the number. Section 3 asks for "the existing osc/
    // family sine (polynomial/lookup)" and, in the same paragraph, for a single
    // evaluated sine to hold THD <= -100 dB. Those cannot both be satisfied:
    // there IS no polynomial or lookup sine in the osc family — `osc/va.hpp`'s
    // sine shape is `std::sin(2*pi*phi)` in double, which is what this engine
    // composes — and the tree's one polynomial sine, `FastMath::sin`, is a
    // Bhaskara-style approximation about 40 dB short of the requirement.
    constexpr int kWindow = 48000;
    constexpr int kCycles = 440;

    Bank bank;
    configure_steady(bank, harmonic_voice(1), 1, 440.0);
    const auto x = render(bank, kWindow);

    const double fundamental = coherent_amplitude(x, kCycles, kWindow);
    double shipped_thd = 0.0;
    for (int h = 2; h <= 20; ++h) {
        const double a = coherent_amplitude(x, kCycles * h, kWindow);
        shipped_thd += a * a;
    }
    shipped_thd = std::sqrt(shipped_thd) / fundamental;
    REQUIRE(db(shipped_thd) < -100.0);

    // The same measurement on the fast approximation, at the same frequency.
    std::vector<double> fast(static_cast<std::size_t>(kWindow));
    for (int i = 0; i < kWindow; ++i) {
        const double ph = static_cast<double>(kCycles) *
                          static_cast<double>(i) / static_cast<double>(kWindow);
        fast[static_cast<std::size_t>(i)] = static_cast<double>(
            FastMath::sin(static_cast<float>(2.0 * kPi * ph)));
    }
    const double fast_fundamental = coherent_amplitude(fast, kCycles, kWindow);
    double fast_thd = 0.0;
    for (int h = 2; h <= 20; ++h) {
        const double a = coherent_amplitude(fast, kCycles * h, kWindow);
        fast_thd += a * a;
    }
    fast_thd = std::sqrt(fast_thd) / fast_fundamental;

    REQUIRE(db(fast_thd) > -100.0);
    // And it is not marginal — it misses by more than 30 dB, which across a
    // 64-partial sum is exactly the accumulated harmonic error the requirement
    // exists to prevent.
    REQUIRE(db(fast_thd) - db(shipped_thd) > 30.0);
}

TEST_CASE("Partial magnitudes match the voice table",
          "[signal][additive][spectrum]") {
    constexpr int kWindow = 48000;
    constexpr double kF0 = 200.0;   // divides fs, so every harmonic is coherent
    const double amps[4] = {1.0, 0.5, 0.25, 0.125};

    VoiceTable v;
    v.harmonic = true;
    for (int p = 0; p < 4; ++p)
        v.add({static_cast<double>(p + 1), amps[p], 0.0, 0.0});

    Bank bank;
    configure_steady(bank, v, 4, kF0);
    const auto x = render(bank, kWindow);

    // The normaliser scales every partial by one common factor, so the RATIOS
    // are what the table promises regardless of whether it engaged. Asserting
    // ratios rather than absolutes is also what makes this test independent of
    // the crest bound, which has its own test.
    const double first = coherent_amplitude(x, static_cast<int>(kF0), kWindow);
    for (int p = 0; p < 4; ++p) {
        const double a = coherent_amplitude(
            x, static_cast<int>(kF0) * (p + 1), kWindow);
        REQUIRE_THAT(db(a / first), WithinAbs(db(amps[p] / amps[0]), 0.1));
    }

    // And the absolute level is the table amplitude times the shipped
    // normaliser — computed here from the same sum the engine forms, so a
    // change to the normaliser law fails this rather than sliding past it.
    double magnitude_sum = 0.0;
    for (double a : amps) magnitude_sum += a;
    const double expected_norm = 1.0 / std::max(1.0, magnitude_sum);
    REQUIRE_THAT(first, WithinRel(amps[0] * expected_norm, 1e-6));

    // No energy anywhere else. Checked at frequencies that are coherent in this
    // window but are not partials, so a leak would have nowhere to hide.
    for (int hz : {100, 150, 250, 333, 700, 1100, 4000})
        REQUIRE(db(coherent_amplitude(x, hz, kWindow) / first) < -100.0);
}

TEST_CASE("Partials land where the stiff-string law puts them",
          "[signal][additive][inharmonicity]") {
    // The law is published (Fletcher, Blackham & Stratton 1962):
    // f_n = n*f0*sqrt(1 + B*n^2). The expectation is computed from the shipped
    // form, and the shipped form is separately checked against the spec's own
    // worked number as external ground truth.
    constexpr double kF0 = 220.0;
    const double kB = Bank::kBPianoMid;

    // External ground truth: the spec's section 7 worked check.
    REQUIRE_THAT(Bank::partial_frequency_hz(kF0, 8.0, kB),
                 WithinAbs(1804.5, 0.05));
    // ...and its 2.53 % stretch claim at the eighth partial.
    REQUIRE_THAT(Bank::partial_frequency_hz(kF0, 8.0, kB) / (8.0 * kF0) - 1.0,
                 WithinAbs(0.0253, 5e-5));

    Bank bank;
    configure_steady(bank, harmonic_voice(12), 12, kF0);
    bank.set_inharmonicity_b(kB);
    bank.reset();
    bank.retrigger();

    // 2^17 samples is 2.7 s — long enough that the Hann main lobe is ~0.7 Hz
    // wide, orders finer than the 0.05 % asserted below.
    const auto x = render(bank, 1 << 17);

    for (int n = 1; n <= 12; ++n) {
        const double predicted = Bank::partial_frequency_hz(
            kF0, static_cast<double>(n), kB);
        // A +/-2 % bracket cannot reach a neighbour: consecutive partials are
        // a whole f0 apart, which is at least 12 % of any of these frequencies.
        const double measured =
            refine_peak(x, predicted * 0.98, predicted * 1.02);
        REQUIRE_THAT(measured, WithinRel(predicted, 5e-4));
        // Sharp of the pure harmonic, increasingly so with n — the stretch
        // that makes struck strings shimmer.
        REQUIRE(measured > static_cast<double>(n) * kF0);
    }

    // B = 0 is exactly the harmonic series, not approximately.
    for (int n = 1; n <= 12; ++n)
        REQUIRE_THAT(Bank::partial_frequency_hz(kF0, n, 0.0),
                     WithinRel(static_cast<double>(n) * kF0, 1e-15));

    // The stretch is monotone in B across the whole legal range, including the
    // uncited headroom past the published band.
    double previous = 0.0;
    for (double b : {0.0, Bank::kBPianoTenor, Bank::kBPianoMid,
                     Bank::kBUpperTreble, Bank::kBElectricPianoTine,
                     Bank::kInharmonicityMax}) {
        const double f = Bank::partial_frequency_hz(kF0, 8.0, b);
        REQUIRE(f > previous);
        previous = f;
    }
    REQUIRE(Bank::kBElectricPianoTine == Bank::kInharmonicityCitedMax);
    REQUIRE(Bank::kInharmonicityMax > Bank::kInharmonicityCitedMax);
}

TEST_CASE("The stiffness stretch is keyed to the mode ratio not the row index",
          "[signal][additive][inharmonicity][spec-defect]") {
    // SPEC DEFECT, and a decision this test exists to pin. Section 7's prose
    // says "a general voice table each partial carries its own ratio_p", but
    // the code line beside it computes the stretch from the array index
    // (`n = p+1`). For a pure harmonic table those are the same number, which
    // is why AT-3 cannot tell them apart — and why a wrong choice would ship.
    //
    // They differ for any table whose ratios are not 1, 2, 3, ... The organ's
    // first row is the 16' sub at ratio 0.5: the index reading stretches it as
    // if it were mode 1, which is 0.7 % sharp of where a half-mode actually
    // sits. The ratio is the mode number of the string that is vibrating, so
    // the ratio is what the physics depends on.
    constexpr double kF0 = 400.0;
    const double kB = Bank::kBElectricPianoTine;   // large enough to resolve

    VoiceTable v;
    v.harmonic = true;
    for (double ratio : {0.5, 1.0, 1.5, 2.0}) v.add({ratio, 1.0, 0.0, 0.0});

    Bank bank;
    configure_steady(bank, v, 4, kF0);
    bank.set_inharmonicity_b(kB);
    bank.reset();

    for (int p = 0; p < 4; ++p) {
        const double ratio = v.partials[static_cast<std::size_t>(p)].ratio;
        const double by_ratio = Bank::partial_frequency_hz(kF0, ratio, kB);
        REQUIRE_THAT(bank.partial_frequency(p), WithinRel(by_ratio, 1e-9));
    }

    // The two readings genuinely disagree here, so the assertion above is a
    // choice being tested rather than a tautology.
    const double by_index =
        kF0 * 0.5 * std::sqrt(1.0 + kB * 1.0 * 1.0);   // n = p+1 = 1
    const double by_ratio = Bank::partial_frequency_hz(kF0, 0.5, kB);
    REQUIRE(std::abs(by_index - by_ratio) / by_ratio > 0.005);
    REQUIRE(bank.partial_frequency(0) < by_index);

    // And it is audible in the render, not only in the getter. `reset()` leaves
    // the onset idle, so the retrigger is what makes this a signal rather than
    // 2.7 seconds of silence that any peak search would happily wander through.
    bank.retrigger();
    const auto x = render(bank, 1 << 17);
    double energy = 0.0;
    for (double v : x) energy += v * v;
    REQUIRE(energy > 0.0);
    const double measured =
        refine_peak(x, by_ratio * 0.99, by_ratio * 1.01);
    REQUIRE_THAT(measured, WithinRel(by_ratio, 1e-3));
}

TEST_CASE("Inharmonicity stretches harmonic voices",
          "[signal][additive][inharmonicity]") {
    Bank harmonic_bank;
    configure_steady(harmonic_bank, harmonic_voice(8), 8, 220.0);
    const double before = harmonic_bank.partial_frequency(7);
    harmonic_bank.set_inharmonicity_b(Bank::kBUpperTreble);
    harmonic_bank.reset();
    REQUIRE(harmonic_bank.partial_frequency(7) > before);
}

TEST_CASE("Nyquist guard tapers rather than cliffs",
          "[signal][additive][nyquist][spec-defect]") {
    // SPEC DEFECT, with the number. AT-4(a) asks that "partials above
    // fs/2 - guard are <= -100 dB". Section 6 defines that same band as a
    // raised-cosine TAPER to zero at Nyquist, so a partial just inside it is
    // barely attenuated: at fs = 48 kHz with the shipped 1000 Hz guard, a
    // partial at 23,200 Hz has gain 0.9045, i.e. -0.87 dB. The two clauses
    // contradict each other, and the taper is the one that matters — a hard
    // cliff is what CLICKS when a glide walks a partial through it, which
    // AT-4(c) separately forbids.
    const double nyquist = 0.5 * kSr;
    const double taper_start = nyquist - Bank::kNyquistGuardBandHz;

    // Unity below the band, exactly zero at and above Nyquist, monotone across.
    REQUIRE_THAT(Bank::nyquist_guard_gain(taper_start, kSr),
                 WithinAbs(1.0, 1e-12));
    REQUIRE_THAT(Bank::nyquist_guard_gain(nyquist, kSr), WithinAbs(0.0, 1e-12));
    REQUIRE(Bank::nyquist_guard_gain(nyquist + 1.0, kSr) == 0.0);
    REQUIRE(Bank::nyquist_guard_gain(nyquist * 4.0, kSr) == 0.0);

    // The midpoint of a raised cosine is exactly one half — the property that
    // distinguishes it from a linear fade or a cliff.
    REQUIRE_THAT(
        Bank::nyquist_guard_gain(taper_start + 0.5 * Bank::kNyquistGuardBandHz,
                                 kSr),
        WithinAbs(0.5, 1e-12));

    double previous = 1.0;
    for (double f = taper_start; f <= nyquist; f += 25.0) {
        const double g = Bank::nyquist_guard_gain(f, kSr);
        REQUIRE(g <= previous + 1e-15);
        previous = g;
    }

    // And this is the spec's own arithmetic, showing the criterion is
    // unmeetable: a partial inside the guard is nowhere near -100 dB.
    REQUIRE(db(Bank::nyquist_guard_gain(23200.0, kSr)) > -1.0);
}

TEST_CASE("Partials past Nyquist are muted rather than folded",
          "[signal][additive][nyquist]") {
    // The spec's f0 = 200 Hz cannot detect a fold: at that fundamental every
    // image lands on another requested partial (48000 - 200n = 200*(240-n)), so
    // a folded partial would be indistinguishable from a legitimate one and the
    // test could not fail. 190 Hz has no such coincidence.
    constexpr double kF0 = 190.0;
    constexpr int kPartials = 128;

    Bank bank;
    configure_steady(bank, harmonic_voice(kPartials), kPartials, kF0);
    const auto x = render(bank, 1 << 17);

    const double nyquist = 0.5 * kSr;
    const double reference = windowed_magnitude(x, kF0);

    int muted = 0, tapered = 0;
    for (int n = 1; n <= kPartials; ++n) {
        const double f = static_cast<double>(n) * kF0;
        const double guard = Bank::nyquist_guard_gain(f, kSr);

        if (guard == 0.0) {
            ++muted;
            // Muted, and its FOLD IMAGE is silent too — which is the actual
            // claim. A partial that was merely quiet at its own frequency but
            // folding at fs-f would pass a "muted" check and still alias.
            const double image = kSr - f;
            if (image > 20.0 && image < nyquist)
                REQUIRE(db(windowed_magnitude(x, image) / reference) < -100.0);
        } else if (guard < 1.0) {
            ++tapered;
        }
    }
    // The stated conditions really do push partials past Nyquist, so the test
    // is exercising the guard rather than describing a band it never reaches.
    REQUIRE(muted > 0);
    REQUIRE(tapered > 0);

    // Whole-band sweep: every peak that rises above the floor is a requested,
    // unmuted partial. This is the general form of "no aliased energy" and
    // needs no bookkeeping about where an image would land.
    const auto s = spectrum_of(x);
    const double floor_db = -80.0;
    for (std::size_t bin = 1; bin + 1 < s.magnitude.size(); ++bin) {
        if (s.magnitude[bin] <= s.magnitude[bin - 1] ||
            s.magnitude[bin] < s.magnitude[bin + 1])
            continue;
        if (db(s.magnitude[bin] / reference) < floor_db) continue;

        const double f = s.frequency(bin);
        const double n = f / kF0;
        const double nearest = std::round(n);
        REQUIRE(std::abs(n - nearest) < 0.02);
        REQUIRE(nearest >= 1.0);
        REQUIRE(nearest <= static_cast<double>(kPartials));
        REQUIRE(Bank::nyquist_guard_gain(nearest * kF0, kSr) > 0.0);
    }
}

TEST_CASE("A partial gliding through the guard does not click",
          "[signal][additive][nyquist]") {
    // AT-4(c). The guard's whole reason for being a taper rather than a switch.
    // Measured as the largest sample-to-sample step, compared against the same
    // voice held still — a mute that stepped would show up as a step far larger
    // than the signal's own slew.
    constexpr int kPartials = 64;
    constexpr int kBlocks = 400;
    constexpr int kBlock = 64;

    const auto max_step = [](const std::vector<double>& x) {
        double m = 0.0;
        for (std::size_t i = 1; i < x.size(); ++i)
            m = std::max(m, std::abs(x[i] - x[i - 1]));
        return m;
    };

    // Held still at the top of the sweep.
    Bank steady;
    configure_steady(steady, harmonic_voice(kPartials), kPartials, 380.0);
    const double still = max_step(render(steady, kBlocks * kBlock));

    // Gliding f0 upward so partials walk through the guard and out past
    // Nyquist one after another.
    Bank sweeping;
    configure_steady(sweeping, harmonic_voice(kPartials), kPartials, 340.0);
    std::vector<double> swept;
    swept.reserve(static_cast<std::size_t>(kBlocks * kBlock));
    for (int b = 0; b < kBlocks; ++b) {
        const double t = static_cast<double>(b) / (kBlocks - 1);
        sweeping.set_fundamental_hz(340.0 + t * 40.0);
        const auto chunk = render(sweeping, kBlock);
        swept.insert(swept.end(), chunk.begin(), chunk.end());
    }

    REQUIRE(max_step(swept) < 1.5 * still);
    for (double v : swept) REQUIRE(std::isfinite(v));
}

TEST_CASE("Morph is a dB-domain crossfade of the two envelopes",
          "[signal][additive][morph]") {
    constexpr int kWindow = 48000;
    constexpr double kF0 = 200.0;
    constexpr int kPartials = 8;
    constexpr double kSlopeDbOct = -6.0;

    const auto envelope_a = SpectralEnvelope::tilt(0.0, kF0);
    const auto envelope_b = SpectralEnvelope::tilt(kSlopeDbOct, kF0);

    for (double m : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        Bank bank;
        configure_steady(bank, harmonic_voice(kPartials), kPartials, kF0);
        bank.set_envelope_a(envelope_a);
        bank.set_envelope_b(envelope_b);
        bank.set_morph(static_cast<float>(m));
        bank.reset();
        bank.retrigger();
        const auto x = render(bank, kWindow);

        const double first = coherent_amplitude(x, static_cast<int>(kF0), kWindow);
        for (int n = 1; n <= kPartials; ++n) {
            const double a = coherent_amplitude(
                x, static_cast<int>(kF0) * n, kWindow);
            // The dB crossfade of a flat envelope and a slope is that slope
            // scaled by m, so partial n sits m*slope*log2(n) below the first.
            const double expected_db =
                m * kSlopeDbOct * std::log2(static_cast<double>(n));
            REQUIRE_THAT(db(a / first), WithinAbs(expected_db, 0.05));
        }
    }

    // The morph really is in dB, not in linear amplitude. At the midpoint the
    // two differ by 1.25 dB on a partial 10 dB down — small, but it is the
    // difference between a timbre that travels evenly and one that lurches.
    Bank probe;
    configure_steady(probe, harmonic_voice(kPartials), kPartials, kF0);
    probe.set_envelope_a(envelope_a);
    probe.set_envelope_b(envelope_b);
    probe.set_morph(0.5f);
    const double db_midpoint = 0.5 * kSlopeDbOct * std::log2(4.0);
    const double linear_midpoint =
        db(0.5 * (1.0 + units::db_to_linear(kSlopeDbOct * std::log2(4.0))));
    REQUIRE(std::abs(db_midpoint - linear_midpoint) > 1.0);
    REQUIRE_THAT(probe.envelope_db_at(kF0 * 4.0),
                 WithinAbs(db_midpoint, 1e-9));
}

TEST_CASE("A tilt envelope morphs identically at every fundamental",
          "[signal][additive][morph][scale-invariance]") {
    // Series law 7, in its strict form: the SAME morph at two fundamentals two
    // octaves and a bit apart must produce the same RELATIVE spectrum. A tilt
    // is affine in log2 frequency, which is exactly the class of envelope for
    // which that holds in the absolute-Hz domain.
    constexpr int kWindow = 48000;
    constexpr int kPartials = 8;
    constexpr double kSlopeDbOct = -6.0;

    const auto relative_spectrum = [&](double f0) {
        Bank bank;
        configure_steady(bank, harmonic_voice(kPartials), kPartials, f0);
        bank.set_envelope_a(SpectralEnvelope::tilt(0.0, 100.0));
        bank.set_envelope_b(SpectralEnvelope::tilt(kSlopeDbOct, 100.0));
        bank.set_morph(0.5f);
        bank.reset();
        bank.retrigger();
        const auto x = render(bank, kWindow);

        std::vector<double> rel;
        const double first =
            coherent_amplitude(x, static_cast<int>(f0), kWindow);
        for (int n = 1; n <= kPartials; ++n)
            rel.push_back(db(coherent_amplitude(
                                 x, static_cast<int>(f0) * n, kWindow) /
                             first));
        return rel;
    };

    // 110 and 880 are three octaves apart, and both divide 48 kHz into a whole
    // number of cycles per analysis window.
    const auto low = relative_spectrum(110.0);
    const auto high = relative_spectrum(880.0);
    REQUIRE(low.size() == high.size());
    for (std::size_t i = 0; i < low.size(); ++i)
        REQUIRE_THAT(low[i], WithinAbs(high[i], 0.05));

    // The tilt CONTROL is scale-invariant by construction, being measured
    // against log2(f_p/f0) rather than against absolute frequency.
    for (double f0 : {110.0, 880.0}) {
        Bank bank;
        configure_steady(bank, harmonic_voice(kPartials), kPartials, f0);
        bank.set_spectral_tilt_db_oct(kSlopeDbOct);
        for (int n = 1; n <= kPartials; ++n)
            REQUIRE_THAT(bank.envelope_db_at(f0 * n),
                         WithinAbs(kSlopeDbOct * std::log2(static_cast<double>(n)),
                                   1e-9));
    }
}

TEST_CASE("A formant envelope stays put in Hz and follows pitch on request",
          "[signal][additive][morph][scale-invariance][spec-defect]") {
    // SPEC DEFECT, resolved rather than papered over. Section 8 claims a
    // sampled spectral envelope makes the morph scale-invariant, and section
    // 10c's V6 says the resulting vowel "tracks pitch, unlike a fixed filter
    // bank". Those are contradictory: a FORMANT is a resonance of a fixed body
    // and does NOT move when the singer changes note — that is what makes an
    // "ah" an "ah" at every pitch. An envelope over absolute Hz cannot both
    // stay put and track pitch.
    //
    // The engine answers both by naming the abscissa. `absolute_hz` is the
    // Rodet & Depalle reading and gives real formants; `relative_to_f0` gives
    // the fully scale-invariant gesture. Only the second is pitch-independent
    // for a general envelope, and this test pins both behaviours so neither can
    // drift into the other.
    constexpr int kPartials = 8;

    SpectralEnvelope bump;   // a formant peak at 700 Hz
    bump.add(100.0, -12.0);
    bump.add(700.0, 0.0);
    bump.add(5000.0, -12.0);

    const auto relative_shape = [&](double f0, SpectralDomain domain) {
        Bank bank;
        configure_steady(bank, harmonic_voice(kPartials), kPartials, f0);
        bank.set_spectral_domain(domain);
        bank.set_envelope_a(bump);
        bank.set_morph(0.0f);
        std::vector<double> rel;
        const double first = bank.envelope_db_at(f0);
        for (int n = 1; n <= kPartials; ++n)
            rel.push_back(bank.envelope_db_at(f0 * n) - first);
        return rel;
    };

    // Absolute domain: the formant is anchored in Hz, so the relative spectrum
    // MUST change with pitch. That is the feature, not a failure.
    const auto abs_low = relative_shape(150.0, SpectralDomain::absolute_hz);
    const auto abs_high = relative_shape(600.0, SpectralDomain::absolute_hz);
    double worst = 0.0;
    for (std::size_t i = 0; i < abs_low.size(); ++i)
        worst = std::max(worst, std::abs(abs_low[i] - abs_high[i]));
    REQUIRE(worst > 6.0);

    // Relative domain: the same envelope, now anchored to the fundamental, is
    // pitch-independent for ANY shape rather than only for affine ones.
    const auto rel_low = relative_shape(150.0, SpectralDomain::relative_to_f0);
    const auto rel_high = relative_shape(600.0, SpectralDomain::relative_to_f0);
    for (std::size_t i = 0; i < rel_low.size(); ++i)
        REQUIRE_THAT(rel_low[i], WithinAbs(rel_high[i], 1e-9));
}

TEST_CASE("The spectral envelope interpolates in log frequency and dB",
          "[signal][additive][morph]") {
    SpectralEnvelope e;
    REQUIRE(e.size() == 0);
    // An empty envelope is the identity, so a default-constructed one changes
    // nothing rather than silencing everything.
    REQUIRE_THAT(e.gain_db_at(1000.0), WithinAbs(0.0, 1e-15));

    REQUIRE(e.add(100.0, 0.0));
    REQUIRE(e.add(400.0, -12.0));
    REQUIRE(e.size() == 2);

    // The geometric midpoint of 100 and 400 is 200, and linear-in-log2 puts it
    // at the arithmetic midpoint in dB. Interpolating in linear frequency would
    // put -12 dB's halfway point at 250 Hz and read -8.7 dB here instead.
    REQUIRE_THAT(e.gain_db_at(200.0), WithinAbs(-6.0, 1e-12));
    REQUIRE_THAT(e.gain_db_at(100.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(e.gain_db_at(400.0), WithinAbs(-12.0, 1e-12));

    // Endpoints are HELD outside the span, not extrapolated — extrapolating a
    // slope past the last break-point is how an envelope quietly produces
    // +40 dB at the top of a 128-partial bank.
    REQUIRE_THAT(e.gain_db_at(10.0), WithinAbs(0.0, 1e-12));
    REQUIRE_THAT(e.gain_db_at(20000.0), WithinAbs(-12.0, 1e-12));

    // Ascending order is a precondition of the search, so a violation is
    // rejected and reported rather than silently corrupting it.
    REQUIRE_FALSE(e.add(200.0, 0.0));
    REQUIRE_FALSE(e.add(400.0, 0.0));
    REQUIRE_FALSE(e.add(-5.0, 0.0));
    REQUIRE(e.size() == 2);

    // Capacity is a hard bound, not a hint.
    SpectralEnvelope full;
    for (int i = 0; i < SpectralEnvelope::kMaxBreakpoints; ++i)
        REQUIRE(full.add(20.0 + static_cast<double>(i), 0.0));
    REQUIRE_FALSE(full.add(100000.0, 0.0));
    REQUIRE(full.size() == SpectralEnvelope::kMaxBreakpoints);

    // The shipped tilt helper really is a constant slope per octave.
    const auto tilt = SpectralEnvelope::tilt(-6.0, 100.0);
    REQUIRE_THAT(tilt.gain_db_at(100.0), WithinAbs(0.0, 1e-9));
    REQUIRE_THAT(tilt.gain_db_at(200.0), WithinAbs(-6.0, 1e-9));
    REQUIRE_THAT(tilt.gain_db_at(800.0), WithinAbs(-18.0, 1e-9));
}

TEST_CASE("Every retrigger policy is deterministic across reset",
          "[signal][additive][retrigger][determinism]") {
    const auto attack_render = [](Bank::RetrigPhase policy, int strikes,
                                  double attack_ms = 0.0) {
        Bank bank;
        configure_steady(bank, make_bell_voice(32), 32, 220.0);
        bank.set_envelope_mode(Bank::EnvelopeMode::per_partial_decay);
        bank.set_attack_ms(attack_ms);
        bank.set_retrig_phase(policy);
        bank.reset();
        std::vector<double> out;
        for (int s = 0; s < strikes; ++s) {
            bank.retrigger();
            const auto chunk = render(bank, 2048);
            out.insert(out.end(), chunk.begin(), chunk.end());
        }
        return out;
    };

    for (auto policy : {Bank::RetrigPhase::reset_stored,
                        Bank::RetrigPhase::free_run,
                        Bank::RetrigPhase::seeded_random}) {
        // Series law 2: renders separated by a `reset()` are BIT-identical, not
        // merely close. The seeded policy included — its generator is rewound
        // by `reset()`, so the "random" attack is reproducible.
        const auto a = attack_render(policy, 3, 1.0);
        const auto b = attack_render(policy, 3, 1.0);
        REQUIRE(a.size() == b.size());
        for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
    }

    // `reset_stored` additionally makes every STRIKE identical to the first —
    // the property the default exists for. Measured with a zero-length attack,
    // which is what ISOLATES the phase policy: see the envelope-continuation
    // test below for why a non-zero attack would make the second strike differ
    // for a reason that has nothing to do with phase.
    {
        const auto x = attack_render(Bank::RetrigPhase::reset_stored, 3);
        for (std::size_t i = 0; i < 2048; ++i) {
            REQUIRE(x[i] == x[i + 2048]);
            REQUIRE(x[i] == x[i + 4096]);
        }
    }

    // `seeded_random` scatters the phases, so its second strike differs from
    // its first. Without this the policy would be reproducible AND inert.
    {
        const auto x = attack_render(Bank::RetrigPhase::seeded_random, 3);
        bool differs = false;
        for (std::size_t i = 0; i < 2048; ++i)
            if (x[i] != x[i + 2048]) differs = true;
        REQUIRE(differs);
    }

    // `free_run` leaves the phases running, so its second strike also differs —
    // the softer, already-sounding re-entry the policy is for.
    {
        const auto x = attack_render(Bank::RetrigPhase::free_run, 3);
        bool differs = false;
        for (std::size_t i = 0; i < 2048; ++i)
            if (x[i] != x[i + 2048]) differs = true;
        REQUIRE(differs);
    }
}

TEST_CASE("Retriggering mid-note continues the onset rather than restarting it",
          "[signal][additive][retrigger][spec-defect]") {
    // SPEC DEFECT, or at least a conflation worth pinning. Section 9 says
    // `reset_stored` makes "the attack bit-identical every strike". The PHASE
    // policy does exactly that — and the test above proves it. But the rendered
    // attack is the phase policy times the shared onset, and the composed
    // envelope core deliberately RETRIGGERS FROM THE CURRENT LEVEL rather than
    // from zero, because restarting from zero clicks. So a strike that lands
    // while the previous note is still sounding is not bit-identical to a
    // strike from silence, and it should not be.
    //
    // Both halves are asserted here so the distinction survives: identical from
    // silence, deliberately not identical mid-note.
    const auto strike_pair = [](double attack_ms) {
        Bank bank;
        configure_steady(bank, harmonic_voice(8), 8, 220.0);
        bank.set_attack_ms(attack_ms);
        bank.set_retrig_phase(Bank::RetrigPhase::reset_stored);
        bank.reset();
        bank.retrigger();
        const auto first = render(bank, 1024);
        bank.retrigger();   // mid-note: the onset is at unity, not at zero
        const auto second = render(bank, 1024);
        return std::pair{first, second};
    };

    // A 20 ms attack has not finished in 1024 samples (21 ms), so the first
    // strike is still climbing while the second starts from the level it
    // reached. The two therefore differ.
    {
        const auto [first, second] = strike_pair(20.0);
        bool differs = false;
        for (std::size_t i = 0; i < first.size(); ++i)
            if (first[i] != second[i]) differs = true;
        REQUIRE(differs);
        // And the second strike is LOUDER, which is the anti-click behaviour
        // rather than a phase error: it resumes from the level the first had
        // climbed to. Compared as RMS over the strike, not at sample 0 — the
        // stored phase here is 0, so sample 0 is `sin(0)` in both renders and
        // carries no level information at all.
        const auto rms = [](const std::vector<double>& v) {
            double sq = 0.0;
            for (double s : v) sq += s * s;
            return std::sqrt(sq / static_cast<double>(v.size()));
        };
        REQUIRE(rms(second) > rms(first));
    }

    // With the onset already at unity in both cases — a zero-length attack —
    // the two strikes are bit-identical, confirming the difference above is the
    // envelope and not the phases.
    {
        const auto [first, second] = strike_pair(0.0);
        for (std::size_t i = 0; i < first.size(); ++i)
            REQUIRE(first[i] == second[i]);
    }
}
