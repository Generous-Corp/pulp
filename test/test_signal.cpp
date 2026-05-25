#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/signal.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
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
    REQUIRE_THAT(dl.process(-0.5f, 0.0f), WithinAbs(-0.5f, 1e-6f));

    dl.reset();
    REQUIRE_THAT(dl.read(0), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("DelayLine fractional reads interpolate across wrapped write positions",
          "[signal][delay][coverage][phase3-batch756]") {
    DelayLine dl;
    dl.prepare(3);

    dl.push(10.0f);
    dl.push(20.0f);
    dl.push(30.0f);
    dl.push(40.0f);
    dl.push(50.0f);

    REQUIRE_THAT(dl.read(0), WithinAbs(50.0f, 1e-6f));
    REQUIRE_THAT(dl.read(0.5f), WithinAbs(45.0f, 1e-6f));
    REQUIRE_THAT(dl.read(1.0f), WithinAbs(40.0f, 1e-6f));
    REQUIRE_THAT(dl.read(1.5f), WithinAbs(35.0f, 1e-6f));
    REQUIRE_THAT(dl.read(2.0f), WithinAbs(30.0f, 1e-6f));
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

TEST_CASE("ADSR retrigger restarts attack from a partial level",
          "[signal][adsr][coverage]") {
    Adsr env;
    env.set_sample_rate(1000.0f);
    env.set_params({0.1f, 0.1f, 0.25f, 0.1f});

    env.note_on();
    const float before = env.next();
    REQUIRE(before > 0.0f);
    REQUIRE(before < 1.0f);

    env.note_on();
    const float after = env.next();

    REQUIRE(env.is_active());
    REQUIRE(env.stage() == Adsr::Stage::attack);
    REQUIRE(after > before);
    REQUIRE(after < 1.0f);
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

TEST_CASE("Oscillator zero frequency leaves phase stationary",
          "[signal][osc][coverage]") {
    Oscillator osc;
    osc.set_sample_rate(48000.0f);
    osc.set_frequency(0.0f);
    osc.set_waveform(Oscillator::Waveform::square);

    for (int i = 0; i < 8; ++i) {
        REQUIRE(std::isfinite(osc.next()));
        REQUIRE_THAT(osc.phase(), WithinAbs(0.0f, 1e-6f));
    }

    osc.reset();
    REQUIRE_THAT(osc.phase(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(osc.frequency(), WithinAbs(0.0f, 1e-6f));
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

TEST_CASE("WaveShaper handles zero and negative length buffers",
          "[signal][waveshaper][coverage][phase3-batch756]") {
    WaveShaper ws;
    ws.set_curve(WaveShaper::Curve::hard_clip);
    ws.set_drive(8.0f);

    float samples[] = {0.125f, -0.25f, 0.5f};
    ws.process(samples, 0);
    ws.process(samples, -4);

    REQUIRE_THAT(samples[0], WithinAbs(0.125f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(-0.25f, 1e-6f));
    REQUIRE_THAT(samples[2], WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("WaveShaper covers soft clip and sine fold curves",
          "[signal][waveshaper][coverage]") {
    WaveShaper ws;
    ws.set_curve(WaveShaper::Curve::soft_clip);
    ws.set_drive(2.0f);

    REQUIRE_THAT(ws.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(ws.process(0.5f), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(ws.process(-0.5f), WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(ws.process(2.0f), WithinAbs(0.8f, 1e-6f));

    ws.set_curve(WaveShaper::Curve::sine_fold);
    ws.set_drive(1.0f);

    REQUIRE_THAT(ws.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(ws.process(0.5f), WithinAbs(std::sin(0.5f * 1.5707963f), 1e-6f));
    REQUIRE_THAT(ws.process(1.0f), WithinAbs(1.0f, 1e-5f));
    REQUIRE_THAT(ws.process(-1.0f), WithinAbs(-1.0f, 1e-5f));
}

TEST_CASE("WaveShaper fold reflects repeated positive and negative overshoot",
          "[signal][waveshaper][coverage]") {
    WaveShaper ws;
    ws.set_curve(WaveShaper::Curve::fold);
    ws.set_drive(1.0f);

    REQUIRE_THAT(ws.process(1.25f), WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(ws.process(1.75f), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(ws.process(2.25f), WithinAbs(-0.25f, 1e-6f));
    REQUIRE_THAT(ws.process(3.25f), WithinAbs(-0.75f, 1e-6f));
    REQUIRE_THAT(ws.process(-1.25f), WithinAbs(-0.75f, 1e-6f));
    REQUIRE_THAT(ws.process(-1.75f), WithinAbs(-0.25f, 1e-6f));
    REQUIRE_THAT(ws.process(-2.25f), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(ws.process(-3.25f), WithinAbs(0.75f, 1e-6f));
}

TEST_CASE("WaveShaper buffer processing applies the selected curve in place",
          "[signal][waveshaper][coverage]") {
    WaveShaper ws;
    ws.set_curve(WaveShaper::Curve::hard_clip);
    ws.set_drive(2.0f);

    float samples[] = {-0.75f, -0.25f, 0.0f, 0.25f, 0.75f};
    ws.process(samples, 5);

    REQUIRE_THAT(samples[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(samples[2], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(samples[3], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(samples[4], WithinAbs(1.0f, 1e-6f));
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

TEST_CASE("NoiseGate ignores nonpositive buffer lengths",
          "[signal][gate][coverage][phase3-batch756]") {
    NoiseGate gate;
    gate.set_sample_rate(1000.0f);
    gate.set_params({-20.0f, 20.0f, 0.0f, 0.0f, -24.0f});

    float samples[] = {0.001f, 1.0f, -0.001f};
    gate.process(samples, 0);
    gate.process(samples, -2);

    REQUIRE_THAT(samples[0], WithinAbs(0.001f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(samples[2], WithinAbs(-0.001f, 1e-6f));
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

TEST_CASE("Panner preserves equal-power mono energy and input sign",
          "[signal][panner][coverage][phase3-batch756]") {
    Panner pan;
    pan.set_pan(-0.25f);

    const auto positive = pan.process(0.8f);
    const auto negative = pan.process(-0.8f);

    REQUIRE(positive.left > 0.0f);
    REQUIRE(positive.right > 0.0f);
    REQUIRE(negative.left < 0.0f);
    REQUIRE(negative.right < 0.0f);

    const float positive_energy =
        positive.left * positive.left + positive.right * positive.right;
    const float negative_energy =
        negative.left * negative.left + negative.right * negative.right;
    REQUIRE_THAT(positive_energy, WithinAbs(0.64f, 1e-5f));
    REQUIRE_THAT(negative_energy, WithinAbs(0.64f, 1e-5f));
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

TEST_CASE("Chorus is safe before prepare and rejects non-finite mix",
          "[signal][chorus][coverage][phase3]") {
    Chorus unprepared;
    unprepared.set_mix(1.0f);
    unprepared.set_depth(1.0f);

    for (float input : {1.0f, -0.25f}) {
        auto out = unprepared.process(input);
        REQUIRE_THAT(out.left, WithinAbs(input, 1e-6f));
        REQUIRE_THAT(out.right, WithinAbs(input, 1e-6f));
    }

    Chorus chorus;
    chorus.prepare(1000.0f);
    chorus.set_mix(std::numeric_limits<float>::quiet_NaN());
    chorus.set_depth(1.0f);
    chorus.set_delay_ms(2.0f);

    for (float input : {0.5f, -0.75f}) {
        auto out = chorus.process(input);
        REQUIRE_THAT(out.left, WithinAbs(input, 1e-6f));
        REQUIRE_THAT(out.right, WithinAbs(input, 1e-6f));
    }
}

TEST_CASE("Chorus wet zero-depth delay is centered and mono-compatible",
          "[signal][chorus][coverage][phase3]") {
    Chorus chorus;
    chorus.prepare(1000.0f);
    chorus.set_rate(0.0f);
    chorus.set_depth(0.0f);
    chorus.set_delay_ms(2.0f);
    chorus.set_mix(1.0f);

    const std::array<float, 5> input{{1.0f, 0.0f, 0.0f, -0.5f, 0.0f}};
    std::array<Chorus::StereoSample, input.size()> out{};
    for (size_t i = 0; i < input.size(); ++i)
        out[i] = chorus.process(input[i]);

    REQUIRE_THAT(out[0].left, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(out[0].right, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(out[1].left, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(out[1].right, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(out[2].left, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(out[2].right, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(out[4].left, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(out[4].right, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Chorus clamps mix and delay to safe callback ranges",
          "[signal][chorus][coverage][phase3]") {
    Chorus dry;
    dry.prepare(1000.0f);
    dry.set_mix(-1.0f);
    dry.set_depth(1.0f);
    dry.set_delay_ms(2.0f);

    for (float input : {0.25f, -0.5f, 0.75f}) {
        auto out = dry.process(input);
        REQUIRE_THAT(out.left, WithinAbs(input, 1e-6f));
        REQUIRE_THAT(out.right, WithinAbs(input, 1e-6f));
    }

    Chorus immediate_wet;
    immediate_wet.prepare(1000.0f);
    immediate_wet.set_mix(2.0f);
    immediate_wet.set_depth(1.0f);
    immediate_wet.set_delay_ms(-10.0f);

    auto positive = immediate_wet.process(0.375f);
    auto negative = immediate_wet.process(-0.625f);
    REQUIRE_THAT(positive.left, WithinAbs(0.375f, 1e-6f));
    REQUIRE_THAT(positive.right, WithinAbs(0.375f, 1e-6f));
    REQUIRE_THAT(negative.left, WithinAbs(-0.625f, 1e-6f));
    REQUIRE_THAT(negative.right, WithinAbs(-0.625f, 1e-6f));
}

TEST_CASE("Chorus depth clamps preserve modulation timing bounds",
          "[signal][chorus][coverage][phase3]") {
    Chorus limited_depth;
    limited_depth.prepare(1000.0f);
    limited_depth.set_rate(0.0f);
    limited_depth.set_depth(2.0f);
    limited_depth.set_delay_ms(2.0f);
    limited_depth.set_mix(1.0f);

    std::array<Chorus::StereoSample, 5> limited{};
    for (size_t i = 0; i < limited.size(); ++i)
        limited[i] = limited_depth.process(i == 0 ? 1.0f : 0.0f);

    REQUIRE_THAT(limited[2].left, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(limited[2].right, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(limited[3].left, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(limited[3].right, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(limited[4].right, WithinAbs(0.0f, 1e-6f));

    Chorus no_depth;
    no_depth.prepare(1000.0f);
    no_depth.set_rate(0.0f);
    no_depth.set_depth(-1.0f);
    no_depth.set_delay_ms(2.0f);
    no_depth.set_mix(1.0f);

    std::array<Chorus::StereoSample, 3> centered{};
    for (size_t i = 0; i < centered.size(); ++i)
        centered[i] = no_depth.process(i == 0 ? -0.5f : 0.0f);

    REQUIRE_THAT(centered[0].left, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(centered[0].right, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(centered[2].left, WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(centered[2].right, WithinAbs(-0.5f, 1e-6f));
}

TEST_CASE("Chorus reset clears delayed audio and restarts the same response",
          "[signal][chorus][coverage][phase3]") {
    Chorus chorus;
    chorus.prepare(1000.0f);
    chorus.set_rate(0.0f);
    chorus.set_depth(0.0f);
    chorus.set_delay_ms(2.0f);
    chorus.set_mix(1.0f);

    const std::array<float, 4> impulse{{1.0f, 0.0f, 0.0f, 0.0f}};
    std::array<Chorus::StereoSample, impulse.size()> first{};
    std::array<Chorus::StereoSample, impulse.size()> second{};
    for (size_t i = 0; i < impulse.size(); ++i)
        first[i] = chorus.process(impulse[i]);

    chorus.reset();
    auto cleared = chorus.process(0.0f);
    REQUIRE_THAT(cleared.left, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(cleared.right, WithinAbs(0.0f, 1e-6f));

    chorus.reset();
    for (size_t i = 0; i < impulse.size(); ++i)
        second[i] = chorus.process(impulse[i]);

    REQUIRE_THAT(second[0].left, WithinAbs(first[0].left, 1e-6f));
    REQUIRE_THAT(second[0].right, WithinAbs(first[0].right, 1e-6f));
    REQUIRE_THAT(second[2].left, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(second[2].right, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(second[3].left, WithinAbs(first[3].left, 1e-6f));
    REQUIRE_THAT(second[3].right, WithinAbs(first[3].right, 1e-6f));
}

TEST_CASE("Chorus invalid sample rates and fast modulation stay finite",
          "[signal][chorus][coverage][phase3]") {
    Chorus chorus;
    chorus.prepare(0.0f);
    chorus.set_rate(std::numeric_limits<float>::infinity());
    chorus.set_depth(std::numeric_limits<float>::infinity());
    chorus.set_delay_ms(std::numeric_limits<float>::infinity());
    chorus.set_mix(1.0f);

    for (float input : {0.25f, -0.25f}) {
        auto out = chorus.process(input);
        REQUIRE(std::isfinite(out.left));
        REQUIRE(std::isfinite(out.right));
    }

    Chorus fast;
    fast.prepare(1000.0f);
    fast.set_rate(3500.0f);
    fast.set_depth(1.0f);
    fast.set_delay_ms(1.0f);
    fast.set_mix(1.0f);

    for (float input : {1.0f, 0.0f}) {
        auto out = fast.process(input);
        REQUIRE(std::isfinite(out.left));
        REQUIRE(std::isfinite(out.right));
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

TEST_CASE("Phaser zero-length buffer processing is a no-op",
          "[signal][phaser][coverage]") {
    Phaser phaser;
    phaser.set_sample_rate(48000.0f);
    phaser.set_rate(1.0f);
    phaser.set_depth(0.75f);
    phaser.set_mix(1.0f);

    std::array<float, 3> samples{{-1.0f, 0.25f, 1.0f}};
    phaser.process(samples.data() + 1, 0);

    REQUIRE(samples == std::array<float, 3>{{-1.0f, 0.25f, 1.0f}});
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

// ── Spectral display helpers ────────────────────────────────────────────────

TEST_CASE("Signal target covers window families and bounded application",
          "[signal][window][coverage]") {
    const auto rectangular = WindowFunction::generate(6, WindowFunction::Type::rectangular);
    REQUIRE(rectangular.size() == 6);
    REQUIRE_THAT(rectangular[3], WithinAbs(1.0f, 1e-6f));

    const auto hann = WindowFunction::generate(6, WindowFunction::Type::hann);
    REQUIRE(hann.size() == 6);
    REQUIRE_THAT(hann.front(), WithinAbs(0.0f, 1e-6f));
    REQUIRE(hann[2] > hann[1]);

    const auto hamming = WindowFunction::generate(5, WindowFunction::Type::hamming);
    REQUIRE_THAT(hamming.front(), WithinAbs(0.08f, 1e-5f));

    const auto blackman = WindowFunction::generate(7, WindowFunction::Type::blackman);
    REQUIRE(blackman[3] > blackman[0]);

    const auto flat_top = WindowFunction::generate(7, WindowFunction::Type::flat_top);
    REQUIRE(flat_top[3] > flat_top[0]);

    const auto kaiser_default = WindowFunction::generate(5, WindowFunction::Type::kaiser);
    const auto kaiser_custom = WindowFunction::generate(5, WindowFunction::Type::kaiser, 8.0f);
    REQUIRE_THAT(kaiser_default.front(), WithinAbs(kaiser_default.back(), 1e-6f));
    REQUIRE(kaiser_custom.front() < kaiser_default.front());

    REQUIRE(WindowFunction::generate(0, WindowFunction::Type::hann).empty());
    const auto single = WindowFunction::generate(1, WindowFunction::Type::kaiser, 4.0f);
    REQUIRE(single.size() == 1);
    REQUIRE_THAT(single[0], WithinAbs(1.0f, 1e-6f));

    float buffer[] = {1.0f, 2.0f, 4.0f};
    WindowFunction::apply(buffer, {1.0f, 0.5f});
    REQUIRE_THAT(buffer[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(4.0f, 1e-6f));
}

TEST_CASE("WindowFunction edge sizes and symmetry stay deterministic",
          "[signal][window][coverage][phase3]") {
    REQUIRE(WindowFunction::generate(-3, WindowFunction::Type::blackman).empty());

    for (auto type : {WindowFunction::Type::hann,
                      WindowFunction::Type::hamming,
                      WindowFunction::Type::blackman,
                      WindowFunction::Type::flat_top,
                      WindowFunction::Type::kaiser}) {
        auto one = WindowFunction::generate(1, type, 5.0f);
        REQUIRE(one.size() == 1);
        REQUIRE_THAT(one[0], WithinAbs(1.0f, 1e-6f));
    }

    const auto hann = WindowFunction::generate(8, WindowFunction::Type::hann);
    REQUIRE_THAT(hann.front(), WithinAbs(hann.back(), 1e-6f));
    REQUIRE_THAT(hann[1], WithinAbs(hann[6], 1e-6f));
    REQUIRE(hann[3] > hann[2]);

    const auto blackman = WindowFunction::generate(8, WindowFunction::Type::blackman);
    REQUIRE_THAT(blackman[0], WithinAbs(blackman[7], 1e-6f));
    REQUIRE_THAT(blackman[2], WithinAbs(blackman[5], 1e-6f));
    REQUIRE(blackman[3] > blackman[1]);

    const auto flat_top = WindowFunction::generate(9, WindowFunction::Type::flat_top);
    REQUIRE(flat_top[4] > 0.99f);
    REQUIRE(flat_top[4] > flat_top[3]);
}

TEST_CASE("WindowFunction applies only provided coefficients and honors Kaiser alpha",
          "[signal][window][coverage][phase3]") {
    float partial[] = {2.0f, 4.0f, 8.0f, 16.0f};
    WindowFunction::apply(partial, {0.25f, 0.5f});
    REQUIRE_THAT(partial[0], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(partial[1], WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(partial[2], WithinAbs(8.0f, 1e-6f));
    REQUIRE_THAT(partial[3], WithinAbs(16.0f, 1e-6f));

    float unchanged[] = {3.0f, 6.0f};
    WindowFunction::apply(unchanged, {});
    REQUIRE_THAT(unchanged[0], WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(unchanged[1], WithinAbs(6.0f, 1e-6f));

    const auto default_alpha = WindowFunction::generate(7, WindowFunction::Type::kaiser, 0.0f);
    const auto negative_alpha = WindowFunction::generate(7, WindowFunction::Type::kaiser, -2.0f);
    const auto high_alpha = WindowFunction::generate(7, WindowFunction::Type::kaiser, 9.0f);

    REQUIRE_THAT(default_alpha.front(), WithinAbs(negative_alpha.front(), 1e-6f));
    REQUIRE_THAT(default_alpha[3], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(high_alpha[3], WithinAbs(1.0f, 1e-6f));
    REQUIRE(high_alpha.front() < default_alpha.front());
    REQUIRE_THAT(high_alpha[1], WithinAbs(high_alpha[5], 1e-6f));
}

TEST_CASE("Signal target covers spectrogram ramps axes and scrolling columns",
          "[signal][spectrogram][coverage]") {
    ColorMapper inferno(ColorRamp::inferno);
    const auto inferno_low = inferno.map(0.0f);
    const auto inferno_mid = inferno.map(0.5f);
    const auto inferno_high = inferno.map(1.0f);
    CHECK(inferno_low.a == 255);
    CHECK(inferno_mid.r > inferno_low.r);
    CHECK(inferno_high.g >= inferno_mid.g);

    ColorMapper viridis(ColorRamp::viridis);
    const auto viridis_low = viridis.map(0.0f);
    const auto viridis_mid = viridis.map(0.5f);
    CHECK(viridis_mid.g > viridis_low.g);

    ColorMapper heat(ColorRamp::heat);
    const auto heat_high = heat.map(1.0f);
    CHECK(heat_high.r == 255);

    ColorMapper grayscale(ColorRamp::grayscale);
    const auto nan_gray = grayscale.map(std::numeric_limits<float>::quiet_NaN());
    const auto clamped_gray = grayscale.map(4.0f);
    CHECK(nan_gray.r == 0);
    CHECK(clamped_gray.r == 255);

    FrequencyAxis axis;
    axis.configure(1024, 48000.0f, FrequencyScale::linear);
    REQUIRE(axis.num_bins() == 513);
    REQUIRE_THAT(axis.nyquist(), WithinAbs(24000.0f, 1e-6f));
    REQUIRE_THAT(axis.bin_to_hz(256), WithinAbs(12000.0f, 1e-6f));
    REQUIRE(axis.hz_to_bin(-100.0f) == 0);
    REQUIRE(axis.hz_to_bin(999999.0f) == 512);
    REQUIRE(axis.display_to_bin(-0.5f) == 0);
    REQUIRE(axis.display_to_bin(2.0f) == 512);

    FrequencyAxis log_axis;
    log_axis.configure(1024, 48000.0f, FrequencyScale::logarithmic);
    REQUIRE(log_axis.display_to_hz(0.0f) >= 1.0f);

    FrequencyAxis mel_axis;
    mel_axis.configure(1024, 48000.0f, FrequencyScale::mel);
    REQUIRE(std::isfinite(mel_axis.hz_to_display(1000.0f)));

    SpectrogramBuffer buffer;
    buffer.configure(3, 4);
    REQUIRE(buffer.width() == 3);
    REQUIRE(buffer.height() == 4);
    REQUIRE(buffer.frames_written() == 0);
    REQUIRE(buffer.write_column() == 0);

    const float magnitudes[] = {-80.0f, -40.0f, 0.0f, -20.0f};
    buffer.push_column(magnitudes, 4, grayscale, -80.0f, 0.0f);
    REQUIRE(buffer.frames_written() == 1);
    REQUIRE(buffer.write_column() == 1);
    CHECK(buffer.pixels()[0].a == 255);
    CHECK(static_cast<int>(buffer.pixels()[3 * buffer.width()].r) == 191);

    buffer.push_column(nullptr, 0, grayscale, -80.0f, -80.0f);
    REQUIRE(buffer.frames_written() == 2);
    REQUIRE(buffer.write_column() == 2);
    CHECK(static_cast<int>(buffer.pixels()[1].r) == 0);
}

TEST_CASE("Signal target covers lookup table clamping and interpolation",
          "[signal][lookup][coverage]") {
    LookupTable table(3, 0.0f, 1.0f, [](float x) { return x * 2.0f; });

    REQUIRE(table.size() == 3);
    REQUIRE_THAT(table.process(-1.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(table.process(0.5f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(table[-2], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(table[99], WithinAbs(2.0f, 1e-6f));
}

TEST_CASE("Signal target covers lookup table construction edge cases",
          "[signal][lookup][coverage]") {
    LookupTable empty(0, -1.0f, 1.0f, [](float x) { return x * x; });
    REQUIRE(empty.empty());
    REQUIRE(empty.size() == 0);
    REQUIRE_THAT(empty.process(0.75f), WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(empty[0], WithinAbs(0.0f, 1e-6f));

    LookupTable singleton(1, 2.0f, 8.0f, [](float x) { return x + 3.0f; });
    REQUIRE_FALSE(singleton.empty());
    REQUIRE(singleton.size() == 1);
    REQUIRE_THAT(singleton.process(-100.0f), WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(singleton.process(100.0f), WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(singleton[-1], WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(singleton[2], WithinAbs(5.0f, 1e-6f));

    LookupTable flat_range(3, 4.0f, 4.0f, [](float x) { return x * 0.25f; });
    REQUIRE(flat_range.size() == 3);
    REQUIRE_THAT(flat_range.process(4.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(flat_range.process(10.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(flat_range[1], WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("Signal target covers lookup table reversed ranges and buffers",
          "[signal][lookup][coverage]") {
    LookupTable reversed(5, 2.0f, -2.0f, [](float x) { return x * x; });

    REQUIRE(reversed.size() == 5);
    REQUIRE_THAT(reversed.process(-3.0f), WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(reversed.process(-1.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(reversed.process(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(reversed.process(1.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(reversed.process(3.0f), WithinAbs(4.0f, 1e-6f));

    float samples[] = {-3.0f, -0.5f, 0.5f, 3.0f};
    reversed.process(samples, 4);
    REQUIRE_THAT(samples[0], WithinAbs(4.0f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(samples[2], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(samples[3], WithinAbs(4.0f, 1e-6f));

    reversed.process(samples, 0);
    REQUIRE_THAT(samples[0], WithinAbs(4.0f, 1e-6f));
}
