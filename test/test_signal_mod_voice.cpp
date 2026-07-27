// Modulation toolkit, voice domain: VCA, low-pass gate, mod matrix, and the
// composition patches those primitives are meant to be wired into.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/signal/envelope.hpp>
#include <pulp/signal/lfo.hpp>
#include <pulp/signal/lpg.hpp>
#include <pulp/signal/mod_matrix.hpp>
#include <pulp/signal/mod_tools.hpp>
#include <pulp/signal/rng.hpp>
#include <pulp/signal/trigger.hpp>
#include <pulp/signal/units.hpp>
#include <pulp/signal/vca.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

using namespace pulp::signal;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

constexpr float kSampleRate = 48000.0f;

float rms(const std::vector<float>& v) {
    if (v.empty())
        return 0.0f;
    double sum = 0.0;
    for (float x : v)
        sum += static_cast<double>(x) * static_cast<double>(x);
    return static_cast<float>(std::sqrt(sum / static_cast<double>(v.size())));
}

std::vector<float> sine(int n, float hz, float amplitude = 1.0f) {
    std::vector<float> out(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        out[static_cast<std::size_t>(i)] =
            amplitude * std::sin(6.2831853071795864f * hz * static_cast<float>(i) / kSampleRate);
    return out;
}

} // namespace

// ── VcaT ─────────────────────────────────────────────────────────────────────

TEST_CASE("Vca response curves hit their defining points", "[signal][mod][vca]") {
    Vca vca;
    vca.prepare(kSampleRate);

    vca.set_response(Vca::Response::linear);
    REQUIRE(vca.gain_for(0.0f) == 0.0f);
    REQUIRE(vca.gain_for(1.0f) == 1.0f);
    REQUIRE_THAT(vca.gain_for(0.5f), WithinAbs(0.5f, 1e-6f));

    vca.set_response(Vca::Response::exponential);
    REQUIRE(vca.gain_for(0.0f) == 0.0f);
    // Unity at full control is a series law, so this must be exact, not close.
    REQUIRE(vca.gain_for(1.0f) == 1.0f);
    // The exponential curve sits below the linear one everywhere in between.
    for (float c = 0.05f; c < 1.0f; c += 0.05f)
        REQUIRE(vca.gain_for(c) < c);
    // ~40 dB of span: the bottom of the control range is perceptually silent.
    REQUIRE(units::linear_to_db(vca.gain_for(0.05f)) < -35.0f);
}

TEST_CASE("Vca lag ramps over exactly the set time and lands on target", "[signal][mod][vca]") {
    Vca vca;
    vca.prepare(kSampleRate);
    vca.set_lag_ms(1.0); // 48 samples
    vca.reset(0.0f);

    for (int i = 0; i < 47; ++i)
        (void)vca.next_gain(1.0f);
    REQUIRE(vca.control() < 1.0f);
    REQUIRE(vca.next_gain(1.0f) == 1.0f);

    // A held control must not restart the ramp every sample: after landing it
    // stays landed.
    for (int i = 0; i < 100; ++i)
        REQUIRE(vca.next_gain(1.0f) == 1.0f);
}

TEST_CASE("Vca lag removes the step from a hard-switched gate", "[signal][mod][vca]") {
    auto largest_jump = [](double lag_ms) {
        Vca vca;
        vca.prepare(kSampleRate);
        vca.set_lag_ms(lag_ms);
        vca.reset(0.0f);
        float previous = 0.0f;
        float largest = 0.0f;
        for (int i = 0; i < 500; ++i) {
            const float gain = vca.next_gain(i < 100 ? 0.0f : 1.0f);
            largest = std::max(largest, std::abs(gain - previous));
            previous = gain;
        }
        return largest;
    };
    REQUIRE(largest_jump(0.0) > 0.9f);  // no lag: the full step in one sample
    REQUIRE(largest_jump(5.0) < 0.01f); // 5 ms: spread over 240 samples
}

TEST_CASE("Vca is bit-transparent when held wide open", "[signal][mod][vca]") {
    Vca vca;
    vca.prepare(kSampleRate);
    vca.set_response(Vca::Response::exponential);
    vca.reset(1.0f);
    const auto input = sine(1000, 440.0f, 0.7f);
    for (float x : input)
        REQUIRE(vca.process(x, 1.0f) == x);
}

