// Tests for the snare, hi-hat and clap voices, and for the kit that sums and
// chokes them.
//
// Each voice is checked against the thing that distinguishes it from a
// simpler construction, because a generic "makes plausible noise" assertion
// would pass for all three interchangeably: the snare against the tone/wire
// balance and the beating pair, the hat against its inharmonic cluster and the
// closed-to-open continuum, the clap against its burst train, and the kit
// against choke groups.
//
// The lifecycle rules the base class enforces are covered once, in
// test_drum_kick.cpp, against the kick; the suites here assume them.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/drum/clap.hpp>
#include <pulp/signal/drum/engine_registry.hpp>
#include <pulp/signal/drum/hat.hpp>
#include <pulp/signal/drum/kit.hpp>
#include <pulp/signal/drum/snare.hpp>
#include <pulp/signal/drum/tom.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

using pulp::signal::NoiseColor;
using pulp::signal::drum::ClapVoice;
using pulp::signal::drum::EngineId;
using pulp::signal::drum::EngineProvenance;
using pulp::signal::drum::HatVoice;
using pulp::signal::drum::Kit;
using pulp::signal::drum::OutputOversampling;
using pulp::signal::drum::ShellLayer;
using pulp::signal::drum::SnareVoice;
using pulp::signal::drum::TomVoice;
using pulp::signal::drum::VelocityResponse;
using pulp::signal::drum::Voice;
using pulp::signal::drum::create_engine;
using pulp::signal::drum::engine_registry;
using pulp::signal::drum::find_engine;

constexpr double kPi = 3.14159265358979323846;
constexpr double kFs = 48000.0;

class LegacyVoice : public Voice {
protected:
    void on_prepare(double) override {}
    void on_reset() override { active_ = false; }
    void on_note_on(float) override { active_ = true; }
    bool on_is_active() const override { return active_; }
    void render_add(float* out, int num_samples) override {
        for (int i = 0; i < num_samples; ++i) out[i] += 0.25f;
        active_ = false;
    }

private:
    bool active_ = false;
};

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

struct StereoRender {
    std::vector<float> left;
    std::vector<float> right;
};

StereoRender stereo_hit(Voice& voice, float velocity, int num_samples,
                        int block = 64) {
    StereoRender result{
        std::vector<float>(static_cast<std::size_t>(num_samples), 0.0f),
        std::vector<float>(static_cast<std::size_t>(num_samples), 0.0f)};
    voice.note_on(velocity);
    for (int i = 0; i < num_samples; i += block) {
        voice.process_stereo(result.left.data() + i, result.right.data() + i,
                             std::min(block, num_samples - i));
    }
    return result;
}

double peak(const std::vector<float>& x, std::size_t from = 0, std::size_t to = 0) {
    if (to == 0) to = x.size();
    double m = 0.0;
    for (std::size_t i = from; i < to && i < x.size(); ++i) {
        m = std::max(m, std::fabs(static_cast<double>(x[i])));
    }
    return m;
}

double rms(const std::vector<float>& x, std::size_t from = 0, std::size_t to = 0) {
    if (to == 0) to = x.size();
    double sum = 0.0;
    for (std::size_t i = from; i < to && i < x.size(); ++i) {
        sum += static_cast<double>(x[i]) * x[i];
    }
    return std::sqrt(sum / static_cast<double>(to - from));
}

double peak_normalized_difference(const std::vector<float>& a,
                                  const std::vector<float>& b) {
    const double a_peak = peak(a);
    const double b_peak = peak(b);
    if (a_peak <= 0.0 || b_peak <= 0.0) return 0.0;
    double sum = 0.0;
    const auto count = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < count; ++i) {
        const double difference =
            static_cast<double>(a[i]) / a_peak -
            static_cast<double>(b[i]) / b_peak;
        sum += difference * difference;
    }
    return std::sqrt(sum / static_cast<double>(count));
}

// Hann-windowed Goertzel amplitude at one frequency.
double tone_amplitude(const std::vector<float>& x, double f) {
    const double w = 2.0 * kPi * f / kFs;
    const double cw = std::cos(w);
    const double coeff = 2.0 * cw;
    double s1 = 0.0, s2 = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double win = 0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) /
                                                static_cast<double>(x.size() - 1));
        const double s0 = coeff * s1 - s2 + win * static_cast<double>(x[i]);
        s2 = s1;
        s1 = s0;
    }
    const double re = s1 - s2 * cw;
    const double im = s2 * std::sin(w);
    return std::sqrt(re * re + im * im) / (static_cast<double>(x.size()) * 0.25);
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

// Frequency from the mean interval between upward zero crossings. Resolves a
// slow drift that a per-window crossing count cannot.
double period_frequency(const std::vector<float>& x, std::size_t from, std::size_t to) {
    std::size_t first = 0, last = 0;
    int intervals = -1;
    for (std::size_t i = std::max<std::size_t>(from, 1); i < to && i < x.size(); ++i) {
        if (x[i - 1] <= 0.0f && x[i] > 0.0f) {
            if (intervals < 0) first = i;
            last = i;
            ++intervals;
        }
    }
    if (intervals <= 0) return 0.0;
    return kFs * static_cast<double>(intervals) / static_cast<double>(last - first);
}

// Crossings per second over a window. Coarse, but unambiguous during a fast
// dive where a period estimate would average across the sweep.
double crossing_rate(const std::vector<float>& x, std::size_t from, std::size_t to) {
    int crossings = 0;
    for (std::size_t i = from + 1; i < to && i < x.size(); ++i) {
        if ((x[i - 1] <= 0.0f) != (x[i] <= 0.0f)) ++crossings;
    }
    return 0.5 * crossings * kFs / static_cast<double>(to - from);
}

// Sample index of the last sample whose magnitude exceeds `floor_level`, i.e.
// how long the voice audibly lasts.
int audible_length(const std::vector<float>& x, double floor_level) {
    for (std::size_t i = x.size(); i > 0; --i) {
        if (std::fabs(static_cast<double>(x[i - 1])) > floor_level) {
            return static_cast<int>(i);
        }
    }
    return 0;
}

// Envelope followed at a few milliseconds, used to count the peaks in a burst
// train without being confused by the noise inside each burst.
std::vector<double> envelope_of(const std::vector<float>& x, double ms) {
    const double a = 1.0 - std::exp(-1.0 / (0.001 * ms * kFs));
    std::vector<double> e(x.size(), 0.0);
    double state = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        const double magnitude = std::fabs(static_cast<double>(x[i]));
        state = magnitude > state ? magnitude : state + a * (magnitude - state);
        e[i] = state;
    }
    return e;
}

// Counts bursts as upward crossings of `high`, re-arming only once the
// envelope has fallen back below `low`. Counting local maxima instead does not
// work: each burst is noise, so its own envelope has many local maxima, and a
// spike part-way down one burst's tail reads as a second burst.
int count_bursts(const std::vector<double>& e, double high, double low) {
    int bursts = 0;
    bool armed = true;
    for (double v : e) {
        if (armed && v > high) {
            ++bursts;
            armed = false;
        } else if (!armed && v < low) {
            armed = true;
        }
    }
    return bursts;
}

}  // namespace

// -- Snare -------------------------------------------------------------------

