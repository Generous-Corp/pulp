// The FM percussion voices.
//
// FM is in the kit for one reason: its sidebands are spaced by the modulator's
// frequency, so a non-integer ratio puts partials where no harmonic series has
// them. That is inharmonicity generated rather than filtered, and it is what
// these tests measure — along with the two decouplings that make FM read as a
// struck object: the index envelope collapsing the spectrum while the note is
// still sounding, and (for the eight-operator voice) operators decaying at
// different rates so the spectrum evolves rather than fades.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/drum/fm.hpp>
#include <pulp/signal/drum/fm6.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

using pulp::signal::drum::Fm6DrumVoice;
using pulp::signal::drum::Fm8DrumVoice;
using pulp::signal::drum::FmDrumVoice;
using pulp::signal::drum::FmWaveTable;
using pulp::signal::drum::VelocityResponse;
using pulp::signal::drum::Voice;

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

template <typename Container>
double tone_amplitude(const Container& x, double f) {
    const std::size_t n = x.size();
    const double w = 2.0 * kPi * f / kFs;
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

// Configures a two-operator voice with everything but the tone switched off,
// so a spectrum measurement sees only the FM.
void bare(FmDrumVoice& voice) {
    voice.prepare(kFs);
    voice.set_click_level(0.0);
    voice.set_cutoff_hz(18000.0);
    voice.set_resonance(0.7);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
}

}  // namespace

// -- Two operators -----------------------------------------------------------

TEST_CASE("All 26 shared FM waves are bounded and distinct",
          "[signal][drum][fm][wave]") {
    std::vector<std::vector<double>> waves;
    for (int wave = 0; wave < FmWaveTable::wave_count; ++wave) {
        std::vector<double> samples(512);
        for (std::size_t i = 0; i < samples.size(); ++i) {
            samples[i] = FmWaveTable::read(
                wave, static_cast<double>(i) / static_cast<double>(samples.size()));
            REQUIRE(std::isfinite(samples[i]));
            REQUIRE(std::fabs(samples[i]) <= 1.0000001);
        }
        waves.push_back(std::move(samples));
    }
    for (std::size_t wave = 0; wave < waves.size(); ++wave) {
        for (std::size_t other = wave + 1; other < waves.size(); ++other) {
            REQUIRE_FALSE(waves[wave] == waves[other]);
        }
    }
}

TEST_CASE("FM wave tables omit harmonics above Nyquist",
          "[signal][drum][fm][wave][aliasing]") {
    constexpr double phase = 0.071;
    constexpr double increment = 0.2;
    const double fundamental = std::sin(2.0 * kPi * phase);
    const double second = std::sin(4.0 * kPi * phase);
    const double expected = 0.5 * (fundamental + second);

    // Table index 20 has four equal harmonics. At a 0.2-cycle increment only its
    // first two fit below Nyquist, so the reader renormalizes that pair.
    REQUIRE(std::fabs(FmWaveTable::read(20, phase, increment) - expected) <
            1.0e-12);
    REQUIRE(FmWaveTable::read(20, phase, increment) !=
            FmWaveTable::read(20, phase));
}

TEST_CASE("FM wave-table Nyquist transitions stay continuous",
          "[signal][drum][fm][wave][aliasing]") {
    constexpr double phase = 0.071;
    const double below = FmWaveTable::read(20, phase, 0.1249);
    const double above = FmWaveTable::read(20, phase, 0.1251);
    REQUIRE(std::fabs(below - above) < 1.0e-3);
}

TEST_CASE("FM2 carrier and modulator wave tables reach emitted audio",
          "[signal][drum][fm][wave]") {
    auto render_wave = [](int carrier, int modulator) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_tune_hz(180.0);
        voice.set_ratio(1.7);
        voice.set_index(5.0);
        voice.set_index_ms(1000.0);
        voice.set_decay_ms(1000.0);
        voice.set_carrier_wave(carrier);
        voice.set_modulator_wave(modulator);
        return hit(voice, 1.0f, 12000);
    };

    const auto sine = render_wave(0, 0);
    REQUIRE_FALSE(render_wave(20, 0) == sine);
    REQUIRE_FALSE(render_wave(0, 20) == sine);
}

