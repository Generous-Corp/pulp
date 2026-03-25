#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/signal.hpp>
#include <cmath>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;

// ── DelayLine ────────────────────────────────────────────────────────────────

TEST_CASE("DelayLine integer delay", "[signal][delay]") {
    DelayLine dl;
    dl.prepare(100);

    // Push 10 samples: 1, 2, 3, ..., 10
    for (int i = 1; i <= 10; ++i) dl.push(static_cast<float>(i));

    // Read at delay 0 = most recent = 10
    REQUIRE_THAT(dl.read(0), WithinAbs(10.0, 0.01));

    // Read at delay 5 = 5 samples ago = 5
    REQUIRE_THAT(dl.read(5), WithinAbs(5.0, 0.01));
}

TEST_CASE("DelayLine fractional delay", "[signal][delay]") {
    DelayLine dl;
    dl.prepare(100);

    for (int i = 0; i < 10; ++i) dl.push(static_cast<float>(i));

    // Delay 0.5 should interpolate between two samples
    float val = dl.read(0.5f);
    REQUIRE(val > 8.0f);
    REQUIRE(val < 9.5f);
}

TEST_CASE("DelayLine process", "[signal][delay]") {
    DelayLine dl;
    dl.prepare(10);

    // Process with delay of 3
    dl.process(1.0f, 3.0f);
    dl.process(2.0f, 3.0f);
    dl.process(3.0f, 3.0f);
    float out = dl.process(4.0f, 3.0f);

    // After 4 pushes with delay 3, should get the first value
    REQUIRE_THAT(out, WithinAbs(1.0, 0.01));
}

// ── Gain ─────────────────────────────────────────────────────────────────────

TEST_CASE("Gain dB conversion", "[signal][gain]") {
    REQUIRE_THAT(db_to_linear(0.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(db_to_linear(6.0f), WithinAbs(1.995, 0.01));
    REQUIRE_THAT(db_to_linear(-6.0f), WithinAbs(0.501, 0.01));

    REQUIRE_THAT(linear_to_db(1.0f), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(linear_to_db(2.0f), WithinAbs(6.02, 0.1));
}

TEST_CASE("Gain processor", "[signal][gain]") {
    Gain g;
    g.set_gain_db(-6.0f);

    float out = g.process(1.0f);
    REQUIRE(out > 0.49f);
    REQUIRE(out < 0.51f);
}

TEST_CASE("DryWetMixer", "[signal][mix]") {
    DryWetMixer mixer;

    mixer.set_mix(0.0f); // Fully dry
    REQUIRE_THAT(mixer.process(1.0f, 0.5f), WithinAbs(1.0, 0.001));

    mixer.set_mix(1.0f); // Fully wet
    REQUIRE_THAT(mixer.process(1.0f, 0.5f), WithinAbs(0.5, 0.001));

    mixer.set_mix(0.5f); // 50/50
    REQUIRE_THAT(mixer.process(1.0f, 0.0f), WithinAbs(0.5, 0.001));
}

// ── Compressor ───────────────────────────────────────────────────────────────

TEST_CASE("Compressor reduces loud signals", "[signal][comp]") {
    Compressor comp;
    comp.set_sample_rate(44100.0f);
    comp.set_params({-20.0f, 4.0f, 0.1f, 50.0f, 0.0f, 0.0f});

    // Process a loud signal
    float loud = 1.0f; // 0 dBFS
    float compressed = 0;
    for (int i = 0; i < 1000; ++i) {
        compressed = comp.process(loud);
    }
    REQUIRE(std::abs(compressed) < 0.9f); // Should be reduced
}

TEST_CASE("Compressor passes quiet signals", "[signal][comp]") {
    Compressor comp;
    comp.set_sample_rate(44100.0f);
    comp.set_params({-10.0f, 4.0f, 1.0f, 50.0f, 0.0f, 0.0f});

    float quiet = 0.01f; // -40 dBFS, well below threshold
    float out = comp.process(quiet);
    REQUIRE_THAT(out, WithinAbs(quiet, 0.001));
}

// ── Limiter ──────────────────────────────────────────────────────────────────

TEST_CASE("Limiter caps at threshold", "[signal][limiter]") {
    Limiter lim;
    lim.set_sample_rate(44100.0f);
    lim.set_threshold_db(-6.0f); // ~0.5 linear

    // Process loud signal
    for (int i = 0; i < 100; ++i) {
        float out = lim.process(1.0f);
        REQUIRE(std::abs(out) <= 0.55f); // Should be limited near 0.5
    }
}

// ── SmoothedValue ────────────────────────────────────────────────────────────

TEST_CASE("SmoothedValue immediate set", "[signal][smooth]") {
    SmoothedValue<float> sv(0.0f);
    sv.set_immediate(1.0f);
    REQUIRE_THAT(sv.current(), WithinAbs(1.0, 0.001));
    REQUIRE_FALSE(sv.is_smoothing());
}

TEST_CASE("SmoothedValue ramp", "[signal][smooth]") {
    SmoothedValue<float> sv(0.0f);
    sv.set_ramp_time(0.1f, 1000.0f); // 100 sample ramp
    sv.set_target(1.0f);

    REQUIRE(sv.is_smoothing());
    float val = sv.next();
    REQUIRE(val > 0.0f);
    REQUIRE(val < 1.0f); // Should be partway

    // After enough samples, should reach target
    for (int i = 0; i < 200; ++i) sv.next();
    REQUIRE_THAT(sv.current(), WithinAbs(1.0, 0.01));
    REQUIRE_FALSE(sv.is_smoothing());
}

TEST_CASE("SmoothedValue skip", "[signal][smooth]") {
    SmoothedValue<float> sv(0.0f);
    sv.set_ramp_time(0.1f, 1000.0f); // 100 samples
    sv.set_target(1.0f);

    sv.skip(100);
    REQUIRE_THAT(sv.current(), WithinAbs(1.0, 0.001));
    REQUIRE_FALSE(sv.is_smoothing());
}

// ── ADSR ─────────────────────────────────────────────────────────────────────

TEST_CASE("ADSR idle by default", "[signal][adsr]") {
    Adsr env;
    REQUIRE_FALSE(env.is_active());
    REQUIRE(env.next() == 0.0f);
}

TEST_CASE("ADSR attack reaches peak", "[signal][adsr]") {
    Adsr env;
    env.set_sample_rate(1000.0f);
    env.set_params({0.01f, 0.1f, 0.5f, 0.1f}); // 10ms attack

    env.note_on();
    REQUIRE(env.is_active());

    // Run through attack
    float peak = 0;
    for (int i = 0; i < 20; ++i) {
        float v = env.next();
        if (v > peak) peak = v;
    }
    REQUIRE(peak > 0.9f); // Should have reached near 1.0
}

TEST_CASE("ADSR sustain level", "[signal][adsr]") {
    Adsr env;
    env.set_sample_rate(10000.0f);
    env.set_params({0.001f, 0.01f, 0.6f, 0.1f});

    env.note_on();

    // Run through attack + decay to sustain
    for (int i = 0; i < 200; ++i) env.next();

    float level = env.next();
    REQUIRE_THAT(level, WithinAbs(0.6, 0.05));
    REQUIRE(env.stage() == Adsr::Stage::sustain);
}

TEST_CASE("ADSR release to zero", "[signal][adsr]") {
    Adsr env;
    env.set_sample_rate(10000.0f);
    env.set_params({0.001f, 0.001f, 0.5f, 0.01f});

    env.note_on();
    for (int i = 0; i < 100; ++i) env.next();

    env.note_off();
    for (int i = 0; i < 200; ++i) env.next();

    REQUIRE_FALSE(env.is_active());
    REQUIRE_THAT(env.next(), WithinAbs(0.0, 0.001));
}

// ── Biquad ───────────────────────────────────────────────────────────────────

TEST_CASE("Biquad lowpass attenuates high frequencies", "[signal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 200.0f, 0.707f, 44100.0f);

    // Generate 10kHz sine (should be attenuated)
    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 10000.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 100) sum_sq += output * output; // Skip transient
    }
    float rms = std::sqrt(sum_sq / 4310.0f);
    REQUIRE(rms < 0.01f); // Should be heavily attenuated
}

TEST_CASE("Biquad lowpass passes low frequencies", "[signal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 5000.0f, 0.707f, 44100.0f);

    // Generate 100Hz sine (should pass through)
    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 200) sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms > 0.6f); // Should pass through mostly
}