// ── LpgT ─────────────────────────────────────────────────────────────────────

TEST_CASE("Lpg strike rises to the vactrol's pulse-window level", "[signal][mod][lpg]") {
    Lpg lpg;
    lpg.prepare(kSampleRate);
    lpg.set_droop(0.0f); // plain first-order model, for the closed form
    lpg.set_decay_ms(150.0);
    lpg.reset();
    lpg.strike(1.0f);

    // The pulse is two rise time constants long, so a cold strike reaches
    // 1 - e^-2 of the strike level.
    for (int i = 0; i < 144; ++i)
        (void)lpg.process(0.0f);
    REQUIRE_THAT(lpg.control(), WithinRel(1.0f - std::exp(-2.0f), 0.01f));

    // ...and then closes as a first-order decay with the set time constant.
    const float peak = lpg.control();
    for (int i = 0; i < 7200; ++i)
        (void)lpg.process(0.0f); // 150 ms
    REQUIRE_THAT(lpg.control(), WithinRel(peak * std::exp(-1.0f), 0.02f));
}

TEST_CASE("Lpg droop lengthens the tail without changing the peak", "[signal][mod][lpg]") {
    auto tail_after = [](float droop, int samples) {
        Lpg lpg;
        lpg.prepare(kSampleRate);
        lpg.set_droop(droop);
        lpg.set_decay_ms(150.0);
        lpg.reset();
        lpg.strike(1.0f);
        for (int i = 0; i < 144 + samples; ++i)
            (void)lpg.process(0.0f);
        return lpg.control();
    };

    // The physical claim in the header: the close is fast at first and then
    // lengthens, so a drooping cell is *ahead* early and *behind* late.
    REQUIRE(tail_after(0.5f, 7200) > tail_after(0.0f, 7200));
    REQUIRE(tail_after(0.9f, 7200) > tail_after(0.5f, 7200));
}

TEST_CASE("Lpg reset synchronizes cutoff telemetry with the filter command", "[signal][mod][lpg]") {
    Lpg lpg;
    lpg.prepare(kSampleRate);
    lpg.set_range_hz(100.0f, 6400.0f);
    lpg.set_colour(0.5f);
    lpg.set_gate(1.0f);
    for (int i = 0; i < 10000; ++i)
        (void)lpg.process(0.0f);
    REQUIRE(lpg.commanded_cutoff_hz() > 6000.0f);

    lpg.reset();

    // At a reset cell state of zero and colour 0.5, the filter control is
    // halfway through the logarithmic range: sqrt(100 * 6400) = 800 Hz.
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinRel(800.0f, 1.0e-6f));
    (void)lpg.process(0.0f);
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinRel(800.0f, 1.0e-6f));
}

TEST_CASE("Lpg reset cutoff remains valid at a non-positive sample rate", "[signal][mod][lpg]") {
    Lpg lpg;
    lpg.prepare(0.0f);

    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinRel(1.0f, 1.0e-6f));
    REQUIRE(std::isfinite(lpg.process(0.0f)));
}

TEST_CASE("Lpg range normalization is finite for every endpoint order", "[signal][mod][lpg]") {
    Lpg lpg;
    lpg.prepare(96000.0f);
    lpg.set_colour(1.0f);

    lpg.set_range_hz(20000.0f, 10.0f);
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinAbs(10.0f, 1.0e-6f));

    lpg.set_range_hz(20000.0f, 20000.0f);
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinAbs(20000.0f, 1.0e-3f));

    lpg.set_range_hz(-std::numeric_limits<float>::infinity(),
                     std::numeric_limits<float>::infinity());
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinAbs(10.0f, 1.0e-6f));

    const float nan = std::numeric_limits<float>::quiet_NaN();
    lpg.set_range_hz(nan, nan);
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinAbs(40.0f, 1.0e-6f));
    REQUIRE(std::isfinite(lpg.process(0.25f)));
}

TEST_CASE("Lpg cutoff-affecting setters update the supported telemetry", "[signal][mod][lpg]") {
    Lpg lpg;
    lpg.prepare(kSampleRate);
    lpg.reset();

    lpg.set_colour(1.0f);
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinAbs(40.0f, 1.0e-6f));

    lpg.set_range_hz(100.0f, 6400.0f);
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinAbs(100.0f, 1.0e-6f));
}

