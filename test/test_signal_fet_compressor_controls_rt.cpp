#include "test_signal_fet_compressor_support.hpp"

TEST_CASE("every control clamps to its published range", "[fet-compressor][controls]") {
    Comp c = probe(FetRatio::r8_1, 0.0);

    c.set_attack_us(1e9);
    const double slowest = c.attack_coefficient();
    c.set_attack_us(Comp::kAttackUsMax);
    REQUIRE_THAT(c.attack_coefficient(), WithinAbs(slowest, 1e-15));
    c.set_attack_us(-5.0);
    const double fastest = c.attack_coefficient();
    c.set_attack_us(Comp::kAttackUsMin);
    REQUIRE_THAT(c.attack_coefficient(), WithinAbs(fastest, 1e-15));

    c.set_release_ms(1e9);
    const double longest = c.release_coefficient();
    c.set_release_ms(Comp::kReleaseMsMax);
    REQUIRE_THAT(c.release_coefficient(), WithinAbs(longest, 1e-15));

    c.set_knee_db(1e6);
    REQUIRE_THAT(c.effective_knee_db(), WithinAbs(Comp::kKneeDbMax, 1e-15));
    c.set_knee_db(-1.0);
    REQUIRE_THAT(c.effective_knee_db(), WithinAbs(Comp::kKneeDbMin, 1e-15));

    c.set_mix(5.0);
    c.set_transformer_amount(5.0);
    c.set_input_gain_db(1e6);
    c.set_output_gain_db(1e6);
    REQUIRE_THAT(c.worst_case_gain(),
                 WithinRel(units::db_to_linear(Comp::kInputGainDbMax) *
                               c.resampler_peak_gain_bound() *
                               units::db_to_linear(Comp::kOutputGainDbMax),
                           1e-12));
}

TEST_CASE("the float and double instantiations agree", "[fet-compressor]") {
    FetCompressorT<float> f;
    FetCompressorT<double> d;
    f.prepare(kSr);
    d.prepare(kSr);
    f.set_ratio(FetRatio::all_buttons_in);
    d.set_ratio(FetRatio::all_buttons_in);
    f.set_input_gain_db(12.0);
    d.set_input_gain_db(12.0);
    f.set_transformer_amount(0.6);
    d.set_transformer_amount(0.6);
    f.reset();
    d.reset();

    REQUIRE(f.latency_samples() == d.latency_samples());

    const double w = kTwoPi * 1000.0 / kSr;
    for (int i = 0; i < static_cast<int>(kSr * 0.5); ++i) {
        const double x = 0.7 * std::sin(w * i);
        const double yf = f.process(static_cast<float>(x));
        const double yd = d.process(x);
        REQUIRE_THAT(yf, WithinAbs(yd, 1e-5));
    }
}

TEST_CASE("49 the compressor allocates nothing on the audio thread",
          "[fet-compressor][rt-safety]") {
    FetCompressorT<float> f;
    FetCompressorT<double> d;
    f.prepare(kSr);
    d.prepare(kSr);

    std::vector<float> block_f(256, 0.1f);
    std::vector<double> block_d(256, 0.1);

    require_allocates_no_memory([&] {
        for (int i = 0; i < 64; ++i) {
            const auto ratio = static_cast<FetRatio>(i % 5);
            f.set_ratio(ratio);
            f.set_input_gain_db(-20.0 + i);
            f.set_output_gain_db(-20.0 + 0.5 * i);
            f.set_attack_us(20.0 + 10.0 * i);
            f.set_release_ms(50.0 + 10.0 * i);
            f.set_knee_db(0.1 * (i % 60));
            f.set_transformer_amount(0.01 * (i % 100));
            f.set_mix(0.01 * (i % 100));

            (void)f.process(0.5f);
            f.process_block(block_f.data(), static_cast<int>(block_f.size()));

            d.set_ratio(ratio);
            d.set_input_gain_db(-20.0 + i);
            (void)d.process(0.5);
            d.process_block(block_d.data(), static_cast<int>(block_d.size()));
        }
        f.reset();
        d.reset();
    });
}

TEST_CASE("a NaN sample cannot latch the FET feedback detector",
          "[fet-compressor][nan-recovery]") {
    for (double sample_rate : {8000.0, 192000.0}) {
        Comp c;
        c.prepare(sample_rate);
        c.set_input_gain_db(12.0);
        c.set_ratio(FetRatio::r8_1);
        for (int i = 0; i < static_cast<int>(sample_rate * 0.1); ++i) c.process(0.5);

        c.process(std::numeric_limits<double>::quiet_NaN());
        for (int i = 0; i < 2 * Comp::kLatencySamples + 64; ++i) {
            REQUIRE(std::isfinite(c.process(0.25)));
            REQUIRE(std::isfinite(c.gain_reduction_db()));
            REQUIRE(std::isfinite(c.control_voltage()));
        }
    }
}

TEST_CASE("non-finite FET controls retain the last valid configuration",
          "[fet-compressor][nan-recovery]") {
    for (double bad : {std::numeric_limits<double>::quiet_NaN(),
                       std::numeric_limits<double>::infinity(),
                       -std::numeric_limits<double>::infinity()}) {
        Comp c, reference;
        for (auto* x : {&c, &reference}) {
            x->prepare(kSr); x->set_input_gain_db(11.0); x->set_output_gain_db(-7.0);
            x->set_attack_us(137.0); x->set_release_ms(777.0); x->set_knee_db(2.5);
            x->set_transformer_amount(0.68); x->set_mix(0.43);
        }
        c.set_input_gain_db(bad); c.set_output_gain_db(bad); c.set_attack_us(bad);
        c.set_release_ms(bad); c.set_knee_db(bad); c.set_transformer_amount(bad); c.set_mix(bad);
        for (int i = 0; i < Comp::kLatencySamples + 512; ++i) {
            const double sample = 0.25 * std::sin(0.031 * i);
            REQUIRE(c.process(sample) == reference.process(sample));
        }
    }
}
