#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/signal/fir_filter.hpp>
#include <pulp/signal/ballistics_filter.hpp>
#include <pulp/signal/log_ramped_value.hpp>
#include <pulp/signal/processor_chain.hpp>
#include <pulp/signal/lookup_table.hpp>
#include <pulp/signal/tpt_filter.hpp>
#include <pulp/signal/gain.hpp>
#include <pulp/signal/interpolator.hpp>
#include <pulp/signal/oscillator.hpp>
#include <pulp/signal/simd_buffer.hpp>
#include <pulp/signal/spectrogram.hpp>
#include <pulp/signal/special_functions.hpp>
#include <pulp/signal/stft.hpp>
#include <pulp/signal/smoothed_value.hpp>
#include <pulp/signal/waveshaper.hpp>
#include <pulp/signal/windowing.hpp>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

// ── Gain and simple mixing ───────────────────────────────────────────────

TEST_CASE("Gain converts dB and processes scalar and buffers",
          "[signal][gain][codecov]") {
    REQUIRE_THAT(db_to_linear(0.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(db_to_linear(6.0f), WithinRel(1.99526f, 1e-4f));
    REQUIRE_THAT(linear_to_db(1.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(linear_to_db(0.0f), WithinAbs(-200.0f, 1e-5f));

    Gain gain;
    gain.set_gain_db(6.0f);
    REQUIRE_THAT(gain.gain_db(), WithinAbs(6.0f, 1e-4f));
    REQUIRE_THAT(gain.process(0.5f), WithinRel(0.99763f, 1e-4f));

    gain.set_gain_linear(0.25f);
    float buffer[] = {4.0f, -2.0f, 0.0f};
    gain.process(buffer, 3);
    REQUIRE_THAT(buffer[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("SimpleMixer clamps mix and processes arrays",
          "[signal][gain][mixer][codecov]") {
    SimpleMixer mixer;
    mixer.set_mix(-1.0f);
    REQUIRE_THAT(mixer.mix(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(mixer.process(2.0f, 10.0f), WithinAbs(2.0f, 1e-6f));

    mixer.set_mix(2.0f);
    REQUIRE_THAT(mixer.mix(), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(mixer.process(2.0f, 10.0f), WithinAbs(10.0f, 1e-6f));

    mixer.set_mix(0.25f);
    const float dry[] = {0.0f, 4.0f, -4.0f};
    const float wet[] = {8.0f, 8.0f, 8.0f};
    float output[] = {0.0f, 0.0f, 0.0f};
    mixer.process(dry, wet, output, 3);
    REQUIRE_THAT(output[0], WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(output[1], WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(output[2], WithinAbs(-1.0f, 1e-6f));
}

// ── WindowFunction ──────────────────────────────────────────────────────

TEST_CASE("WindowFunction generates all supported window families",
          "[signal][window][codecov]") {
    const auto rectangular = WindowFunction::generate(8, WindowFunction::Type::rectangular);
    REQUIRE(rectangular.size() == 8);
    for (float value : rectangular)
        REQUIRE_THAT(value, WithinAbs(1.0f, 1e-6f));

    const auto hann = WindowFunction::generate(8, WindowFunction::Type::hann);
    REQUIRE_THAT(hann.front(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(hann.back(), WithinAbs(0.0f, 1e-6f));
    REQUIRE(hann[3] > hann[1]);

    const auto hamming = WindowFunction::generate(8, WindowFunction::Type::hamming);
    REQUIRE_THAT(hamming.front(), WithinAbs(0.08f, 1e-5f));
    REQUIRE_THAT(hamming.back(), WithinAbs(0.08f, 1e-5f));

    const auto blackman = WindowFunction::generate(8, WindowFunction::Type::blackman);
    REQUIRE(blackman[3] > blackman[1]);

    const auto flat_top = WindowFunction::generate(8, WindowFunction::Type::flat_top);
    REQUIRE(flat_top[3] > flat_top[0]);

    const auto kaiser_default = WindowFunction::generate(8, WindowFunction::Type::kaiser);
    const auto kaiser_custom = WindowFunction::generate(8, WindowFunction::Type::kaiser, 8.0f);
    REQUIRE_THAT(kaiser_default[3], WithinAbs(kaiser_default[4], 1e-6f));
    REQUIRE(kaiser_custom[0] < kaiser_default[0]);
}

TEST_CASE("WindowFunction applies bounded windows in place",
          "[signal][window][codecov]") {
    float buffer[] = {1.0f, 2.0f, 3.0f, 4.0f};
    const std::vector<float> window = {1.0f, 0.5f, 0.25f};

    WindowFunction::apply(buffer, window);

    REQUIRE_THAT(buffer[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(0.75f, 1e-6f));
    REQUIRE_THAT(buffer[3], WithinAbs(4.0f, 1e-6f));
}

// ── Oscillator ──────────────────────────────────────────────────────────

TEST_CASE("Oscillator sine phase advances wraps and reset restores phase",
          "[signal][oscillator][codecov]") {
    Oscillator osc;
    osc.set_sample_rate(4.0f);
    osc.set_frequency(1.0f);
    osc.set_waveform(Oscillator::Waveform::sine);

    REQUIRE_THAT(osc.phase(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(osc.next(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(osc.phase(), WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(osc.next(), WithinAbs(1.0f, 1e-6f));

    osc.next();
    osc.next();
    REQUIRE_THAT(osc.phase(), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(osc.frequency(), WithinAbs(1.0f, 1e-6f));

    osc.next();
    osc.reset();
    REQUIRE_THAT(osc.phase(), WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("Oscillator waveforms produce finite bounded startup samples",
          "[signal][oscillator][codecov]") {
    Oscillator osc;
    osc.set_sample_rate(16.0f);
    osc.set_frequency(1.0f);

    for (auto waveform : {
             Oscillator::Waveform::saw,
             Oscillator::Waveform::square,
             Oscillator::Waveform::triangle,
         }) {
        osc.reset();
        osc.set_waveform(waveform);
        for (int i = 0; i < 32; ++i) {
            const float sample = osc.next();
            REQUIRE(std::isfinite(sample));
            REQUIRE(sample >= -1.5f);
            REQUIRE(sample <= 1.5f);
        }
    }
}

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

TEST_CASE("FirFilter empty coefficients pass samples through and reset safely",
          "[signal][fir][issue-645]") {
    FirFilter fir;
    REQUIRE(fir.order() == 0);
    REQUIRE_THAT(fir.process(-0.25f), WithinAbs(-0.25f, 1e-6f));

    float buffer[] = {0.25f, -0.5f, 0.75f};
    fir.process(buffer, 3);
    REQUIRE_THAT(buffer[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(-0.5f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(0.75f, 1e-6f));

    fir.reset();
    REQUIRE(fir.order() == 0);
    REQUIRE_THAT(fir.process(0.5f), WithinAbs(0.5f, 1e-6f));
}

TEST_CASE("FirFilter coefficient replacement clears stale delay samples",
          "[signal][fir][coverage][phase3]") {
    FirFilter fir;
    fir.set_coefficients({0.5f, 0.5f});
    REQUIRE_THAT(fir.process(1.0f), WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(fir.process(1.0f), WithinAbs(1.0f, 1e-6f));

    fir.set_coefficients({1.0f});
    REQUIRE(fir.order() == 1);
    REQUIRE_THAT(fir.process(0.25f), WithinAbs(0.25f, 1e-6f));
}

TEST_CASE("FirFilter highpass spectral inversion boosts center tap",
          "[signal][fir][coverage][phase3]") {
    auto lowpass = FirFilter::lowpass(5, 1000.0f, 48000.0f);
    auto highpass = FirFilter::highpass(5, 1000.0f, 48000.0f);

    REQUIRE(lowpass.size() == highpass.size());
    for (std::size_t i = 0; i < highpass.size(); ++i) {
        if (i == 2) {
            REQUIRE_THAT(highpass[i], WithinAbs(1.0f - lowpass[i], 1e-6f));
        } else {
            REQUIRE_THAT(highpass[i], WithinAbs(-lowpass[i], 1e-6f));
        }
    }
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

TEST_CASE("BallisticsFilter ignores invalid sample rates without unstable state",
          "[signal][ballistics][coverage][phase3]") {
    BallisticsFilter env;
    env.prepare(0.0f);
    env.set_attack_ms(10.0f);
    env.set_release_ms(10.0f);

    REQUIRE_THAT(env.process(1.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(env.current(), WithinAbs(0.0f, 1e-6f));

    env.prepare(1000.0f);
    REQUIRE(env.process(1.0f) > 0.0f);
}

TEST_CASE("BallisticsFilter mode switch reports current envelope consistently",
          "[signal][ballistics][coverage][phase3]") {
    BallisticsFilter env;
    env.prepare(1000.0f);
    env.set_attack_ms(0.01f);
    env.set_release_ms(100.0f);

    REQUIRE_THAT(env.process(0.25f), WithinAbs(0.25f, 1e-6f));
    env.set_mode(BallisticsFilter::Mode::rms);
    REQUIRE_THAT(env.current(), WithinAbs(0.5f, 1e-6f));

    env.reset();
    REQUIRE_THAT(env.current(), WithinAbs(0.0f, 1e-6f));
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

TEST_CASE("SmoothedValue supports double ramps and terminal skips",
          "[signal][smooth][coverage]") {
    SmoothedValue<double> value(2.0);
    value.set_ramp_time(0.004, 1000.0);
    value.set_target(10.0);

    REQUIRE(value.is_smoothing());
    REQUIRE_THAT(value.next(), WithinAbs(4.0, 1e-12));

    value.skip(2);
    REQUIRE_THAT(value.current(), WithinAbs(8.0, 1e-12));
    REQUIRE(value.is_smoothing());

    value.skip(99);
    REQUIRE_FALSE(value.is_smoothing());
    REQUIRE_THAT(value.current(), WithinAbs(10.0, 1e-12));
    REQUIRE_THAT(value.target(), WithinAbs(10.0, 1e-12));
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

TEST_CASE("LogRampedValue ignores non-positive skips",
          "[signal][log_ramp][coverage][phase3]") {
    LogRampedValue v(100.0f);
    v.set_ramp_time(0.01f, 1000.0f);
    v.set_target(200.0f);

    v.skip(0);
    v.skip(-3);

    REQUIRE(v.is_smoothing());
    REQUIRE_THAT(v.current_value(), WithinAbs(100.0f, 1e-6f));
    REQUIRE(v.next() > 100.0f);
    REQUIRE(v.next() < 200.0f);
}

TEST_CASE("LogRampedValue skip to exact ramp end snaps to target",
          "[signal][log_ramp][coverage][phase3]") {
    LogRampedValue value(100.0f);
    value.set_ramp_time(0.004f, 1000.0f);
    value.set_target(1600.0f);

    REQUIRE(value.is_smoothing());
    value.skip(4);

    REQUIRE_FALSE(value.is_smoothing());
    REQUIRE_THAT(value.current_value(), WithinAbs(1600.0f, 1e-6f));
    REQUIRE_THAT(value.next(), WithinAbs(1600.0f, 1e-6f));
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

struct ResettableScale {
    float factor = 2.0f;

    float process(float x) { return x * factor; }
    void reset() { factor = 1.0f; }
};

struct ResettableOffset {
    float offset = 3.0f;

    float process(float x) { return x + offset; }
    void reset() { offset = 0.0f; }
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

TEST_CASE("ProcessorChain reset skips processors without reset methods",
          "[signal][chain][issue-645]") {
    ProcessorChain<ResettableScale, AddOne, ResettableOffset> chain;

    REQUIRE_THAT(chain.process(2.0f), WithinAbs(8.0f, 1e-6f));

    chain.reset();
    const auto& const_chain = chain;
    REQUIRE_THAT(const_chain.get<0>().factor, WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(const_chain.get<2>().offset, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(chain.process(2.0f), WithinAbs(3.0f, 1e-6f));
}

TEST_CASE("ProcessorChain zero and negative length buffers are no-ops",
          "[signal][chain][coverage][phase3]") {
    ProcessorChain<ScaleBy2, AddOne> chain;
    float samples[] = {1.0f, 2.0f};

    chain.process(samples, 0);
    chain.process(samples, -4);

    REQUIRE_THAT(samples[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(2.0f, 1e-6f));
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

TEST_CASE("LookupTable indexed access clamps and zero-length buffers are no-ops",
          "[signal][lookup][issue-645]") {
    LookupTable table(5, 0.0f, 1.0f, [](float x) { return 10.0f + x; });

    REQUIRE_THAT(table[-100], WithinAbs(10.0f, 1e-6f));
    REQUIRE_THAT(table[0], WithinAbs(10.0f, 1e-6f));
    REQUIRE_THAT(table[2], WithinAbs(10.5f, 1e-6f));
    REQUIRE_THAT(table[99], WithinAbs(11.0f, 1e-6f));

    float samples[] = {-1.0f, 0.5f, 2.0f};
    table.process(samples, 0);
    table.process(samples, -3);
    REQUIRE_THAT(samples[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(0.5f, 1e-6f));
    REQUIRE_THAT(samples[2], WithinAbs(2.0f, 1e-6f));

    table.process(samples, 3);
    REQUIRE_THAT(samples[0], WithinAbs(10.0f, 1e-6f));
    REQUIRE_THAT(samples[1], WithinAbs(10.5f, 1e-6f));
    REQUIRE_THAT(samples[2], WithinAbs(11.0f, 1e-6f));
}

TEST_CASE("LookupTable interpolates between adjacent entries",
          "[signal][lookup][coverage][phase3]") {
    LookupTable table(3, 0.0f, 2.0f, [](float x) { return x * 10.0f; });

    REQUIRE(table.size() == 3);
    REQUIRE_THAT(table.process(0.25f), WithinAbs(2.5f, 1e-6f));
    REQUIRE_THAT(table.process(0.5f), WithinAbs(5.0f, 1e-6f));
    REQUIRE_THAT(table.process(1.5f), WithinAbs(15.0f, 1e-6f));
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

TEST_CASE("TptFilter combined output resets to initial impulse response",
          "[signal][tpt][coverage][phase3]") {
    TptFilter filter;
    filter.prepare(48000.0f);
    filter.set_cutoff(1200.0f);

    const auto first = filter.process(1.0f);
    filter.process(0.5f);
    filter.process(-0.25f);
    filter.reset();
    const auto after_reset = filter.process(1.0f);

    REQUIRE_THAT(first.lowpass, WithinAbs(after_reset.lowpass, 1e-6f));
    REQUIRE_THAT(first.highpass, WithinAbs(after_reset.highpass, 1e-6f));
    REQUIRE_THAT(first.allpass, WithinAbs(after_reset.allpass, 1e-6f));
}

// ── Visualization helpers ───────────────────────────────────────────────

TEST_CASE("ColorMapper clamps values and switches color ramps",
          "[signal][spectrogram][codecov]") {
    ColorMapper mapper(ColorRamp::grayscale);

    auto black = mapper.map(-1.0f);
    REQUIRE(black.r == 0);
    REQUIRE(black.g == 0);
    REQUIRE(black.b == 0);
    REQUIRE(black.a == 255);

    auto white = mapper.map(2.0f);
    REQUIRE(white.r == 255);
    REQUIRE(white.g == 255);
    REQUIRE(white.b == 255);

    mapper.set_ramp(ColorRamp::heat);
    REQUIRE(mapper.ramp() == ColorRamp::heat);
    auto heat_mid = mapper.map(0.5f);
    REQUIRE(heat_mid.r >= heat_mid.g);
    REQUIRE(heat_mid.a == 255);

    mapper.set_ramp(ColorRamp::viridis);
    auto viridis_mid = mapper.map(0.5f);
    REQUIRE(viridis_mid.g > viridis_mid.r);

    mapper.set_ramp(ColorRamp::inferno);
    auto inferno_low = mapper.map(0.0f);
    REQUIRE(inferno_low.b > inferno_low.r);
}

TEST_CASE("FrequencyAxis maps linear logarithmic and mel scales",
          "[signal][spectrogram][codecov]") {
    FrequencyAxis axis;
    axis.configure(1024, 48000.0f, FrequencyScale::linear);
    REQUIRE(axis.num_bins() == 513);
    REQUIRE_THAT(axis.nyquist(), WithinAbs(24000.0f, 1e-5f));
    REQUIRE(axis.scale() == FrequencyScale::linear);
    REQUIRE_THAT(axis.bin_to_hz(256), WithinAbs(12000.0f, 1e-5f));
    REQUIRE(axis.hz_to_bin(-100.0f) == 0);
    REQUIRE(axis.hz_to_bin(999999.0f) == 512);
    REQUIRE_THAT(axis.hz_to_display(12000.0f), WithinAbs(0.5f, 1e-5f));
    REQUIRE_THAT(axis.display_to_hz(0.5f), WithinAbs(12000.0f, 1e-5f));

    axis.configure(1024, 48000.0f, FrequencyScale::logarithmic);
    REQUIRE(axis.display_to_bin(axis.bin_to_display(64)) == 64);
    REQUIRE(axis.display_to_hz(-1.0f) >= 1.0f);
    REQUIRE_THAT(axis.display_to_hz(2.0f), WithinAbs(axis.nyquist(), 1e-2f));

    axis.configure(1024, 48000.0f, FrequencyScale::mel);
    const float mel_pos = axis.hz_to_display(1000.0f);
    REQUIRE(mel_pos > 0.0f);
    REQUIRE(mel_pos < 1.0f);
    REQUIRE(axis.display_to_bin(mel_pos) == axis.hz_to_bin(1000.0f));
}

TEST_CASE("SpectrogramBuffer scrolls columns and maps dB ranges",
          "[signal][spectrogram][codecov]") {
    SpectrogramBuffer buffer;
    ColorMapper mapper(ColorRamp::grayscale);
    buffer.configure(3, 2);

    const float first[] = {-80.0f, -40.0f, 0.0f, -20.0f};
    buffer.push_column(first, 4, mapper, -80.0f, 0.0f);
    REQUIRE(buffer.width() == 3);
    REQUIRE(buffer.height() == 2);
    REQUIRE(buffer.frames_written() == 1);
    REQUIRE(buffer.write_column() == 1);
    REQUIRE(buffer.pixels()[0].r == 0);
    REQUIRE(buffer.pixels()[3].r == 255);

    const float second[] = {-20.0f, -20.0f};
    const float third[] = {-10.0f, -10.0f};
    const float fourth[] = {-5.0f, -5.0f};
    buffer.push_column(second, 2, mapper, 0.0f, 0.0f);
    buffer.push_column(third, 2, mapper);
    buffer.push_column(fourth, 2, mapper);

    REQUIRE(buffer.frames_written() == 4);
    REQUIRE(buffer.write_column() == 1);
    REQUIRE(buffer.pixels()[0].r > 200);
}

TEST_CASE("STFT emits frames converts dB and resets state",
          "[signal][stft][codecov]") {
    StftConfig config;
    config.fft_size = 256;
    config.hop_size = 64;
    config.window = WindowFunction::Type::hann;

    Stft stft(config);
    REQUIRE(stft.fft_size() == 256);
    REQUIRE(stft.hop_size() == 64);
    REQUIRE(stft.num_bins() == 129);
    REQUIRE_FALSE(stft.frame_ready());

    std::vector<float> samples(255, 0.0f);
    REQUIRE_FALSE(stft.push_samples(samples.data(), static_cast<int>(samples.size())));
    samples.assign(1, 1.0f);
    REQUIRE(stft.push_samples(samples.data(), 1));
    REQUIRE(stft.frame_ready());
    REQUIRE(stft.latest_frame().num_bins == 129);

    auto db = stft.latest_magnitude_db(-90.0f);
    REQUIRE(db.size() == 129);
    for (float value : db)
        REQUIRE(value >= -90.0f);

    float magnitudes[] = {0.0f, 1.0f};
    Stft::to_db(magnitudes, 2, -60.0f);
    REQUIRE_THAT(magnitudes[0], WithinAbs(-60.0f, 1e-5f));
    REQUIRE_THAT(magnitudes[1], WithinAbs(0.0f, 1e-5f));

    stft.reset();
    REQUIRE_FALSE(stft.frame_ready());
    for (float mag : stft.latest_frame().magnitude)
        REQUIRE_THAT(mag, WithinAbs(0.0f, 1e-6f));
}

TEST_CASE("AlignedBuffer resizes moves clears and copies bounded input",
          "[signal][simd][codecov]") {
    AlignedBuffer empty;
    REQUIRE(empty.empty());
    REQUIRE(empty.data() == nullptr);

    AlignedBuffer buffer(3);
    REQUIRE(buffer.size() == 3);
    REQUIRE_FALSE(buffer.empty());
    REQUIRE(reinterpret_cast<std::uintptr_t>(buffer.data()) % kSimdAlignment == 0);
    REQUIRE_THAT(buffer[0], WithinAbs(0.0f, 1e-6f));

    const float source[] = {1.0f, 2.0f, 3.0f, 4.0f};
    buffer.copy_from(source, 4);
    REQUIRE_THAT(buffer[0], WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(3.0f, 1e-6f));

    buffer.clear();
    REQUIRE_THAT(buffer[1], WithinAbs(0.0f, 1e-6f));

    buffer.resize(5);
    REQUIRE(buffer.size() == 5);
    REQUIRE_THAT(buffer[4], WithinAbs(0.0f, 1e-6f));

    AlignedBuffer moved(std::move(buffer));
    REQUIRE(moved.size() == 5);
    REQUIRE(buffer.empty());

    AlignedBuffer assigned;
    assigned = std::move(moved);
    REQUIRE(assigned.size() == 5);
    REQUIRE(moved.empty());
    assigned.resize(0);
    REQUIRE(assigned.empty());
}

TEST_CASE("AlignedBuffer partial copy preserves untouched suffix",
          "[signal][simd][coverage][phase3]") {
    AlignedBuffer buffer(4);
    float initial[] = {1.0f, 2.0f, 3.0f, 4.0f};
    buffer.copy_from(initial, 4);

    float prefix[] = {-1.0f, -2.0f};
    buffer.copy_from(prefix, 2);

    REQUIRE_THAT(buffer[0], WithinAbs(-1.0f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(-2.0f, 1e-6f));
    REQUIRE_THAT(buffer[2], WithinAbs(3.0f, 1e-6f));
    REQUIRE_THAT(buffer[3], WithinAbs(4.0f, 1e-6f));
}

TEST_CASE("AlignedBuffer resize to same size preserves allocation contents",
          "[signal][simd][coverage][phase3]") {
    AlignedBuffer buffer(2);
    buffer[0] = 0.25f;
    buffer[1] = -0.5f;
    auto* before = buffer.data();

    buffer.resize(2);

    REQUIRE(buffer.data() == before);
    REQUIRE_THAT(buffer[0], WithinAbs(0.25f, 1e-6f));
    REQUIRE_THAT(buffer[1], WithinAbs(-0.5f, 1e-6f));
}

TEST_CASE("Interpolator kernels hit exact endpoints and smooth midpoints",
          "[signal][interp][codecov]") {
    REQUIRE_THAT(Interpolator::linear(0.0f, 2.0f, 6.0f), WithinAbs(2.0f, 1e-6f));
    REQUIRE_THAT(Interpolator::linear(1.0f, 2.0f, 6.0f), WithinAbs(6.0f, 1e-6f));
    REQUIRE_THAT(Interpolator::linear(0.25f, 2.0f, 6.0f), WithinAbs(3.0f, 1e-6f));

    REQUIRE_THAT(Interpolator::hermite(0.0f, -1.0f, 0.0f, 1.0f, 2.0f),
                 WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(Interpolator::hermite(1.0f, -1.0f, 0.0f, 1.0f, 2.0f),
                 WithinAbs(1.0f, 1e-6f));

    REQUIRE_THAT(Interpolator::lagrange(0.5f, 0.0f, 1.0f, 4.0f, 9.0f),
                 WithinAbs(2.25f, 1e-6f));

    const float sinc_mid = Interpolator::sinc6(0.5f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f);
    REQUIRE(sinc_mid > 0.75f);
    REQUIRE(sinc_mid < 1.25f);
}

TEST_CASE("Special functions cover sinc and Lanczos boundaries",
          "[signal][math][coverage][phase3]") {
    REQUIRE_THAT(sinc(0.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(sinc(1.0f), WithinAbs(0.0f, 1e-6f));

    REQUIRE_THAT(lanczos(0.0f, 3), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(lanczos(3.0f, 3), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(lanczos(-3.0f, 3), WithinAbs(0.0f, 1e-6f));
    REQUIRE(lanczos(0.5f, 3) > 0.0f);
}

TEST_CASE("Special functions convert gain and MIDI reference points",
          "[signal][math][coverage][phase3]") {
    REQUIRE_THAT(db_to_linear(0.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(linear_to_db(1.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(linear_to_db(-1.0f), WithinAbs(-200.0f, 1e-6f));

    REQUIRE_THAT(freq_to_midi(440.0f), WithinAbs(69.0f, 1e-6f));
    REQUIRE_THAT(midi_to_freq(69.0f), WithinAbs(440.0f, 1e-6f));
    REQUIRE_THAT(midi_to_freq(81.0f), WithinAbs(880.0f, 1e-5f));
}

TEST_CASE("Special functions wrap standard gamma and error functions",
          "[signal][math][coverage][phase3]") {
    REQUIRE_THAT(bessel_i0(0.0f), WithinAbs(1.0f, 1e-6f));
    REQUIRE(bessel_i0(2.0f) > 2.0f);

    REQUIRE_THAT(gamma_fn(5.0f), WithinAbs(24.0f, 1e-5f));
    REQUIRE_THAT(erf_fn(0.0f), WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(erfc_fn(0.0f), WithinAbs(1.0f, 1e-6f));
}