TEST_CASE("Lpg64 retains double precision in controls and mappings",
          "[signal][mod][lpg][precision]") {
    constexpr double velocity = 0.50000001;
    const double mapped = Lpg64::velocity_to_strike(velocity);
    REQUIRE_THAT(mapped, WithinAbs(std::pow(velocity, Lpg64::kVelocityCurve), 1.0e-15));
    REQUIRE(mapped != static_cast<double>(std::pow(static_cast<float>(velocity),
                                                   static_cast<float>(Lpg64::kVelocityCurve))));

    constexpr double cutoff = 123.4567890123;
    Lpg64 lpg;
    lpg.prepare(192000.0);
    lpg.set_colour(1.0);
    lpg.set_range_hz(cutoff, cutoff);
    REQUIRE_THAT(lpg.commanded_cutoff_hz(), WithinAbs(cutoff, 1.0e-12));
    REQUIRE(lpg.commanded_cutoff_hz() != static_cast<double>(static_cast<float>(cutoff)));
}

TEST_CASE("Lpg re-strike accumulates: a roll crescendos", "[signal][mod][lpg]") {
    Lpg lpg;
    lpg.prepare(kSampleRate);
    lpg.set_decay_ms(150.0);
    lpg.set_colour(0.5f);
    lpg.reset();

    constexpr int kSpacing = 1440; // 30 ms
    std::vector<float> peak_control;
    std::vector<float> peak_output;
    std::vector<float> peak_cutoff;
    const auto tone = sine(3 * kSpacing, 1000.0f);

    float control_peak = 0.0f;
    float output_peak = 0.0f;
    float cutoff_peak = 0.0f;
    for (int i = 0; i < 3 * kSpacing; ++i) {
        if (i % kSpacing == 0) {
            if (i > 0) {
                peak_control.push_back(control_peak);
                peak_output.push_back(output_peak);
                peak_cutoff.push_back(cutoff_peak);
            }
            control_peak = 0.0f;
            output_peak = 0.0f;
            cutoff_peak = 0.0f;
            lpg.strike(1.0f); // identical strikes: any growth is the cell's
        }
        const float out = lpg.process(tone[static_cast<std::size_t>(i)]);
        control_peak = std::max(control_peak, lpg.control());
        output_peak = std::max(output_peak, std::abs(out));
        cutoff_peak = std::max(cutoff_peak, lpg.commanded_cutoff_hz());
    }
    peak_control.push_back(control_peak);
    peak_output.push_back(output_peak);
    peak_cutoff.push_back(cutoff_peak);

    REQUIRE(peak_control.size() == 3);
    REQUIRE(peak_control[1] > peak_control[0]);
    REQUIRE(peak_control[2] > peak_control[1]);
    REQUIRE(peak_output[1] > peak_output[0]);
    REQUIRE(peak_output[2] > peak_output[1]);
    REQUIRE(peak_cutoff[1] > peak_cutoff[0]);
    REQUIRE(peak_cutoff[2] > peak_cutoff[1]);

    // A cold strike after a full recovery lands back where the first one did:
    // the accumulation is the cell's state, not a drifting counter.
    for (int i = 0; i < 48000; ++i)
        (void)lpg.process(0.0f);
    lpg.strike(1.0f);
    float cold_peak = 0.0f;
    float cold_cutoff = 0.0f;
    for (int i = 0; i < kSpacing; ++i) {
        (void)lpg.process(0.0f);
        cold_peak = std::max(cold_peak, lpg.control());
        cold_cutoff = std::max(cold_cutoff, lpg.commanded_cutoff_hz());
    }
    REQUIRE_THAT(cold_peak, WithinRel(peak_control[0], 0.01f));
    REQUIRE_THAT(cold_cutoff, WithinRel(peak_cutoff[0], 0.01f));
}

