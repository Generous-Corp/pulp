// The physically-modelled percussion voices and the primitives they need:
// the frequency shifter, the Karplus-Strong string, and the phase-distortion
// oscillator, plus the membrane, cymbal and zap voices built on them.
//
// Each of these is worth having only because it does something a simpler
// construction cannot, so that is what the tests measure: the shifter is
// checked against a pitch shift (which it is not), the string against a
// harmonic series it was never given, the membrane against the inharmonic
// ratios that stop it having a pitch, and the cymbal against the chordal
// bank it would be without the shifter.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/drum/cymbal.hpp>
#include <pulp/signal/drum/hit_life.hpp>
#include <pulp/signal/drum/membrane.hpp>
#include <pulp/signal/drum/string.hpp>
#include <pulp/signal/drum/zap.hpp>
#include <pulp/signal/frequency_shifter.hpp>
#include <pulp/signal/karplus_strong.hpp>
#include <pulp/signal/phase_distortion.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using pulp::signal::FrequencyShifter64;
using pulp::signal::KarplusStrong;
using pulp::signal::PhaseDistortionOsc;
using pulp::signal::PhaseDistortionShape;
using pulp::signal::drum::CymbalVoice;
using pulp::signal::drum::HitLife;
using pulp::signal::drum::HitLifeMode;
using pulp::signal::drum::MembraneExciter;
using pulp::signal::drum::MembraneVoice;
using pulp::signal::drum::StringModulation;
using pulp::signal::drum::StringVoice;
using pulp::signal::drum::VelocityResponse;
using pulp::signal::drum::Voice;
using pulp::signal::drum::ZapVoice;

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

std::vector<float> render(Voice& voice, int num_samples, int block = 64) {
    std::vector<float> out(static_cast<std::size_t>(num_samples), 0.0f);
    for (int i = 0; i < num_samples; i += block) {
        voice.process(out.data() + i, std::min(block, num_samples - i));
    }
    return out;
}

std::vector<float> hit(Voice& voice, float velocity, int num_samples) {
    voice.note_on(velocity);
    return render(voice, num_samples);
}

double peak(const std::vector<float>& x) {
    double m = 0.0;
    for (float v : x) m = std::max(m, std::fabs(static_cast<double>(v)));
    return m;
}

// Hann-windowed Goertzel amplitude at one frequency.
template <typename Container>
double tone_amplitude(const Container& x, double f, double fs = kFs) {
    const std::size_t n = x.size();
    const double w = 2.0 * kPi * f / fs;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                static_cast<double>(n - 1));
        const double s0 = coeff * s1 - s2 + win * static_cast<double>(x[i]);
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (static_cast<double>(n) * 0.25);
}

std::vector<double> sine(double f, std::size_t n, double amplitude = 1.0) {
    std::vector<double> y(n);
    for (std::size_t i = 0; i < n; ++i) {
        y[i] = amplitude * std::sin(2.0 * kPi * f * static_cast<double>(i) / kFs);
    }
    return y;
}

// Fraction of energy above `split_hz`.
double high_fraction(const std::vector<float>& x, double split_hz) {
    const double a = 1.0 - std::exp(-2.0 * kPi * split_hz / kFs);
    double lp = 0.0, low = 0.0, high = 0.0;
    for (float v : x) {
        lp += a * (static_cast<double>(v) - lp);
        low += lp * lp;
        const double hp = static_cast<double>(v) - lp;
        high += hp * hp;
    }
    return high / (high + low + 1e-30);
}

int audible_length(const std::vector<float>& x, double floor_level) {
    for (std::size_t i = x.size(); i > 0; --i) {
        if (std::fabs(static_cast<double>(x[i - 1])) > floor_level) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

}  // namespace

// -- Frequency shifter -------------------------------------------------------

TEST_CASE("The shifter adds hertz rather than multiplying by a ratio",
          "[signal][shifter]") {
    // The whole distinction from a pitch shift, and the reason the cymbal
    // needs it. A tone at 500 Hz shifted by 60 must land at 560, and one at
    // 1000 Hz must land at 1060 -- not at 1120, which is what a ratio would do.
    constexpr double kShift = 60.0;
    for (double f : {500.0, 1000.0}) {
        FrequencyShifter64 shifter;
        shifter.set_sample_rate(kFs);
        shifter.set_shift_hz(kShift);
        shifter.reset();

        const auto input = sine(f, 32768);
        std::vector<double> out(input.size());
        for (std::size_t i = 0; i < input.size(); ++i) out[i] = shifter.process(input[i]);

        const double shifted = tone_amplitude(out, f + kShift);
        const double original = tone_amplitude(out, f);
        const double ratio_result = tone_amplitude(out, f * (1.0 + kShift / 500.0));

        INFO("input " << f << " Hz");
        REQUIRE(shifted > 0.3);
        // The original frequency is suppressed -- this is a shift, not a
        // detune added alongside.
        REQUIRE(original < shifted * 0.2);
        if (f > 500.0) REQUIRE(ratio_result < shifted * 0.3);
    }
}

TEST_CASE("A negative shift moves frequencies down", "[signal][shifter]") {
    FrequencyShifter64 shifter;
    shifter.set_sample_rate(kFs);
    shifter.set_shift_hz(-120.0);
    shifter.reset();

    const auto input = sine(800.0, 32768);
    std::vector<double> out(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) out[i] = shifter.process(input[i]);
    REQUIRE(tone_amplitude(out, 680.0) > tone_amplitude(out, 800.0) * 3.0);
}

TEST_CASE("A zero shift is close to transparent", "[signal][shifter]") {
    FrequencyShifter64 shifter;
    shifter.set_sample_rate(kFs);
    shifter.set_shift_hz(0.0);
    shifter.reset();

    const auto input = sine(1000.0, 32768);
    std::vector<double> out(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) out[i] = shifter.process(input[i]);
    REQUIRE(tone_amplitude(out, 1000.0) > 0.8);
}

TEST_CASE("Shifting destroys a harmonic series", "[signal][shifter]") {
    // The property the cymbal actually relies on. A harmonic input has energy
    // at every multiple of its fundamental; after a shift, it does not.
    FrequencyShifter64 shifter;
    shifter.set_sample_rate(kFs);
    shifter.set_shift_hz(37.0);
    shifter.reset();

    std::vector<double> harmonic(32768, 0.0);
    for (int h = 1; h <= 4; ++h) {
        const auto partial = sine(200.0 * h, harmonic.size(), 0.25);
        for (std::size_t i = 0; i < harmonic.size(); ++i) harmonic[i] += partial[i];
    }

    std::vector<double> out(harmonic.size());
    for (std::size_t i = 0; i < harmonic.size(); ++i) out[i] = shifter.process(harmonic[i]);

    // Every partial has moved by the same 37 Hz, so they now sit at 237, 437,
    // 637, 837 -- which are not multiples of any common fundamental.
    for (int h = 1; h <= 4; ++h) {
        INFO("partial " << h);
        REQUIRE(tone_amplitude(out, 200.0 * h + 37.0) >
                tone_amplitude(out, 200.0 * h) * 2.0);
    }
}

TEST_CASE("The shifter allocates nothing on the audio thread",
          "[signal][shifter][rt-safety]") {
    FrequencyShifter64 shifter;
    shifter.set_sample_rate(kFs);
    shifter.set_shift_hz(50.0);
    shifter.reset();

    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int i = 0; i < 16384; ++i) sink += shifter.process(std::sin(0.01 * i));
        shifter.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(std::isfinite(sink));
    REQUIRE(allocations == 0);
}