TEST_CASE("An FM retrigger keeps pending output-stage samples",
          "[signal][drum][fm][output][lifecycle]") {
    auto prepare = [](FmDrumVoice& voice) {
        bare(voice);
        voice.set_tune_hz(180.0);
        voice.set_index(3.0);
        voice.set_index_ms(1000.0);
        voice.set_decay_ms(2000.0);
        voice.note_on(1.0f);
        (void)render(voice, 6000);
    };

    FmDrumVoice uninterrupted;
    FmDrumVoice retriggered;
    prepare(uninterrupted);
    prepare(retriggered);
    retriggered.note_on(1.0f);

    const auto control = render(uninterrupted, 32);
    const auto after_retrigger = render(retriggered, 32);
    REQUIRE(peak(control) > 1.0e-5);
    REQUIRE(peak(after_retrigger) > peak(control) * 0.1);
}

TEST_CASE("FM2 operator warp envelopes collapse independently",
          "[signal][drum][fm][warp]") {
    auto warped = [](double amount, double decay_ms) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_tune_hz(180.0);
        voice.set_index(0.0);
        voice.set_decay_ms(1000.0);
        voice.set_carrier_warp(amount);
        voice.set_carrier_warp_ms(decay_ms);
        voice.set_modulator_warp(0.0);
        return hit(voice, 1.0f, 24000);
    };

    const auto unwarped = warped(0.0, 5.0);
    const auto fast = warped(1.0, 5.0);
    const auto slow = warped(1.0, 1000.0);
    REQUIRE_FALSE(fast == slow);
    auto late_error = [&unwarped](const std::vector<float>& candidate) {
        double energy = 0.0;
        for (std::size_t i = 12000; i < candidate.size(); ++i) {
            const double difference =
                static_cast<double>(candidate[i] - unwarped[i]);
            energy += difference * difference;
        }
        return energy;
    };
    REQUIRE(late_error(fast) < late_error(slow) * 0.1);

    auto modulator_warp = [](double amount) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_tune_hz(180.0);
        voice.set_ratio(1.7);
        voice.set_index(6.0);
        voice.set_index_ms(1000.0);
        voice.set_modulator_warp(amount);
        voice.set_modulator_warp_ms(1000.0);
        return hit(voice, 1.0f, 12000);
    };
    REQUIRE_FALSE(modulator_warp(0.0) == modulator_warp(1.0));
}

TEST_CASE("FM2 pitch LFO waits, fades in, and then bends pitch",
          "[signal][drum][fm][lfo]") {
    auto lfo = [](double depth) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_tune_hz(220.0);
        voice.set_index(0.0);
        voice.set_decay_ms(1000.0);
        voice.set_lfo_rate_hz(7.0);
        voice.set_lfo_depth_octaves(depth);
        voice.set_lfo_delay_ms(20.0);
        voice.set_lfo_fade_ms(30.0);
        return hit(voice, 1.0f, 12000);
    };

    const auto still = lfo(0.0);
    const auto moving = lfo(0.25);
    REQUIRE(std::equal(still.begin(), still.begin() + 900, moving.begin()));
    REQUIRE_FALSE(still == moving);
}

TEST_CASE("FM2 hard sync resets the modulator from the carrier",
          "[signal][drum][fm][sync]") {
    auto synced = [](bool enabled) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_tune_hz(200.0);
        voice.set_ratio(1.73);
        voice.set_index(6.0);
        voice.set_index_ms(1000.0);
        voice.set_decay_ms(1000.0);
        voice.set_hard_sync(enabled);
        return hit(voice, 1.0f, 12000);
    };
    REQUIRE_FALSE(synced(false) == synced(true));
}

TEST_CASE("Every procedural FM transient renders a distinct finite strike",
          "[signal][drum][fm][transient]") {
    std::vector<float> previous;
    for (int transient = 0; transient < 24; ++transient) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_index(0.0);
        voice.set_transient(transient);
        const auto y = hit(voice, 1.0f, 2400);
        INFO("transient " << transient);
        REQUIRE(peak(y) > 1e-4);
        for (float sample : y) REQUIRE(std::isfinite(sample));
        if (!previous.empty()) REQUIRE_FALSE(y == previous);
        previous = y;
    }
}