TEST_CASE("Lpg colour 0 is a pure VCA with the filter wide open", "[signal][mod][lpg]") {
    Lpg lpg;
    lpg.prepare(kSampleRate);
    lpg.set_colour(0.0f);
    lpg.reset();
    lpg.set_gate(1.0f);
    for (int i = 0; i < 20000; ++i)
        (void)lpg.process(0.0f); // settle wide open
    REQUIRE_THAT(lpg.control(), WithinAbs(1.0f, 1e-4f));

    const auto input = sine(4800, 1000.0f);
    std::vector<float> output;
    output.reserve(input.size());
    for (float x : input)
        output.push_back(lpg.process(x));

    const float attenuation_db = units::linear_to_db(rms(output) / rms(input));
    REQUIRE_THAT(attenuation_db, WithinAbs(0.0f, 0.1f));
}

TEST_CASE("Lpg colour 1 is a pure filter at unity gain", "[signal][mod][lpg]") {
    Lpg lpg;
    lpg.prepare(kSampleRate);
    lpg.set_colour(1.0f);
    lpg.reset(); // control 0 -> cutoff at fc_min

    const auto input = sine(4800, 1000.0f);
    std::vector<float> closed;
    closed.reserve(input.size());
    for (float x : input)
        closed.push_back(lpg.process(x));
    // Closed cell, 40 Hz cutoff: a 1 kHz tone is far down, but the gain stage
    // itself is untouched.
    REQUIRE(units::linear_to_db(rms(closed) / rms(input)) < -20.0f);

    lpg.set_gate(1.0f);
    for (int i = 0; i < 20000; ++i)
        (void)lpg.process(0.0f);
    std::vector<float> open;
    open.reserve(input.size());
    for (float x : input)
        open.push_back(lpg.process(x));
    REQUIRE_THAT(units::linear_to_db(rms(open) / rms(input)), WithinAbs(0.0f, 0.2f));
}

TEST_CASE("Lpg peak output never exceeds the input peak near Nyquist", "[signal][mod][lpg]") {
    // A trapezoidal one-pole commanded above sample_rate / 4 has a
    // greater-than-one step response: full-scale near-Nyquist alternation
    // pumps the integrator state past the input bound, and the next
    // low-frequency sample reads that state straight out. The cell caps its
    // commanded cutoff at sample_rate / 4 so the no-boost contract holds at
    // every sample rate and brightness setting.
    Lpg lpg;
    lpg.prepare(44100.0f);
    lpg.set_colour(1.0f); // unity gain stage: any boost is the filter's
    lpg.set_range_hz(40.0f, 18000.0f);
    lpg.set_gate(1.0f);
    for (int i = 0; i < 48000; ++i)
        (void)lpg.process(0.0f); // cell fully open

    float peak = 0.0f;
    float x = 1.0f;
    for (int i = 0; i < 512; ++i) { // full-scale alternation at Nyquist
        peak = std::max(peak, std::abs(lpg.process(x)));
        x = -x;
    }
    for (int i = 0; i < 64; ++i) // a step to DC exposes the pumped state
        peak = std::max(peak, std::abs(lpg.process(1.0f)));
    REQUIRE(peak <= 1.0f + 1.0e-4f);
}

TEST_CASE("Lpg couples loudness and brightness", "[signal][mod][lpg]") {
    // The defining property: a quieter moment is also a darker one. Compare the
    // high-frequency content of a loud strike against a soft one.
    auto brightness = [](float level) {
        Lpg lpg;
        lpg.prepare(kSampleRate);
        lpg.set_colour(0.5f);
        lpg.reset();
        lpg.strike(level);
        // White-ish excitation so the spectrum has something above the cutoff.
        Xorshift32 rng(1234u);
        std::vector<float> out;
        out.reserve(4800);
        for (int i = 0; i < 4800; ++i)
            out.push_back(lpg.process(rng.next_bipolar()));
        // A crude high-frequency measure: the RMS of the first difference,
        // normalized by the RMS of the signal.
        std::vector<float> difference;
        difference.reserve(out.size());
        for (std::size_t i = 1; i < out.size(); ++i)
            difference.push_back(out[i] - out[i - 1]);
        return rms(difference) / std::max(rms(out), 1e-9f);
    };

    REQUIRE(brightness(1.0f) > brightness(0.2f));
}