// -- Karplus-Strong ----------------------------------------------------------

TEST_CASE("The string produces a harmonic series it was never given",
          "[signal][string]") {
    // Nothing writes a harmonic anywhere: the partials appear because only
    // frequencies whose period divides the loop survive going round it. That
    // is the whole idea, so it is what gets measured.
    KarplusStrong string;
    string.prepare(kFs);
    string.set_frequency(220.0);
    string.set_decay_seconds(3.0);
    string.set_damping(0.1);
    string.set_pluck_position(0.25);
    string.reset();
    string.pluck();

    pulp::signal::NoiseSource noise;
    noise.prepare(kFs);
    noise.reset();

    std::vector<float> y(32768);
    for (std::size_t i = 0; i < y.size(); ++i) {
        y[i] = string.process(i < 256 ? noise.white() : 0.0f);
    }

    REQUIRE(tone_amplitude(y, 220.0) > 1e-4);
    REQUIRE(tone_amplitude(y, 440.0) > 1e-4);
    REQUIRE(tone_amplitude(y, 660.0) > 1e-4);
    // ...and nothing in between, which is what makes it a series rather than
    // filtered noise.
    REQUIRE(tone_amplitude(y, 330.0) < tone_amplitude(y, 220.0) * 0.35);
}

TEST_CASE("A higher note decays faster at the same decay setting",
          "[signal][string]") {
    // The loop runs f0 times a second, so the same per-trip loss is a much
    // shorter note at a high pitch. A fixed loop gain would make every note
    // last the same time, which no string does.
    auto length_for = [](double hz) {
        KarplusStrong string;
        string.prepare(kFs);
        string.set_frequency(hz);
        string.set_decay_seconds(1.0);
        string.set_damping(0.2);
        string.reset();
        string.pluck();

        pulp::signal::NoiseSource noise;
        noise.prepare(kFs);
        noise.reset();
        std::vector<float> y(static_cast<std::size_t>(2.0 * kFs));
        for (std::size_t i = 0; i < y.size(); ++i) {
            y[i] = string.process(i < 512 ? noise.white() : 0.0f);
        }
        return audible_length(y, 1e-4);
    };

    // Both reach -60 dB at about the requested second, which is the point of
    // deriving the gain from the frequency.
    const int low = length_for(110.0);
    const int high = length_for(880.0);
    REQUIRE(low > 0);
    REQUIRE(high > 0);
    REQUIRE(std::fabs(static_cast<double>(low - high)) < 0.4 * kFs);
}

TEST_CASE("Damping removes the upper partials", "[signal][string]") {
    // Measured early, where the fundamental and the upper partials are both
    // still present. Later in the note a damped string has lost most of its
    // level too, so a ratio taken over the whole render compares two kinds of
    // quiet rather than two kinds of bright.
    auto brightness = [](double damping) {
        KarplusStrong string;
        string.prepare(kFs);
        string.set_frequency(220.0);
        string.set_decay_seconds(2.0);
        string.set_damping(damping);
        string.reset();
        string.pluck();

        pulp::signal::NoiseSource noise;
        noise.prepare(kFs);
        noise.reset();
        std::vector<float> y(4096);
        for (std::size_t i = 0; i < y.size(); ++i) {
            y[i] = string.process(i < 256 ? noise.white() : 0.0f);
        }
        double lp = 0.0, low = 0.0, high = 0.0;
        const double a = 1.0 - std::exp(-2.0 * kPi * 1500.0 / kFs);
        for (float v : y) {
            lp += a * (static_cast<double>(v) - lp);
            low += lp * lp;
            const double hp = static_cast<double>(v) - lp;
            high += hp * hp;
        }
        return high / (high + low + 1e-30);
    };

    REQUIRE(brightness(0.0) > brightness(0.9) * 2.0);
}

TEST_CASE("Pluck position removes the partials with a node there",
          "[signal][string]") {
    // Plucking at exactly one third cancels the third harmonic, because that
    // is where its node is. This is the comb, and it is why plucking near the
    // bridge sounds thin.
    auto third_harmonic = [](double position) {
        KarplusStrong string;
        string.prepare(kFs);
        string.set_frequency(200.0);
        string.set_decay_seconds(2.0);
        string.set_damping(0.05);
        string.set_pluck_position(position);
        string.reset();
        string.pluck();

        pulp::signal::NoiseSource noise;
        noise.prepare(kFs);
        noise.reset();
        std::vector<float> y(32768);
        for (std::size_t i = 0; i < y.size(); ++i) {
            y[i] = string.process(i < 480 ? noise.white() : 0.0f);
        }
        return tone_amplitude(y, 600.0) / (tone_amplitude(y, 200.0) + 1e-20);
    };

    REQUIRE(third_harmonic(1.0 / 3.0) < third_harmonic(0.2) * 0.6);
}

TEST_CASE("The string allocates nothing once prepared",
          "[signal][string][rt-safety]") {
    KarplusStrong string;
    string.prepare(kFs);
    string.set_frequency(330.0);
    string.set_decay_seconds(1.0);
    string.reset();

    double sink = 0.0;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int pluck = 0; pluck < 4; ++pluck) {
            string.set_frequency(220.0 + 55.0 * pluck);
            string.pluck();
            for (int i = 0; i < 4096; ++i) sink += string.process(i < 128 ? 0.5f : 0.0f);
        }
        string.reset();
        allocations = probe.allocation_count();
    }
    REQUIRE(std::isfinite(sink));
    REQUIRE(allocations == 0);
}

// -- Phase distortion --------------------------------------------------------

TEST_CASE("Zero distortion is a plain cosine", "[signal][phase-distortion]") {
    PhaseDistortionOsc osc;
    osc.set_sample_rate(kFs);
    osc.set_frequency(500.0);
    osc.set_shape(PhaseDistortionShape::saw);
    osc.set_amount(0.0);
    osc.reset();

    std::vector<float> y(16384);
    for (auto& v : y) v = osc.process();
    REQUIRE(tone_amplitude(y, 500.0) > 0.8);
    // A cosine has no second harmonic.
    REQUIRE(tone_amplitude(y, 1000.0) < 0.05);
}

