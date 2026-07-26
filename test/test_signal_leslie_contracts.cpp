#include "harness/leslie_test_support.hpp"

// ── 8. Determinism, latency, sizing, parity ───────────────────────────────

TEST_CASE("render, reset, re-render is bit-identical", "[leslie][scanner][determinism]") {
    // Including the seeded drift path, which is the only randomness in either
    // engine and is rewound by `reset()` (series law 2).
    for (double drift : {0.0, Leslie::kDriftCents}) {
        Leslie l = make_leslie();
        l.set_drift_cents(drift);
        l.set_seed(20260725u);
        l.reset();

        const auto render = [&](int n) {
            std::vector<double> out;
            out.reserve(static_cast<std::size_t>(2 * n));
            for (int i = 0; i < n; ++i) {
                // Program material, plus a param move partway through, so the
                // determinism claim covers automation and not just a static
                // render.
                if (i == n / 2) l.set_speed(LeslieSpeed::chorale);
                const double t = i / kSr;
                double a = 0.0;
                double b = 0.0;
                l.process(0.4 * std::sin(kTwoPi * 220.0 * t) + 0.3 * std::sin(kTwoPi * 1310.0 * t),
                          a, b);
                out.push_back(a);
                out.push_back(b);
            }
            return out;
        };

        const auto first = render(static_cast<int>(kSr * 2));
        l.set_speed(LeslieSpeed::tremolo);
        l.reset();
        const auto second = render(static_cast<int>(kSr * 2));
        REQUIRE(first.size() == second.size());
        for (std::size_t i = 0; i < first.size(); ++i) REQUIRE(first[i] == second[i]);
    }

    Scanner s;
    s.prepare(kSr);
    s.set_mode(ScannerMode::c2);
    s.reset();
    const auto render = [&](int n) {
        std::vector<double> out;
        for (int i = 0; i < n; ++i)
            out.push_back(s.process(0.5 * std::sin(kTwoPi * 330.0 * i / kSr)));
        return out;
    };
    const auto a = render(static_cast<int>(kSr * 2));
    s.reset();
    const auto b = render(static_cast<int>(kSr * 2));
    for (std::size_t i = 0; i < a.size(); ++i) REQUIRE(a[i] == b[i]);
}

TEST_CASE("both engines report zero latency and align sample-for-sample",
          "[leslie][scanner][latency]") {
    // Zero, and honestly so: every delay inside is a modelled acoustic or
    // electrical path, not processing latency. The claim is checked against the
    // bypass alignment rather than trusted.
    REQUIRE(Leslie::latency_samples() == 0);
    REQUIRE(Scanner::latency_samples() == 0);

    Leslie l = make_leslie();
    l.set_mix(0.0);
    l.reset();
    for (int i = 0; i < 512; ++i) {
        const double x = i == 7 ? 1.0 : 0.0;
        double a = 0.0;
        double b = 0.0;
        l.process(x, a, b);
        REQUIRE_THAT(a, WithinAbs(x, 1e-12));
        REQUIRE_THAT(b, WithinAbs(x, 1e-12));
    }

    Scanner s;
    s.prepare(kSr);
    s.set_mode(ScannerMode::off);
    s.reset();
    for (int i = 0; i < 512; ++i) {
        const double x = i == 7 ? 1.0 : 0.0;
        REQUIRE(s.process(x) == x);
    }
}