TEST_CASE("The snare's tone and wire layers are independently audible",
          "[signal][drum][snare]") {
    // Neither layer alone is a snare, so both have to be reachable. This also
    // catches a mix where one layer is swamped by the other at default levels.
    SnareVoice voice;
    voice.prepare(kFs);
    voice.set_tone_level(1.0);
    voice.set_noise_level(0.0);
    voice.set_snap_level(0.0);
    const auto tone_only = hit(voice, 1.0f, 24000);

    voice.set_tone_level(0.0);
    voice.set_noise_level(1.0);
    const auto wires_only = hit(voice, 1.0f, 24000);

    REQUIRE(rms(tone_only) > 1e-3);
    REQUIRE(rms(wires_only) > 1e-3);
    // The tone is pitched and the wires are not, so they must differ where it
    // counts: the tone carries far more energy at its fundamental.
    REQUIRE(tone_amplitude(tone_only, 180.0) > tone_amplitude(wires_only, 180.0) * 4.0);
}

TEST_CASE("The tone pair beats at the difference between its two modes",
          "[signal][drum][snare]") {
    // The beating is why there are two oscillators. With the ratio at exactly
    // 1 they collapse to one and the beat disappears, which is the control.
    SnareVoice voice;
    voice.prepare(kFs);
    voice.set_noise_level(0.0);
    voice.set_snap_level(0.0);
    voice.set_tone_level(1.0);
    voice.set_pitch_sweep_octaves(0.0);
    voice.set_tone_decay_ms(1500.0);
    voice.set_tune_hz(180.0);

    voice.set_tone_ratio(1.6);
    const auto pair = hit(voice, 1.0f, 48000);
    REQUIRE(tone_amplitude(pair, 180.0) > 1e-3);
    REQUIRE(tone_amplitude(pair, 288.0) > 1e-3);  // 180 * 1.6

    voice.set_tone_ratio(1.0);
    const auto unison = hit(voice, 1.0f, 48000);
    REQUIRE(tone_amplitude(unison, 288.0) < tone_amplitude(pair, 288.0) * 0.1);
}

TEST_CASE("Ring modulation removes the fundamentals",
          "[signal][drum][snare]") {
    // That is what ring modulation does -- it leaves only sum and difference
    // tones -- and it is the difference between a hollow snare and a taut one.
    SnareVoice voice;
    voice.prepare(kFs);
    voice.set_noise_level(0.0);
    voice.set_snap_level(0.0);
    voice.set_tone_level(1.0);
    voice.set_pitch_sweep_octaves(0.0);
    voice.set_tone_decay_ms(1000.0);
    voice.set_tune_hz(200.0);
    voice.set_tone_ratio(1.5);

    const auto plain = hit(voice, 1.0f, 48000);
    voice.set_ring(1.0);
    const auto ringed = hit(voice, 1.0f, 48000);

    REQUIRE(tone_amplitude(ringed, 200.0) < tone_amplitude(plain, 200.0) * 0.2);
    // 200 * 1.5 = 300; sum and difference are 500 and 100.
    REQUIRE(tone_amplitude(ringed, 100.0) > tone_amplitude(plain, 100.0) * 2.0);
}

TEST_CASE("The wire rattle modulates the noise at its stated rate",
          "[signal][drum][snare]") {
    SnareVoice voice;
    voice.prepare(kFs);
    voice.set_tone_level(0.0);
    voice.set_snap_level(0.0);
    voice.set_noise_level(1.0);
    voice.set_noise_decay_ms(2000.0);
    voice.set_rattle(0.0);
    const auto steady = hit(voice, 1.0f, 48000);

    voice.set_rattle(0.9);
    voice.set_rattle_hz(50.0);
    const auto buzzing = hit(voice, 1.0f, 48000);

    // Amplitude modulation of noise puts energy at the modulation rate into the
    // signal's envelope, which a steady noise layer does not have.
    const auto steady_env = envelope_of(steady, 3.0);
    const auto buzz_env = envelope_of(buzzing, 3.0);
    std::vector<float> steady_f(steady_env.begin(), steady_env.end());
    std::vector<float> buzz_f(buzz_env.begin(), buzz_env.end());
    REQUIRE(tone_amplitude(buzz_f, 50.0) > tone_amplitude(steady_f, 50.0) * 3.0);
}

TEST_CASE("The noise filter sweep changes the wires as they decay",
          "[signal][drum][snare]") {
    SnareVoice voice;
    voice.prepare(kFs);
    voice.set_tone_level(0.0);
    voice.set_snap_level(0.0);
    voice.set_noise_level(1.0);
    voice.set_noise_decay_ms(400.0);
    voice.set_noise_cutoff_hz(2000.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    voice.set_noise_sweep_octaves(0.0);
    const auto flat = hit(voice, 1.0f, 24000);
    voice.set_noise_sweep_octaves(2.5);
    const auto swept = hit(voice, 1.0f, 24000);

    // Sweeping up while the envelope is high makes the early part brighter;
    // the tail lands in the same place either way.
    REQUIRE(high_fraction(swept, 3000.0) > high_fraction(flat, 3000.0) * 1.2);
}

TEST_CASE("Velocity shifts the snare toward its wires",
          "[signal][drum][snare][velocity]") {
    // The physical claim: a harder strike puts proportionally more energy into
    // the snares than into the shell. Level-normalise so only balance remains.
    SnareVoice voice;
    voice.prepare(kFs);
    voice.set_tone_level(0.7);
    voice.set_noise_level(0.7);
    voice.set_pitch_sweep_octaves(0.0);

    auto soft = hit(voice, 0.2f, 24000);
    auto loud = hit(voice, 1.0f, 24000);
    const double soft_peak = peak(soft);
    const double loud_peak = peak(loud);
    REQUIRE(loud_peak > soft_peak);
    for (auto& v : soft) v = static_cast<float>(v / soft_peak);
    for (auto& v : loud) v = static_cast<float>(v / loud_peak);

    REQUIRE(high_fraction(loud, 1000.0) > high_fraction(soft, 1000.0) * 1.1);
}

TEST_CASE("The snare's tone and wire lo-fi chains are independent",
          "[signal][drum][snare]") {
    // One shared chain cannot age the pitched shell and wires independently.
    // Each direction has a negative control so a disconnected setter cannot
    // satisfy the test merely because the deterministic render repeated.
    SnareVoice voice;
    voice.prepare(kFs);
    voice.set_noise_level(0.0);
    voice.set_snap_level(0.0);
    voice.set_tone_level(1.0);
    auto reset_hit = [&voice] {
        voice.reset();
        return hit(voice, 1.0f, 12000);
    };
    const auto tone_before = reset_hit();

    voice.noise_lofi().set_bits(3.0);
    const auto tone_after = reset_hit();
    REQUIRE(tone_before == tone_after);

    voice.tone_lofi().set_bits(3.0);
    const auto tone_crushed = reset_hit();
    REQUIRE_FALSE(tone_before == tone_crushed);

    voice.set_tone_level(0.0);
    voice.set_noise_level(1.0);
    voice.tone_lofi().set_bits(24.0);
    voice.noise_lofi().set_bits(24.0);
    const auto wires_clean = reset_hit();

    voice.tone_lofi().set_bits(3.0);
    const auto wires_after_tone_change = reset_hit();
    REQUIRE(wires_clean == wires_after_tone_change);

    voice.noise_lofi().set_bits(3.0);
    const auto wires_crushed = reset_hit();
    REQUIRE_FALSE(wires_clean == wires_crushed);
}

// -- Hi-hat ------------------------------------------------------------------

TEST_CASE("The hat's oscillator bank is inharmonic",
          "[signal][drum][hat]") {
    // A harmonic cluster reads as a pitch; an inharmonic one reads as metal.
    // Every documented ratio must be present as its own partial, and the
    // spectrum must not be an integer series.
    HatVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(300.0);
    voice.set_metal(1.0);
    voice.set_decay_ms(2000.0);
    voice.set_cutoff_hz(200.0);
    voice.set_resonance(0.7);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, 48000);
    for (double ratio : HatVoice::partial_ratios) {
        REQUIRE(tone_amplitude(y, 300.0 * ratio) > 1e-4);
    }
    // 600 Hz is the second harmonic of the fundamental and is not one of the
    // bank's ratios, so it must be far weaker than the partials that are.
    REQUIRE(tone_amplitude(y, 600.0) <
            tone_amplitude(y, 300.0 * HatVoice::partial_ratios[1]) * 0.5);
}