TEST_CASE("Warping the phase adds harmonics without adding anything else",
          "[signal][phase-distortion]") {
    auto harmonics_at = [](double amount) {
        PhaseDistortionOsc osc;
        osc.set_sample_rate(kFs);
        osc.set_frequency(500.0);
        osc.set_shape(PhaseDistortionShape::saw);
        osc.set_amount(amount);
        osc.reset();
        std::vector<float> y(16384);
        for (auto& v : y) v = osc.process();
        return (tone_amplitude(y, 1000.0) + tone_amplitude(y, 1500.0)) /
               (tone_amplitude(y, 500.0) + 1e-20);
    };

    REQUIRE(harmonics_at(0.9) > harmonics_at(0.0) * 10.0);
}

TEST_CASE("The resonant shape puts a peak at a swept multiple",
          "[signal][phase-distortion]") {
    // The signature behaviour: the apparent formant sits at a multiple of the
    // fundamental set by the amount, with no filter anywhere.
    auto peak_multiple = [](double amount, double multiple) {
        PhaseDistortionOsc osc;
        osc.set_sample_rate(kFs);
        osc.set_frequency(300.0);
        osc.set_shape(PhaseDistortionShape::resonant_saw);
        osc.set_resonant_depth(11.0);
        osc.set_amount(amount);
        osc.reset();
        std::vector<float> y(16384);
        for (auto& v : y) v = osc.process();
        return tone_amplitude(y, 300.0 * multiple);
    };

    // At full amount the carrier sits at the 11th multiple, so there is more
    // there than at the 3rd; at zero amount the reverse.
    REQUIRE(peak_multiple(1.0, 11.0) > peak_multiple(1.0, 3.0));
    REQUIRE(peak_multiple(0.0, 1.0) > peak_multiple(0.0, 11.0) * 5.0);
}

TEST_CASE("Every phase-distortion shape stays bounded",
          "[signal][phase-distortion]") {
    for (auto shape : {PhaseDistortionShape::saw, PhaseDistortionShape::pulse,
                       PhaseDistortionShape::resonant_saw,
                       PhaseDistortionShape::resonant_triangle,
                       PhaseDistortionShape::resonant_trapezoid}) {
        PhaseDistortionOsc osc;
        osc.set_sample_rate(kFs);
        osc.set_frequency(220.0);
        osc.set_shape(shape);
        osc.set_amount(1.0);
        osc.reset();
        for (int i = 0; i < 8192; ++i) {
            const float v = osc.process();
            REQUIRE(std::isfinite(v));
            REQUIRE(std::fabs(v) <= 1.0001f);
        }
    }
}

// -- Membrane ----------------------------------------------------------------

TEST_CASE("The membrane's modes sit at inharmonic ratios",
          "[signal][drum][membrane]") {
    // The reason a real drum has no clear pitch. At structure 1 the modes are
    // the published circular-membrane ratios, which are not multiples of the
    // fundamental.
    MembraneVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(200.0);
    voice.set_structure(1.0);
    voice.set_spread(0.0);
    voice.set_damping(0.0);
    voice.set_decay_ms(3000.0);
    voice.set_position(0.35);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, 48000);
    REQUIRE(peak(y) > 1e-4);

    const double fundamental = tone_amplitude(y, 200.0);
    // The second mode is at 1.5933x, not at 2x.
    REQUIRE(tone_amplitude(y, 200.0 * 1.5933) > tone_amplitude(y, 400.0) * 1.5);
    REQUIRE(fundamental > 1e-5);
}

TEST_CASE("Structure zero gives a harmonic series instead",
          "[signal][drum][membrane]") {
    // The negative control: the same voice at structure 0 must put energy
    // exactly where the membrane ratios do not.
    MembraneVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(200.0);
    voice.set_structure(0.0);
    voice.set_spread(0.0);
    voice.set_damping(0.0);
    voice.set_decay_ms(3000.0);
    voice.set_position(0.35);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, 48000);
    REQUIRE(tone_amplitude(y, 400.0) > tone_amplitude(y, 200.0 * 1.5933) * 1.5);
}