TEST_CASE("Lpg velocity mapping raises level and brightness together", "[signal][mod][lpg]") {
    REQUIRE(Lpg::velocity_to_strike(0.0f) == 0.0f);
    REQUIRE(Lpg::velocity_to_strike(1.0f) == 1.0f);
    // The 0.7 exponent makes soft hits louder than a linear map would, which is
    // what keeps a quiet hit audible rather than merely dark.
    REQUIRE(Lpg::velocity_to_strike(0.25f) > 0.25f);
    REQUIRE(Lpg::velocity_to_strike(0.5f) > Lpg::velocity_to_strike(0.25f));
}

// ── ModMatrixT ───────────────────────────────────────────────────────────────

TEST_CASE("ModMatrix routes, accumulates, and applies via", "[signal][mod][matrix]") {
    ModMatrix matrix;
    // sources: 0 = LFO, 1 = envelope, 2 = mod wheel
    const float sources[3] = {0.5f, 0.8f, 0.25f};
    float dests[2] = {0.0f, 0.0f};

    matrix.set_slot(0, 0, 0, 1.0f);    // LFO -> cutoff, full depth
    matrix.set_slot(1, 1, 0, 0.5f);    // envelope -> cutoff, accumulating
    matrix.set_slot(2, 0, 1, 1.0f, 2); // LFO -> pitch, ridden by the wheel

    matrix.evaluate(std::span<const float>(sources, 3), std::span<float>(dests, 2));

    REQUIRE_THAT(dests[0], WithinAbs(0.5f + 0.4f, 1e-6f));
    REQUIRE_THAT(dests[1], WithinAbs(0.5f * 0.25f, 1e-6f));
    REQUIRE(matrix.active_count() == 3);
}

TEST_CASE("ModMatrix accumulates onto the caller's base values", "[signal][mod][matrix]") {
    ModMatrix matrix;
    const float sources[1] = {-1.0f};
    float dests[1] = {1000.0f}; // an unmodulated cutoff in Hz
    matrix.set_slot(0, 0, 0, 200.0f);
    matrix.evaluate(std::span<const float>(sources, 1), std::span<float>(dests, 1));
    // The matrix does not clamp: destination units are the caller's business.
    REQUIRE_THAT(dests[0], WithinAbs(800.0f, 1e-3f));
}

TEST_CASE("ModMatrix ignores inactive, zeroed, and out-of-range slots", "[signal][mod][matrix]") {
    ModMatrix matrix;
    const float sources[1] = {1.0f};
    float dests[1] = {0.0f};

    // A fresh matrix is inert.
    matrix.evaluate(std::span<const float>(sources, 1), std::span<float>(dests, 1));
    REQUIRE(dests[0] == 0.0f);
    REQUIRE(matrix.active_count() == 0);

    // So is one whose bytes are all zero — the shape a memset or a
    // TripleBuffer's untouched back slot arrives in.
    ModMatrix zeroed;
    std::memset(static_cast<void*>(&zeroed), 0, sizeof(zeroed));
    zeroed.evaluate(std::span<const float>(sources, 1), std::span<float>(dests, 1));
    REQUIRE(dests[0] == 0.0f);

    // A stale routing left over from a longer source list is skipped, not read
    // out of bounds.
    matrix.set_slot(0, 99, 0, 1.0f);
    matrix.set_slot(1, 0, 99, 1.0f);
    matrix.set_slot(2, 0, 0, 0.0f); // zero depth contributes nothing
    matrix.evaluate(std::span<const float>(sources, 1), std::span<float>(dests, 1));
    REQUIRE(dests[0] == 0.0f);

    matrix.clear_slot(0);
    matrix.set_slot(1, 0, 0, 0.25f);
    matrix.evaluate(std::span<const float>(sources, 1), std::span<float>(dests, 1));
    REQUIRE_THAT(dests[0], WithinAbs(0.25f, 1e-6f));

    matrix.clear();
    REQUIRE(matrix.active_count() == 0);
}

TEST_CASE("ModMatrix is trivially copyable so it can be hot-swapped", "[signal][mod][matrix]") {
    STATIC_REQUIRE(std::is_trivially_copyable_v<ModMatrix>);
    STATIC_REQUIRE(std::is_trivially_copyable_v<ModMatrix64>);
}

// ── composition: the patch cookbook ──────────────────────────────────────────