TEST_CASE("Spread collapses the bank to unison", "[signal][drum][hat]") {
    // At spread 0 the voice is a square wave: the metallic quality lives
    // entirely in the spread, not in the oscillator type.
    HatVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(300.0);
    voice.set_metal(1.0);
    voice.set_decay_ms(2000.0);
    voice.set_cutoff_hz(200.0);
    voice.set_spread(0.0);

    const auto y = hit(voice, 1.0f, 48000);
    const double fundamental = tone_amplitude(y, 300.0);
    const double spread_partial = tone_amplitude(y, 300.0 * HatVoice::partial_ratios[3]);
    REQUIRE(fundamental > spread_partial * 20.0);
}

TEST_CASE("Decay spans closed hat to cymbal", "[signal][drum][hat]") {
    // One control, one voice. A kit does not need four hat implementations.
    auto length_for = [](double decay_ms) {
        HatVoice voice;
        voice.prepare(kFs);
        voice.set_decay_ms(decay_ms);
        const auto y = hit(voice, 1.0f, static_cast<int>(4.0 * kFs));
        return audible_length(y, 1e-4);
    };

    const int closed = length_for(40.0);
    const int open = length_for(400.0);
    const int crash = length_for(3000.0);
    REQUIRE(open > closed * 4);
    REQUIRE(crash > open * 4);
}

