#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/fir_filter.hpp>
#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/log_ramped_value.hpp>
#include <pulp/signal/processor_chain.hpp>
#include <pulp/signal/lookup_table.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/gain.hpp>
#include <pulp/signal/waveshaper.hpp>
#include <cmath>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// ── FirFilter ────────────────────────────────────────────────────────────

TEST_CASE("FirFilter identity with single-tap", "[signal][fir]") {
    FirFilter fir;
    fir.set_coefficients({1.0f});
    REQUIRE_THAT(fir.process(0.5f), WithinAbs(0.5, 0.001));
    REQUIRE_THAT(fir.process(1.0f), WithinAbs(1.0, 0.001));
}

TEST_CASE("FirFilter 3-tap averaging", "[signal][fir]") {
    FirFilter fir;
    fir.set_coefficients({1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f});

    // Feed impulse: 1, 0, 0, 0...
    float y0 = fir.process(1.0f);
    float y1 = fir.process(0.0f);
    float y2 = fir.process(0.0f);
    float y3 = fir.process(0.0f);

    // Impulse response should be [1/3, 1/3, 1/3, 0]
    REQUIRE_THAT(y0, WithinAbs(1.0 / 3.0, 0.001));
    REQUIRE_THAT(y1, WithinAbs(1.0 / 3.0, 0.001));
    REQUIRE_THAT(y2, WithinAbs(1.0 / 3.0, 0.001));
    REQUIRE_THAT(y3, WithinAbs(0.0, 0.001));
}

TEST_CASE("FirFilter reset clears state", "[signal][fir]") {
    FirFilter fir;
    fir.set_coefficients({0.5f, 0.5f});
    fir.process(1.0f);
    fir.reset();
    REQUIRE_THAT(fir.process(0.0f), WithinAbs(0.0, 0.001));
}

TEST_CASE("FirFilter lowpass generator produces normalized coefficients", "[signal][fir]") {
    auto coeffs = FirFilter::lowpass(31, 1000.0f, 44100.0f);
    REQUIRE(coeffs.size() == 31);

    // Sum should be approximately 1.0 (normalized)
    float sum = 0;
    for (auto c : coeffs) sum += c;
    REQUIRE_THAT(sum, WithinAbs(1.0, 0.01));
}

TEST_CASE("FirFilter highpass generator", "[signal][fir]") {
    auto coeffs = FirFilter::highpass(31, 1000.0f, 44100.0f);
    REQUIRE(coeffs.size() == 31);

    // Sum should be approximately 0 (blocks DC)
    float sum = 0;
    for (auto c : coeffs) sum += c;
    REQUIRE_THAT(sum, WithinAbs(0.0, 0.05));
}

TEST_CASE("FirFilter order returns tap count", "[signal][fir]") {
    FirFilter fir;
    fir.set_coefficients({0.5f, 0.3f, 0.2f});
    REQUIRE(fir.order() == 3);
}

// ── BallisticsFilter ─────────────────────────────────────────────────────

TEST_CASE("BallisticsFilter tracks rising signal", "[signal][ballistics]") {
    BallisticsFilter env;
    env.prepare(44100.0f);
    env.set_attack_ms(1.0f);
    env.set_release_ms(100.0f);

    // Feed a step from 0 to 1
    float val = 0;
    for (int i = 0; i < 200; ++i) val = env.process(1.0f);

    // After 200 samples with 1ms attack at 44.1kHz, should be close to 1
    REQUIRE(val > 0.9f);
}

TEST_CASE("BallisticsFilter decays on silence", "[signal][ballistics]") {
    BallisticsFilter env;
    env.prepare(44100.0f);
    env.set_attack_ms(0.1f);
    env.set_release_ms(10.0f);

    // Ramp up
    for (int i = 0; i < 100; ++i) env.process(1.0f);
    float peak = env.current();
    REQUIRE(peak > 0.5f);

    // Feed silence — should decay
    for (int i = 0; i < 4410; ++i) env.process(0.0f);
    REQUIRE(env.current() < 0.01f);
}

TEST_CASE("BallisticsFilter RMS mode", "[signal][ballistics]") {
    BallisticsFilter env;
    env.prepare(44100.0f);
    env.set_attack_ms(0.1f);
    env.set_release_ms(10.0f);
    env.set_mode(BallisticsFilter::Mode::rms);

    // Feed constant signal
    float val = 0;
    for (int i = 0; i < 500; ++i) val = env.process(0.5f);
    REQUIRE_THAT(val, WithinAbs(0.5, 0.05));
}