TEST_CASE("Patch: smooth-random LFO through slew, matrix, and VCA", "[signal][mod][patch]") {
    auto render = [] {
        Lfo lfo;
        lfo.prepare(static_cast<double>(kSampleRate));
        lfo.set_wave(Lfo::Wave::smooth_random);
        lfo.set_random_segments(Lfo::kDefaultRandomSegments);
        lfo.set_rate_hz(2.0);
        lfo.set_seed(2718u);
        lfo.reset();

        SlewLimiter slew;
        slew.prepare(kSampleRate);
        slew.set_mode(SlewLimiter::Mode::exponential);
        slew.set_times_ms(20.0f, 20.0f);
        slew.reset(0.0f);

        ModMatrix matrix;
        matrix.set_slot(0, 0, 0, 0.4f); // LFO -> VCA control, 40% depth

        Vca vca;
        vca.prepare(kSampleRate);
        vca.set_response(Vca::Response::exponential);
        vca.reset(0.6f);

        const auto tone = sine(24000, 220.0f);
        std::vector<float> out;
        out.reserve(tone.size());
        for (std::size_t i = 0; i < tone.size(); ++i) {
            const float source[1] = {slew.process(lfo.next())};
            float control[1] = {0.6f}; // unmodulated base
            matrix.evaluate(std::span<const float>(source, 1), std::span<float>(control, 1));
            out.push_back(vca.process(tone[i], std::clamp(control[0], 0.0f, 1.0f)));
        }
        return out;
    };

    const auto first = render();
    const auto second = render();
    REQUIRE(first == second); // the whole chain is deterministic

    for (float x : first) {
        REQUIRE(std::isfinite(x));
        REQUIRE(std::abs(x) <= 1.0f);
    }
    // It has to actually modulate: an unmodulated render is not this patch.
    const auto range = std::minmax_element(first.begin(), first.end());
    REQUIRE(*range.second - *range.first > 0.5f);
}

TEST_CASE("Patch: ratchet bongo — trigger, burst, AHD, and LPG strike", "[signal][mod][patch]") {
    TriggerDetect detect;
    detect.prepare(static_cast<double>(kSampleRate));
    detect.set_refractory_ms(1.0);

    BurstGen burst;
    burst.prepare(static_cast<double>(kSampleRate));
    burst.set_count(4);
    burst.set_spacing_ms(40.0);
    burst.set_spacing_curve(-1.0f); // decelerating: a drag, not a machine gun
    burst.set_levels(1.0f, 0.45f);

    Ahd body;
    body.prepare(static_cast<double>(kSampleRate));
    body.set_attack_ms(1.0);
    body.set_hold_ms(4.0);
    body.set_decay_ms(30.0);

    Lpg lpg;
    lpg.prepare(kSampleRate);
    lpg.set_decay_ms(150.0);
    lpg.set_colour(0.5f);
    lpg.reset();

    Xorshift32 noise(4242u);

    std::vector<int> hit_times;
    std::vector<float> hit_levels;
    std::vector<float> out;
    constexpr int kSamples = 24000;
    out.reserve(kSamples);

    for (int i = 0; i < kSamples; ++i) {
        const bool trigger = detect.process(i == 0);
        const auto hit = burst.process(trigger);
        if (hit.fired) {
            hit_times.push_back(i);
            hit_levels.push_back(hit.level);
            body.trigger(hit.level);
            lpg.strike(Lpg::velocity_to_strike(hit.level));
        }
        const float excitation = noise.next_bipolar() * body.next();
        out.push_back(lpg.process(excitation));
    }

    // Event times against the closed form the burst generator is specified by.
    const double span = 3.0 * 40.0 * 0.001 * static_cast<double>(kSampleRate);
    REQUIRE(hit_times.size() == 4);
    for (int i = 0; i < 4; ++i) {
        const auto expected = static_cast<int>(std::llround(
            span * static_cast<double>(stage_curve(static_cast<float>(i) / 3.0f, -1.0f))));
        REQUIRE(hit_times[static_cast<std::size_t>(i)] == expected);
    }
    // Decelerating: each gap is longer than the last.
    for (std::size_t i = 2; i < hit_times.size(); ++i)
        REQUIRE(hit_times[i] - hit_times[i - 1] > hit_times[i - 1] - hit_times[i - 2]);

    REQUIRE_THAT(hit_levels.front(), WithinAbs(1.0f, 1e-6f));
    REQUIRE_THAT(hit_levels.back(), WithinAbs(0.45f, 1e-6f));

    for (float x : out)
        REQUIRE(std::isfinite(x));
    REQUIRE(rms(out) > 0.0f);
}