TEST_CASE("The metal control blends toward noise", "[signal][drum][hat]") {
    HatVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(300.0);
    voice.set_decay_ms(1000.0);
    voice.set_cutoff_hz(200.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    voice.set_metal(1.0);
    const auto metallic = hit(voice, 1.0f, 24000);
    voice.set_metal(0.0);
    const auto noisy = hit(voice, 1.0f, 24000);

    // The bank puts a strong discrete partial where noise puts nothing in
    // particular.
    REQUIRE(tone_amplitude(metallic, 300.0) > tone_amplitude(noisy, 300.0) * 5.0);
    REQUIRE(rms(noisy) > 1e-4);
}

TEST_CASE("The highpass is what leaves only the metallic band",
          "[signal][drum][hat]") {
    HatVoice voice;
    voice.prepare(kFs);
    voice.set_decay_ms(400.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    voice.set_cutoff_hz(400.0);
    const auto open_filter = hit(voice, 1.0f, 24000);
    voice.set_cutoff_hz(9000.0);
    const auto tight = hit(voice, 1.0f, 24000);

    REQUIRE(high_fraction(tight, 6000.0) > high_fraction(open_filter, 6000.0) * 1.5);
}

TEST_CASE("Successive hat hits are not identical", "[signal][drum][hat]") {
    // The oscillator bank keeps its phases across hits. A cymbal is not
    // re-struck from a known state, and resetting the cluster every time makes
    // every hit open with the same broadband click.
    HatVoice voice;
    voice.prepare(kFs);
    voice.set_decay_ms(300.0);

    const auto first = hit(voice, 1.0f, 12000);
    const auto second = hit(voice, 1.0f, 12000);
    REQUIRE_FALSE(first == second);

    // ...but a reset in between makes the voice reproducible again, which is
    // what tells "carries phase" apart from "is random".
    voice.reset();
    const auto third = hit(voice, 1.0f, 12000);
    voice.reset();
    const auto fourth = hit(voice, 1.0f, 12000);
    REQUIRE(third == fourth);
}

TEST_CASE("Physical drum retriggers preserve shared output history",
          "[signal][drum][retrigger][output]") {
    auto retrigger_ratio = [](auto& uninterrupted, auto& retriggered,
                              int lead_samples) {
        uninterrupted.prepare(kFs);
        retriggered.prepare(kFs);
        uninterrupted.note_on(1.0f);
        retriggered.note_on(1.0f);
        (void)render(uninterrupted, lead_samples);
        (void)render(retriggered, lead_samples);
        retriggered.note_on(1.0f);
        const auto control = render(uninterrupted, 32);
        const auto after_retrigger = render(retriggered, 32);
        const double control_energy = rms(control);
        INFO("control energy=" << control_energy
                                << " retrigger energy="
                                << rms(after_retrigger));
        REQUIRE(control_energy > 1.0e-6);
        return rms(after_retrigger) / control_energy;
    };

    HatVoice hat_control;
    HatVoice hat_retrigger;
    REQUIRE(retrigger_ratio(hat_control, hat_retrigger, 1000) > 0.1);

    SnareVoice snare_control;
    SnareVoice snare_retrigger;
    REQUIRE(retrigger_ratio(snare_control, snare_retrigger, 1000) > 0.1);
}

// -- Clap --------------------------------------------------------------------

TEST_CASE("The clap renders a burst train", "[signal][drum][clap]") {
    // The defining feature. A single shaped noise envelope would show one peak
    // here however it was shaped.
    ClapVoice voice;
    voice.prepare(kFs);
    voice.set_burst_count(4);
    voice.set_burst_spacing_ms(12.0);
    voice.set_burst_decay_ms(4.0);
    voice.set_tail_level(0.0);
    voice.set_gap_jitter(0.0);

    const auto y = hit(voice, 1.0f, 24000);
    const auto envelope = envelope_of(y, 2.0);
    REQUIRE(count_bursts(envelope, peak(y) * 0.4, peak(y) * 0.1) == 4);
}

TEST_CASE("Burst spacing moves the train", "[signal][drum][clap]") {
    auto train_length = [](double spacing_ms) {
        ClapVoice voice;
        voice.prepare(kFs);
        voice.set_burst_count(4);
        voice.set_burst_spacing_ms(spacing_ms);
        voice.set_burst_decay_ms(3.0);
        voice.set_tail_level(0.0);
        const auto y = hit(voice, 1.0f, 48000);
        return audible_length(y, peak(y) * 0.02);
    };

    const int tight = train_length(6.0);
    const int wide = train_length(24.0);
    REQUIRE(wide > tight * 2);
}

TEST_CASE("Each burst is quieter than the one before it",
          "[signal][drum][clap]") {
    // The hands lose energy each time they meet; a train at constant level
    // reads as a machine rather than a person.
    ClapVoice voice;
    voice.prepare(kFs);
    voice.set_burst_count(4);
    voice.set_burst_spacing_ms(12.0);
    voice.set_burst_decay_ms(4.0);
    voice.set_burst_falloff(0.7);
    voice.set_tail_level(0.0);
    // Jitter off: this measures level per burst by slicing at the nominal gap,
    // which only lines up when the gaps are nominal.
    voice.set_gap_jitter(0.0);

    const auto y = hit(voice, 1.0f, 24000);
    const int spacing = static_cast<int>(0.012 * kFs);
    double previous = peak(y, 0, static_cast<std::size_t>(spacing));
    for (int burst = 1; burst < 4; ++burst) {
        const double level = peak(y, static_cast<std::size_t>(burst * spacing),
                                  static_cast<std::size_t>((burst + 1) * spacing));
        REQUIRE(level < previous);
        previous = level;
    }
}

TEST_CASE("The tail runs from the first burst and outlasts the train",
          "[signal][drum][clap]") {
    // The tail is what fuses the train into one event, so it has to be present
    // under burst one rather than starting after the last.
    ClapVoice voice;
    voice.prepare(kFs);
    voice.set_burst_count(3);
    voice.set_burst_spacing_ms(10.0);
    voice.set_burst_decay_ms(3.0);
    voice.set_tail_decay_ms(400.0);

    voice.set_tail_level(0.0);
    const auto dry = hit(voice, 1.0f, 48000);
    voice.set_tail_level(0.6);
    const auto wet = hit(voice, 1.0f, 48000);

    // Between bursts, where the burst envelopes have decayed, only the tail is
    // present -- so that gap must be louder with the tail on.
    const std::size_t gap_from = static_cast<std::size_t>(0.006 * kFs);
    const std::size_t gap_to = static_cast<std::size_t>(0.009 * kFs);
    REQUIRE(rms(wet, gap_from, gap_to) > rms(dry, gap_from, gap_to) * 1.5);
    REQUIRE(audible_length(wet, 1e-4) > audible_length(dry, 1e-4) * 2);
}

TEST_CASE("Gap jitter breaks the train's regularity without breaking its count",
          "[signal][drum][clap]") {
    // Evenly spaced identical bursts comb against each other and give the train
    // a pitch it should not have. Jitter is the cheap fix, and it must not cost
    // a burst or make the voice non-reproducible.
    auto gaps_for = [](double jitter) {
        ClapVoice voice;
        voice.prepare(kFs);
        voice.set_burst_count(5);
        voice.set_burst_spacing_ms(12.0);
        voice.set_burst_decay_ms(3.0);
        voice.set_tail_level(0.0);
        voice.set_gap_jitter(jitter);
        // Equal-height bursts, so an onset is detected at the same point in
        // each burst's attack. With the default falloff the later, quieter
        // bursts cross a fixed threshold slightly later, which shows up as
        // timing spread that is really detection spread.
        voice.set_burst_falloff(1.0);

        const auto y = hit(voice, 1.0f, 24000);
        const auto envelope = envelope_of(y, 1.0);
        std::vector<int> onsets;
        bool armed = true;
        const double high = peak(y) * 0.4;
        const double low = peak(y) * 0.1;
        for (std::size_t i = 0; i < envelope.size(); ++i) {
            if (armed && envelope[i] > high) {
                onsets.push_back(static_cast<int>(i));
                armed = false;
            } else if (!armed && envelope[i] < low) {
                armed = true;
            }
        }
        return onsets;
    };

    const auto even = gaps_for(0.0);
    const auto jittered = gaps_for(0.8);
    REQUIRE(even.size() == 5);
    REQUIRE(jittered.size() == 5);

    // Even spacing means every gap is the same; jitter means they are not.
    auto spread = [](const std::vector<int>& onsets) {
        int smallest = 1 << 30, largest = 0;
        for (std::size_t i = 1; i < onsets.size(); ++i) {
            const int gap = onsets[i] - onsets[i - 1];
            smallest = std::min(smallest, gap);
            largest = std::max(largest, gap);
        }
        return largest - smallest;
    };
    // Each burst is noise, so an onset lands wherever that burst happens to be
    // loud first -- a handful of samples of detection spread even when the
    // timing is exact. The jittered case is an order of magnitude beyond it.
    REQUIRE(spread(even) < 20);
    REQUIRE(spread(jittered) > 40);

    // ...and it stays reproducible.
    REQUIRE(gaps_for(0.8) == jittered);
}

TEST_CASE("Alternating polarity flips every other burst",
          "[signal][drum][clap]") {
    // The other decorrelation route, and the one that costs no timing change:
    // opposite-signed bursts cannot reinforce each other's comb.
    auto first_two_signs = [](bool alternate) {
        ClapVoice voice;
        voice.prepare(kFs);
        voice.set_burst_count(2);
        voice.set_burst_spacing_ms(12.0);
        voice.set_burst_decay_ms(3.0);
        voice.set_tail_level(0.0);
        voice.set_gap_jitter(0.0);
        voice.set_alternate_polarity(alternate);
        return hit(voice, 1.0f, 24000);
    };

    const auto plain = first_two_signs(false);
    const auto flipped = first_two_signs(true);

    // The first burst is identical either way; the second is inverted.
    const std::size_t gap = static_cast<std::size_t>(0.012 * kFs);
    for (std::size_t i = 0; i < gap / 2; ++i) {
        REQUIRE(plain[i] == flipped[i]);
    }
    double correlation = 0.0;
    for (std::size_t i = gap; i < gap * 2; ++i) {
        correlation += static_cast<double>(plain[i]) * flipped[i];
    }
    REQUIRE(correlation < 0.0);
}

TEST_CASE("Clap stereo alternates bursts and preserves the mono sum",
          "[signal][drum][clap][stereo]") {
    auto configure = [](ClapVoice& voice) {
        voice.prepare(kFs);
        voice.set_burst_count(4);
        voice.set_burst_spacing_ms(12.0);
        voice.set_burst_decay_ms(3.0);
        voice.set_burst_falloff(1.0);
        voice.set_tail_level(0.0);
        voice.set_gap_jitter(0.0);
        voice.set_stereo_width(1.0);
    };

    ClapVoice mono_voice;
    configure(mono_voice);
    const auto mono = hit(mono_voice, 1.0f, 6000);

    ClapVoice stereo_voice;
    configure(stereo_voice);
    const auto stereo = stereo_hit(stereo_voice, 1.0f, 6000);

    REQUIRE(mono.size() == stereo.left.size());
    for (std::size_t i = 0; i < mono.size(); ++i) {
        REQUIRE(stereo.left[i] + stereo.right[i] ==
                Catch::Approx(mono[i]).margin(1.0e-6));
    }

    const std::size_t spacing = static_cast<std::size_t>(0.012 * kFs);
    auto channel_energy = [](const std::vector<float>& channel,
                             std::size_t burst) {
        double energy = 0.0;
        const std::size_t begin = burst * spacing;
        const std::size_t end = std::min(begin + spacing, channel.size());
        for (std::size_t i = begin; i < end; ++i)
            energy += static_cast<double>(channel[i]) * channel[i];
        return energy;
    };
    for (std::size_t burst = 0; burst < 4; ++burst) {
        const double left = channel_energy(stereo.left, burst);
        const double right = channel_energy(stereo.right, burst);
        INFO("burst " << burst);
        if ((burst & 1u) == 0)
            REQUIRE(left > right * 100.0);
        else
            REQUIRE(right > left * 100.0);
    }
}

TEST_CASE("Clap stereo keeps its room tail centred",
          "[signal][drum][clap][stereo]") {
    ClapVoice voice;
    voice.prepare(kFs);
    voice.set_burst_count(1);
    voice.set_burst_decay_ms(1.0);
    voice.set_tail_level(1.0);
    voice.set_tail_decay_ms(300.0);
    voice.set_stereo_width(1.0);

    const auto stereo = stereo_hit(voice, 1.0f, 12000);
    const std::size_t tail_begin = static_cast<std::size_t>(0.050 * kFs);
    double difference = 0.0;
    double level = 0.0;
    for (std::size_t i = tail_begin; i < stereo.left.size(); ++i) {
        difference += std::fabs(static_cast<double>(stereo.left[i]) -
                                stereo.right[i]);
        level += std::fabs(static_cast<double>(stereo.left[i])) +
                 std::fabs(static_cast<double>(stereo.right[i]));
    }
    REQUIRE(level > 1.0e-3);
    REQUIRE(difference < level * 1.0e-4);
}

TEST_CASE("Centered clap layers do not alter the burst side signal",
          "[signal][drum][clap][stereo]") {
    auto render_with_centre = [](double tail, double body) {
        ClapVoice voice;
        voice.prepare(kFs);
        voice.set_burst_count(1);
        voice.set_burst_decay_ms(20.0);
        voice.set_tail_level(tail);
        voice.set_tail_decay_ms(100.0);
        voice.set_body_level(body);
        voice.set_body_hz(271.0);
        voice.set_stereo_width(1.0);
        voice.output().set_oversampling(
            pulp::signal::drum::OutputOversampling::bypass);
        return stereo_hit(voice, 1.0f, 4000);
    };

    const auto burst_only = render_with_centre(0.0, 0.0);
    const auto layered = render_with_centre(1.0, 0.8);
    double side_error = 0.0;
    double side_level = 0.0;
    for (std::size_t i = 0; i < burst_only.left.size(); ++i) {
        const double expected =
            static_cast<double>(burst_only.left[i] - burst_only.right[i]);
        const double actual =
            static_cast<double>(layered.left[i] - layered.right[i]);
        side_error += std::fabs(actual - expected);
        side_level += std::fabs(expected);
    }
    INFO("side error=" << side_error << " side level=" << side_level);
    REQUIRE(side_level > 1.0e-3);
    REQUIRE(side_error < side_level * 1.0e-5);
}

TEST_CASE("Clap burst width carries the fully processed output",
          "[signal][drum][clap][stereo][output]") {
    auto configure = [](ClapVoice& voice) {
        voice.prepare(kFs);
        voice.set_burst_count(1);
        voice.set_burst_decay_ms(20.0);
        voice.set_tail_level(0.0);
        voice.set_body_level(0.0);
        voice.set_stereo_width(1.0);
        voice.output().set_drive(0.8);
        voice.output().set_fold(0.4);
        voice.output().lofi().set_bits(5.0);
        voice.output().lofi().set_hold_rate_hz(12000.0);
        voice.output().lofi().set_dead_zone(0.05);
    };

    ClapVoice mono_voice;
    configure(mono_voice);
    const auto mono = hit(mono_voice, 1.0f, 4000);

    ClapVoice stereo_voice;
    configure(stereo_voice);
    const auto stereo = stereo_hit(stereo_voice, 1.0f, 4000);

    const double processed_peak = peak(mono);
    for (std::size_t i = 0; i < mono.size(); ++i) {
        REQUIRE(stereo.left[i] - stereo.right[i] ==
                Catch::Approx(mono[i]).margin(1.0e-6));
        REQUIRE(stereo.left[i] + stereo.right[i] ==
                Catch::Approx(mono[i]).margin(1.0e-6));
        REQUIRE(std::fabs(stereo.left[i]) <= processed_peak + 1.0e-6);
        REQUIRE(std::fabs(stereo.right[i]) <= processed_peak + 1.0e-6);
    }
}

TEST_CASE("A clap renders identically for the same parameters",
          "[signal][drum][clap]") {
    ClapVoice voice;
    voice.prepare(kFs);
    const auto first = hit(voice, 0.8f, 24000);
    const auto second = hit(voice, 0.8f, 24000);
    REQUIRE(first == second);
}

TEST_CASE("A single-burst clap is still a valid voice",
          "[signal][drum][clap]") {
    // The degenerate schedule has to work: it is what a caller reaches for to
    // get a plain noise hit out of the same voice.
    ClapVoice voice;
    voice.prepare(kFs);
    voice.set_burst_count(1);
    voice.set_tail_level(0.0);
    voice.set_gap_jitter(0.0);
    const auto y = hit(voice, 1.0f, 24000);
    REQUIRE(peak(y) > 1e-3);
    const auto envelope = envelope_of(y, 2.0);
    REQUIRE(count_bursts(envelope, peak(y) * 0.4, peak(y) * 0.1) == 1);
    REQUIRE_FALSE(voice.is_active());
}

TEST_CASE("A snare with a silenced layer still finishes",
          "[signal][drum][snare]") {
    // A layer at zero level renders nothing, so its envelope never advances --
    // and a voice that counted it as active would stay active forever. Both
    // layers get the check because each has its own early return.
    for (int silenced = 0; silenced < 2; ++silenced) {
        SnareVoice voice;
        voice.prepare(kFs);
        voice.set_tone_level(silenced == 0 ? 0.0 : 1.0);
        voice.set_noise_level(silenced == 0 ? 1.0 : 0.0);
        voice.set_snap_level(0.0);
        voice.set_tone_decay_ms(50.0);
        voice.set_noise_decay_ms(50.0);

        voice.note_on(1.0f);
        REQUIRE(voice.is_active());
        render(voice, static_cast<int>(kFs));
        INFO("silenced layer index " << silenced);
        REQUIRE_FALSE(voice.is_active());
    }
}

TEST_CASE("The reusable shell block bypasses cleanly and preserves its ring",
          "[signal][drum][layers][shell]") {
    ShellLayer shell;
    shell.prepare(kFs);
    CHECK(shell.process(0.5) == 0.5);

    shell.set_level(1.0);
    shell.set_frequency_hz(1000.0);
    shell.set_resonance(12.0);
    double ring_energy = 0.0;
    (void)shell.process(1.0);
    CHECK(shell.is_ringing());
    for (int i = 0; i < 2048; ++i) {
        const double sample = shell.process(0.0);
        ring_energy += sample * sample;
    }
    CHECK(ring_energy > 1.0e-4);

    shell.reset();
    CHECK_FALSE(shell.is_ringing());
    for (int i = 0; i < 512; ++i) {
        CHECK(shell.process(0.0) == 0.0);
    }
}

TEST_CASE("The snare drains its shell resonance after the strike ends",
          "[signal][drum][snare][shell][lifecycle]") {
    SnareVoice voice;
    voice.prepare(kFs);
    voice.set_tone_level(0.0);
    voice.set_noise_level(0.0);
    voice.set_snap_level(1.0);
    voice.set_snap_decay_ms(0.5);
    voice.set_shell_level(1.0);
    voice.set_shell_resonance(30.0);
    voice.output().set_oversampling(
        pulp::signal::drum::OutputOversampling::bypass);

    voice.note_on(1.0f);
    (void)render(voice, 256);
    REQUIRE(voice.is_active());
    (void)render(voice, 48000);
    REQUIRE_FALSE(voice.is_active());
}

// -- Tom ---------------------------------------------------------------------

TEST_CASE("The tom's pitch dive finishes long before the note does",
          "[signal][drum][tom]") {
    // The defining property: two independent envelopes. Tying them together
    // would take the level down with the swoop, which is the sound this voice
    // exists to avoid.
    TomVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(110.0);
    voice.set_bend_octaves(2.0);
    voice.set_bend_ms(25.0);
    voice.set_decay_ms(1500.0);
    voice.set_noise_balance(0.0);
    voice.set_click_level(0.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, static_cast<int>(kFs));

    // The pitch has landed by ~150 ms.
    const double settled = period_frequency(y, 7200, 24000);
    REQUIRE(std::fabs(settled - 110.0) < 8.0);

    // The decoupling itself: with the SAME bend, changing only the decay must
    // change how long the note lasts and leave the pitch it arrives at alone.
    // One shared envelope could not do that -- shortening the note would drag
    // the swoop with it.
    voice.set_decay_ms(120.0);
    const auto brief = hit(voice, 1.0f, static_cast<int>(kFs));
    REQUIRE(audible_length(y, 1e-4) > audible_length(brief, 1e-4) * 3);
    REQUIRE(std::fabs(period_frequency(brief, 4800, 9600) - settled) < 8.0);
}

TEST_CASE("The tom's bend depth sets where the dive starts",
          "[signal][drum][tom]") {
    auto opening = [](double octaves) {
        TomVoice voice;
        voice.prepare(kFs);
        voice.set_tune_hz(100.0);
        voice.set_bend_octaves(octaves);
        voice.set_bend_ms(40.0);
        voice.set_noise_balance(0.0);
        voice.set_click_level(0.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto y = hit(voice, 1.0f, 24000);
        const auto latency =
            static_cast<std::size_t>(voice.output().latency_samples());
        return crossing_rate(y, latency, latency + 960);
    };

    REQUIRE(opening(3.0) > opening(1.0) * 2.0);
    REQUIRE(std::fabs(opening(0.0) - 100.0) < 20.0);
}

TEST_CASE("Velocity deepens the tom's dive rather than only raising its level",
          "[signal][drum][tom][velocity]") {
    // The clearest case of the velocity-to-timbre contract in the whole
    // percussion set: a harder strike deflects the head further, so the pitch
    // starts higher. A gain-only implementation would open at the same pitch.
    TomVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(100.0);
    voice.set_bend_octaves(0.5);
    voice.set_bend_ms(40.0);
    voice.set_noise_balance(0.0);
    voice.set_click_level(0.0);

    const auto soft = hit(voice, 0.15f, 24000);
    const auto loud = hit(voice, 1.0f, 24000);
    const auto latency =
        static_cast<std::size_t>(voice.output().latency_samples());
    REQUIRE(crossing_rate(loud, latency, latency + 960) >
            crossing_rate(soft, latency, latency + 960) * 1.4);

    // ...and both land on the same tuning, because velocity moves the dive and
    // not the pitch it arrives at.
    REQUIRE(std::fabs(period_frequency(loud, 9600, 24000) -
                      period_frequency(soft, 9600, 24000)) < 8.0);
}

TEST_CASE("Velocity does not change how long the tom rings",
          "[signal][drum][tom][velocity]") {
    // The negative control for the test above. Velocity is wired to the bend
    // and the level; wiring it to the decay as well would make a soft hit a
    // shorter note, which is not what a drum does.
    TomVoice voice;
    voice.prepare(kFs);
    voice.set_decay_ms(400.0);
    voice.set_noise_balance(0.0);
    voice.set_click_level(0.0);

    auto soft = hit(voice, 0.2f, static_cast<int>(kFs));
    auto loud = hit(voice, 1.0f, static_cast<int>(kFs));
    const double soft_length = audible_length(soft, peak(soft) * 1e-3);
    const double loud_length = audible_length(loud, peak(loud) * 1e-3);
    REQUIRE(std::fabs(soft_length - loud_length) < soft_length * 0.15);
}

TEST_CASE("The tom's noise balance reaches from oscillator to filtered noise",
          "[signal][drum][tom]") {
    TomVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(120.0);
    voice.set_bend_octaves(0.0);
    voice.set_decay_ms(600.0);
    voice.set_click_level(0.0);
    voice.set_noise_cutoff_hz(4000.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    voice.set_noise_balance(0.0);
    const auto tonal = hit(voice, 1.0f, 24000);
    voice.set_noise_balance(1.0);
    const auto noisy = hit(voice, 1.0f, 24000);

    REQUIRE(tone_amplitude(tonal, 120.0) > tone_amplitude(noisy, 120.0) * 4.0);
    REQUIRE(high_fraction(noisy, 1000.0) > high_fraction(tonal, 1000.0) * 2.0);
}

TEST_CASE("A resonant noise filter gives the noise a pitch",
          "[signal][drum][tom]") {
    // Why the noise path is four poles rather than two: at high resonance it
    // rings, which is what lets one voice cover a snare-like sound without a
    // second oscillator.
    auto ring_at = [](double resonance) {
        TomVoice voice;
        voice.prepare(kFs);
        voice.set_noise_balance(1.0);
        voice.set_click_level(0.0);
        voice.set_decay_ms(800.0);
        voice.set_noise_cutoff_hz(900.0);
        voice.set_noise_resonance(resonance);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto y = hit(voice, 1.0f, 24000);
        return tone_amplitude(y, 900.0) / (tone_amplitude(y, 300.0) + 1e-20);
    };

    REQUIRE(ring_at(0.95) > ring_at(0.0) * 2.0);
}

TEST_CASE("The tom stays finite at extreme settings", "[signal][drum][tom]") {
    TomVoice voice;
    voice.prepare(kFs);
    voice.set_tune_hz(1200.0);
    voice.set_bend_octaves(6.0);
    voice.set_bend_ms(0.5);
    voice.set_decay_ms(4000.0);
    voice.set_noise_balance(1.0);
    voice.set_noise_resonance(1.0);
    voice.set_click_level(2.0);
    voice.output().set_drive(1.0);
    voice.output().set_fold(1.0);

    for (int repeat = 0; repeat < 6; ++repeat) {
        const auto y = hit(voice, 1.0f, 12000);
        for (float v : y) REQUIRE(std::isfinite(v));
        REQUIRE(peak(y) < 20.0);
    }
}

TEST_CASE("The one tom topology ships all named clean voicings",
          "[signal][drum][tom][preset]") {
    REQUIRE(TomVoice::presets.size() == 8);

    std::vector<std::vector<float>> renders;
    for (const auto& preset : TomVoice::presets) {
        INFO("preset " << preset.name);
        REQUIRE_FALSE(preset.name.empty());
        TomVoice voice;
        voice.prepare(kFs);
        voice.apply_preset(preset.id);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        auto y = hit(voice, 1.0f, 24000);
        REQUIRE(peak(y) > 1e-4);
        for (float sample : y) REQUIRE(std::isfinite(sample));
        renders.push_back(std::move(y));
    }

    for (std::size_t i = 1; i < renders.size(); ++i) {
        REQUIRE_FALSE(renders[i] == renders[i - 1]);
    }
}

TEST_CASE("Low, mid, and high tom presets settle in pitch order",
          "[signal][drum][tom][preset]") {
    auto settled = [](TomVoice::Preset preset) {
        TomVoice voice;
        voice.prepare(kFs);
        voice.apply_preset(preset);
        voice.set_noise_balance(0.0);
        voice.set_click_level(0.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        const auto y = hit(voice, 1.0f, 24000);
        return period_frequency(y, 9600, 22000);
    };

    const double low = settled(TomVoice::Preset::low_tom);
    const double mid = settled(TomVoice::Preset::mid_tom);
    const double high = settled(TomVoice::Preset::hi_tom);
    REQUIRE(low < mid);
    REQUIRE(mid < high);
}

TEST_CASE("Tom preset application allocates nothing on the audio thread",
          "[signal][drum][tom][rt-safety]") {
    TomVoice voice;
    voice.prepare(kFs);
    std::vector<float> buffer(256, 0.0f);
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (const auto& preset : TomVoice::presets) {
            voice.apply_preset(preset.id);
            voice.note_on(0.8f);
            for (int block = 0; block < 4; ++block) {
                std::fill(buffer.begin(), buffer.end(), 0.0f);
                voice.process(buffer.data(), static_cast<int>(buffer.size()));
            }
            voice.reset();
        }
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

// -- Kit ---------------------------------------------------------------------

TEST_CASE("A kit publishes and enforces one output latency",
          "[signal][drum][kit][latency]") {
    SnareVoice snare;
    HatVoice hat;
    snare.output().set_oversampling(OutputOversampling::bypass);

    Kit kit;
    kit.add_voice(&snare, 38);
    kit.add_voice(&hat, 42);
    REQUIRE(kit.latency_samples() == 32);
    REQUIRE(snare.latency_samples() == kit.latency_samples());
    REQUIRE(hat.latency_samples() == kit.latency_samples());

    kit.set_output_oversampling(OutputOversampling::x4);
    REQUIRE(kit.latency_samples() == 48);
    REQUIRE(snare.latency_samples() == kit.latency_samples());
    REQUIRE(hat.latency_samples() == kit.latency_samples());

    // A caller retaining the concrete voice can still reach its output stage,
    // but the kit restores its own summed-path contract before the next hit.
    snare.output().set_oversampling(OutputOversampling::bypass);
    kit.trigger(38, 1.0f);
    REQUIRE(snare.latency_samples() == kit.latency_samples());
}

TEST_CASE("A legacy custom voice keeps the kit source-compatible and aligned",
          "[signal][drum][kit][latency][compatibility]") {
    LegacyVoice legacy;
    SnareVoice snare;
    Kit kit;
    kit.add_voice(&snare, 38);
    REQUIRE(kit.latency_samples() == 32);

    kit.add_voice(&legacy, 60);
    REQUIRE(legacy.latency_samples() == 0);
    REQUIRE(legacy.output_oversampling() == OutputOversampling::bypass);
    REQUIRE(kit.output_oversampling() == OutputOversampling::bypass);
    REQUIRE(kit.latency_samples() == 0);
    REQUIRE(snare.latency_samples() == 0);

    kit.trigger(60, 1.0f);
    std::vector<float> out(16, 0.0f);
    kit.process(out.data(), static_cast<int>(out.size()));
    REQUIRE(peak(out) > 0.0);
}

TEST_CASE("A voice registered after the kit was prepared still runs at the "
          "kit's rate",
          "[signal][drum][kit]") {
    // Registering in response to a preset load is reasonable, and a voice that
    // missed prepare() would render at its construction-time rate -- audible as
    // a wrongly-pitched drum rather than as an error.
    Kit kit;
    kit.prepare(kFs);

    HatVoice late;
    late.set_tune_hz(400.0);
    late.set_decay_ms(500.0);
    kit.add_voice(&late, 42);

    kit.trigger(42, 1.0f);
    std::vector<float> out(24000, 0.0f);
    kit.process(out.data(), static_cast<int>(out.size()));

    // Rendered at 48 kHz the hat's 500 ms decay is still audible half a second
    // in; at a default 44.1 kHz it would be measurably shorter, and at a wrong
    // rate the partials would land elsewhere.
    REQUIRE(peak(out) > 1e-3);
    HatVoice reference;
    reference.set_tune_hz(400.0);
    reference.set_decay_ms(500.0);
    reference.prepare(kFs);
    const auto expected = hit(reference, 1.0f, 24000);
    REQUIRE(rms(out, 12000, 24000) > rms(expected, 12000, 24000) * 0.5);
}

TEST_CASE("A kit sums its voices", "[signal][drum][kit]") {
    SnareVoice snare;
    HatVoice hat;
    Kit kit;
    kit.add_voice(&snare, 38);
    kit.add_voice(&hat, 42);
    kit.prepare(kFs);

    std::vector<float> out(12000, 0.0f);
    kit.trigger(38, 1.0f);
    kit.process(out.data(), static_cast<int>(out.size()));
    const double snare_only = rms(out);

    std::fill(out.begin(), out.end(), 0.0f);
    kit.trigger(38, 1.0f);
    kit.trigger(42, 1.0f);
    kit.process(out.data(), static_cast<int>(out.size()));
    REQUIRE(rms(out) > snare_only);
}

TEST_CASE("A kit adds into its buffer rather than overwriting it",
          "[signal][drum][kit]") {
    SnareVoice snare;
    Kit kit;
    kit.add_voice(&snare, 38);
    kit.prepare(kFs);

    std::vector<float> out(1024, 0.25f);
    kit.trigger(38, 1.0f);
    kit.process(out.data(), static_cast<int>(out.size()));
    // Whatever was in the buffer must still be reflected in the result.
    double mean = 0.0;
    for (float v : out) mean += static_cast<double>(v);
    mean /= static_cast<double>(out.size());
    REQUIRE(mean > 0.1);
}

TEST_CASE("A closed hat chokes an open one", "[signal][drum][kit]") {
    // The one place a kit must silence a voice it did not trigger. Physically
    // the two are the same pair of cymbals and cannot both ring.
    HatVoice open;
    open.set_decay_ms(3000.0);
    HatVoice closed;
    closed.set_decay_ms(40.0);

    Kit kit;
    kit.add_voice(&open, 46, /*choke_group=*/1);
    kit.add_voice(&closed, 42, /*choke_group=*/1);
    kit.prepare(kFs);

    kit.trigger(46, 1.0f);
    std::vector<float> ringing(4800, 0.0f);
    kit.process(ringing.data(), static_cast<int>(ringing.size()));
    REQUIRE(open.is_active());

    kit.trigger(42, 1.0f);
    std::vector<float> after(static_cast<std::size_t>(kFs), 0.0f);
    kit.process(after.data(), static_cast<int>(after.size()));
    REQUIRE_FALSE(open.is_active());
}

TEST_CASE("Voices outside a choke group are left alone",
          "[signal][drum][kit]") {
    // The negative control. Without it the choke test would also pass for an
    // implementation that silenced everything on every hit.
    HatVoice open;
    open.set_decay_ms(3000.0);
    SnareVoice snare;

    Kit kit;
    kit.add_voice(&open, 46, /*choke_group=*/1);
    kit.add_voice(&snare, 38, /*choke_group=*/0);
    kit.prepare(kFs);

    kit.trigger(46, 1.0f);
    std::vector<float> buffer(4800, 0.0f);
    kit.process(buffer.data(), static_cast<int>(buffer.size()));

    kit.trigger(38, 1.0f);
    kit.process(buffer.data(), static_cast<int>(buffer.size()));
    REQUIRE(open.is_active());
}

TEST_CASE("A choke group can be released without a hit",
          "[signal][drum][kit]") {
    // A pedal-up event chokes with nothing to trigger, which `trigger` cannot
    // express.
    HatVoice open;
    open.set_decay_ms(3000.0);
    Kit kit;
    kit.add_voice(&open, 46, /*choke_group=*/2);
    kit.prepare(kFs);

    kit.trigger(46, 1.0f);
    std::vector<float> buffer(4800, 0.0f);
    kit.process(buffer.data(), static_cast<int>(buffer.size()));
    REQUIRE(open.is_active());

    kit.choke_group(2);
    std::vector<float> after(static_cast<std::size_t>(kFs), 0.0f);
    kit.process(after.data(), static_cast<int>(after.size()));
    REQUIRE_FALSE(open.is_active());
}

TEST_CASE("Choking group zero is a no-op", "[signal][drum][kit]") {
    HatVoice hat;
    hat.set_decay_ms(3000.0);
    Kit kit;
    kit.add_voice(&hat, 42, /*choke_group=*/0);
    kit.prepare(kFs);

    kit.trigger(42, 1.0f);
    std::vector<float> buffer(4800, 0.0f);
    kit.process(buffer.data(), static_cast<int>(buffer.size()));
    kit.choke_group(0);
    kit.process(buffer.data(), static_cast<int>(buffer.size()));
    REQUIRE(hat.is_active());
}

TEST_CASE("An unknown note is ignored", "[signal][drum][kit]") {
    // A kit is driven from a whole MIDI stream, so it must tolerate notes it
    // does not own rather than requiring the caller to filter them.
    SnareVoice snare;
    Kit kit;
    kit.add_voice(&snare, 38);
    kit.prepare(kFs);

    kit.trigger(60, 1.0f);
    REQUIRE_FALSE(kit.is_active());
    std::vector<float> buffer(512, 0.0f);
    kit.process(buffer.data(), static_cast<int>(buffer.size()));
    REQUIRE(peak(buffer) == 0.0);
}

TEST_CASE("A kit reports itself idle once every voice has finished",
          "[signal][drum][kit]") {
    SnareVoice snare;
    snare.set_tone_decay_ms(40.0);
    snare.set_noise_decay_ms(40.0);
    snare.set_snap_decay_ms(2.0);
    Kit kit;
    kit.add_voice(&snare, 38);
    kit.prepare(kFs);

    kit.trigger(38, 1.0f);
    REQUIRE(kit.is_active());
    std::vector<float> buffer(static_cast<std::size_t>(kFs), 0.0f);
    kit.process(buffer.data(), static_cast<int>(buffer.size()));
    REQUIRE_FALSE(kit.is_active());
}

// -- Realtime contract -------------------------------------------------------

TEST_CASE("The voices and the kit allocate nothing on the audio thread",
          "[signal][drum][rt-safety]") {
    SnareVoice snare;
    HatVoice closed;
    HatVoice open;
    ClapVoice clap;

    snare.set_rattle(0.6);
    snare.set_shell_level(0.4);
    snare.set_noise_sweep_octaves(1.5);
    snare.set_noise_color(NoiseColor::pink);
    snare.noise_lofi().set_bits(8.0);
    snare.output().set_drive(0.4);
    closed.set_decay_ms(45.0);
    closed.set_grit(0.5);
    open.set_decay_ms(600.0);
    clap.set_burst_count(5);
    clap.output().set_drive(0.3);

    Kit kit;
    kit.add_voice(&snare, 38);
    kit.add_voice(&closed, 42, 1);
    kit.add_voice(&open, 46, 1);
    kit.add_voice(&clap, 39);
    kit.prepare(kFs);

    std::vector<float> buffer(256, 0.0f);
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int step = 0; step < 32; ++step) {
            kit.trigger(38 + (step % 3) * 2, 0.5f + 0.01f * static_cast<float>(step));
            for (int block = 0; block < 8; ++block) {
                std::fill(buffer.begin(), buffer.end(), 0.0f);
                kit.process(buffer.data(), static_cast<int>(buffer.size()));
            }
        }
        kit.choke_group(1);
        kit.process(buffer.data(), static_cast<int>(buffer.size()));
        kit.reset();
        allocations = probe.allocation_count();
    }

    REQUIRE(allocations == 0);
}

TEST_CASE("Stereo drum rendering and choking allocate nothing",
          "[signal][drum][stereo][rt-safety]") {
    ClapVoice clap;
    clap.prepare(kFs);
    clap.set_burst_count(ClapVoice::max_bursts);
    std::vector<float> left(1024, 0.0f);
    std::vector<float> right(1024, 0.0f);

    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        clap.note_on(1.0f);
        clap.process_stereo(left.data(), right.data(),
                            static_cast<int>(left.size()));
        clap.choke(4.0f);
        clap.process_stereo(left.data(), right.data(),
                            static_cast<int>(left.size()));
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("Every voice stays finite at extreme settings",
          "[signal][drum]") {
    SnareVoice snare;
    snare.prepare(kFs);
    snare.set_tone_level(2.0);
    snare.set_noise_level(2.0);
    snare.set_snap_level(2.0);
    snare.set_shell_level(2.0);
    snare.set_shell_resonance(30.0);
    snare.set_rattle(1.0);
    snare.set_fm_amount(8.0);
    snare.set_ring(1.0);
    snare.set_noise_sweep_octaves(4.0);
    snare.output().set_drive(1.0);
    snare.output().set_fold(1.0);

    HatVoice hat;
    hat.prepare(kFs);
    hat.set_grit(1.0);
    hat.set_resonance(12.0);
    hat.set_decay_ms(8000.0);
    hat.output().set_drive(1.0);
    hat.output().set_fold(1.0);

    ClapVoice clap;
    clap.prepare(kFs);
    clap.set_burst_count(ClapVoice::max_bursts);
    clap.set_burst_falloff(1.5);
    clap.set_body_level(2.0);
    clap.output().set_drive(1.0);
    clap.output().set_fold(1.0);

    Voice* voices[] = {&snare, &hat, &clap};
    for (Voice* voice : voices) {
        for (int repeat = 0; repeat < 6; ++repeat) {
            const auto y = hit(*voice, 1.0f, 12000);
            for (float v : y) REQUIRE(std::isfinite(v));
            REQUIRE(peak(y) < 20.0);
        }
    }
}

TEST_CASE("The engine registry has stable unique names and an explicit DX7 hold",
          "[signal][drum][registry]") {
    for (std::size_t i = 0; i < engine_registry.size(); ++i) {
        const auto& engine = engine_registry[i];
        REQUIRE_FALSE(engine.name.empty());
        REQUIRE_FALSE(engine.display_name.empty());
        REQUIRE_FALSE(engine.lineage.empty());
        REQUIRE(engine.velocity_changes_timbre == engine.available);
        REQUIRE(find_engine(engine.name) == &engine);
        for (std::size_t j = i + 1; j < engine_registry.size(); ++j) {
            REQUIRE(engine.name != engine_registry[j].name);
            REQUIRE(engine.id != engine_registry[j].id);
        }
    }

    REQUIRE(find_engine("not-a-drum") == nullptr);
    const auto* held = find_engine("dx7.msfa");
    REQUIRE(held != nullptr);
    REQUIRE_FALSE(held->available);
    REQUIRE_FALSE(held->velocity_changes_timbre);
    REQUIRE(held->provenance == EngineProvenance::license_hold);
    REQUIRE(create_engine(EngineId::dx7_msfa) == nullptr);
}

TEST_CASE("Every available engine changes timbre with velocity without hidden decay",
          "[signal][drum][registry][velocity]") {
    constexpr int render_samples = static_cast<int>(2.0 * kFs);
    for (const auto& engine : engine_registry) {
        if (!engine.available) continue;
        INFO("engine " << engine.name);

        auto soft_voice = create_engine(engine.id);
        auto loud_voice = create_engine(engine.id);
        REQUIRE(soft_voice != nullptr);
        REQUIRE(loud_voice != nullptr);
        soft_voice->prepare(kFs);
        loud_voice->prepare(kFs);
        const auto soft = hit(*soft_voice, 0.2f, render_samples);
        const auto loud = hit(*loud_voice, 1.0f, render_samples);
        const double soft_peak = peak(soft);
        const double loud_peak = peak(loud);
        REQUIRE(soft_peak > 1.0e-6);
        REQUIRE(loud_peak > 1.0e-6);

        // Peak normalization removes velocity gain. What remains must still
        // differ, proving pitch/brightness/noise balance reaches the render.
        REQUIRE(peak_normalized_difference(soft, loud) > 1.0e-4);

        if (!engine.velocity_may_change_decay) {
            const int soft_length =
                audible_length(soft, soft_peak * 1.0e-3);
            const int loud_length =
                audible_length(loud, loud_peak * 1.0e-3);
            const int tolerance =
                std::max(256, std::max(soft_length, loud_length) / 10);
            REQUIRE(std::abs(soft_length - loud_length) <= tolerance);
        }
    }
}

TEST_CASE("Every available registry entry constructs a finite audible voice",
          "[signal][drum][registry]") {
    for (const auto& engine : engine_registry) {
        auto voice = create_engine(engine.id);
        INFO(engine.name);
        if (!engine.available) {
            REQUIRE(voice == nullptr);
            continue;
        }

        REQUIRE(voice != nullptr);
        REQUIRE(voice->latency_samples() == 32);
        voice->prepare(kFs);
        const auto y = hit(*voice, 0.8f, 4096);
        for (float sample : y) REQUIRE(std::isfinite(sample));
        REQUIRE(peak(y) > 1e-6);
    }
}
