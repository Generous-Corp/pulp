#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/signal.hpp>
#include <array>
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

TEST_CASE("SimpleMixer", "[signal][mix]") {
    SimpleMixer mixer;

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
    REQUIRE(sum_diff > 0.01f);
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