TEST_CASE("BallisticsFilter reset returns to zero", "[signal][ballistics]") {
    BallisticsFilter env;
    env.prepare(44100.0f);
    for (int i = 0; i < 100; ++i) env.process(1.0f);
    env.reset();
    REQUIRE_THAT(env.current(), WithinAbs(0.0, 0.001));
}

TEST_CASE("BallisticsFilter clamps time constants and processes buffers",
          "[signal][ballistics][issue-645]") {
    BallisticsFilter env;
    env.prepare(48000.0f);
    env.set_attack_ms(-5.0f);
    env.set_release_ms(0.0f);

    REQUIRE_THAT(env.process(-0.5f), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(env.process(0.0f), WithinAbs(0.0f, 1e-6f));

    env.set_mode(BallisticsFilter::Mode::rms);
    const float input[] = {-0.25f, 0.5f, -0.75f};
    float output[] = {0.0f, 0.0f, 0.0f};
    env.process(input, output, 3);

    REQUIRE_THAT(output[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(output[1], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(output[2], WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(env.current(), WithinAbs(0.75f, 1e-6f));
}

// ── LogRampedValue ───────────────────────────────────────────────────────

TEST_CASE("LogRampedValue reaches target", "[signal][log_ramp]") {
    LogRampedValue v(440.0f);
    v.set_ramp_time(0.01f, 44100.0f); // ~441 samples
    v.set_target(880.0f);

    float last = 0;
    for (int i = 0; i < 500; ++i) last = v.next();
    REQUIRE_THAT(last, WithinAbs(880.0, 0.1));
    REQUIRE_FALSE(v.is_smoothing());
}

TEST_CASE("LogRampedValue multiplies exponentially", "[signal][log_ramp]") {
    LogRampedValue v(100.0f);
    v.set_ramp_time(0.001f, 1000.0f); // 1 sample
    v.set_target(200.0f);

    float s1 = v.next();
    REQUIRE_THAT(s1, WithinAbs(200.0, 0.1));
}

TEST_CASE("LogRampedValue set_immediate is instant", "[signal][log_ramp]") {
    LogRampedValue v(100.0f);
    v.set_ramp_time(1.0f, 44100.0f);
    v.set_immediate(500.0f);
    REQUIRE_THAT(v.current_value(), WithinAbs(500.0, 0.001));
    REQUIRE_FALSE(v.is_smoothing());
}

TEST_CASE("LogRampedValue skip advances correctly", "[signal][log_ramp]") {
    LogRampedValue v(100.0f);
    v.set_ramp_time(0.1f, 44100.0f);
    v.set_target(1000.0f);

    // Skip most of the ramp
    v.skip(4400);
    float remaining = v.next();
    REQUIRE(remaining > 900.0f);
}

TEST_CASE("LogRampedValue jumps for non-positive endpoints",
          "[signal][log_ramp][issue-645]") {
    LogRampedValue zero_start;
    zero_start.set_ramp_time(1.0f, 48000.0f);
    zero_start.skip(32);
    zero_start.set_target(440.0f);
    REQUIRE_FALSE(zero_start.is_smoothing());
    REQUIRE_THAT(zero_start.current_value(), WithinAbs(440.0f, 1e-6f));
    REQUIRE_THAT(zero_start.target_value(), WithinAbs(440.0f, 1e-6f));

    LogRampedValue positive(220.0f);
    positive.set_ramp_time(1.0f, 48000.0f);
    positive.set_target(0.0f);
    REQUIRE_FALSE(positive.is_smoothing());
    REQUIRE_THAT(positive.next(), WithinAbs(0.0f, 1e-6f));
}

// ── WaveShaper ───────────────────────────────────────────────────────────

TEST_CASE("WaveShaper covers all curve branches", "[signal][waveshaper][issue-645]") {
    WaveShaper shaper;
    shaper.set_drive(2.0f);

    shaper.set_curve(WaveShaper::Curve::soft_clip);
    REQUIRE_THAT(shaper.process(1.0f), WithinAbs(2.0f / 3.0f, 1e-6f));

    shaper.set_curve(WaveShaper::Curve::hard_clip);
    REQUIRE_THAT(shaper.process(0.75f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(shaper.process(-0.75f), WithinAbs(-1.0f, 1e-6f));

    shaper.set_curve(WaveShaper::Curve::tanh_clip);
    REQUIRE_THAT(shaper.process(-0.5f), WithinAbs(std::tanh(-1.0f), 1e-6f));

    shaper.set_curve(WaveShaper::Curve::fold);
    REQUIRE_THAT(shaper.process(0.75f), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(shaper.process(-0.75f), WithinAbs(-0.5f, 1e-6f));

    shaper.set_drive(1.0f);
    shaper.set_curve(WaveShaper::Curve::sine_fold);
    REQUIRE_THAT(shaper.process(1.0f), WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("WaveShaper processes buffers in-place", "[signal][waveshaper][issue-645]") {
    WaveShaper shaper;
    shaper.set_curve(WaveShaper::Curve::hard_clip);
    shaper.set_drive(3.0f);

    float buffer[] = {-0.5f, 0.0f, 0.25f, 0.5f};
    shaper.process(buffer, 4);

    REQUIRE_THAT(buffer[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(buffer[3], WithinAbs(1.0f, 1e-6f));
}

// ── ProcessorChain ───────────────────────────────────────────────────────

// Simple test processor
struct ScaleBy2 {
    float process(float x) { return x * 2.0f; }
};

struct AddOne {
    float process(float x) { return x + 1.0f; }
};

TEST_CASE("ProcessorChain processes in order", "[signal][chain]") {
    ProcessorChain<ScaleBy2, AddOne> chain;
    // input=3: scale(3)=6, add(6)=7
    REQUIRE_THAT(chain.process(3.0f), WithinAbs(7.0, 0.001));
}

TEST_CASE("ProcessorChain reversed order gives different result", "[signal][chain]") {
    ProcessorChain<AddOne, ScaleBy2> chain;
    // input=3: add(3)=4, scale(4)=8
    REQUIRE_THAT(chain.process(3.0f), WithinAbs(8.0, 0.001));
}

TEST_CASE("ProcessorChain size", "[signal][chain]") {
    ProcessorChain<ScaleBy2, AddOne, ScaleBy2> chain;
    REQUIRE(chain.size() == 3);
}

TEST_CASE("ProcessorChain get accesses elements", "[signal][chain]") {
    ProcessorChain<ScaleBy2, AddOne> chain;
    auto& first = chain.get<0>();
    auto& second = chain.get<1>();
    // Just verify they compile and are accessible
    REQUIRE(first.process(1.0f) == 2.0f);
    REQUIRE(second.process(1.0f) == 2.0f);
}

TEST_CASE("ProcessorChain buffer processing", "[signal][chain]") {
    ProcessorChain<ScaleBy2> chain;
    float buf[] = {1.0f, 2.0f, 3.0f};
    chain.process(buf, 3);
    REQUIRE_THAT(buf[0], WithinAbs(2.0, 0.001));
    REQUIRE_THAT(buf[1], WithinAbs(4.0, 0.001));
    REQUIRE_THAT(buf[2], WithinAbs(6.0, 0.001));
}

// ── LookupTable ──────────────────────────────────────────────────────────

TEST_CASE("LookupTable sine approximation", "[signal][lookup]") {
    LookupTable sine(4096, 0.0f, 1.0f, [](float phase) {
        return std::sin(phase * 2.0f * 3.14159265f);
    });

    REQUIRE(sine.size() == 4096);

    // Check known values
    REQUIRE_THAT(sine.process(0.0f), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(sine.process(0.25f), WithinAbs(1.0, 0.01));
    REQUIRE_THAT(sine.process(0.5f), WithinAbs(0.0, 0.01));
    REQUIRE_THAT(sine.process(0.75f), WithinAbs(-1.0, 0.01));
}

TEST_CASE("LookupTable tanh approximation", "[signal][lookup]") {
    LookupTable tanh_t(1024, -4.0f, 4.0f, [](float x) {
        return std::tanh(x);
    });

    // tanh(0) = 0
    REQUIRE_THAT(tanh_t.process(0.0f), WithinAbs(0.0, 0.01));
    // tanh(large) ≈ 1
    REQUIRE_THAT(tanh_t.process(3.0f), WithinAbs(std::tanh(3.0f), 0.02));
    // tanh(-large) ≈ -1
    REQUIRE_THAT(tanh_t.process(-3.0f), WithinAbs(std::tanh(-3.0f), 0.02));
}

TEST_CASE("LookupTable clamps out-of-range input", "[signal][lookup]") {
    LookupTable lin(100, 0.0f, 1.0f, [](float x) { return x; });

    // Below min
    REQUIRE_THAT(lin.process(-1.0f), WithinAbs(0.0, 0.02));
    // Above max
    REQUIRE_THAT(lin.process(2.0f), WithinAbs(1.0, 0.02));
}

TEST_CASE("LookupTable empty returns input", "[signal][lookup]") {
    LookupTable empty;
    REQUIRE_THAT(empty.process(5.0f), WithinAbs(5.0, 0.001));
    REQUIRE(empty.empty());
}

TEST_CASE("LookupTable buffer processing", "[signal][lookup]") {
    LookupTable sq(1000, -2.0f, 2.0f, [](float x) { return x * x; });
    float buf[] = {0.0f, 1.0f, -1.0f};
    sq.process(buf, 3);
    REQUIRE_THAT(buf[0], WithinAbs(0.0, 0.01));
    REQUIRE_THAT(buf[1], WithinAbs(1.0, 0.01));
    REQUIRE_THAT(buf[2], WithinAbs(1.0, 0.01));
}

// ── TptFilter ────────────────────────────────────────────────────────────

TEST_CASE("TptFilter lowpass passes DC", "[signal][tpt]") {
    TptFilter f;
    f.prepare(44100.0f);
    f.set_cutoff(5000.0f);

    // Feed constant signal — LP should converge to it
    float val = 0;
    for (int i = 0; i < 2000; ++i) val = f.process_lowpass(1.0f);
    REQUIRE_THAT(val, WithinAbs(1.0, 0.01));
}

TEST_CASE("TptFilter highpass blocks DC", "[signal][tpt]") {
    TptFilter f;
    f.prepare(44100.0f);
    f.set_cutoff(1000.0f);

    // Feed constant signal — HP should converge to 0
    float val = 0;
    for (int i = 0; i < 2000; ++i) val = f.process_highpass(1.0f);
    REQUIRE_THAT(val, WithinAbs(0.0, 0.01));
}

TEST_CASE("TptFilter allpass preserves amplitude", "[signal][tpt]") {
    TptFilter f;
    f.prepare(44100.0f);
    f.set_cutoff(2000.0f);

    // After settling, allpass of a constant = constant (or -constant)
    float val = 0;
    for (int i = 0; i < 5000; ++i) val = f.process_allpass(1.0f);
    // Allpass converges: LP→1, HP→0, AP = 2*LP - input = 2*1 - 1 = 1
    REQUIRE_THAT(val, WithinAbs(1.0, 0.02));
}

TEST_CASE("TptFilter multi-output consistency", "[signal][tpt]") {
    TptFilter f;
    f.prepare(44100.0f);
    f.set_cutoff(3000.0f);

    // LP + HP should equal input
    // But since process() advances state, we need a separate instance
    TptFilter f2;
    f2.prepare(44100.0f);
    f2.set_cutoff(3000.0f);

    float input = 0.7f;
    auto out = f2.process(input);
    REQUIRE_THAT(out.lowpass + out.highpass, WithinAbs(input, 0.001));
}

TEST_CASE("TptFilter reset clears state", "[signal][tpt]") {
    TptFilter f;
    f.prepare(44100.0f);
    f.set_cutoff(1000.0f);
    for (int i = 0; i < 100; ++i) f.process_lowpass(1.0f);
    f.reset();
    // After reset, output of 0 input should be 0
    REQUIRE_THAT(f.process_lowpass(0.0f), WithinAbs(0.0, 0.001));
}

TEST_CASE("TptFilter cutoff clamping", "[signal][tpt]") {
    TptFilter f;
    f.prepare(44100.0f);
    f.set_cutoff(0.0f); // should clamp to 1 Hz
    REQUIRE(f.cutoff() >= 1.0f);
    f.set_cutoff(100000.0f); // should clamp to near Nyquist
    REQUIRE(f.cutoff() < 44100.0f * 0.5f);
}