TEST_CASE("FM2 remains active for the full shared noise transient",
          "[signal][drum][fm][transient][lifecycle]") {
    auto noise_is_still_active = [](auto& voice) {
        voice.prepare(kFs);
        voice.set_noise_level(1.0);
        voice.set_noise_decay_ms(500.0);
        voice.set_click_level(0.0);
        voice.output().set_oversampling(
            pulp::signal::drum::OutputOversampling::bypass);
        voice.note_on(1.0f);
        (void)render(voice, 24000);
        return voice.is_active();
    };

    Fm8DrumVoice control;
    REQUIRE(noise_is_still_active(control));

    FmDrumVoice voice;
    voice.set_decay_ms(10.0);
    REQUIRE(noise_is_still_active(voice));
}

TEST_CASE("Muting an FM noise transient does not freeze its lifecycle",
          "[signal][drum][fm][transient][lifecycle]") {
    auto muted_noise_finishes = [](auto& voice) {
        voice.prepare(kFs);
        voice.set_noise_level(1.0);
        voice.set_noise_decay_ms(10.0);
        voice.set_click_level(0.0);
        voice.output().set_oversampling(
            pulp::signal::drum::OutputOversampling::bypass);
        voice.note_on(1.0f);
        (void)render(voice, 32);
        voice.set_noise_level(0.0);
        (void)render(voice, 48000);
        return voice.is_active();
    };

    FmDrumVoice two;
    two.set_decay_ms(1.0);
    REQUIRE_FALSE(muted_noise_finishes(two));

    Fm8DrumVoice eight;
    for (int op = 0; op < Fm8DrumVoice::operator_count; ++op)
        eight.set_operator_level(op, 0.0);
    REQUIRE_FALSE(muted_noise_finishes(eight));
}

TEST_CASE("Sidebands land at the carrier plus and minus the modulator",
          "[signal][drum][fm]") {
    // The defining property. With a carrier at 200 and a ratio of 2 the
    // modulator is at 400, so energy must appear at 200 ± 400 — i.e. at 600 and
    // at |−200| = 200. Use ratio 1.5 instead so the arithmetic is unambiguous:
    // modulator 300, sidebands at 500 and 100.
    FmDrumVoice voice;
    bare(voice);
    voice.set_tune_hz(200.0);
    voice.set_ratio(1.5);
    voice.set_index(4.0);
    voice.set_index_ms(2000.0);   // hold the index open across the measurement
    voice.set_decay_ms(2000.0);

    const auto y = hit(voice, 1.0f, 32768);
    REQUIRE(peak(y) > 1e-3);

    const double carrier = tone_amplitude(y, 200.0);
    REQUIRE(tone_amplitude(y, 500.0) > carrier * 0.1);   // carrier + modulator
    REQUIRE(tone_amplitude(y, 100.0) > carrier * 0.1);   // carrier − modulator
    // ...and a frequency that is neither must be far weaker.
    REQUIRE(tone_amplitude(y, 350.0) < tone_amplitude(y, 500.0) * 0.2);
}

TEST_CASE("An integer ratio stays harmonic, a non-integer one does not",
          "[signal][drum][fm]") {
    // This is why FM is in a drum kit rather than only in a synth: the ratio
    // decides whether the result has a pitch at all.
    auto energy_at = [](double ratio, double f) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_tune_hz(200.0);
        voice.set_ratio(ratio);
        voice.set_index(5.0);
        voice.set_index_ms(2000.0);
        voice.set_decay_ms(2000.0);
        const auto y = hit(voice, 1.0f, 32768);
        return tone_amplitude(y, f);
    };

    // Ratio 2: sidebands at 200 ± 400 → 600, and 400, 800 … all multiples of
    // 200. So 600 is strong and 500 is not.
    REQUIRE(energy_at(2.0, 600.0) > energy_at(2.0, 500.0) * 5.0);

    // Ratio 1.77: sidebands at 200 ± 354 → 554, which is not a multiple of
    // anything the carrier defines. Energy appears off the harmonic grid.
    REQUIRE(energy_at(1.77, 554.0) > energy_at(1.77, 600.0));
}

TEST_CASE("The index envelope collapses the spectrum while the note sounds",
          "[signal][drum][fm]") {
    // The decoupling that makes FM read as a struck object: bright at the
    // strike, simple in the tail, with the amplitude envelope unchanged.
    FmDrumVoice voice;
    bare(voice);
    voice.set_tune_hz(150.0);
    voice.set_ratio(2.4);
    voice.set_index(8.0);
    voice.set_index_ms(15.0);
    voice.set_decay_ms(1500.0);

    const auto y = hit(voice, 1.0f, 48000);
    const std::vector<float> early(y.begin(), y.begin() + 4800);
    const std::vector<float> late(y.begin() + 24000, y.end());
    REQUIRE(high_fraction(early, 800.0) > high_fraction(late, 800.0) * 3.0);
}