TEST_CASE("Patch: comparator clock, divider, and sample-and-hold", "[signal][mod][patch]") {
    Lfo clock_source;
    clock_source.prepare(static_cast<double>(kSampleRate));
    clock_source.set_period_samples(480.0);

    Lfo modulator;
    modulator.prepare(static_cast<double>(kSampleRate));
    modulator.set_period_samples(2011.0); // deliberately not a clock multiple

    Comparator comparator;
    comparator.set_threshold(0.0f);
    comparator.set_hysteresis(0.05f);

    TriggerDetect edge;
    edge.prepare(static_cast<double>(kSampleRate));
    edge.set_refractory_ms(0.0);

    ClockDivider divider;
    divider.set_division(2);

    SampleHold hold;
    hold.prepare(kSampleRate);
    hold.reset(0.0f);

    std::vector<int> latch_times;
    std::vector<float> held;
    for (int i = 0; i < 4800; ++i) {
        const bool gate = comparator.process(clock_source.next());
        const bool tick = divider.process(edge.process(gate));
        const float value = hold.process(modulator.next(), tick);
        if (tick) {
            latch_times.push_back(i);
            held.push_back(value);
        }
    }

    // The comparator fires once per LFO cycle; the divider halves that.
    REQUIRE(latch_times.size() >= 4);
    for (std::size_t i = 1; i < latch_times.size(); ++i)
        REQUIRE(latch_times[i] - latch_times[i - 1] == 960);

    // Because the modulator is not clock-locked, successive latches differ.
    REQUIRE(held[1] != held[0]);
}

TEST_CASE("Patch: sidechain pump with no compressor", "[signal][mod][patch]") {
    // P2: kick trigger -> TriggerDetect -> Ar -> inverting Attenuverter -> Vca.
    auto render = [](float kick_level) {
        TriggerDetect detect;
        detect.prepare(static_cast<double>(kSampleRate));
        detect.set_refractory_ms(5.0);
        detect.set_threshold(0.5f);

        Ar duck;
        duck.prepare(static_cast<double>(kSampleRate));
        duck.set_attack_ms(5.0);    // how fast it ducks
        duck.set_release_ms(220.0); // how fast it recovers

        Attenuverter invert;
        invert.set_gain(-0.8f);
        invert.set_offset(1.0f);

        Vca vca;
        vca.prepare(kSampleRate);
        vca.set_lag_ms(1.0);
        vca.reset(1.0f);

        const auto pad = sine(48000, 110.0f, 0.6f);
        std::vector<float> out;
        out.reserve(pad.size());
        for (int i = 0; i < 48000; ++i) {
            // A kick every 24000 samples (0.5 s), at whatever level.
            const bool kick_present = (i % 24000) < 480;
            const float kick = kick_present ? kick_level : 0.0f;
            duck.gate(detect.armed() && kick > 0.5f ? true : kick > 0.5f);
            (void)detect.process_signal(kick);
            const float control = std::clamp(invert.process(duck.next()), 0.0f, 1.0f);
            out.push_back(vca.process(pad[static_cast<std::size_t>(i)], control));
        }
        return out;
    };

    const auto loud = render(1.0f);
    const auto quiet = render(0.6f);

    // The pump is driven by the *event*, not by the kick's amplitude, so a
    // quieter kick ducks exactly as hard. That is the property a compressor
    // sidechain does not have.
    REQUIRE(loud.size() == quiet.size());
    for (std::size_t i = 0; i < loud.size(); ++i)
        REQUIRE_THAT(quiet[i], WithinAbs(loud[i], 1e-6f));

    // And it really ducks: right after a kick the pad is far quieter than it is
    // just before the next one.
    const std::vector<float> after(loud.begin() + 500, loud.begin() + 2500);
    const std::vector<float> recovered(loud.begin() + 20000, loud.begin() + 22000);
    REQUIRE(rms(after) < 0.5f * rms(recovered));
}

