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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace {

using pulp::signal::drum::Fm8DrumVoice;
using pulp::signal::drum::FmDrumVoice;
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
    }
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
    two.output().set_drive(0.4);

    Fm8DrumVoice eight;
    eight.prepare(kFs);
    eight.set_algorithm(12);
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