TEST_CASE("Biquad highpass attenuates low frequencies", "[signal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::highpass, 5000.0f, 0.707f, 44100.0f);

    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 100.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 200) sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms < 0.05f);
}

TEST_CASE("Biquad reset clears state", "[signal][biquad]") {
    Biquad filter;
    filter.set_coefficients(Biquad::Type::lowpass, 1000.0f, 0.707f, 44100.0f);

    filter.process(1.0f);
    filter.process(0.5f);
    filter.reset();

    // After reset, should behave as if freshly initialized
    float out = filter.process(0.0f);
    REQUIRE_THAT(out, WithinAbs(0.0, 0.001));
}

// ── Oscillator ───────────────────────────────────────────────────────────────

TEST_CASE("Oscillator sine generates correct frequency", "[signal][osc]") {
    Oscillator osc;
    osc.set_sample_rate(44100.0f);
    osc.set_frequency(440.0f);
    osc.set_waveform(Oscillator::Waveform::sine);

    // Count zero crossings over 1 second (should be ~880 for 440Hz)
    int crossings = 0;
    float prev = osc.next();
    for (int i = 1; i < 44100; ++i) {
        float curr = osc.next();
        if ((prev >= 0 && curr < 0) || (prev < 0 && curr >= 0))
            ++crossings;
        prev = curr;
    }
    // 440Hz = 880 zero crossings per second
    REQUIRE(crossings > 870);
    REQUIRE(crossings < 890);
}

TEST_CASE("Oscillator saw produces aliased output", "[signal][osc]") {
    Oscillator osc;
    osc.set_sample_rate(44100.0f);
    osc.set_frequency(440.0f);
    osc.set_waveform(Oscillator::Waveform::saw);

    // Generate and check it produces non-zero output
    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float s = osc.next();
        sum_sq += s * s;
    }
    float rms = std::sqrt(sum_sq / 4410.0f);
    REQUIRE(rms > 0.3f); // Saw RMS ≈ 1/sqrt(3) ≈ 0.577
}

TEST_CASE("Oscillator square output", "[signal][osc]") {
    Oscillator osc;
    osc.set_sample_rate(44100.0f);
    osc.set_frequency(440.0f);
    osc.set_waveform(Oscillator::Waveform::square);

    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float s = osc.next();
        sum_sq += s * s;
    }
    float rms = std::sqrt(sum_sq / 4410.0f);
    REQUIRE(rms > 0.9f); // Square RMS ≈ 1.0
}

TEST_CASE("Oscillator triangle output", "[signal][osc]") {
    Oscillator osc;
    osc.set_sample_rate(44100.0f);
    osc.set_frequency(440.0f);
    osc.set_waveform(Oscillator::Waveform::triangle);

    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float s = osc.next();
        sum_sq += s * s;
    }
    float rms = std::sqrt(sum_sq / 4410.0f);
    REQUIRE(rms > 0.2f); // Triangle RMS ≈ 1/sqrt(3) ≈ 0.577
}