TEST_CASE("Index depth changes the spectrum, not the level",
          "[signal][drum][fm]") {
    auto measure = [](double index) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_tune_hz(200.0);
        voice.set_ratio(2.0);
        voice.set_index(index);
        voice.set_index_ms(2000.0);
        voice.set_decay_ms(1000.0);
        const auto y = hit(voice, 1.0f, 24000);
        return std::pair<double, double>{peak(y), high_fraction(y, 800.0)};
    };

    const auto quiet = measure(0.0);
    const auto rich = measure(9.0);
    REQUIRE(rich.second > quiet.second * 5.0);
    // A modulation index is not a gain: the peak must stay in the same region.
    REQUIRE(rich.first < quiet.first * 2.0);
    REQUIRE(rich.first > quiet.first * 0.5);
}

TEST_CASE("Operator feedback drives the modulator toward noise",
          "[signal][drum][fm]") {
    auto brightness = [](double feedback) {
        FmDrumVoice voice;
        bare(voice);
        voice.set_tune_hz(200.0);
        voice.set_ratio(1.0);
        voice.set_index(3.0);
        voice.set_index_ms(2000.0);
        voice.set_decay_ms(1000.0);
        voice.set_feedback(feedback);
        const auto y = hit(voice, 1.0f, 24000);
        return high_fraction(y, 1500.0);
    };

    REQUIRE(brightness(1.0) > brightness(0.0) * 3.0);
}

TEST_CASE("Velocity opens the index rather than only the level",
          "[signal][drum][fm][velocity]") {
    FmDrumVoice voice;
    voice.prepare(kFs);
    voice.set_click_level(0.0);
    voice.set_tune_hz(180.0);
    voice.set_ratio(2.3);
    voice.set_index(6.0);
    voice.set_index_ms(600.0);
    voice.set_decay_ms(800.0);

    auto soft = hit(voice, 0.2f, 24000);
    auto loud = hit(voice, 1.0f, 24000);
    const double soft_peak = peak(soft);
    const double loud_peak = peak(loud);
    REQUIRE(loud_peak > soft_peak);

    for (auto& v : soft) v = static_cast<float>(v / soft_peak);
    for (auto& v : loud) v = static_cast<float>(v / loud_peak);
    REQUIRE(high_fraction(loud, 900.0) > high_fraction(soft, 900.0) * 1.2);
}

// -- Eight operators ---------------------------------------------------------

TEST_CASE("FM8 selects a wave independently for every operator",
          "[signal][drum][fm8][wave]") {
    auto wave = [](int op_to_hear, int selected) {
        Fm8DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(0);
        voice.set_depth(0.0);
        voice.set_formant_hz(16000.0);
        voice.set_formant_q(0.6);
        for (int op = 0; op < Fm8DrumVoice::operator_count; ++op) {
            voice.set_operator_level(op, op == op_to_hear ? 1.0 : 0.0);
        }
        voice.set_operator_wave(op_to_hear, selected);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return hit(voice, 1.0f, 12000);
    };
    for (int op = 0; op < Fm8DrumVoice::operator_count; ++op) {
        INFO("operator " << op);
        REQUIRE_FALSE(wave(op, 0) == wave(op, 20));
    }
}

TEST_CASE("FM8 can emit the shared tinted-noise transient without operators",
          "[signal][drum][fm8][transient]") {
    Fm8DrumVoice voice;
    voice.prepare(kFs);
    for (int op = 0; op < Fm8DrumVoice::operator_count; ++op) {
        voice.set_operator_level(op, 0.0);
    }
    voice.set_transient(23);
    const auto y = hit(voice, 1.0f, 4800);
    REQUIRE(peak(y) > 1e-3);
    for (float sample : y) REQUIRE(std::isfinite(sample));
}

