#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/signal.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <utility>
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

TEST_CASE("DelayLine handles empty, wraparound, and reset edges",
          "[signal][delay][issue-645]") {
    DelayLine dl;
    REQUIRE_THAT(dl.read(0), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(dl.read(0.5f), WithinAbs(0.0f, 1e-6f));
    REQUIRE(dl.max_delay() == -1);

    dl.prepare(2);
    REQUIRE(dl.max_delay() == 2);

    dl.push(1.0f);
    dl.push(2.0f);
    dl.push(3.0f);

    REQUIRE_THAT(dl.read(0), WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(dl.read(1), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(dl.read(1.5f), WithinAbs(1.5f, 1e-6f));

    dl.reset();
    REQUIRE_THAT(dl.read(0), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(dl.process(4.0f, 0.0f), WithinAbs(4.0f, 1e-6f));
}

TEST_CASE("DelayLine zero max delay uses a stable single-sample buffer",
          "[signal][delay][coverage][phase3-github]") {
    DelayLine dl;
    dl.prepare(0);

    REQUIRE(dl.max_delay() == 0);
    REQUIRE_THAT(dl.read(0), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(dl.read(4), WithinAbs(0.0f, 1e-6f));

    dl.push(0.25f);
    REQUIRE_THAT(dl.read(0), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(dl.read(99), WithinAbs(0.25f, 1e-6f));

    dl.reset();
    REQUIRE_THAT(dl.read(0), WithinAbs(0.0f, 1e-6f));
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

TEST_CASE("Gain linear setter and buffer processing", "[signal][gain][issue-645]") {
    Gain g;
    g.set_gain_linear(0.25f);
    REQUIRE_THAT(g.gain_linear(), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(g.gain_db(), WithinAbs(-12.0412f, 0.01f));

    float buffer[] = {1.0f, -2.0f, 0.5f};
    g.process(buffer, 3);
    REQUIRE_THAT(buffer[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(0.125f, 1e-6f));
}

TEST_CASE("Gain dB conversion floors zero and negative linear values",
          "[signal][gain][coverage][phase3]") {
    REQUIRE_THAT(linear_to_db(0.0f), WithinAbs(-200.0f, 0.001f));
    REQUIRE_THAT(linear_to_db(-1.0f), WithinAbs(-200.0f, 0.001f));

    Gain g;
    g.set_gain_linear(0.0f);
    REQUIRE_THAT(g.gain_db(), WithinAbs(-200.0f, 0.001f));
    REQUIRE_THAT(g.process(0.5f), WithinAbs(0.0f, 1e-6f));

    float buffer[] = {1.0f, -1.0f};
    g.process(buffer, 0);
    REQUIRE_THAT(buffer[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(-1.0f, 1e-6f));
}

TEST_CASE("Gain zero-length buffer calls leave sentinels untouched",
          "[signal][gain][coverage][phase3-github]") {
    Gain g;
    g.set_gain_linear(-2.0f);

    float buffer[] = {0.25f, -0.5f};
    g.process(buffer, 0);

    REQUIRE_THAT(buffer[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(g.process(0.5f), WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(g.gain_db(), WithinAbs(-200.0f, 1e-3f));
}

TEST_CASE("SimpleMixer", "[signal][mix]") {
    SimpleMixer mixer;

    mixer.set_mix(0.0f); // Fully dry
    REQUIRE_THAT(mixer.process(1.0f, 0.5f), WithinAbs(1.0, 0.001));

    mixer.set_mix(1.0f); // Fully wet
    REQUIRE_THAT(mixer.process(1.0f, 0.5f), WithinAbs(0.5, 0.001));

    mixer.set_mix(0.5f); // 50/50
    REQUIRE_THAT(mixer.process(1.0f, 0.0f), WithinAbs(0.5, 0.001));
}

TEST_CASE("SimpleMixer clamps mix and processes buffers",
          "[signal][mix][issue-645]") {
    SimpleMixer mixer;
    mixer.set_mix(-1.0f);
    REQUIRE_THAT(mixer.mix(), WithinAbs(0.0f, 1e-6f));
    mixer.set_mix(2.0f);
    REQUIRE_THAT(mixer.mix(), WithinAbs(1.0f, 1e-6f));

    mixer.set_mix(0.25f);
    const float dry[] = {1.0f, 0.0f, -1.0f};
    const float wet[] = {0.0f, 1.0f, 1.0f};
    float output[] = {9.0f, 9.0f, 9.0f};

    mixer.process(dry, wet, output, 3);
    REQUIRE_THAT(output[0], WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(output[1], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(output[2], WithinAbs(-0.5f, 1e-6f));
}

TEST_CASE("SimpleMixer zero-length buffer processing leaves output untouched",
          "[signal][mix][coverage][phase3]") {
    SimpleMixer mixer;
    mixer.set_mix(0.5f);

    const float dry[] = {1.0f};
    const float wet[] = {-1.0f};
    float output[] = {42.0f};
    mixer.process(dry, wet, output, 0);

    REQUIRE_THAT(output[0], WithinAbs(42.0f, 1e-6f));
}

TEST_CASE("SimpleMixer scalar path clamps endpoint mixes",
          "[signal][mix][coverage][phase3-github]") {
    SimpleMixer mixer;

    mixer.set_mix(-0.25f);
    REQUIRE_THAT(mixer.mix(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(mixer.process(-0.5f, 0.75f), WithinAbs(-0.5f, 1e-6f));

    mixer.set_mix(1.25f);
    REQUIRE_THAT(mixer.mix(), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(mixer.process(-0.5f, 0.75f), WithinAbs(0.75f, 1e-6f));
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

TEST_CASE("Compressor hard knee uses instant attack release and reset", "[signal][comp][issue-645]") {
    Compressor comp;
    comp.set_sample_rate(1000.0f);
    comp.set_params({-20.0f, 2.0f, 0.0f, 0.0f, 0.0f, 0.0f});

    REQUIRE_THAT(comp.process(0.1f), WithinAbs(0.1f, 1e-5f));
    REQUIRE_THAT(comp.process(1.0f), WithinAbs(0.316228f, 1e-4f));
    REQUIRE(comp.gain_reduction_db() < -9.9f);

    REQUIRE_THAT(comp.process(0.01f), WithinAbs(0.01f, 1e-5f));
    REQUIRE_THAT(comp.gain_reduction_db(), WithinAbs(0.0f, 1e-5f));

    comp.process(1.0f);
    comp.reset();
    REQUIRE_THAT(comp.gain_reduction_db(), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Compressor soft knee and buffer processing are deterministic", "[signal][comp][issue-645]") {
    Compressor comp;
    comp.set_sample_rate(1000.0f);
    comp.set_params({-20.0f, 4.0f, 0.0f, 0.0f, 10.0f, 0.0f});

    float knee_sample = comp.process(0.1f);
    REQUIRE(knee_sample < 0.1f);
    REQUIRE(knee_sample > 0.085f);

    float buffer[] = {0.01f, 0.1f, 1.0f};
    comp.process(buffer, 3);
    REQUIRE_THAT(buffer[0], WithinAbs(0.01f, 1e-5f));
    REQUIRE(buffer[1] < 0.1f);
    REQUIRE(buffer[2] < 0.2f);
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

TEST_CASE("Limiter buffer processing and reset restore clean state", "[signal][limiter][issue-645]") {
    Limiter lim;
    lim.set_sample_rate(1000.0f);
    lim.set_threshold_db(-6.0f);
    lim.set_release_ms(1.0f);

    float buffer[] = {2.0f, 0.25f, -2.0f};
    lim.process(buffer, 3);
    REQUIRE(std::abs(buffer[0]) <= 0.51f);
    REQUIRE(std::abs(buffer[2]) <= 0.51f);

    lim.reset();
    REQUIRE_THAT(lim.process(0.25f), WithinAbs(0.25f, 1e-5f));
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

TEST_CASE("SmoothedValue clamps one-sample ramps and partially skips",
          "[signal][smooth][issue-645]") {
    SmoothedValue<float> immediate(0.0f);
    immediate.set_ramp_time(0.0f, 48000.0f);
    immediate.set_target(2.0f);

    REQUIRE_THAT(immediate.current(), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(immediate.target(), WithinAbs(2.0f, 1e-6f));
    REQUIRE_FALSE(immediate.is_smoothing());
    immediate.skip(4);
    REQUIRE_THAT(immediate.next(), WithinAbs(2.0f, 1e-6f));

    SmoothedValue<float> partial(0.0f);
    partial.set_ramp_time(0.01f, 1000.0f);
    partial.set_target(10.0f);
    partial.skip(3);

    REQUIRE(partial.is_smoothing());
    REQUIRE_THAT(partial.current(), WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(partial.next(), WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(partial.target(), WithinAbs(10.0f, 1e-6f));
}

TEST_CASE("SmoothedValue ignores non-positive skips",
          "[signal][smooth][coverage][phase3]") {
    SmoothedValue<float> sv(0.0f);
    sv.set_ramp_time(0.01f, 1000.0f);
    sv.set_target(10.0f);

    sv.skip(0);
    sv.skip(-4);

    REQUIRE(sv.is_smoothing());
    REQUIRE_THAT(sv.current(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(sv.next(), WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("SmoothedValue double precision path reaches targets after skip",
          "[signal][smooth][coverage][phase3-github]") {
    SmoothedValue<double> sv(2.0);
    sv.set_ramp_time(0.004, 1000.0);
    sv.set_target(6.0);

    REQUIRE(sv.is_smoothing());
    sv.skip(2);
    REQUIRE_THAT(sv.current(), WithinAbs(4.0, 1e-12));

    sv.skip(8);
    REQUIRE_FALSE(sv.is_smoothing());
    REQUIRE_THAT(sv.current(), WithinAbs(6.0, 1e-12));
    REQUIRE_THAT(sv.next(), WithinAbs(6.0, 1e-12));
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

TEST_CASE("ADSR handles immediate stages, idle note_off, and reset",
          "[signal][adsr][issue-645]") {
    Adsr env;
    env.set_sample_rate(1000.0f);
    env.set_params({0.0f, -0.01f, 0.25f, 0.0f});

    env.note_off();
    REQUIRE_FALSE(env.is_active());
    REQUIRE(env.stage() == Adsr::Stage::idle);
    REQUIRE_THAT(env.next(), WithinAbs(0.0f, 1e-6f));

    env.note_on();
    REQUIRE(env.stage() == Adsr::Stage::attack);
    REQUIRE_THAT(env.next(), WithinAbs(1.0f, 1e-6f));
    REQUIRE(env.stage() == Adsr::Stage::decay);
    REQUIRE_THAT(env.next(), WithinAbs(0.25f, 1e-6f));
    REQUIRE(env.stage() == Adsr::Stage::sustain);
    REQUIRE_THAT(env.next(), WithinAbs(0.25f, 1e-6f));

    env.note_off();
    REQUIRE(env.stage() == Adsr::Stage::release);
    REQUIRE_THAT(env.next(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(env.is_active());
    REQUIRE(env.stage() == Adsr::Stage::idle);

    env.note_on();
    REQUIRE_THAT(env.next(), WithinAbs(1.0f, 1e-6f));
    env.reset();
    REQUIRE_FALSE(env.is_active());
    REQUIRE(env.stage() == Adsr::Stage::idle);
    REQUIRE_THAT(env.next(), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("ADSR retrigger continues from current release level",
          "[signal][adsr][coverage][phase3]") {
    Adsr env;
    env.set_sample_rate(1000.0f);
    env.set_params({0.01f, 0.01f, 0.5f, 0.02f});

    env.note_on();
    for (int i = 0; i < 10; ++i) env.next();
    REQUIRE(env.stage() == Adsr::Stage::decay);

    env.note_off();
    float release_level = 0.0f;
    for (int i = 0; i < 5; ++i) release_level = env.next();
    REQUIRE(env.stage() == Adsr::Stage::release);
    REQUIRE(release_level > 0.0f);
    REQUIRE(release_level < 1.0f);

    env.note_on();
    REQUIRE(env.stage() == Adsr::Stage::attack);
    const float retriggered = env.next();

    REQUIRE(retriggered > release_level);
    REQUIRE(retriggered < 1.0f);
    REQUIRE(env.is_active());
}

TEST_CASE("ADSR retrigger continues from current level",
          "[signal][adsr][coverage][phase3-github]") {
    Adsr env;
    env.set_sample_rate(100.0f);
    env.set_params({0.1f, 0.2f, 0.25f, 0.1f});

    env.note_on();
    const float first = env.next();
    const float second = env.next();
    REQUIRE(second > first);

    env.note_on();
    const float retriggered = env.next();
    REQUIRE(retriggered > second);
    REQUIRE(env.is_active());
    REQUIRE(env.stage() == Adsr::Stage::attack);
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

TEST_CASE("Biquad coefficient variants process finite impulse buffers", "[signal][biquad]") {
    struct Case {
        Biquad::Type type;
        float gain_db;
    };

    const std::array<Case, 6> cases{{
        {Biquad::Type::bandpass, 0.0f},
        {Biquad::Type::notch, 0.0f},
        {Biquad::Type::allpass, 0.0f},
        {Biquad::Type::peaking, 6.0f},
        {Biquad::Type::low_shelf, 6.0f},
        {Biquad::Type::high_shelf, -6.0f},
    }};

    for (const auto& c : cases) {
        Biquad filter;
        filter.set_coefficients(c.type, 1200.0f, 0.707f, 48000.0f, c.gain_db);

        float impulse[] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        filter.process(impulse, 8);

        for (float sample : impulse) {
            REQUIRE(std::isfinite(sample));
        }
    }
}

// ── Oversampler ──────────────────────────────────────────────────────────────

TEST_CASE("Oversampler dispatches x2 and x4 callback phases",
          "[signal][oversampling][issue-645]") {
    Oversampler os;
    std::vector<float> callback_inputs;

    float x2 = os.process(0.25f, [&](float sample) {
        callback_inputs.push_back(sample);
        return sample + 1.0f;
    });

    REQUIRE(callback_inputs.size() == 2);
    REQUIRE_THAT(callback_inputs[0], WithinAbs(0.5f, 1e-5));
    REQUIRE_THAT(callback_inputs[1], WithinAbs(0.0f, 1e-5));
    REQUIRE_THAT(x2, WithinAbs(1.5f, 1e-5));

    os.set_factor(Oversampler::Factor::x4);
    callback_inputs.clear();

    float x4 = os.process(0.25f, [&](float sample) {
        callback_inputs.push_back(sample);
        return sample * 2.0f;
    });

    REQUIRE(callback_inputs.size() == 4);
    REQUIRE_THAT(callback_inputs[0], WithinAbs(1.0f, 1e-5));
    REQUIRE_THAT(callback_inputs[1], WithinAbs(0.0f, 1e-5));
    REQUIRE_THAT(callback_inputs[2], WithinAbs(0.0f, 1e-5));
    REQUIRE_THAT(callback_inputs[3], WithinAbs(0.0f, 1e-5));
    REQUIRE_THAT(x4, WithinAbs(2.0f, 1e-5));
}

TEST_CASE("Oversampler configured filters reset deterministically",
          "[signal][oversampling][issue-645]") {
    Oversampler os;
    os.set_factor(Oversampler::Factor::x4);
    os.set_sample_rate(48000.0f);

    int callback_count = 0;
    auto passthrough = [&](float sample) {
        ++callback_count;
        return sample;
    };

    float first = os.process(1.0f, passthrough);
    float tail = os.process(0.0f, passthrough);

    REQUIRE(callback_count == 8);
    REQUIRE(std::isfinite(first));
    REQUIRE(std::isfinite(tail));
    REQUIRE(first > 0.0f);

    os.reset();
    callback_count = 0;
    float after_reset = os.process(1.0f, passthrough);

    REQUIRE(callback_count == 4);
    REQUIRE_THAT(after_reset, WithinAbs(first, 1e-6));
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

TEST_CASE("Oscillator reset, phase wrap, and PolyBLEP edges are deterministic",
          "[signal][osc][issue-645]") {
    Oscillator sine;
    sine.set_sample_rate(8.0f);
    sine.set_frequency(2.0f);
    sine.set_waveform(Oscillator::Waveform::sine);

    REQUIRE_THAT(sine.frequency(), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(sine.phase(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(sine.next(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(sine.phase(), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(sine.next(), WithinAbs(1.0f, 1e-6f));
    sine.reset();
    REQUIRE_THAT(sine.phase(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(sine.next(), WithinAbs(0.0f, 1e-6f));

    Oscillator saw;
    saw.set_sample_rate(10.0f);
    saw.set_frequency(3.0f);
    saw.set_waveform(Oscillator::Waveform::saw);

    for (int i = 0; i < 4; ++i) {
        REQUIRE(std::isfinite(saw.next()));
    }
    REQUIRE_THAT(saw.phase(), WithinAbs(0.2f, 1e-6f));

    Oscillator square;
    square.set_sample_rate(10.0f);
    square.set_frequency(3.0f);
    square.set_waveform(Oscillator::Waveform::square);

    for (int i = 0; i < 4; ++i) {
        REQUIRE(std::isfinite(square.next()));
    }
    REQUIRE_THAT(square.phase(), WithinAbs(0.2f, 1e-6f));

    Oscillator triangle;
    triangle.set_sample_rate(10.0f);
    triangle.set_frequency(3.0f);
    triangle.set_waveform(Oscillator::Waveform::triangle);

    for (int i = 0; i < 4; ++i) {
        REQUIRE(std::isfinite(triangle.next()));
    }
    REQUIRE_THAT(triangle.phase(), WithinAbs(0.2f, 1e-6f));
}

TEST_CASE("Oscillator zero frequency leaves phase fixed across waveforms",
          "[signal][osc][coverage][phase3-github]") {
    for (auto waveform : {
             Oscillator::Waveform::sine,
             Oscillator::Waveform::saw,
             Oscillator::Waveform::square,
             Oscillator::Waveform::triangle,
         }) {
        Oscillator osc;
        osc.set_frequency(0.0f);
        osc.set_waveform(waveform);

        const float first = osc.next();
        const float second = osc.next();
        REQUIRE(std::isfinite(first));
        REQUIRE_THAT(second, WithinAbs(first, 1e-6f));
        REQUIRE_THAT(osc.phase(), WithinAbs(0.0f, 1e-6f));
    }
}

// ── SVF ──────────────────────────────────────────────────────────────────────

TEST_CASE("SVF lowpass attenuates high frequencies", "[signal][svf]") {
    Svf filter;
    filter.set_sample_rate(44100.0f);
    filter.set_frequency(200.0f);
    filter.set_resonance(0.707f);
    filter.set_mode(Svf::Mode::lowpass);

    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 10000.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 100) sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4310.0f);
    REQUIRE(rms < 0.01f);
}

TEST_CASE("SVF highpass passes high frequencies", "[signal][svf]") {
    Svf filter;
    filter.set_sample_rate(44100.0f);
    filter.set_frequency(200.0f);
    filter.set_resonance(0.707f);
    filter.set_mode(Svf::Mode::highpass);

    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 5000.0f * i / 44100.0f);
        float output = filter.process(input);
        if (i > 200) sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms > 0.5f);
}

TEST_CASE("SVF covers bandpass notch buffer and reset paths",
          "[signal][svf][issue-645]") {
    Svf filter;
    filter.set_sample_rate(48000.0f);
    filter.set_frequency(1000.0f);
    filter.set_resonance(0.9f);

    filter.set_mode(Svf::Mode::bandpass);
    float band = filter.process(1.0f);
    REQUIRE(std::isfinite(band));

    filter.set_mode(Svf::Mode::notch);
    float notched = filter.process(1.0f);
    REQUIRE(std::isfinite(notched));

    float buffer[] = {1.0f, 0.5f, 0.0f, -0.5f};
    filter.process(buffer, 4);
    for (float sample : buffer) {
        REQUIRE(std::isfinite(sample));
    }

    filter.reset();
    filter.set_mode(Svf::Mode::lowpass);
    float after_reset = filter.process(0.0f);
    REQUIRE_THAT(after_reset, WithinAbs(0.0f, 1e-6f));
}

// ── WaveShaper ───────────────────────────────────────────────────────────────

TEST_CASE("WaveShaper tanh clips", "[signal][waveshaper]") {
    WaveShaper ws;
    ws.set_curve(WaveShaper::Curve::tanh_clip);
    ws.set_drive(5.0f);

    float out = ws.process(1.0f);
    REQUIRE(out > 0.9f);
    REQUIRE(out < 1.0f); // tanh(5) ≈ 0.9999
}

TEST_CASE("WaveShaper hard clip", "[signal][waveshaper]") {
    WaveShaper ws;
    ws.set_curve(WaveShaper::Curve::hard_clip);
    ws.set_drive(3.0f);

    REQUIRE_THAT(ws.process(1.0f), WithinAbs(1.0, 0.001));
    REQUIRE_THAT(ws.process(-1.0f), WithinAbs(-1.0, 0.001));
}

TEST_CASE("WaveShaper fold", "[signal][waveshaper]") {
    WaveShaper ws;
    ws.set_curve(WaveShaper::Curve::fold);
    ws.set_drive(2.0f);

    float out = ws.process(0.75f); // 0.75 * 2 = 1.5, folds to 0.5
    REQUIRE(out > -1.0f);
    REQUIRE(out < 1.0f);
}

// ── NoiseGate ────────────────────────────────────────────────────────────────

TEST_CASE("NoiseGate attenuates quiet signals", "[signal][gate]") {
    NoiseGate gate;
    gate.set_sample_rate(44100.0f);
    gate.set_params({-30.0f, 10.0f, 0.1f, 10.0f, -80.0f});

    // Process a quiet signal well below threshold
    float quiet = 0.001f; // -60 dBFS
    float out = 0;
    for (int i = 0; i < 1000; ++i)
        out = gate.process(quiet);

    REQUIRE(std::abs(out) < std::abs(quiet)); // Should be attenuated
}

TEST_CASE("NoiseGate passes loud signals", "[signal][gate]") {
    NoiseGate gate;
    gate.set_sample_rate(44100.0f);
    gate.set_params({-30.0f, 10.0f, 0.1f, 10.0f, -80.0f});

    float loud = 0.5f; // -6 dBFS, well above threshold
    float out = gate.process(loud);
    REQUIRE_THAT(out, WithinAbs(loud, 0.01));
}

TEST_CASE("NoiseGate clamps range, instant timing, reset, and buffers",
          "[signal][gate][issue-645]") {
    NoiseGate instant;
    instant.set_sample_rate(1000.0f);
    instant.set_params({-20.0f, 20.0f, 0.0f, 0.0f, -12.0f});

    REQUIRE(instant.process(0.0f) == 0.0f);

    const float quiet = 0.001f;
    const float expected_range_gain = std::pow(10.0f, -12.0f / 20.0f);
    REQUIRE_THAT(instant.process(quiet),
                 WithinAbs(quiet * expected_range_gain, 1e-7f));
    REQUIRE_THAT(instant.process(1.0f), WithinAbs(1.0f, 1e-6f));

    float buffer[] = {quiet, 1.0f, -quiet};
    instant.process(buffer, 3);
    REQUIRE_THAT(buffer[0], WithinAbs(quiet * expected_range_gain, 1e-7f));
    REQUIRE_THAT(buffer[1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(-quiet * expected_range_gain, 1e-7f));

    NoiseGate held;
    held.set_sample_rate(1000.0f);
    held.set_params({-20.0f, 20.0f, 0.0f, 1000.0f, -12.0f});
    REQUIRE(std::abs(held.process(quiet)) < quiet);
    REQUIRE(held.process(1.0f) < 1.0f);
    held.reset();
    REQUIRE_THAT(held.process(1.0f), WithinAbs(1.0f, 1e-6f));
}

// ── Panner ───────────────────────────────────────────────────────────────────

TEST_CASE("Panner center", "[signal][panner]") {
    Panner pan;
    pan.set_pan(0.0f); // Center

    auto result = pan.process(1.0f);
    REQUIRE_THAT(result.left, WithinAbs(result.right, 0.01));
    // Equal power: at center both should be ~0.707
    REQUIRE(result.left > 0.6f);
    REQUIRE(result.right > 0.6f);
}

TEST_CASE("Panner hard left", "[signal][panner]") {
    Panner pan;
    pan.set_pan(-1.0f);

    auto result = pan.process(1.0f);
    REQUIRE_THAT(result.left, WithinAbs(1.0, 0.01));
    REQUIRE_THAT(result.right, WithinAbs(0.0, 0.01));
}

TEST_CASE("Panner hard right", "[signal][panner]") {
    Panner pan;
    pan.set_pan(1.0f);

    auto result = pan.process(1.0f);
    REQUIRE_THAT(result.left, WithinAbs(0.0, 0.01));
    REQUIRE_THAT(result.right, WithinAbs(1.0, 0.01));
}

TEST_CASE("Panner clamps and processes stereo in place",
          "[signal][panner][issue-645]") {
    Panner pan;
    pan.set_pan(-2.0f);
    REQUIRE_THAT(pan.pan(), WithinAbs(-1.0f, 1e-6f));
    pan.set_pan(2.0f);
    REQUIRE_THAT(pan.pan(), WithinAbs(1.0f, 1e-6f));

    pan.set_pan(0.0f);
    float left = 2.0f;
    float right = -2.0f;
    pan.process(left, right);

    REQUIRE_THAT(left, WithinAbs(std::sqrt(2.0f), 1e-5f));
    REQUIRE_THAT(right, WithinAbs(-std::sqrt(2.0f), 1e-5f));
}

// ── Chorus ───────────────────────────────────────────────────────────────────

TEST_CASE("Chorus produces stereo output", "[signal][chorus]") {
    Chorus chorus;
    chorus.prepare(44100.0f);
    chorus.set_rate(1.0f);
    chorus.set_depth(0.5f);
    chorus.set_mix(0.5f);

    float sum_l = 0, sum_r = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
        auto out = chorus.process(input);
        sum_l += out.left * out.left;
        sum_r += out.right * out.right;
    }
    REQUIRE(sum_l > 0);
    REQUIRE(sum_r > 0);
    // Left and right should differ (stereo widening)
    REQUIRE(std::abs(sum_l - sum_r) > 0.01f);
}

TEST_CASE("Chorus dry mix, phase wrap, and reset are deterministic",
          "[signal][chorus][issue-645]") {
    Chorus dry;
    dry.prepare(32.0f);
    dry.set_rate(32.0f);
    dry.set_depth(1.0f);
    dry.set_delay_ms(20.0f);
    dry.set_mix(0.0f);

    for (float input : {1.0f, -0.5f, 0.25f}) {
        auto out = dry.process(input);
        REQUIRE_THAT(out.left, WithinAbs(input, 1e-6f));
        REQUIRE_THAT(out.right, WithinAbs(input, 1e-6f));
    }

    Chorus chorus;
    chorus.prepare(1000.0f);
    chorus.set_rate(1000.0f);
    chorus.set_depth(1.0f);
    chorus.set_delay_ms(1.0f);
    chorus.set_mix(1.0f);

    const std::array<float, 6> input{{1.0f, 0.5f, -0.25f, 0.0f, 0.125f, 0.0f}};
    std::array<Chorus::StereoSample, input.size()> first{};
    std::array<Chorus::StereoSample, input.size()> second{};

    for (size_t i = 0; i < input.size(); ++i) {
        first[i] = chorus.process(input[i]);
        REQUIRE(std::isfinite(first[i].left));
        REQUIRE(std::isfinite(first[i].right));
    }

    chorus.reset();
    for (size_t i = 0; i < input.size(); ++i) {
        second[i] = chorus.process(input[i]);
        REQUIRE_THAT(second[i].left, WithinAbs(first[i].left, 1e-6f));
        REQUIRE_THAT(second[i].right, WithinAbs(first[i].right, 1e-6f));
    }
}

// ── Phaser ───────────────────────────────────────────────────────────────────

TEST_CASE("Phaser modifies signal", "[signal][phaser]") {
    Phaser phaser;
    phaser.set_sample_rate(44100.0f);
    phaser.set_rate(1.0f);
    phaser.set_depth(0.8f);
    phaser.set_mix(1.0f);

    float sum_in = 0, sum_diff = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);
        float output = phaser.process(input);
        sum_in += input * input;
        sum_diff += (output - input) * (output - input);
    }
    // Output should differ from input (phase cancellation effects)
    REQUIRE(sum_in > 0.0f);
    REQUIRE(sum_diff > 0.01f);
}

TEST_CASE("Phaser clamps stage and feedback settings and resets",
          "[signal][phaser][issue-645]") {
    Phaser phaser;
    phaser.set_sample_rate(48000.0f);
    phaser.set_rate(2.0f);
    phaser.set_depth(1.0f);
    phaser.set_feedback(4.0f);
    phaser.set_stages(99);
    phaser.set_mix(0.75f);

    std::array<float, 5> first{{1.0f, 0.25f, -0.5f, 0.0f, 0.125f}};
    auto second = first;

    phaser.process(first.data(), static_cast<int>(first.size()));
    for (float sample : first) {
        REQUIRE(std::isfinite(sample));
    }

    phaser.reset();
    phaser.process(second.data(), static_cast<int>(second.size()));
    for (size_t i = 0; i < first.size(); ++i) {
        REQUIRE_THAT(first[i], WithinAbs(second[i], 1e-6f));
    }

    Phaser dry;
    dry.set_mix(0.0f);
    REQUIRE_THAT(dry.process(-0.5f), WithinAbs(-0.5f, 1e-6f));
}

TEST_CASE("Phaser zero-length block processing is a no-op",
          "[signal][phaser][coverage][phase3-github]") {
    Phaser phaser;
    phaser.set_sample_rate(48000.0f);
    phaser.set_mix(1.0f);

    float samples[] = {0.125f, -0.25f};
    phaser.process(samples, 0);

    REQUIRE_THAT(samples[0], WithinAbs(0.125f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(-0.25f, 1e-6f));
}

// ── Reverb ───────────────────────────────────────────────────────────────────

TEST_CASE("Reverb produces decay tail", "[signal][reverb]") {
    Reverb reverb;
    reverb.prepare(44100.0f);
    reverb.set_decay(1.0f);
    reverb.set_mix(1.0f);

    // Feed an impulse
    auto out = reverb.process(1.0f);
    (void)out;

    // Feed silence and check for decay tail
    float energy = 0;
    for (int i = 0; i < 4410; ++i) {
        auto s = reverb.process(0.0f);
        energy += s.left * s.left + s.right * s.right;
    }
    REQUIRE(energy > 0.001f); // Should have reverb tail
}

TEST_CASE("Reverb handles zero decay, damping clamp, dry mix, and reset",
          "[signal][reverb][issue-645]") {
    Reverb reverb;
    reverb.prepare(441.0f);
    reverb.set_decay(0.0f);
    reverb.set_damping(-1.0f);
    reverb.set_mix(1.0f);

    (void)reverb.process(1.0f);

    float early_energy = 0.0f;
    float late_energy = 0.0f;
    for (int i = 0; i < 40; ++i) {
        auto out = reverb.process(0.0f);
        REQUIRE(std::isfinite(out.left));
        REQUIRE(std::isfinite(out.right));
        float energy = out.left * out.left + out.right * out.right;
        if (i < 20) {
            early_energy += energy;
        } else {
            late_energy += energy;
        }
    }

    REQUIRE(early_energy > 0.0f);
    REQUIRE_THAT(late_energy, WithinAbs(0.0f, 1e-6f));

    reverb.set_decay(0.5f);
    reverb.set_damping(4.0f);
    (void)reverb.process(1.0f);
    reverb.reset();

    for (int i = 0; i < 24; ++i) {
        auto out = reverb.process(0.0f);
        REQUIRE_THAT(out.left, WithinAbs(0.0f, 1e-6f));
        REQUIRE_THAT(out.right, WithinAbs(0.0f, 1e-6f));
    }

    Reverb dry;
    dry.prepare(441.0f);
    dry.set_decay(2.0f);
    dry.set_damping(2.0f);
    dry.set_mix(0.0f);

    auto out = dry.process(0.25f);
    REQUIRE_THAT(out.left, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(out.right, WithinAbs(0.25f, 1e-6f));
}

// ── LadderFilter ─────────────────────────────────────────────────────────────

TEST_CASE("LadderFilter lowpass behavior", "[signal][ladder]") {
    LadderFilter ladder;
    ladder.set_sample_rate(44100.0f);
    ladder.set_frequency(200.0f);
    ladder.set_resonance(0.3f);

    // High frequency should be attenuated
    float sum_sq = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 10000.0f * i / 44100.0f);
        float output = ladder.process(input);
        if (i > 200) sum_sq += output * output;
    }
    float rms = std::sqrt(sum_sq / 4210.0f);
    REQUIRE(rms < 0.05f); // 24dB/oct should heavily attenuate
}

TEST_CASE("LadderFilter resets buffer state and clamps resonance inputs",
          "[signal][ladder][issue-645]") {
    LadderFilter ladder;
    ladder.set_sample_rate(48000.0f);
    ladder.set_frequency(1200.0f);
    ladder.set_resonance(-2.0f);

    float impulse[] = {1.0f, 0.0f, 0.0f, 0.0f};
    ladder.process(impulse, 4);
    for (float sample : impulse) {
        REQUIRE(std::isfinite(sample));
    }

    ladder.reset();
    REQUIRE_THAT(ladder.process(0.0f), WithinAbs(0.0f, 1e-6f));

    ladder.set_resonance(3.0f);
    ladder.set_frequency(8000.0f);
    for (int i = 0; i < 32; ++i) {
        const float sample = (i == 0) ? 0.75f : 0.0f;
        REQUIRE(std::isfinite(ladder.process(sample)));
    }

    float unchanged[] = {0.25f, -0.25f};
    ladder.process(unchanged, 0);
    REQUIRE_THAT(unchanged[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(unchanged[1], WithinAbs(-0.25f, 1e-6f));
}

// ── LinkwitzRiley ────────────────────────────────────────────────────────────

TEST_CASE("LinkwitzRiley splits into low and high bands", "[signal][lr]") {
    LinkwitzRiley lr;
    lr.set_frequency(1000.0f, 44100.0f);

    // 200Hz signal should be mostly in the low band
    float low_energy = 0, high_energy = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 200.0f * i / 44100.0f);
        auto split = lr.process(input);
        if (i > 200) { // Skip transient
            low_energy += split.low * split.low;
            high_energy += split.high * split.high;
        }
    }
    REQUIRE(low_energy > high_energy * 10.0f); // Low band should dominate

    // 5kHz signal should be mostly in the high band
    lr.reset();
    low_energy = 0; high_energy = 0;
    for (int i = 0; i < 4410; ++i) {
        float input = std::sin(2.0f * 3.14159f * 5000.0f * i / 44100.0f);
        auto split = lr.process(input);
        if (i > 200) {
            low_energy += split.low * split.low;
            high_energy += split.high * split.high;
        }
    }
    REQUIRE(high_energy > low_energy * 10.0f); // High band should dominate
}

TEST_CASE("LinkwitzRiley reset clears history while preserving coefficients",
          "[signal][lr][issue-645]") {
    auto impulse_response = [](LinkwitzRiley& lr) {
        std::vector<LinkwitzRiley::BandSplit> response;
        response.reserve(24);
        for (int i = 0; i < 24; ++i) {
            response.push_back(lr.process(i == 0 ? 1.0f : 0.0f));
        }
        return response;
    };

    LinkwitzRiley fresh;
    fresh.set_frequency(1200.0f, 48000.0f);
    auto expected = impulse_response(fresh);

    LinkwitzRiley reused;
    reused.set_frequency(1200.0f, 48000.0f);
    for (int i = 0; i < 128; ++i) {
        auto split = reused.process((i % 3 == 0) ? 0.75f : -0.25f);
        REQUIRE(std::isfinite(split.low));
        REQUIRE(std::isfinite(split.high));
    }

    reused.reset();
    auto actual = impulse_response(reused);

    REQUIRE(actual.size() == expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        REQUIRE_THAT(actual[i].low, WithinAbs(expected[i].low, 1e-6f));
        REQUIRE_THAT(actual[i].high, WithinAbs(expected[i].high, 1e-6f));
    }
}

TEST_CASE("LinkwitzRiley cutoff boundary processing stays finite",
          "[signal][lr][issue-645]") {
    for (float cutoff : {0.0f, 20.0f, 20000.0f, 24000.0f}) {
        LinkwitzRiley lr;
        lr.set_frequency(cutoff, 48000.0f);

        for (int i = 0; i < 64; ++i) {
            float input = (i == 0) ? 1.0f : ((i % 2 == 0) ? 0.125f : -0.125f);
            auto split = lr.process(input);
            INFO("cutoff=" << cutoff << " sample=" << i);
            REQUIRE(std::isfinite(split.low));
            REQUIRE(std::isfinite(split.high));
        }
    }
}

TEST_CASE("LinkwitzRiley retuning after active history stays finite",
          "[signal][lr][coverage][phase3-github]") {
    LinkwitzRiley lr;
    lr.set_frequency(300.0f, 48000.0f);
    for (int i = 0; i < 32; ++i) {
        (void)lr.process((i % 2 == 0) ? 0.5f : -0.5f);
    }

    lr.set_frequency(2400.0f, 48000.0f);
    for (int i = 0; i < 16; ++i) {
        auto split = lr.process(i == 0 ? 1.0f : 0.0f);
        REQUIRE(std::isfinite(split.low));
        REQUIRE(std::isfinite(split.high));
    }
}

// ── WindowFunction ───────────────────────────────────────────────────────────

TEST_CASE("WindowFunction Hann", "[signal][window]") {
    auto w = WindowFunction::generate(256, WindowFunction::Type::hann);
    REQUIRE(w.size() == 256);
    REQUIRE_THAT(w[0], WithinAbs(0.0, 0.001));
    REQUIRE_THAT(w[128], WithinAbs(1.0, 0.001)); // Peak at center
    REQUIRE_THAT(w[255], WithinAbs(0.0, 0.001));
}

TEST_CASE("WindowFunction Blackman", "[signal][window]") {
    auto w = WindowFunction::generate(512, WindowFunction::Type::blackman);
    REQUIRE(w.size() == 512);
    REQUIRE(w[256] > 0.9f); // Near peak
    REQUIRE(w[0] < 0.01f);  // Near zero at edges
}

TEST_CASE("WindowFunction apply", "[signal][window]") {
    auto w = WindowFunction::generate(4, WindowFunction::Type::rectangular);
    float buf[] = {1.0f, 2.0f, 3.0f, 4.0f};
    WindowFunction::apply(buf, w);
    // Rectangular window should not change values
    REQUIRE_THAT(buf[0], WithinAbs(1.0, 0.001));
    REQUIRE_THAT(buf[3], WithinAbs(4.0, 0.001));
}

TEST_CASE("WindowFunction covers hamming flat-top and kaiser branches", "[signal][window][issue-645]") {
    auto hamming = WindowFunction::generate(5, WindowFunction::Type::hamming);
    REQUIRE_THAT(hamming.front(), WithinAbs(0.08f, 1e-5f));
    REQUIRE_THAT(hamming.back(), WithinAbs(0.08f, 1e-5f));
    REQUIRE_THAT(hamming[2], WithinAbs(1.0f, 1e-5f));

    auto flat_top = WindowFunction::generate(9, WindowFunction::Type::flat_top);
    REQUIRE(flat_top[4] > 0.99f);
    REQUIRE(std::abs(flat_top.front()) < 0.001f);

    auto kaiser_default = WindowFunction::generate(5, WindowFunction::Type::kaiser);
    auto kaiser_custom = WindowFunction::generate(5, WindowFunction::Type::kaiser, 6.0f);
    REQUIRE_THAT(kaiser_default.front(), WithinAbs(kaiser_default.back(), 1e-6f));
    REQUIRE(kaiser_default[2] > kaiser_default.front());
    REQUIRE(kaiser_custom.front() < kaiser_default.front());

    float shaped[] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
    WindowFunction::apply(shaped, hamming);
    REQUIRE_THAT(shaped[0], WithinAbs(0.08f, 1e-5f));
    REQUIRE_THAT(shaped[2], WithinAbs(1.0f, 1e-5f));
}

// ── FFT ──────────────────────────────────────────────────────────────────────

TEST_CASE("FFT forward/inverse round-trip", "[signal][fft]") {
    Fft fft(256);

    // Create a simple signal
    std::vector<std::complex<float>> data(256);
    for (int i = 0; i < 256; ++i)
        data[i] = {std::sin(2.0f * 3.14159f * 10.0f * i / 256.0f), 0.0f};

    auto original = data;

    fft.forward(data.data());
    fft.inverse(data.data());

    // Should recover original signal
    for (int i = 0; i < 256; ++i) {
        REQUIRE(std::abs(data[i].real() - original[i].real()) < 0.001f);
    }
}

TEST_CASE("FFT detects single frequency", "[signal][fft]") {
    constexpr int N = 1024;
    Fft fft(N);

    // Generate 100Hz sine at 44100 sample rate
    std::vector<std::complex<float>> data(N);
    float freq = 100.0f;
    float sr = 44100.0f;
    for (int i = 0; i < N; ++i)
        data[i] = {std::sin(2.0f * 3.14159f * freq * i / sr), 0.0f};

    fft.forward(data.data());

    // Find the peak bin
    int peak_bin = 0;
    float peak_mag = 0;
    for (int i = 1; i < N / 2; ++i) {
        float mag = std::abs(data[i]);
        if (mag > peak_mag) { peak_mag = mag; peak_bin = i; }
    }

    // Expected bin: freq * N / sr = 100 * 1024 / 44100 ≈ 2.32
    float expected_bin = freq * N / sr;
    REQUIRE(std::abs(peak_bin - expected_bin) < 2.0f);
}

TEST_CASE("FFT magnitude_db", "[signal][fft]") {
    constexpr int N = 256;
    Fft fft(N);

    std::vector<std::complex<float>> freq(N);
    std::vector<float> mag_db(N / 2);

    // DC component = 1.0 → 0 dB
    freq[0] = {1.0f, 0.0f};
    fft.magnitude_db(freq.data(), mag_db.data(), N / 2);
    REQUIRE_THAT(mag_db[0], WithinAbs(0.0, 0.01));

    // 0.01 → -40 dB
    freq[0] = {0.01f, 0.0f};
    fft.magnitude_db(freq.data(), mag_db.data(), 1);
    REQUIRE_THAT(mag_db[0], WithinAbs(-40.0, 0.1));
}

TEST_CASE("FFT forward_real", "[signal][fft]") {
    constexpr int N = 256;
    Fft fft(N);

    std::vector<float> input(N, 0.0f);
    input[0] = 1.0f; // Impulse

    std::vector<std::complex<float>> output(N);
    fft.forward_real(input.data(), output.data());

    // FFT of impulse: all bins should have magnitude 1
    for (int i = 0; i < N; ++i) {
        REQUIRE(std::abs(std::abs(output[i]) - 1.0f) < 0.01f);
    }
}

TEST_CASE("FFT move preserves transform state", "[signal][fft][issue-645]") {
    Fft original(8);
    Fft moved(std::move(original));

    REQUIRE(original.size() == 0);
    REQUIRE(moved.size() == 8);

    std::vector<std::complex<float>> data(8, {1.0f, 0.0f});
    moved.forward(data.data());

    REQUIRE_THAT(data[0].real(), WithinAbs(8.0f, 1e-4f));
    REQUIRE_THAT(data[0].imag(), WithinAbs(0.0f, 1e-4f));
    for (int i = 1; i < 8; ++i) {
        REQUIRE(std::abs(data[i]) < 1e-4f);
    }

    Fft assigned;
    assigned = std::move(moved);
    REQUIRE(moved.size() == 0);
    REQUIRE(assigned.size() == 8);

    assigned.inverse(data.data());
    for (const auto& sample : data) {
        REQUIRE_THAT(sample.real(), WithinAbs(1.0f, 1e-4f));
        REQUIRE_THAT(sample.imag(), WithinAbs(0.0f, 1e-4f));
    }
}

TEST_CASE("FFT magnitude helpers handle silence and complex bins", "[signal][fft][issue-645]") {
    Fft fft(8);
    std::vector<std::complex<float>> freq = {
        {0.0f, 0.0f},
        {3.0f, 4.0f},
        {-0.5f, 0.0f},
        {0.0f, -2.0f},
        {0.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 0.0f},
        {0.0f, 0.0f},
    };
    std::vector<float> linear(4, 0.0f);
    std::vector<float> db(4, 0.0f);

    fft.magnitude(freq.data(), linear.data(), 4);
    fft.magnitude_db(freq.data(), db.data(), 4);

    REQUIRE_THAT(linear[0], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(linear[1], WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(linear[2], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(linear[3], WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(db[0], WithinAbs(-200.0f, 0.01f));
    REQUIRE_THAT(db[1], WithinAbs(13.9794f, 0.01f));
    REQUIRE_THAT(db[2], WithinAbs(-6.0206f, 0.01f));
    REQUIRE_THAT(db[3], WithinAbs(6.0206f, 0.01f));
}

TEST_CASE("FFT forward_real preserves exact-bin conjugate symmetry", "[signal][fft][issue-645]") {
    constexpr int N = 64;
    constexpr int bin = 5;
    Fft fft(N);
    std::vector<float> input(N);
    for (int i = 0; i < N; ++i) {
        input[i] = std::sin(2.0f * 3.14159265358979323846f * bin * i / N);
    }

    std::vector<std::complex<float>> output(N);
    std::vector<float> magnitude(N, 0.0f);
    fft.forward_real(input.data(), output.data());
    fft.magnitude(output.data(), magnitude.data(), N);

    REQUIRE_THAT(magnitude[0], WithinAbs(0.0f, 1e-4f));
    REQUIRE_THAT(magnitude[bin], WithinAbs(static_cast<float>(N) / 2.0f, 1e-3f));
    REQUIRE_THAT(magnitude[N - bin], WithinAbs(static_cast<float>(N) / 2.0f, 1e-3f));
    REQUIRE_THAT(output[bin].real(), WithinAbs(output[N - bin].real(), 1e-4f));
    REQUIRE_THAT(output[bin].imag(), WithinAbs(-output[N - bin].imag(), 1e-4f));
}

// ── Convolver ────────────────────────────────────────────────────────────────

TEST_CASE("Convolver with identity IR", "[signal][convolver]") {
    Convolver conv;

    // Identity IR: [1, 0, 0, ...]
    float ir[] = {1.0f};
    conv.load_ir(ir, 1, 64);

    // Process a known signal
    std::vector<float> input(256);
    std::vector<float> output(256);
    for (int i = 0; i < 256; ++i)
        input[i] = std::sin(2.0f * 3.14159f * 440.0f * i / 44100.0f);

    conv.process(input.data(), output.data(), 256);

    // Output should match input (after initial latency of one block)
    float max_error = 0;
    for (int i = 64; i < 200; ++i) {
        float error = std::abs(output[i] - input[i - 64]);
        if (error > max_error) max_error = error;
    }
    REQUIRE(max_error < 0.01f);
}

TEST_CASE("Convolver with simple delay IR", "[signal][convolver]") {
    Convolver conv;

    // Delay IR: [0, 0, 0, 0, 1] — 4-sample delay
    float ir[] = {0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    conv.load_ir(ir, 5, 32);

    // Send an impulse and collect output
    std::vector<float> output(128, 0.0f);
    output[0] = conv.process(1.0f);
    for (int i = 1; i < 128; ++i)
        output[i] = conv.process(0.0f);

    // Should see the impulse delayed by 4 samples + block latency
    float peak = 0;
    int peak_pos = 0;
    for (int i = 0; i < 128; ++i) {
        if (std::abs(output[i]) > peak) {
            peak = std::abs(output[i]);
            peak_pos = i;
        }
    }
    REQUIRE(peak > 0.9f); // Should find the delayed impulse
    REQUIRE(peak_pos >= 4); // At least 4 samples delayed
}

TEST_CASE("Convolver reset clears buffered overlap", "[signal][convolver][issue-645]") {
    Convolver conv;
    float ir[] = {0.0f, 1.0f, 0.5f};
    conv.load_ir(ir, 3, 8);

    std::vector<float> impulse(24, 0.0f);
    std::vector<float> output(24, 0.0f);
    impulse[0] = 1.0f;
    conv.process(impulse.data(), output.data(), static_cast<int>(output.size()));

    bool produced_tail = false;
    for (float sample : output) {
        produced_tail = produced_tail || std::abs(sample) > 1e-4f;
    }
    REQUIRE(produced_tail);

    conv.reset();
    std::fill(output.begin(), output.end(), 99.0f);
    std::fill(impulse.begin(), impulse.end(), 0.0f);
    conv.process(impulse.data(), output.data(), static_cast<int>(output.size()));

    for (float sample : output) {
        REQUIRE_THAT(sample, WithinAbs(0.0f, 1e-4f));
    }
}

// ── MultiChannelMeter ────────────────────────────────────────────────────────

TEST_CASE("MultiChannelMeter emits peak RMS clipping and stereo correlation", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(44100.0f, 2);

    std::vector<float> left(441, 0.5f);
    std::vector<float> right(441, 0.5f);
    left[10] = 1.2f;

    const float* channels[] = {left.data(), right.data()};
    meter.process(channels, 2, static_cast<int>(left.size()));

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(1.2f, 0.001f));
    REQUIRE(snap.channels[0].clipped);
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(snap.channels[1].rms, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(snap.correlation, WithinAbs(1.0f, 0.005f));
    REQUIRE(std::isfinite(snap.channels[0].lufs_momentary));
}

TEST_CASE("MultiChannelMeter tracks negative correlation and integrated loudness", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(1000.0f, 2);

    std::vector<float> left(10, 0.25f);
    std::vector<float> right(10, -0.25f);
    const float* channels[] = {left.data(), right.data()};

    for (int i = 0; i < 40; ++i) {
        meter.process(channels, 2, static_cast<int>(left.size()));
    }

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.correlation, WithinAbs(-1.0f, 0.001f));
    REQUIRE(std::isfinite(snap.lufs_integrated));
    REQUIRE_THAT(snap.lufs_integrated, WithinAbs(-12.73f, 0.1f));
}

TEST_CASE("MultiChannelMeter prepare clears stale clip flags", "[signal][meter]") {
    MultiChannelMeter meter;
    meter.prepare(1000.0f, 1);

    std::vector<float> clipped(10, 1.2f);
    const float* clipped_channels[] = {clipped.data()};
    meter.process(clipped_channels, 1, static_cast<int>(clipped.size()));
    REQUIRE(meter.snapshot().channels[0].clipped);

    meter.prepare(1000.0f, 1);

    std::vector<float> clean(10, 0.25f);
    const float* clean_channels[] = {clean.data()};
    meter.process(clean_channels, 1, static_cast<int>(clean.size()));
    REQUIRE_FALSE(meter.snapshot().channels[0].clipped);
}

TEST_CASE("MultiChannelMeter clamps channels and reset clears snapshot", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, kMaxMeterChannels + 4);
    REQUIRE(meter.snapshot().num_channels == kMaxMeterChannels);

    std::array<std::array<float, 1>, kMaxMeterChannels> samples{};
    std::array<const float*, kMaxMeterChannels> channels{};
    for (int ch = 0; ch < kMaxMeterChannels; ++ch) {
        samples[ch][0] = static_cast<float>(ch + 1) / 20.0f;
        channels[ch] = samples[ch].data();
    }

    meter.process(channels.data(), kMaxMeterChannels, 1);
    REQUIRE(meter.snapshot().num_channels == kMaxMeterChannels);
    REQUIRE_THAT(meter.snapshot().channels[15].peak, WithinAbs(0.8f, 1e-6f));

    meter.reset();
    REQUIRE(meter.snapshot().num_channels == 0);
}

TEST_CASE("MultiChannelMeter silent stereo emits finite zero correlation", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(10.0f, 2);

    std::vector<float> left(4, 0.0f);
    std::vector<float> right(4, 0.0f);
    const float* channels[] = {left.data(), right.data()};
    meter.process(channels, 2, static_cast<int>(left.size()));

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 2);
    REQUIRE_THAT(snap.correlation, WithinAbs(0.0f, 1e-6f));
    REQUIRE(std::isinf(snap.channels[0].lufs_momentary));
    REQUIRE(std::isinf(snap.lufs_integrated));
}

TEST_CASE("MultiChannelMeter waits for block threshold before emitting", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(1000.0f, 1);

    std::vector<float> samples(5, 0.75f);
    const float* channels[] = {samples.data()};
    meter.process(channels, 1, static_cast<int>(samples.size()));

    REQUIRE(meter.snapshot().num_channels == 1);
    REQUIRE_THAT(meter.snapshot().channels[0].peak, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter resets correlation accumulation window", "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(10.0f, 2);

    float left[] = {0.5f};
    float right[] = {0.5f};
    const float* channels[] = {left, right};
    meter.process(channels, 2, 1);

    REQUIRE_THAT(meter.snapshot().correlation, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter clamps negative prepared channel counts",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, -3);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 0);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("MultiChannelMeter clamps process channel count to prepared channels",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 1);

    float left[] = {0.25f};
    float ignored_right[] = {1.0f};
    std::vector<const float*> channels;
    channels.push_back(left);
    channels.push_back(ignored_right);
    channels.push_back(nullptr);

    meter.process(channels.data(), static_cast<int>(channels.size()), 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 1);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(snap.channels[1].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(snap.channels[1].clipped);
}

TEST_CASE("MultiChannelMeter clamps negative process channel counts",
          "[signal][meter][issue-645]") {
    MultiChannelMeter meter;
    meter.prepare(100.0f, 1);

    float sample[] = {1.0f};
    const float* channels[] = {sample};
    meter.process(channels, -1, 1);

    const auto& snap = meter.snapshot();
    REQUIRE(snap.num_channels == 0);
    REQUIRE_THAT(snap.channels[0].peak, WithinAbs(0.0f, 1e-6f));
    REQUIRE_FALSE(snap.channels[0].clipped);
}

TEST_CASE("MultiChannelBallistics holds peaks and clip indicators", "[signal][meter]") {
    MultiChannelBallistics ballistics;

    MultiChannelMeterData data;
    data.num_channels = 1;
    data.channels[0].peak = 1.0f;
    data.channels[0].rms = 0.5f;
    data.channels[0].clipped = true;

    ballistics.update(data, 0.016f);
    REQUIRE(ballistics.num_channels == 1);
    REQUIRE(ballistics.channels[0].display_peak > 0.9f);
    REQUIRE(ballistics.channels[0].display_rms > 0.45f);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(1.0f, 0.001f));
    REQUIRE(ballistics.channels[0].clip_indicator);

    data.channels[0].peak = 0.0f;
    data.channels[0].rms = 0.0f;
    data.channels[0].clipped = false;

    ballistics.update(data, 0.5f);
    REQUIRE(ballistics.channels[0].clip_indicator);
    REQUIRE_THAT(ballistics.channels[0].held_peak, WithinAbs(1.0f, 0.001f));

    ballistics.update(data, 3.0f);
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);

    data.channels[0].clipped = true;
    ballistics.update(data, 0.016f);
    REQUIRE(ballistics.channels[0].clip_indicator);
    ballistics.clear_clips();
    REQUIRE_FALSE(ballistics.channels[0].clip_indicator);
}

TEST_CASE("MultiChannelBallistics clamps invalid snapshot channel counts",
          "[signal][meter][issue-645]") {
    MultiChannelBallistics ballistics;

    MultiChannelMeterData data;
    data.num_channels = -2;
    ballistics.update(data, 0.01f);
    REQUIRE(ballistics.num_channels == 0);

    data.num_channels = kMaxMeterChannels + 3;
    for (int ch = 0; ch < kMaxMeterChannels; ++ch) {
        data.channels[ch].peak = static_cast<float>(ch + 1) / 20.0f;
    }

    ballistics.update(data, 0.01f);
    REQUIRE(ballistics.num_channels == kMaxMeterChannels);
    REQUIRE(ballistics.channels[0].display_peak > 0.0f);
    REQUIRE(ballistics.channels[kMaxMeterChannels - 1].display_peak > 0.0f);
}