TEST_CASE("Patch: live echo — LPG in a feedback path struck by transients",
          "[signal][mod][patch]") {
    // P8: each echo is quieter AND darker, and the loop's gate is opened by the
    // dry input's own attacks.
    constexpr int kDelay = 6000; // 125 ms
    constexpr int kSamples = 48000;

    TransientDetector transient;
    transient.prepare(kSampleRate);
    transient.reset();

    TriggerDetect detect;
    detect.prepare(static_cast<double>(kSampleRate));
    detect.set_refractory_ms(20.0);
    detect.set_threshold(0.4f);

    Lpg loop_gate;
    loop_gate.prepare(kSampleRate);
    loop_gate.set_decay_ms(400.0);
    loop_gate.set_colour(0.6f);
    loop_gate.reset();

    std::vector<float> line(kDelay, 0.0f);
    std::vector<float> out;
    out.reserve(kSamples);
    int write = 0;

    Xorshift32 noise(31337u);
    for (int i = 0; i < kSamples; ++i) {
        // A percussive dry input at the start only, so the tail is all echo.
        const int phase = i;
        const float dry = phase < 2000
                              ? noise.next_bipolar() * std::exp(-static_cast<float>(phase) / 300.0f)
                              : 0.0f;

        if (detect.process_signal(transient.process(dry)))
            loop_gate.strike(0.9f);

        const float delayed = line[static_cast<std::size_t>(write)];
        const float fed_back = loop_gate.process(delayed * 0.75f);
        line[static_cast<std::size_t>(write)] = dry + fed_back;
        write = (write + 1) % kDelay;
        out.push_back(dry + delayed);
    }

    auto window = [&](int start) {
        return std::vector<float>(out.begin() + start, out.begin() + start + kDelay);
    };
    auto brightness = [](const std::vector<float>& v) {
        std::vector<float> difference;
        difference.reserve(v.size());
        for (std::size_t i = 1; i < v.size(); ++i)
            difference.push_back(v[i] - v[i - 1]);
        return rms(difference) / std::max(rms(v), 1e-9f);
    };

    const auto first_echo = window(kDelay);
    const auto second_echo = window(2 * kDelay);
    const auto third_echo = window(3 * kDelay);

    REQUIRE(rms(first_echo) > 0.0f);
    // Quieter each pass...
    REQUIRE(rms(second_echo) < rms(first_echo));
    REQUIRE(rms(third_echo) < rms(second_echo));
    // ...and darker each pass, which a static lowpass in the loop would not do.
    REQUIRE(brightness(third_echo) < brightness(first_echo));

    for (float x : out)
        REQUIRE(std::isfinite(x));
}

TEST_CASE("Patch: delayed vibrato arrives after the note settles", "[signal][mod][patch]") {
    // P6: the LFO's delay and fade are the performance behavior, not an
    // envelope bolted onto the depth.
    Lfo vibrato;
    vibrato.prepare(static_cast<double>(kSampleRate));
    vibrato.set_rate_hz(5.8);
    vibrato.set_delay_ms(400.0);
    vibrato.set_fade_in_ms(600.0);
    vibrato.set_mode(Lfo::Mode::retrig);
    vibrato.reset();
    vibrato.retrigger();

    // Consumes `count` samples and returns the peak-to-peak pitch excursion in
    // cents over that window.
    auto depth_over = [&](int count) {
        float lowest = 1.0e9f;
        float highest = -1.0e9f;
        for (int i = 0; i < count; ++i) {
            const float cents = 25.0f * vibrato.next();
            REQUIRE(std::isfinite(units::cents_to_ratio(cents)));
            lowest = std::min(lowest, cents);
            highest = std::max(highest, cents);
        }
        return highest - lowest;
    };

    REQUIRE(depth_over(19200) == 0.0f);   // 400 ms of delay: nothing at all
    const float early = depth_over(9600); // first 200 ms of the 600 ms fade
    (void)depth_over(19200);              // the rest of the fade
    const float late = depth_over(19200); // fully faded in
    REQUIRE(early > 0.0f);
    REQUIRE(late > early);
    REQUIRE_THAT(late, WithinRel(50.0f, 0.02f)); // full +/-25 cents
}