TEST_CASE("buffers are sized from the parameter ranges, not the current setting",
          "[leslie][scanner][sizing]") {
    // Series law 6, applied to storage: `prepare` must size for the WORST case
    // any later `set_*` can ask for, because a `set_*` is not allowed to
    // allocate. The Leslie's worst rotor read is at maximum radius and is
    // independent of mic distance — `L_max − L_min = (d+r) − (d−r) = 2r`, so
    // the distance cancels — while its longest read overall is the last
    // reflection tap.
    for (double sr : {44100.0, 48000.0, 96000.0, 192000.0}) {
        const double doppler_ms =
            2.0 * Leslie::kMaxRadiusM / 343.0 * 1000.0 + Leslie::kMaxDBiasMs;
        const double reflection_ms =
            Leslie::kMaxReflDelayMs +
            static_cast<double>(Leslie::kMaxReflections - 1) * Leslie::kMaxReflSpacingMs;
        const double longest_ms = std::max(doppler_ms, reflection_ms);
        REQUIRE(static_cast<double>(Leslie::worst_case_delay_samples(sr)) >=
                longest_ms * 0.001 * sr);
        REQUIRE(static_cast<double>(Scanner::worst_case_delay_samples(sr)) >=
                Scanner::kMaxLineDelayMs * 0.001 * sr);
    }

    // And the sizing survives contact with the extremes: prepared at defaults,
    // then pushed to every maximum without a re-prepare, the cabinet still
    // produces finite, bounded output.
    Leslie l;
    l.prepare(kSr);
    l.set_horn_radius_m(Leslie::kMaxRadiusM);
    l.set_drum_radius_m(Leslie::kMaxRadiusM);
    l.set_d_bias_ms(Leslie::kMaxDBiasMs);
    l.set_mic_distance_m(0.3);
    l.set_refl_delay_ms(Leslie::kMaxReflDelayMs);
    l.set_refl_spacing_ms(Leslie::kMaxReflSpacingMs);
    l.set_num_reflections(Leslie::kMaxReflections);
    l.set_reflection_db(-6.0);
    l.reset();
    for (int i = 0; i < static_cast<int>(kSr * 0.5); ++i) {
        double a = 0.0;
        double b = 0.0;
        l.process(std::sin(kTwoPi * 100.0 * i / kSr), a, b);
        REQUIRE(std::isfinite(a));
        REQUIRE(std::isfinite(b));
        REQUIRE(std::abs(a) <= Leslie::kWorstCaseGain);
        REQUIRE(std::abs(b) <= Leslie::kWorstCaseGain);
    }
}

TEST_CASE("the float and double instantiations agree", "[leslie][scanner]") {
    // Guards against a `SampleType`-dependent constant leaking in — the
    // modulation maths is deliberately all in double so the two differ only by
    // the audio path's own precision.
    LeslieRotaryT<float> single;
    LeslieRotaryT<double> dbl;
    single.prepare(kSr);
    dbl.prepare(kSr);
    single.reset();
    dbl.reset();
    for (int i = 0; i < 24000; ++i) {
        const double x = 0.6 * std::sin(kTwoPi * 330.0 * i / kSr);
        float fa = 0.0f;
        float fb = 0.0f;
        double da = 0.0;
        double db = 0.0;
        single.process(static_cast<float>(x), fa, fb);
        dbl.process(x, da, db);
        REQUIRE_THAT(static_cast<double>(fa), WithinAbs(da, 1e-3));
        REQUIRE_THAT(static_cast<double>(fb), WithinAbs(db, 1e-3));
    }

    ScannerVibratoT<float> s_single;
    ScannerVibratoT<double> s_dbl;
    s_single.prepare(kSr);
    s_dbl.prepare(kSr);
    s_single.set_mode(ScannerMode::c3);
    s_dbl.set_mode(ScannerMode::c3);
    s_single.reset();
    s_dbl.reset();
    for (int i = 0; i < 24000; ++i) {
        const double x = 0.6 * std::sin(kTwoPi * 330.0 * i / kSr);
        REQUIRE_THAT(static_cast<double>(s_single.process(static_cast<float>(x))),
                     WithinAbs(s_dbl.process(x), 1e-3));
    }
}

// ── 9. RT allocation probe ────────────────────────────────────────────────