TEST_CASE("Every algorithm renders audio and stays finite",
          "[signal][drum][fm8]") {
    // The routing table is data, so the cheapest guard against a bad row is to
    // run all of them. A mask naming an operator that never sounds, or a
    // carrier flag of zero, would show up here as silence.
    for (int algorithm = 0; algorithm < Fm8DrumVoice::algorithm_count; ++algorithm) {
        Fm8DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(algorithm);
        voice.set_tune_hz(120.0);
        voice.set_depth(4.0);

        const auto y = hit(voice, 1.0f, 12000);
        INFO("algorithm " << algorithm);
        REQUIRE(peak(y) > 1e-4);
        for (float v : y) REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("A deeper stack produces a denser spectrum than a parallel bank",
          "[signal][drum][fm8]") {
    // Algorithm 0 is eight independent carriers -- additive, so its spectrum is
    // exactly its eight ratios. Algorithm 15 is one eight-deep stack, where
    // each operator modulates the next and the sidebands modulate each other.
    auto brightness = [](int algorithm) {
        Fm8DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(algorithm);
        voice.set_tune_hz(120.0);
        voice.set_depth(5.0);
        voice.set_formant_hz(16000.0);
        voice.set_formant_q(0.6);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto y = hit(voice, 1.0f, 24000);
        return high_fraction(y, 2000.0);
    };

    REQUIRE(brightness(15) > brightness(0) * 1.5);
}

TEST_CASE("Modulation depth is what the algorithm scales",
          "[signal][drum][fm8]") {
    // At depth 0 every routing collapses to the same thing -- eight sines --
    // so the algorithm can only matter when there is depth for it to act on.
    auto brightness = [](int algorithm, double depth) {
        Fm8DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(algorithm);
        voice.set_tune_hz(120.0);
        voice.set_depth(depth);
        voice.set_formant_hz(16000.0);
        voice.set_formant_q(0.6);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto y = hit(voice, 1.0f, 24000);
        return high_fraction(y, 2000.0);
    };

    REQUIRE(brightness(15, 8.0) > brightness(15, 0.0) * 2.0);

    // The negative control has to compare algorithms that share a CARRIER set,
    // not just any two: an algorithm defines both who modulates whom and who is
    // heard, and at zero depth only the second still matters. Algorithms 7, 11,
    // 12 and 13 all sound operators 0, 5, 6 and 7, so with modulation disabled
    // they must be indistinguishable -- and with it, they must not be.
    REQUIRE(std::fabs(brightness(7, 0.0) - brightness(13, 0.0)) < 1e-9);
    REQUIRE(std::fabs(brightness(7, 6.0) - brightness(13, 6.0)) > 1e-3);
}

TEST_CASE("Per-operator decays make the spectrum evolve rather than fade",
          "[signal][drum][fm8]") {
    // A modulator that dies before its carrier takes its sidebands with it.
    // With every operator on the same decay the spectrum would keep its shape
    // all the way down, which is the sound of a synth pad rather than a drum.
    auto tail_brightness = [](bool staggered) {
        Fm8DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(9);
        voice.set_tune_hz(120.0);
        voice.set_depth(5.0);
        voice.set_formant_hz(16000.0);
        voice.set_formant_q(0.6);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        for (int op = 0; op < Fm8DrumVoice::operator_count; ++op) {
            voice.set_operator_decay_ms(op, staggered ? 800.0 / (op + 1) : 800.0);
        }
        const auto y = hit(voice, 1.0f, 48000);
        const std::vector<float> tail(y.begin() + 24000, y.end());
        return high_fraction(tail, 1500.0);
    };

    REQUIRE(tail_brightness(false) > tail_brightness(true) * 1.5);
}

TEST_CASE("The eight-operator voice is deterministic",
          "[signal][drum][fm8]") {
    Fm8DrumVoice voice;
    voice.prepare(kFs);
    voice.set_algorithm(11);
    const auto first = hit(voice, 0.8f, 12000);
    voice.reset();
    const auto second = hit(voice, 0.8f, 12000);
    REQUIRE(first == second);
}

TEST_CASE("Successive FM8 transient hits restart deterministic excitation",
          "[signal][drum][fm8][transient][lifecycle]") {
    Fm8DrumVoice voice;
    voice.prepare(kFs);
    for (int op = 0; op < Fm8DrumVoice::operator_count; ++op)
        voice.set_operator_level(op, 0.0);
    voice.set_transient(23);
    voice.output().set_oversampling(
        pulp::signal::drum::OutputOversampling::bypass);

    const auto first = hit(voice, 1.0f, 12000);
    const auto second = hit(voice, 1.0f, 12000);
    REQUIRE(peak(first) > 1.0e-4);
    REQUIRE(second == first);
}

TEST_CASE("An out-of-range algorithm or operator index is clamped, not UB",
          "[signal][drum][fm8]") {
    Fm8DrumVoice voice;
    voice.prepare(kFs);
    voice.set_algorithm(999);
    REQUIRE(voice.algorithm() == Fm8DrumVoice::algorithm_count - 1);
    voice.set_algorithm(-5);
    REQUIRE(voice.algorithm() == 0);

    // Out-of-range operator indices are ignored rather than writing past the
    // arrays, so a caller looping past the count cannot corrupt the voice.
    voice.set_operator_ratio(-1, 3.0);
    voice.set_operator_ratio(Fm8DrumVoice::operator_count, 3.0);
    voice.set_operator_level(99, 1.0);
    voice.set_operator_decay_ms(-2, 100.0);
    voice.set_operator_feedback(50, 1.0);
    voice.set_operator_wave(-1, 3);
    voice.set_operator_wave(Fm8DrumVoice::operator_count, 3);

    const auto y = hit(voice, 1.0f, 4800);
    for (float v : y) REQUIRE(std::isfinite(v));
}

TEST_CASE("The FM voices stay finite at extreme settings", "[signal][drum][fm]") {
    FmDrumVoice two;
    two.prepare(kFs);
    two.set_tune_hz(4000.0);
    two.set_ratio(24.0);
    two.set_index(24.0);
    two.set_feedback(1.0);
    two.set_pitch_sweep_octaves(6.0);
    two.set_carrier_wave(25);
    two.set_modulator_wave(24);
    two.set_carrier_warp(1.0);
    two.set_modulator_warp(1.0);
    two.set_lfo_depth_octaves(2.0);
    two.set_hard_sync(true);
    two.set_transient(23);
    two.output().set_drive(1.0);
    two.output().set_fold(1.0);

    Fm8DrumVoice eight;
    eight.prepare(kFs);
    eight.set_algorithm(15);
    eight.set_tune_hz(2000.0);
    eight.set_depth(12.0);
    for (int op = 0; op < Fm8DrumVoice::operator_count; ++op) {
        eight.set_operator_feedback(op, 1.0);
        eight.set_operator_level(op, 1.0);
        eight.set_operator_wave(op, 25 - op);
    }
    eight.set_transient(23);
    eight.output().set_drive(1.0);
    eight.output().set_fold(1.0);

    Voice* voices[] = {&two, &eight};
    for (Voice* voice : voices) {
        for (int repeat = 0; repeat < 4; ++repeat) {
            const auto y = hit(*voice, 1.0f, 12000);
            for (float v : y) REQUIRE(std::isfinite(v));
            REQUIRE(peak(y) < 20.0);
        }
    }
}

TEST_CASE("The FM voices allocate nothing on the audio thread",
          "[signal][drum][fm][rt-safety]") {
    FmDrumVoice two;
    two.prepare(kFs);
    two.set_feedback(0.5);
    two.set_carrier_wave(20);
    two.set_modulator_wave(21);
    two.set_carrier_warp(0.7);
    two.set_modulator_warp(0.6);
    two.set_lfo_depth_octaves(0.2);
    two.set_hard_sync(true);
    two.set_transient(23);
    two.output().set_drive(0.4);

    Fm8DrumVoice eight;
    eight.prepare(kFs);
    eight.set_algorithm(12);
    eight.set_transient(23);
    for (int op = 0; op < Fm8DrumVoice::operator_count; ++op) {
        eight.set_operator_wave(op, op * 3);
    }
    eight.output().set_drive(0.4);

    std::vector<float> buffer(256, 0.0f);
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        Voice* voices[] = {&two, &eight};
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


// -- Six operators, thirty-two algorithms ------------------------------------

TEST_CASE("Every six-operator routing is structurally well formed",
          "[signal][drum][fm6]") {
    // The routing table is transcribed data, and the header is explicit that it
    // has not been verified row by row against a service manual. These
    // invariants are what stop a MALFORMED row shipping silently -- they cannot
    // catch a row that is well formed and simply wrong, which is why the
    // caveat stays in the header rather than being retired by this test.
    for (int index = 0; index < Fm6DrumVoice::algorithm_count; ++index) {
        const auto& alg = Fm6DrumVoice::algorithms[static_cast<std::size_t>(index)];
        INFO("algorithm " << (index + 1));

        // Something has to reach the output.
        REQUIRE(alg.carriers != 0);
        // Carriers and masks may only name real operators.
        REQUIRE((alg.carriers & ~0x3Fu) == 0);
        REQUIRE(alg.feedback_op < Fm6DrumVoice::operator_count);

        std::uint8_t reachable = alg.carriers;
        for (int pass = 0; pass < Fm6DrumVoice::operator_count; ++pass) {
            for (int op = 0; op < Fm6DrumVoice::operator_count; ++op) {
                if (reachable & (1u << op)) {
                    reachable = static_cast<std::uint8_t>(
                        reachable | alg.modulated_by[static_cast<std::size_t>(op)]);
                }
            }
        }
        for (int op = 0; op < Fm6DrumVoice::operator_count; ++op) {
            const auto mask = alg.modulated_by[static_cast<std::size_t>(op)];
            // A mask may only name real operators, and an operator must not
            // modulate itself through the mask -- self-modulation is the
            // feedback path, which is a separate, single, named operator.
            REQUIRE((mask & ~0x3Fu) == 0);
            REQUIRE((mask & (1u << op)) == 0);
            // Every operator either sounds or feeds something that does. An
            // unreachable operator is silent CPU and a certain sign the row is
            // mistyped.
            INFO("operator " << (op + 1) << " unreachable");
            REQUIRE((reachable & (1u << op)) != 0);
        }
    }
}

TEST_CASE("Modulation only ever flows from higher-numbered operators",
          "[signal][drum][fm6]") {
    // The documented layout of the original algorithm set: the highest-numbered
    // operator sits at the top and an operator is modulated only by ones above
    // it. This is the sharpest check available on the transcription -- an arrow
    // pointing the wrong way is the easiest error to make and among the hardest
    // to notice by ear, since a wrong-way routing still produces plausible FM.
    for (int index = 0; index < Fm6DrumVoice::algorithm_count; ++index) {
        const auto& alg = Fm6DrumVoice::algorithms[static_cast<std::size_t>(index)];
        for (int op = 0; op < Fm6DrumVoice::operator_count; ++op) {
            for (int src = 0; src < Fm6DrumVoice::operator_count; ++src) {
                if (alg.modulated_by[static_cast<std::size_t>(op)] & (1u << src)) {
                    INFO("algorithm " << (index + 1) << ": operator " << (op + 1)
                                      << " modulated by operator " << (src + 1));
                    REQUIRE(src > op);
                }
            }
        }
    }
}

TEST_CASE("Algorithm 8 matches its documented topology",
          "[signal][drum][fm6]") {
    // One row pinned against the documentation as a worked example: operators 1
    // and 3 are the carriers, operator 2 modulates 1, operators 4 and 5 both
    // modulate 3, operator 6 modulates 5, and the self-feedback sits on
    // operator 4. A transcription that drifted would almost certainly break
    // this row along with others.
    const auto& alg = Fm6DrumVoice::algorithms[7];
    REQUIRE(alg.carriers == 0x05);          // operators 1 and 3
    REQUIRE(alg.modulated_by[0] == 0x02);   // 1 <- 2
    REQUIRE(alg.modulated_by[2] == 0x18);   // 3 <- 4 and 5
    REQUIRE(alg.modulated_by[4] == 0x20);   // 5 <- 6
    REQUIRE(alg.feedback_op == 3);          // self-feedback on operator 4
}

TEST_CASE("Carrier outputs are scaled by the carrier count",
          "[signal][drum][fm6]") {
    // The original compensates for how many operators reach the output, so
    // routings with very different carrier counts arrive at comparable levels
    // -- the fully additive one is scaled by a sixth. Without it, switching
    // algorithm would be mostly a volume change.
    auto level_for = [](int algorithm) {
        Fm6DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(algorithm);
        voice.set_tune_hz(140.0);
        voice.set_depth(0.0);   // no modulation, so only the summing differs
        voice.set_formant_hz(14000.0);
        voice.set_formant_q(0.6);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return peak(hit(voice, 1.0f, 12000));
    };

    // Algorithm 32 sounds six operators, algorithm 1 sounds two. Unscaled, the
    // first would be around three times louder.
    const double six = level_for(31);
    const double two = level_for(0);
    REQUIRE(six < two * 2.0);
    REQUIRE(six > two * 0.5);
}

TEST_CASE("Every six-operator routing renders audio and stays finite",
          "[signal][drum][fm6]") {
    for (int index = 0; index < Fm6DrumVoice::algorithm_count; ++index) {
        Fm6DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(index);
        voice.set_tune_hz(140.0);
        voice.set_depth(4.0);
        voice.set_formant_hz(14000.0);
        voice.set_formant_q(0.6);

        const auto y = hit(voice, 1.0f, 12000);
        INFO("algorithm " << (index + 1));
        REQUIRE(peak(y) > 1e-4);
        for (float v : y) REQUIRE(std::isfinite(v));
    }
}

TEST_CASE("The fully additive routing carries no modulation",
          "[signal][drum][fm6]") {
    // Algorithm 32 is six independent carriers, so modulation depth must do
    // nothing at all to it -- the one row whose behaviour is unambiguous
    // regardless of how the rest of the table is verified.
    auto render_at = [](double depth) {
        Fm6DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(31);
        voice.set_tune_hz(140.0);
        voice.set_depth(depth);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return hit(voice, 1.0f, 12000);
    };

    REQUIRE(render_at(0.0) == render_at(9.0));
}

TEST_CASE("A stacked routing does respond to modulation depth",
          "[signal][drum][fm6]") {
    // The negative control for the additive test: depth must matter where
    // there is routing for it to act on.
    auto brightness = [](double depth) {
        Fm6DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(0);
        voice.set_tune_hz(140.0);
        voice.set_depth(depth);
        voice.set_formant_hz(14000.0);
        voice.set_formant_q(0.6);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return high_fraction(hit(voice, 1.0f, 24000), 1500.0);
    };

    REQUIRE(brightness(8.0) > brightness(0.0) * 2.0);
}

TEST_CASE("Feedback acts only on the routing's designated operator",
          "[signal][drum][fm6]") {
    // Algorithm 32 is additive with its feedback operator among the carriers,
    // so feedback is audible there; the depth control is not.
    auto brightness = [](double feedback) {
        Fm6DrumVoice voice;
        voice.prepare(kFs);
        voice.set_algorithm(31);
        voice.set_tune_hz(140.0);
        voice.set_feedback(feedback);
        voice.set_formant_hz(14000.0);
        voice.set_formant_q(0.6);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return high_fraction(hit(voice, 1.0f, 24000), 2000.0);
    };

    REQUIRE(brightness(1.0) > brightness(0.0) * 1.5);
}

TEST_CASE("The pitch envelope moves every operator together",
          "[signal][drum][fm6]") {
    // A global sweep, so the spectrum keeps its shape while the whole voice
    // moves -- unlike a per-operator ratio change, which reshapes it.
    Fm6DrumVoice voice;
    voice.prepare(kFs);
    voice.set_algorithm(31);
    voice.set_tune_hz(150.0);
    voice.set_pitch_sweep_octaves(2.0);
    voice.set_pitch_sweep_ms(60.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, 48000);
    const std::vector<float> early(y.begin(), y.begin() + 4800);
    const std::vector<float> late(y.begin() + 24000, y.end());
    REQUIRE(high_fraction(early, 1200.0) > high_fraction(late, 1200.0) * 1.5);
}

TEST_CASE("The six-operator voice is deterministic and bounds its indices",
          "[signal][drum][fm6]") {
    Fm6DrumVoice voice;
    voice.prepare(kFs);
    voice.set_algorithm(7);
    const auto first = hit(voice, 0.8f, 12000);
    voice.reset();
    REQUIRE(hit(voice, 0.8f, 12000) == first);

    voice.set_algorithm(999);
    REQUIRE(voice.algorithm() == Fm6DrumVoice::algorithm_count - 1);
    voice.set_algorithm(-3);
    REQUIRE(voice.algorithm() == 0);
    voice.set_operator_ratio(-1, 2.0);
    voice.set_operator_level(Fm6DrumVoice::operator_count, 1.0);
    voice.set_operator_decay_ms(42, 100.0);
    const auto y = hit(voice, 1.0f, 4800);
    for (float v : y) REQUIRE(std::isfinite(v));
}

TEST_CASE("The six-operator voice allocates nothing on the audio thread",
          "[signal][drum][fm6][rt-safety]") {
    Fm6DrumVoice voice;
    voice.prepare(kFs);
    voice.set_algorithm(4);
    voice.set_feedback(0.5);
    voice.output().set_drive(0.4);

    std::vector<float> buffer(256, 0.0f);
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int repeat = 0; repeat < 6; ++repeat) {
            voice.set_algorithm(repeat * 5);
            voice.note_on(0.5f + 0.05f * static_cast<float>(repeat));
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