TEST_CASE("Damping makes the upper modes die first",
          "[signal][drum][membrane]") {
    auto late_brightness = [](double damping) {
        MembraneVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(150.0);
        voice.set_damping(damping);
        voice.set_decay_ms(2000.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto y = hit(voice, 1.0f, 48000);
        const std::vector<float> tail(y.begin() + 24000, y.end());
        return tone_amplitude(tail, 150.0 * 2.9173) /
               (tone_amplitude(tail, 150.0) + 1e-20);
    };

    REQUIRE(late_brightness(0.0) > late_brightness(0.95) * 2.0);
}

TEST_CASE("Strike position removes the modes with a node there",
          "[signal][drum][membrane]") {
    // The comb is not a level control: at the midpoint the second mode has a
    // node exactly there and disappears, while the first mode -- which has an
    // antinode there -- gets louder. Measuring overall energy would miss this
    // entirely, because what changes is which partials exist.
    auto modes_at = [](double position) {
        MembraneVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(150.0);
        voice.set_position(position);
        voice.set_spread(0.0);
        voice.set_decay_ms(1500.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto y = hit(voice, 1.0f, 24000);
        return std::pair<double, double>{tone_amplitude(y, 150.0),
                                         tone_amplitude(y, 150.0 * 1.5933)};
    };

    const auto off_centre = modes_at(0.25);
    const auto centre = modes_at(0.5);

    REQUIRE(off_centre.second > 1e-4);            // present off-centre
    REQUIRE(centre.second < off_centre.second * 0.01);  // nulled at the midpoint
    REQUIRE(centre.first > off_centre.first);     // and the first mode is stronger
}

TEST_CASE("Membrane strike position preserves total mode-gain energy",
          "[signal][drum][membrane]") {
    MembraneVoice voice;
    voice.prepare(kFs);
    voice.set_spread(0.0);
    voice.set_brightness(0.8);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    constexpr double expected = 1.0 / MembraneVoice::mode_count;
    for (double position : {0.08, 0.21, 0.35, 0.5}) {
        voice.set_position(position);
        voice.note_on(1.0f);
        REQUIRE(std::fabs(voice.mode_gain_energy() - expected) < 1e-7);
        voice.reset();
    }
}

TEST_CASE("The membrane can be struck by a single-sample pluck",
          "[signal][drum][membrane]") {
    MembraneVoice voice;
    voice.prepare(kFs);
    voice.set_spread(0.0);
    voice.set_exciter(MembraneExciter::pluck);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto first = hit(voice, 1.0f, 24000);
    voice.reset();
    const auto second = hit(voice, 1.0f, 24000);
    REQUIRE(peak(first) > 1e-4);
    REQUIRE(first == second);

    voice.reset();
    voice.set_exciter(MembraneExciter::noise_burst);
    const auto burst = hit(voice, 1.0f, 24000);
    REQUIRE_FALSE(first == burst);
}

TEST_CASE("The membrane's sub, air, and click layers reach emitted audio",
          "[signal][drum][membrane]") {
    auto layer = [](int selected) {
        MembraneVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(100.0);
        voice.set_spread(0.0);
        voice.set_brightness(0.0);
        voice.set_exciter_cutoff_hz(500.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        if (selected == 1) voice.set_sub_level(1.0);
        if (selected == 2) voice.set_air_level(1.0);
        if (selected == 3) voice.set_click_level(3.0);
        return hit(voice, 1.0f, 24000);
    };

    const auto body = layer(0);
    const auto sub = layer(1);
    const auto air = layer(2);
    const auto click = layer(3);
    REQUIRE(tone_amplitude(sub, 50.0) > tone_amplitude(body, 50.0) * 3.0);
    REQUIRE(high_fraction(air, 4000.0) > high_fraction(body, 4000.0) * 2.0);

    const std::vector<float> body_attack(body.begin(), body.begin() + 1000);
    const std::vector<float> click_attack(click.begin(), click.begin() + 1000);
    REQUIRE(peak(click_attack) > peak(body_attack) * 1.5);
}

TEST_CASE("Muted membrane layers drain instead of resuming stale transients",
          "[signal][drum][membrane][lifecycle]") {
    MembraneVoice voice;
    voice.prepare(kFs);
    voice.set_decay_ms(20.0);
    voice.set_sub_level(1.0);
    voice.set_air_level(1.0);
    voice.set_air_decay_ms(10.0);
    voice.set_click_level(1.0);
    voice.set_click_decay_ms(5.0);
    voice.note_on(1.0f);

    voice.set_sub_level(0.0);
    voice.set_air_level(0.0);
    voice.set_click_level(0.0);
    (void)render(voice, 96000);

    voice.set_sub_level(1.0);
    voice.set_air_level(1.0);
    voice.set_click_level(1.0);
    REQUIRE_FALSE(voice.is_active());
}

TEST_CASE("Membrane velocity tension moves the body and sub together",
          "[signal][drum][membrane][velocity]") {
    MembraneVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(100.0);
    voice.set_structure(0.0);
    voice.set_spread(0.0);
    voice.set_position(0.5);
    voice.set_brightness(0.0);
    voice.set_sub_level(8.0);
    voice.set_air_level(0.0);
    voice.set_click_level(0.0);
    voice.set_velocity_response(
        VelocityResponse{0.0f, 0.25f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, 48000);
    const double tension = std::exp2(0.25);
    const double moved_sub = tone_amplitude(y, 50.0 * tension);
    const double stale_sub = tone_amplitude(y, 50.0);
    INFO("moved sub=" << moved_sub << " stale sub=" << stale_sub);
    REQUIRE(moved_sub > stale_sub * 4.0);
}

TEST_CASE("A membrane retrigger keeps the output filter ringing",
          "[signal][drum][membrane][output][lifecycle]") {
    auto prepare = [](MembraneVoice& voice) {
        voice.prepare(kFs);
        voice.set_tune_hz(180.0);
        voice.set_decay_ms(3000.0);
        voice.set_spread(0.0);
        voice.set_exciter(MembraneExciter::pluck);
        voice.set_velocity_response(
            VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        voice.note_on(1.0f);
        (void)render(voice, 6000);
    };

    MembraneVoice control;
    MembraneVoice retriggered;
    prepare(control);
    prepare(retriggered);

    retriggered.note_on(1.0f);
    const auto uninterrupted = render(control, 32);
    const auto after_retrigger = render(retriggered, 32);

    auto energy = [](const std::vector<float>& samples) {
        double sum = 0.0;
        for (float sample : samples) {
            sum += static_cast<double>(sample) *
                   static_cast<double>(sample);
        }
        return sum;
    };
    const double uninterrupted_energy = energy(uninterrupted);
    REQUIRE(uninterrupted_energy > 1e-8);
    REQUIRE(energy(after_retrigger) > uninterrupted_energy * 0.5);
}

// -- Cymbal ------------------------------------------------------------------

TEST_CASE("The cymbal's shifter is what removes the pitch",
          "[signal][drum][cymbal]") {
    // Without the shift the comb bank is chordal -- each comb rings on its own
    // harmonic series. The shift is what makes it a cymbal, so the test is
    // that turning it off brings a pitch back.
    auto harmonic_strength = [](double shift_hz) {
        CymbalVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(300.0);
        voice.set_decay_ms(2000.0);
        voice.set_shift_hz(shift_hz);
        voice.set_variation(0.0);
        voice.set_low_cut_hz(100.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto y = hit(voice, 1.0f, 48000);
        REQUIRE(peak(y) > 1e-4);
        // Energy at the lowest comb's own frequency and its octave, relative to
        // a frequency that belongs to no comb.
        return (tone_amplitude(y, 300.0) + tone_amplitude(y, 600.0)) /
               (tone_amplitude(y, 337.0) + 1e-20);
    };

    REQUIRE(harmonic_strength(0.0) > harmonic_strength(60.0) * 1.5);
}

TEST_CASE("A cymbal with variation off is reproducible",
          "[signal][drum][cymbal]") {
    CymbalVoice voice;
    voice.prepare(kFs);
    voice.set_variation(0.0);
    const auto first = hit(voice, 0.8f, 12000);
    voice.reset();
    const auto second = hit(voice, 0.8f, 12000);
    REQUIRE(first == second);
}

TEST_CASE("Variation makes successive crashes differ",
          "[signal][drum][cymbal]") {
    // Two hits in a row, with no reset between them: `reset()` deliberately
    // rewinds the variation sequence, so resetting between hits would put the
    // second one back on the first one's seed and hide the feature.
    CymbalVoice voice;
    voice.prepare(kFs);
    voice.set_variation(1.0);
    const auto first = hit(voice, 0.8f, 12000);
    const auto second = hit(voice, 0.8f, 12000);
    REQUIRE_FALSE(first == second);
}

TEST_CASE("Cymbal variation preserves the ringing body",
          "[signal][drum][cymbal][life]") {
    auto configure = [](CymbalVoice& voice) {
        voice.prepare(kFs);
        voice.set_decay_ms(8000.0);
        voice.set_variation(1.0);
        voice.output().set_oversampling(
            pulp::signal::drum::OutputOversampling::bypass);
    };

    CymbalVoice control;
    CymbalVoice retriggered;
    configure(control);
    configure(retriggered);
    REQUIRE(hit(control, 0.8f, 6000) ==
            hit(retriggered, 0.8f, 6000));

    control.set_noise_level(0.0);
    control.set_strike_level(0.0);
    retriggered.set_noise_level(0.0);
    retriggered.set_strike_level(0.0);
    retriggered.note_on(0.8f);

    const auto uninterrupted = render(control, 512);
    const auto after_retrigger = render(retriggered, 512);
    REQUIRE(peak(uninterrupted) > 1.0e-4);
    REQUIRE(peak(after_retrigger) > peak(uninterrupted) * 0.5);
}

TEST_CASE("Reapplying an advancing hit-life mode preserves its sequence",
          "[signal][drum][life]") {
    constexpr std::uint32_t seed = 0x12345678u;
    HitLife life{HitLifeMode::advancing_seed};
    const auto first = life.trigger(seed);

    life.set_mode(HitLifeMode::advancing_seed);
    const auto second = life.trigger(seed);

    HitLife control{HitLifeMode::advancing_seed};
    (void)control.trigger(seed);
    const auto expected_second = control.trigger(seed);
    REQUIRE(second.seed == expected_second.seed);
    REQUIRE(second.seed != first.seed);
}

TEST_CASE("Cymbal hit-life policy exposes fixed advancing and preserved modes",
          "[signal][drum][cymbal][life]") {
    CymbalVoice voice;
    voice.prepare(kFs);

    // set_variation is a convenience over set_hit_life, so it must land on a
    // real policy rather than a private fourth behaviour. Both values it can
    // pick preserve the body: a cymbal that reset its body on every hit would
    // cut its own ring off.
    voice.set_variation(1.0);
    REQUIRE(voice.hit_life() == HitLifeMode::advancing_seed_preserved_body);
    voice.set_variation(0.0);
    REQUIRE(voice.hit_life() == HitLifeMode::fixed_seed_preserved_body);

    voice.set_hit_life(HitLifeMode::fixed_seed);
    const auto fixed_a = hit(voice, 0.8f, 12000);
    voice.reset();
    const auto fixed_b = hit(voice, 0.8f, 12000);
    REQUIRE(fixed_a == fixed_b);

    voice.reset();
    const auto fixed_after_reset = hit(voice, 0.8f, 12000);
    REQUIRE(fixed_after_reset == fixed_a);

    voice.set_hit_life(HitLifeMode::preserved_state);
    const auto living_a = hit(voice, 0.8f, 12000);
    const auto living_b = hit(voice, 0.8f, 12000);
    REQUIRE_FALSE(living_a == living_b);
}

TEST_CASE("Cymbal body preservation is a policy of its own, not a side effect",
          "[signal][drum][cymbal][life]") {
    // The two modes below drive an identical excitation sequence and differ
    // only in what happens to the resonating body. Landing a second hit while
    // the first is still ringing is the only way to tell them apart, and it is
    // exactly the arrangement the policy exists for.
    const auto second_hit_under = [](HitLifeMode mode) {
        CymbalVoice voice;
        voice.prepare(kFs);
        voice.set_hit_life(mode);
        (void)hit(voice, 0.8f, 4000);
        return hit(voice, 0.8f, 12000);
    };

    const auto reset_body = second_hit_under(HitLifeMode::advancing_seed);
    const auto kept_body = second_hit_under(HitLifeMode::advancing_seed_preserved_body);
    REQUIRE_FALSE(reset_body == kept_body);

    const auto fixed_reset = second_hit_under(HitLifeMode::fixed_seed);
    const auto fixed_kept = second_hit_under(HitLifeMode::fixed_seed_preserved_body);
    REQUIRE_FALSE(fixed_reset == fixed_kept);

    // The seed axis stays independent of the body axis: same body policy,
    // different excitation, so the two must still separate.
    REQUIRE_FALSE(kept_body == fixed_kept);
}

TEST_CASE("Cymbal decay controls how long the wash lasts",
          "[signal][drum][cymbal]") {
    auto length_for = [](double decay_ms) {
        CymbalVoice voice;
        voice.prepare(kFs);
        voice.set_decay_ms(decay_ms);
        voice.set_variation(0.0);
        const auto y = hit(voice, 1.0f, static_cast<int>(4.0 * kFs));
        return audible_length(y, 1e-4);
    };

    REQUIRE(length_for(3000.0) > length_for(200.0) * 3);
}

TEST_CASE("Upper cymbal combs carry their own high-pass damping",
          "[signal][drum][cymbal]") {
    auto render_with_corner = [](double corner_hz) {
        CymbalVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(180.0);
        voice.set_decay_ms(1200.0);
        voice.set_shift_hz(0.0);
        voice.set_variation(0.0);
        voice.set_low_cut_hz(20.0);
        voice.set_tone_hz(20000.0);
        voice.set_upper_highpass_hz(corner_hz);
        voice.set_velocity_feedback(0.0);
        voice.set_velocity_high_mode_db(0.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return hit(voice, 1.0f, 48000);
    };

    const auto bypassed = render_with_corner(0.0);
    const auto damped = render_with_corner(3000.0);
    REQUIRE(bypassed != damped);
    REQUIRE(high_fraction(damped, 1200.0) >
            high_fraction(bypassed, 1200.0) * 1.05);
}

TEST_CASE("Cymbal velocity excites feedback and upper modes",
          "[signal][drum][cymbal][velocity]") {
    auto pair_for = [](double feedback, double high_mode_db) {
        CymbalVoice voice;
        voice.prepare(kFs);
        voice.set_variation(0.0);
        voice.set_shift_hz(0.0);
        voice.set_velocity_feedback(feedback);
        voice.set_velocity_high_mode_db(high_mode_db);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto soft = hit(voice, 0.1f, 48000);
        voice.reset();
        const auto loud = hit(voice, 1.0f, 48000);
        return std::pair{soft, loud};
    };

    // With level, generic brightness, and both cymbal-specific couplings off,
    // velocity is a sample-exact no-op. This is the detector's negative
    // control: either positive case can differ only through the named path.
    const auto neutral = pair_for(0.0, 0.0);
    REQUIRE(neutral.first == neutral.second);

    const auto emphasized = pair_for(0.0, 10.0);
    REQUIRE_FALSE(emphasized.first == emphasized.second);

    const auto ringing = pair_for(1.0, 0.0);
    const std::vector<float> soft_tail(ringing.first.begin() + 24000,
                                       ringing.first.end());
    const std::vector<float> loud_tail(ringing.second.begin() + 24000,
                                       ringing.second.end());
    REQUIRE(peak(loud_tail) > peak(soft_tail) * 1.1);
}

// -- Zap ---------------------------------------------------------------------

TEST_CASE("The zap's distortion sweep is independent of its pitch sweep",
          "[signal][drum][zap]") {
    // The point of the voice. With the same pitch sweep, changing only the
    // distortion decay must change the timbre and leave the pitch trajectory
    // alone -- which a single shared envelope could not do.
    auto render_zap = [](double distortion_ms) {
        ZapVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(200.0);
        voice.set_pitch_sweep_octaves(1.5);
        voice.set_pitch_sweep_ms(40.0);
        voice.set_distortion(1.0);
        voice.set_distortion_ms(distortion_ms);
        voice.set_decay_ms(500.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return hit(voice, 1.0f, 24000);
    };

    const auto quick = render_zap(5.0);
    const auto slow = render_zap(200.0);
    REQUIRE(peak(quick) > 1e-3);
    REQUIRE(peak(slow) > 1e-3);

    // A distortion still open late puts far more energy up high. Measured as a
    // band fraction rather than at one frequency, because the formant's centre
    // moves as the sweep runs and a single bin would be sampling a moving
    // target.
    auto high_band = [](const std::vector<float>& x) {
        const double a = 1.0 - std::exp(-2.0 * kPi * 1000.0 / kFs);
        double lp = 0.0, low = 0.0, high = 0.0;
        for (float v : x) {
            lp += a * (static_cast<double>(v) - lp);
            low += lp * lp;
            const double hp = static_cast<double>(v) - lp;
            high += hp * hp;
        }
        return high / (high + low + 1e-30);
    };

    const std::vector<float> quick_tail(quick.begin() + 4800, quick.end());
    const std::vector<float> slow_tail(slow.begin() + 4800, slow.end());
    REQUIRE(high_band(slow_tail) > high_band(quick_tail) * 3.0);
}

TEST_CASE("Zero distortion leaves a clean swept tone", "[signal][drum][zap]") {
    ZapVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(200.0);
    voice.set_pitch_sweep_octaves(0.0);
    voice.set_distortion(0.0);
    voice.set_decay_ms(600.0);
    voice.set_detune_cents(0.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
    voice.gate().set_colour(0.0);

    const auto y = hit(voice, 1.0f, 24000);
    REQUIRE(tone_amplitude(y, 200.0) > tone_amplitude(y, 600.0) * 4.0);
}

TEST_CASE("Every physical voice stays finite at extreme settings",
          "[signal][drum]") {
    MembraneVoice membrane;
    membrane.prepare(kFs);
    membrane.set_exciter(MembraneExciter::pluck);
    membrane.set_sub_level(0.5);
    membrane.set_air_level(0.5);
    membrane.set_click_level(0.5);
    membrane.set_tune_hz(2000.0);
    membrane.set_stretch(1.0);
    membrane.set_spread(1.0);
    membrane.set_decay_ms(8000.0);
    membrane.output().set_drive(1.0);
    membrane.output().set_fold(1.0);

    CymbalVoice cymbal;
    cymbal.prepare(kFs);
    cymbal.set_tune_hz(2000.0);
    cymbal.set_decay_ms(8000.0);
    cymbal.set_shift_hz(400.0);
    cymbal.set_inharmonicity(1.0);
    cymbal.output().set_drive(1.0);
    cymbal.output().set_fold(1.0);

    ZapVoice zap;
    zap.prepare(kFs);
    zap.set_tune_hz(4000.0);
    zap.set_pitch_sweep_octaves(6.0);
    zap.set_distortion(1.0);
    zap.set_resonant_depth(32.0);
    zap.set_ring(1.0);
    zap.output().set_drive(1.0);
    zap.output().set_fold(1.0);

    Voice* voices[] = {&membrane, &cymbal, &zap};
    for (Voice* voice : voices) {
        for (int repeat = 0; repeat < 4; ++repeat) {
            const auto y = hit(*voice, 1.0f, 12000);
            for (float v : y) REQUIRE(std::isfinite(v));
            REQUIRE(peak(y) < 20.0);
        }
    }
}

TEST_CASE("The physical voices allocate nothing on the audio thread",
          "[signal][drum][rt-safety]") {
    MembraneVoice membrane;
    membrane.prepare(kFs);
    membrane.set_exciter(MembraneExciter::pluck);
    membrane.set_sub_level(0.5);
    membrane.set_air_level(0.5);
    membrane.set_click_level(0.5);
    CymbalVoice cymbal;
    cymbal.prepare(kFs);
    ZapVoice zap;
    zap.prepare(kFs);

    std::vector<float> buffer(256, 0.0f);
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        Voice* voices[] = {&membrane, &cymbal, &zap};
        for (Voice* voice : voices) {
            for (int repeat = 0; repeat < 4; ++repeat) {
                voice->note_on(0.5f + 0.1f * static_cast<float>(repeat));
                for (int block = 0; block < 12; ++block) {
                    std::fill(buffer.begin(), buffer.end(), 0.0f);
                    voice->process(buffer.data(), static_cast<int>(buffer.size()));
                }
                voice->choke(3.0f);
                voice->process(buffer.data(), static_cast<int>(buffer.size()));
                voice->reset();
            }
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

// -- Extended Karplus-Strong blocks -------------------------------------------

TEST_CASE("The tuning allpass costs the decay nothing",
          "[signal][string]") {
    // The reason the fractional delay is an allpass rather than an interpolator.
    // Linear interpolation is itself a lowpass sitting inside the loop next to
    // the damping filter, so its attenuation compounds every round trip and the
    // note dies early -- by an amount that depends on where the fractional part
    // happens to land. An allpass has unity magnitude, so two pitches whose loop
    // lengths differ only in their fractional part must ring for the same time.
    auto ring_length = [](double hz) {
        KarplusStrong string;
        string.prepare(kFs);
        string.set_frequency(hz);
        string.set_decay_seconds(1.5);
        string.set_damping(0.1);
        string.reset();
        string.pluck();

        pulp::signal::NoiseSource noise;
        noise.prepare(kFs);
        noise.reset();
        std::vector<float> y(static_cast<std::size_t>(3.0 * kFs));
        for (std::size_t i = 0; i < y.size(); ++i) {
            y[i] = string.process(i < 256 ? noise.white() : 0.0f);
        }
        return audible_length(y, 1e-4);
    };

    // 48000/220 = 218.18 (fraction 0.18); 48000/218.18... pick a pitch whose
    // loop length is near-integer and one whose fraction is near a half.
    const int near_integer = ring_length(kFs / 200.0);   // exactly 240 samples
    const int half_sample = ring_length(kFs / 240.5);    // 199.58 samples
    REQUIRE(near_integer > 0);
    REQUIRE(half_sample > 0);
    const double ratio = static_cast<double>(half_sample) / near_integer;
    REQUIRE(ratio > 0.7);
    REQUIRE(ratio < 1.4);
}

TEST_CASE("Playing harder is brighter, not just louder",
          "[signal][string][velocity]") {
    // The dynamic-level filter. Without it a string is the one voice in this set
    // that would answer velocity with gain alone, which is the failure the whole
    // VelocityResponse contract exists to prevent.
    auto brightness = [](double bandwidth_hz) {
        KarplusStrong string;
        string.prepare(kFs);
        string.set_frequency(220.0);
        string.set_decay_seconds(2.0);
        string.set_damping(0.1);
        string.set_dynamic_bandwidth_hz(bandwidth_hz);
        string.reset();
        string.pluck();

        pulp::signal::NoiseSource noise;
        noise.prepare(kFs);
        noise.reset();
        std::vector<float> y(8192);
        for (std::size_t i = 0; i < y.size(); ++i) {
            y[i] = string.process(i < 256 ? noise.white() : 0.0f);
        }
        return tone_amplitude(y, 2200.0) / (tone_amplitude(y, 220.0) + 1e-20);
    };

    REQUIRE(brightness(16000.0) > brightness(800.0) * 2.0);
}

TEST_CASE("Pick direction darkens the attack without retuning the string",
          "[signal][string]") {
    auto measure = [](double direction) {
        KarplusStrong string;
        string.prepare(kFs);
        string.set_frequency(220.0);
        string.set_decay_seconds(2.0);
        string.set_damping(0.1);
        string.set_pick_direction(direction);
        string.reset();
        string.pluck();

        pulp::signal::NoiseSource noise;
        noise.prepare(kFs);
        noise.reset();
        std::vector<float> y(16384);
        for (std::size_t i = 0; i < y.size(); ++i) {
            y[i] = string.process(i < 256 ? noise.white() : 0.0f);
        }
        return std::pair<double, double>{
            tone_amplitude(y, 2200.0) / (tone_amplitude(y, 220.0) + 1e-20),
            tone_amplitude(y, 220.0) / (tone_amplitude(y, 250.0) + 1e-20)};
    };

    const auto bright = measure(0.0);
    const auto dark = measure(0.9);
    REQUIRE(bright.first > dark.first * 1.5);

    // ...and the harmonic series stays where it was. Measured as the
    // fundamental against a neighbouring non-harmonic frequency, because a
    // zero-crossing rate here tracks how BRIGHT the signal is rather than how
    // high it is -- which is the very thing the control changes.
    REQUIRE(bright.second > 3.0);
    REQUIRE(dark.second > 3.0);
}


// -- String voice -------------------------------------------------------------

TEST_CASE("The string voice rings at its tuning with a harmonic series",
          "[signal][drum][string]") {
    StringVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(220.0);
    voice.set_decay_seconds(3.0);
    voice.set_damping(0.1);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, 32768);
    REQUIRE(peak(y) > 1e-3);
    REQUIRE(tone_amplitude(y, 220.0) > 1e-4);
    REQUIRE(tone_amplitude(y, 440.0) > 1e-4);
    // ...and nothing between the partials, which is what makes it a series.
    REQUIRE(tone_amplitude(y, 330.0) < tone_amplitude(y, 220.0) * 0.4);
}

TEST_CASE("A harder string hit is brighter, not only louder",
          "[signal][drum][string][velocity]") {
    // The voice's velocity reaches the excitation bandwidth, so the contract
    // every other drum here honours holds for the string too.
    StringVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(220.0);
    voice.set_decay_seconds(2.0);

    auto soft_full = hit(voice, 0.15f, 16384);
    auto loud_full = hit(voice, 1.0f, 16384);
    const double soft_peak = peak(soft_full);
    const double loud_peak = peak(loud_full);
    REQUIRE(soft_peak > 1e-5);
    REQUIRE(loud_peak > soft_peak);

    // Measured over the attack. The dynamic-level filter shapes the EXCITATION,
    // and the loop's own damping then works on whatever it was handed -- so by
    // the tail both notes have converged on the same spectrum and the
    // difference to measure has already happened.
    std::vector<float> soft(soft_full.begin(), soft_full.begin() + 2048);
    std::vector<float> loud(loud_full.begin(), loud_full.begin() + 2048);
    for (auto& v : soft) v = static_cast<float>(v / soft_peak);
    for (auto& v : loud) v = static_cast<float>(v / loud_peak);
    REQUIRE(high_fraction(loud, 1200.0) > high_fraction(soft, 1200.0) * 1.2);
}

TEST_CASE("Stiffness stretches the partials sharp", "[signal][drum][string]") {
    // What separates a struck bar or a piano from a guitar: the upper partials
    // sit above their true harmonic positions.
    auto partial_at = [](double stiffness, double f) {
        StringVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(200.0);
        voice.set_decay_seconds(3.0);
        voice.set_damping(0.05);
        voice.set_stiffness(stiffness);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return tone_amplitude(hit(voice, 1.0f, 32768), f);
    };

    // With no stiffness the fourth partial sits at 800; with stiffness it moves
    // up, so 800 loses energy relative to a point above it.
    const double rigid_at_800 = partial_at(0.0, 800.0);
    const double stiff_at_800 = partial_at(0.9, 800.0);
    REQUIRE(stiff_at_800 < rigid_at_800 * 0.8);
}

TEST_CASE("The string crossfades FM, ring, and sync modulation",
          "[signal][drum][string]") {
    auto modulated = [](StringModulation mode, double mix) {
        StringVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(180.0);
        voice.set_decay_seconds(1.0);
        voice.set_damping(0.2);
        voice.set_restart_on_hit(true);
        voice.set_lpg_amount(0.0);
        voice.set_modulation(mode);
        voice.set_modulation_mix(mix);
        voice.set_modulation_ratio(2.7);
        voice.set_fm_depth_octaves(0.4);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return hit(voice, 1.0f, 12000);
    };

    const auto dry = modulated(StringModulation::none, 0.0);
    // A zero crossfade is the sample-exact detector floor for every mode.
    REQUIRE(modulated(StringModulation::fm, 0.0) == dry);
    REQUIRE(modulated(StringModulation::ring, 0.0) == dry);
    REQUIRE(modulated(StringModulation::sync, 0.0) == dry);

    for (auto mode : {StringModulation::fm, StringModulation::ring,
                      StringModulation::sync}) {
        const auto wet = modulated(mode, 1.0);
        REQUIRE_FALSE(wet == dry);
        REQUIRE(peak(wet) > 1e-4);
    }

    const auto fm_wet = modulated(StringModulation::fm, 1.0);
    const auto fm_half = modulated(StringModulation::fm, 0.5);
    REQUIRE(fm_half.size() == dry.size());
    for (std::size_t i = 0; i < fm_half.size(); ++i) {
        const float expected = 0.5f * (dry[i] + fm_wet[i]);
        REQUIRE(std::fabs(fm_half[i] - expected) < 1e-6f);
    }
}

TEST_CASE("String sync modulation decays with the physical body",
          "[signal][drum][string][modulation][lifecycle]") {
    StringVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(180.0);
    voice.set_decay_seconds(2.0);
    voice.set_damping(0.2);
    voice.set_restart_on_hit(true);
    voice.set_lpg_amount(0.0);
    voice.set_modulation(StringModulation::sync);
    voice.set_modulation_mix(1.0);
    voice.set_modulation_ratio(2.7);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
    const auto y = hit(voice, 1.0f, 48000);

    auto energy = [&y](std::size_t begin, std::size_t end) {
        double sum = 0.0;
        for (std::size_t i = begin; i < end; ++i) {
            sum += static_cast<double>(y[i]) * static_cast<double>(y[i]);
        }
        return sum / static_cast<double>(end - begin);
    };
    const double early = energy(4800, 9600);
    const double late = energy(38400, 43200);
    REQUIRE(early > 1e-8);
    REQUIRE(late < early * 0.3);
}

TEST_CASE("Short string sync notes follow the configured decay",
          "[signal][drum][string][modulation][lifecycle]") {
    StringVoice voice;
    voice.prepare(kFs);
    voice.set_decay_seconds(0.05);
    voice.set_modulation(StringModulation::sync);
    voice.set_modulation_mix(1.0);
    voice.set_velocity_response(
        VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
    const auto y = hit(voice, 1.0f, 48000);
    const std::vector<float> tail(y.begin() + 24000, y.end());
    REQUIRE(peak(tail) < 1.0e-4);
}

TEST_CASE("Rearming string FM cannot revive a dormant auxiliary tail",
          "[signal][drum][string][modulation][lifecycle]") {
    auto configure = [](StringVoice& voice) {
        voice.prepare(kFs);
        voice.set_tune_hz(180.0);
        voice.set_decay_seconds(2.0);
        voice.set_modulation_mix(1.0);
        voice.set_fm_depth_octaves(0.4);
        voice.output().set_oversampling(
            pulp::signal::drum::OutputOversampling::bypass);
        voice.set_velocity_response(
            VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
    };

    StringVoice recovered;
    configure(recovered);
    recovered.set_modulation(StringModulation::fm);
    (void)hit(recovered, 1.0f, 1000);
    recovered.set_modulation(StringModulation::none);
    (void)hit(recovered, 1.0f, 1000);
    recovered.set_modulation(StringModulation::fm);
    const auto rearmed = hit(recovered, 1.0f, 12000);

    StringVoice fresh;
    configure(fresh);
    fresh.set_modulation(StringModulation::fm);
    REQUIRE(rearmed == hit(fresh, 1.0f, 12000));

    StringVoice reentered;
    StringVoice stayed_dry;
    configure(reentered);
    configure(stayed_dry);
    reentered.set_modulation(StringModulation::fm);
    stayed_dry.set_modulation(StringModulation::fm);
    REQUIRE(hit(reentered, 1.0f, 1000) ==
            hit(stayed_dry, 1.0f, 1000));
    reentered.set_modulation(StringModulation::none);
    stayed_dry.set_modulation(StringModulation::none);
    REQUIRE(render(reentered, 1000) == render(stayed_dry, 1000));
    reentered.set_modulation(StringModulation::fm);
    REQUIRE(render(reentered, 12000) == render(stayed_dry, 12000));
}

TEST_CASE("The string's lowpass gate darkens its release",
          "[signal][drum][string]") {
    auto gated = [](double amount) {
        StringVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(180.0);
        voice.set_decay_seconds(2.0);
        voice.set_damping(0.05);
        voice.set_lpg_amount(amount);
        voice.gate().set_colour(1.0);
        voice.gate().set_fall_ms(120.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return hit(voice, 1.0f, 24000);
    };

    const auto bypassed = gated(0.0);
    const auto full = gated(1.0);
    REQUIRE_FALSE(bypassed == full);
    const std::vector<float> bypassed_tail(bypassed.begin() + 12000,
                                            bypassed.end());
    const std::vector<float> full_tail(full.begin() + 12000, full.end());
    REQUIRE(high_fraction(full_tail, 1200.0) <
            high_fraction(bypassed_tail, 1200.0) * 0.8);
}

TEST_CASE("The optional string lowpass gate is bypassed by default",
          "[signal][drum][string][compatibility]") {
    auto render_default = [](bool explicit_bypass) {
        StringVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(180.0);
        voice.set_decay_seconds(2.0);
        voice.set_damping(0.05);
        if (explicit_bypass) voice.set_lpg_amount(0.0);
        voice.gate().set_colour(1.0);
        voice.gate().set_fall_ms(120.0);
        voice.set_velocity_response(
            VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return hit(voice, 1.0f, 24000);
    };

    REQUIRE(render_default(false) == render_default(true));
}

TEST_CASE("A second hit adds to the ringing string by default",
          "[signal][drum][string]") {
    // The physical behaviour, and why a fast repeated figure does not sound
    // like one sample fired twice.
    StringVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(220.0);
    voice.set_decay_seconds(3.0);
    voice.set_restart_on_hit(false);

    const auto single = hit(voice, 1.0f, 24000);

    voice.reset();
    voice.note_on(1.0f);
    std::vector<float> doubled(24000, 0.0f);
    for (int i = 0; i < 6000; i += 64)
        voice.process(doubled.data() + i, std::min(64, 6000 - i));
    voice.note_on(1.0f);
    for (int i = 6000; i < 24000; i += 64)
        voice.process(doubled.data() + i, std::min(64, 24000 - i));

    bool differs = false;
    for (std::size_t i = 6000; i < doubled.size(); ++i) {
        if (std::fabs(static_cast<double>(doubled[i] - single[i])) > 1e-4) {
            differs = true;
            break;
        }
    }
    REQUIRE(differs);
}

TEST_CASE("An additive string retrigger keeps the output filter ringing",
          "[signal][drum][string][output][lifecycle]") {
    auto prepare = [](StringVoice& voice) {
        voice.prepare(kFs);
        voice.set_tune_hz(220.0);
        voice.set_decay_seconds(3.0);
        voice.set_restart_on_hit(false);
        voice.note_on(1.0f);
        (void)render(voice, 6000);
    };

    StringVoice control;
    StringVoice retriggered;
    prepare(control);
    prepare(retriggered);

    retriggered.note_on(1.0f);
    const auto uninterrupted = render(control, 32);
    const auto after_retrigger = render(retriggered, 32);

    auto energy = [](const std::vector<float>& samples) {
        double sum = 0.0;
        for (float sample : samples) {
            sum += static_cast<double>(sample) *
                   static_cast<double>(sample);
        }
        return sum;
    };
    const double uninterrupted_energy = energy(uninterrupted);
    REQUIRE(uninterrupted_energy > 1e-8);
    REQUIRE(energy(after_retrigger) > uninterrupted_energy * 0.5);
}

TEST_CASE("The string voice allocates nothing on the audio thread",
          "[signal][drum][string][rt-safety]") {
    StringVoice voice;
    voice.prepare(kFs);
    voice.set_stiffness(0.4);
    voice.set_pick_direction(0.3);
    voice.set_modulation(StringModulation::fm);
    voice.set_modulation_mix(0.7);
    voice.set_lpg_amount(1.0);
    voice.output().set_drive(0.3);

    std::vector<float> buffer(256, 0.0f);
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int repeat = 0; repeat < 6; ++repeat) {
            voice.set_tune_hz(180.0 + 40.0 * repeat);
            voice.note_on(0.5f + 0.07f * static_cast<float>(repeat));
            for (int block = 0; block < 12; ++block) {
                std::fill(buffer.begin(), buffer.end(), 0.0f);
                voice.process(buffer.data(), static_cast<int>(buffer.size()));
            }
            voice.choke(3.0f);
            voice.process(buffer.data(), static_cast<int>(buffer.size()));
            voice.reset();
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}