TEST_CASE("neither engine allocates on the audio thread", "[leslie][scanner][rt-safety]") {
    LeslieRotaryT<float> leslie_f;
    LeslieRotaryT<double> leslie_d;
    ScannerVibratoT<float> scanner_f;
    ScannerVibratoT<double> scanner_d;
    leslie_f.prepare(kSr);
    leslie_d.prepare(kSr);
    scanner_f.prepare(kSr);
    scanner_d.prepare(kSr);
    leslie_f.set_drift_cents(LeslieRotaryT<float>::kDriftCents);
    leslie_d.set_drift_cents(LeslieRotaryT<double>::kDriftCents);

    std::vector<float> in_f(256, 0.1f);
    std::vector<float> out_a(256, 0.0f);
    std::vector<float> out_b(256, 0.0f);
    std::vector<double> in_d(256, 0.1);
    std::vector<double> out_c(256, 0.0);
    std::vector<double> out_e(256, 0.0);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 32; ++i) {
            const auto speed = static_cast<LeslieSpeed>(i % 3);
            leslie_f.set_speed(speed);
            leslie_f.set_horn_fast_hz(5.5 + 0.05 * i);
            leslie_f.set_drum_fast_hz(4.5 + 0.05 * i);
            leslie_f.set_horn_accel_s(0.3 + 0.05 * i);
            leslie_f.set_drum_accel_s(1.0 + 0.1 * i);
            leslie_f.set_crossover_hz(700.0 + 5.0 * i);
            leslie_f.set_horn_radius_m(0.10 + 0.004 * i);
            leslie_f.set_drum_radius_m(0.08 + 0.003 * i);
            leslie_f.set_mic_distance_m(0.3 + 0.08 * i);
            leslie_f.set_mic_angle_deg(1.0 * i);
            leslie_f.set_am_depth(0.02 * i);
            leslie_f.set_dir_depth_db(0.3 * i);
            leslie_f.set_drum_dir_depth_db(0.15 * i);
            leslie_f.set_dir_corner_hz(1000.0 + 90.0 * i);
            leslie_f.set_d_bias_ms(0.2 + 0.02 * i);
            leslie_f.set_reflection_db(-60.0 + 1.6 * i);
            leslie_f.set_num_reflections(1 + i % 4);
            leslie_f.set_refl_delay_ms(2.5 + 0.1 * i);
            leslie_f.set_refl_spacing_ms(1.0 + 0.06 * i);
            leslie_f.set_refl_corner_hz(1000.0 + 200.0 * i);
            leslie_f.set_drift_cents(0.3 * i);
            leslie_f.set_mix(0.03 * i);
            leslie_d.set_speed(speed);
            leslie_d.set_mix(0.03 * i);

            scanner_f.set_mode(static_cast<ScannerMode>(i % 7));
            scanner_f.set_scan_hz(6.0 + 0.04 * i);
            scanner_f.set_line_ms(0.6 + 0.02 * i);
            scanner_f.set_v1_frac(0.1 + 0.01 * i);
            scanner_f.set_v2_frac(0.4 + 0.01 * i);
            scanner_f.set_v3_frac(0.7 + 0.009 * i);
            scanner_f.set_chorus_mix(0.03 * i);
            scanner_d.set_mode(static_cast<ScannerMode>(i % 7));

            float la = 0.0f;
            float lb = 0.0f;
            leslie_f.process(0.5f, la, lb);
            leslie_f.process_block(in_f.data(), out_a.data(), out_b.data(),
                                   static_cast<int>(in_f.size()));
            double da = 0.0;
            double db = 0.0;
            leslie_d.process(0.5, da, db);
            leslie_d.process_block(in_d.data(), out_c.data(), out_e.data(),
                                   static_cast<int>(in_d.size()));
            (void)scanner_f.process(0.5f);
            scanner_f.process_block(in_f.data(), out_a.data(), static_cast<int>(in_f.size()));
            (void)scanner_d.process(0.5);
            scanner_d.process_block(in_d.data(), out_c.data(), static_cast<int>(in_d.size()));
        }
        leslie_f.reset();
        leslie_d.reset();
        scanner_f.reset();
        scanner_d.reset();
    });
}
TEST_CASE("Leslie and scanner recover from non-finite audio with controls retained",
          "[signal][leslie][nonfinite]") {
    for(double bad:{NAN,INFINITY,-INFINITY}){
        Leslie a,b;for(auto* x:{&a,&b}){x->prepare(kSr);x->set_horn_fast_hz(7);x->set_drum_fast_hz(5);x->set_mic_distance_m(1.7);x->set_mix(.73);x->reset();}
        a.set_horn_fast_hz(bad);a.set_drum_fast_hz(bad);a.set_mic_distance_m(bad);a.set_mix(bad);double l=1,r=1;a.process(bad,.2,l,r);REQUIRE(l==0);REQUIRE(r==0);b.reset();for(int i=0;i<64;++i){double bl=0,br=0;a.process(.2,.2,l,r);b.process(.2,.2,bl,br);REQUIRE(l==bl);REQUIRE(r==br);}
        Scanner sa,sb;for(auto* x:{&sa,&sb}){x->prepare(kSr);x->set_scan_hz(7.1);x->set_line_ms(1.3);x->set_v1_frac(.2);x->set_v2_frac(.5);x->set_v3_frac(.8);x->set_chorus_mix(.4);x->reset();}sa.set_scan_hz(bad);sa.set_line_ms(bad);sa.set_v1_frac(bad);sa.set_v2_frac(bad);sa.set_v3_frac(bad);sa.set_chorus_mix(bad);REQUIRE(sa.process(bad)==0);sb.reset();for(int i=0;i<64;++i)REQUIRE(sa.process(.2)==sb.process(.2));
    }
}

