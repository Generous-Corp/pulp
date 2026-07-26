// Tests for the percussion voice lifecycle and the kick drum's three bodies.
//
// Two groups of assertions. The first covers the rules the base class exists
// to enforce -- additive rendering, a choke that fades rather than cuts,
// velocity reaching timbre and not just level -- because a voice that broke
// any of them would still produce a plausible drum sound on its own and only
// misbehave once it shared a bus or a choke group. The second covers what each
// body mode is actually claimed to do: an explicit sweep for the oscillator, a
// ring that depends on its exciter for the resonator, and an emergent pitch
// drop plus preserved state for the circuit.

#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"

#include <pulp/signal/drum/kick.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>
#include <vector>

namespace {

using pulp::signal::NoiseColor;
using pulp::signal::drum::KickBody;
using pulp::signal::drum::KickVoice;
using pulp::signal::drum::OutputOversampling;
using pulp::signal::drum::OutputStage;
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

std::vector<float> hit(KickVoice& voice, float velocity, int num_samples) {
    voice.note_on(velocity);
    return render(voice, num_samples);
}

double peak(const std::vector<float>& x) {
    double m = 0.0;
    for (float v : x) m = std::max(m, std::fabs(static_cast<double>(v)));
    return m;
}

double rms(const std::vector<float>& x, std::size_t from = 0, std::size_t to = 0) {
    if (to == 0) to = x.size();
    double sum = 0.0;
    for (std::size_t i = from; i < to; ++i) sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / static_cast<double>(to - from));
}

// Zero-crossing rate over a window, in crossings per second. Cheap and
// unambiguous during a fast sweep, but its resolution is one crossing per
// window -- for a 50 Hz body over 10 ms that is the whole measurement -- so it
// is only used where the frequencies being compared differ by a lot.
double crossing_rate(const std::vector<float>& x, std::size_t from, std::size_t to) {
    int crossings = 0;
    for (std::size_t i = from + 1; i < to && i < x.size(); ++i) {
        if ((x[i - 1] <= 0.0f) != (x[i] <= 0.0f)) ++crossings;
    }
    return 0.5 * crossings * kFs / static_cast<double>(to - from);
}

// Frequency from the mean interval between upward zero crossings inside a
// window. Resolves a drift of a fraction of a hertz, which a crossing count
// over a short window cannot -- and the circuit body's pitch sigh is a couple
// of hertz, so it needs this rather than the estimator above.
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

// Amplitude of one frequency, by Hann-windowed Goertzel. Used where a claim is
// about a specific harmonic rather than about the balance of a whole band.
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

// Spectral centroid via a coarse band split: energy above `split_hz` as a
// fraction of the total. A one-pole pair either side is enough to compare two
// renders of the same voice, which is all these tests do with it.
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

// A Voice is non-copyable by design -- it owns filter state that must not be
// duplicated silently -- so the fixture configures in place rather than
// returning by value.
void init(KickVoice& voice, KickBody body) {
    voice.set_body(body);
    voice.prepare(kFs);
}

std::vector<float> render_output_sine(OutputStage& output, double frequency,
                                      double amplitude, int num_samples) {
    std::vector<float> rendered(static_cast<std::size_t>(num_samples), 0.0f);
    for (int i = 0; i < num_samples; ++i) {
        const double phase = 2.0 * kPi * frequency * static_cast<double>(i) / kFs;
        rendered[static_cast<std::size_t>(i)] =
            output.process(static_cast<float>(amplitude * std::sin(phase)));
    }
    return rendered;
}

}  // namespace

// -- Lifecycle rules ---------------------------------------------------------

TEST_CASE("A voice adds into the buffer rather than overwriting it",
          "[signal][drum][voice]") {
    // Two voices summed into one buffer must both be audible. Assignment would
    // silence whichever rendered first, which is the failure this catches.
    KickVoice a;
    init(a, KickBody::oscillator);
    KickVoice b;
    init(b, KickBody::oscillator);
    b.set_tune_hz(200.0);

    std::vector<float> both(8192, 0.0f);
    a.note_on(1.0f);
    b.note_on(1.0f);
    a.process(both.data(), static_cast<int>(both.size()));
    const double after_first = rms(both);
    b.process(both.data(), static_cast<int>(both.size()));
    const double after_second = rms(both);

    REQUIRE(after_first > 0.0);
    REQUIRE(after_second > after_first);
}

TEST_CASE("An inactive voice contributes nothing", "[signal][drum][voice]") {
    KickVoice voice;
    init(voice, KickBody::oscillator);
    REQUIRE_FALSE(voice.is_active());

    std::vector<float> buffer(512, 0.5f);
    voice.process(buffer.data(), static_cast<int>(buffer.size()));
    for (float v : buffer) REQUIRE(v == 0.5f);
}

TEST_CASE("A voice reports itself finished once its layers have decayed",
          "[signal][drum][voice]") {
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_body_decay_ms(80.0);
    voice.set_click_decay_ms(2.0);
    voice.note_on(1.0f);
    REQUIRE(voice.is_active());

    render(voice, static_cast<int>(kFs));
    REQUIRE_FALSE(voice.is_active());
}

TEST_CASE("A voice whose layers are silent still finishes",
          "[signal][drum][voice]") {
    // A layer at zero level must not report itself active. It renders nothing,
    // so it never advances the envelope that would eventually clear the flag,
    // and a voice that asked it would stay active forever -- an idle kit that
    // never leaves the CPU and a choke group that never frees its voice.
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_click_level(0.0);
    voice.set_noise_level(0.0);
    voice.set_sub_level(0.0);
    voice.set_body_decay_ms(60.0);
    voice.note_on(1.0f);
    REQUIRE(voice.is_active());

    render(voice, static_cast<int>(kFs));
    REQUIRE_FALSE(voice.is_active());
}

TEST_CASE("Choking fades rather than cutting", "[signal][drum][voice]") {
    // An instant cut is a step edge, which is broadband and audible as a click
    // exactly where a choke group fires. The fade is what stops that.
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_body_decay_ms(2000.0);
    voice.note_on(1.0f);
    render(voice, 4800);

    voice.choke(4.0f);
    const auto tail = render(voice, 4800);

    double largest_step = 0.0;
    for (std::size_t i = 1; i < tail.size(); ++i) {
        largest_step = std::max(largest_step,
                                std::fabs(static_cast<double>(tail[i] - tail[i - 1])));
    }
    // A 4 ms fade at 48 kHz spreads the shutdown over 192 samples, so no single
    // step may approach the signal's own amplitude.
    REQUIRE(largest_step < 0.2);
    REQUIRE(rms(tail, 1000, tail.size()) < 1e-4);
    REQUIRE_FALSE(voice.is_active());
}

TEST_CASE("Voice state distinguishes idle ringing and choking lifecycle",
          "[signal][drum][voice][state]") {
    using pulp::signal::drum::VoiceState;

    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_body_decay_ms(2000.0);
    REQUIRE(voice.state() == VoiceState::idle);

    voice.note_on(1.0f);
    REQUIRE(voice.state() == VoiceState::ringing);
    voice.choke(4.0f);
    REQUIRE(voice.state() == VoiceState::choking);

    render(voice, 4800);
    REQUIRE(voice.state() == VoiceState::idle);

    voice.note_on(1.0f);
    REQUIRE(voice.state() == VoiceState::ringing);
    voice.reset();
    REQUIRE(voice.state() == VoiceState::idle);
}

TEST_CASE("Choking an idle voice is a no-op", "[signal][drum][voice]") {
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.choke();
    REQUIRE_FALSE(voice.is_active());
    std::vector<float> buffer(256, 0.0f);
    voice.process(buffer.data(), static_cast<int>(buffer.size()));
    REQUIRE(peak(buffer) == 0.0);
}

TEST_CASE("A choke longer than one scratch block still completes",
          "[signal][drum][voice]") {
    // The fade is applied through a fixed-size scratch buffer, so a fade
    // spanning many blocks and a render block larger than the scratch are both
    // paths that have to work.
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_body_decay_ms(2000.0);
    voice.note_on(1.0f);
    render(voice, 4800);

    voice.choke(50.0f);
    std::vector<float> out(48000, 0.0f);
    voice.process(out.data(), static_cast<int>(out.size()));  // one 48000-sample block
    REQUIRE_FALSE(voice.is_active());
    REQUIRE(rms(out, 8000, out.size()) < 1e-4);
}

TEST_CASE("Reset silences a ringing voice immediately",
          "[signal][drum][voice]") {
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_body_decay_ms(2000.0);
    voice.note_on(1.0f);
    render(voice, 2400);
    REQUIRE(voice.is_active());

    voice.reset();
    REQUIRE_FALSE(voice.is_active());
    const auto after = render(voice, 2400);
    REQUIRE(peak(after) == 0.0);
}

TEST_CASE("The same parameters and velocity render identical samples",
          "[signal][drum][voice]") {
    // Determinism is what makes a bounce reproducible and a golden-file test
    // possible. The noise layers reseed on every hit for exactly this reason.
    KickVoice a;
    init(a, KickBody::oscillator);
    KickVoice b;
    init(b, KickBody::oscillator);
    a.set_noise_level(0.5);
    b.set_noise_level(0.5);
    a.set_click_level(0.8);
    b.set_click_level(0.8);

    const auto first = hit(a, 0.7f, 8192);
    const auto second = hit(b, 0.7f, 8192);
    REQUIRE(first == second);

    // ...and the same voice hit twice must repeat too.
    const auto third = hit(a, 0.7f, 8192);
    REQUIRE(third == first);
}

TEST_CASE("Velocity changes timbre, not only level",
          "[signal][drum][voice][velocity]") {
    // The contract that separates a drum from a sample player triggered at
    // different gains. Normalise both renders to the same peak and the
    // difference that remains must be spectral.
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_click_level(0.6);

    auto soft = hit(voice, 0.25f, 8192);
    auto loud = hit(voice, 1.0f, 8192);

    const double soft_peak = peak(soft);
    const double loud_peak = peak(loud);
    REQUIRE(soft_peak > 0.0);
    REQUIRE(loud_peak > soft_peak);  // it must still be louder

    for (auto& v : soft) v = static_cast<float>(v / soft_peak);
    for (auto& v : loud) v = static_cast<float>(v / loud_peak);

    // Level-normalised, the hard hit must still carry more high-frequency
    // energy. A gain-only velocity implementation would make these equal.
    REQUIRE(high_fraction(loud, 800.0) > high_fraction(soft, 800.0) * 1.15);
}

TEST_CASE("A level-only velocity response produces a level-only difference",
          "[signal][drum][voice][velocity]") {
    // The negative control for the test above: with the timbral terms zeroed,
    // the two renders must differ by a scalar. If this failed, the previous
    // test could be passing for some reason other than the velocity wiring.
    KickVoice voice;
    init(voice, KickBody::oscillator);
    VelocityResponse response;
    response.level_db = 12.0f;
    response.bend_octaves = 0.0f;
    response.brightness_octaves = 0.0f;
    voice.set_velocity_response(response);
    voice.set_click_level(0.6);

    auto soft = hit(voice, 0.3f, 8192);
    auto loud = hit(voice, 1.0f, 8192);
    const double ratio = peak(loud) / peak(soft);
    for (auto& v : soft) v = static_cast<float>(v * ratio);

    double largest_difference = 0.0;
    for (std::size_t i = 0; i < soft.size(); ++i) {
        largest_difference = std::max(largest_difference,
                                      std::fabs(static_cast<double>(soft[i] - loud[i])));
    }
    REQUIRE(largest_difference < 1e-5);
}

TEST_CASE("The velocity response curve reaches its stated endpoints",
          "[signal][drum][velocity]") {
    VelocityResponse response;
    response.level_db = 20.0f;
    response.bend_octaves = 1.0f;
    response.brightness_octaves = 2.0f;
    response.noise_balance = 0.5f;

    REQUIRE(response.gain(1.0f) == 1.0f);
    REQUIRE(std::fabs(response.gain(0.0f) - 0.1f) < 1e-6);  // -20 dB
    REQUIRE(response.bend(0.0f) == 0.0f);
    REQUIRE(response.bend(1.0f) == 1.0f);
    REQUIRE(response.brightness_scale(0.0f) == 1.0f);
    REQUIRE(std::fabs(response.brightness_scale(1.0f) - 4.0f) < 1e-5);
    REQUIRE(std::fabs(response.noise_shift(1.0f) - 0.5f) < 1e-6);

    // Out-of-range velocities are clamped rather than extrapolated.
    REQUIRE(response.gain(2.0f) == response.gain(1.0f));
    REQUIRE(response.gain(-1.0f) == response.gain(0.0f));
}

// -- Oscillator body ---------------------------------------------------------

TEST_CASE("The oscillator body sweeps downward to its tuning",
          "[signal][drum][kick]") {
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_tune_hz(50.0);
    voice.set_pitch_sweep_octaves(3.0);
    voice.set_pitch_sweep_ms(40.0);
    voice.set_body_decay_ms(800.0);
    voice.set_click_level(0.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, static_cast<int>(kFs));

    const double early = crossing_rate(y, 0, 480);
    const double late = crossing_rate(y, 19200, 24000);
    REQUIRE(early > late * 3.0);
    // Three octaves above 50 Hz is 400 Hz; the sweep starts near there.
    REQUIRE(early > 250.0);
    // ...and settles at the tuning.
    REQUIRE(std::fabs(late - 50.0) < 6.0);
}

TEST_CASE("Sweep depth changes how high the body starts",
          "[signal][drum][kick]") {
    auto render_sweep = [](double octaves) {
        KickVoice voice;
        init(voice, KickBody::oscillator);
        voice.set_tune_hz(50.0);
        voice.set_pitch_sweep_octaves(octaves);
        voice.set_body_decay_ms(600.0);
        voice.set_click_level(0.0);
        voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});
        return hit(voice, 1.0f, 9600);
    };

    const auto deep = render_sweep(4.0);
    const auto shallow = render_sweep(2.0);
    OutputStage default_output;
    default_output.prepare(kFs);
    const auto output_latency =
        static_cast<std::size_t>(default_output.latency_samples());
    REQUIRE(crossing_rate(deep, output_latency, output_latency + 480) >
            crossing_rate(shallow, output_latency, output_latency + 480) * 1.8);

    // With no sweep the body sits at its tuning from the first cycle. Measured
    // by period, because 50 Hz has half a cycle in a 10 ms window.
    const auto flat = render_sweep(0.0);
    REQUIRE(std::fabs(period_frequency(flat, output_latency, 9600) - 50.0) < 2.0);
}

TEST_CASE("Body decay sets how long the kick rings", "[signal][drum][kick]") {
    auto tail_energy = [](double decay_ms) {
        KickVoice voice;
    init(voice, KickBody::oscillator);
        voice.set_body_decay_ms(decay_ms);
        voice.set_click_level(0.0);
        const auto y = hit(voice, 1.0f, static_cast<int>(kFs));
        return rms(y, 14400, 19200);  // 300-400 ms in
    };

    REQUIRE(tail_energy(600.0) > tail_energy(100.0) * 5.0);
}

TEST_CASE("Phase modulation adds partials without lengthening the hit",
          "[signal][drum][kick]") {
    KickVoice clean;
    init(clean, KickBody::oscillator);
    clean.set_click_level(0.0);
    clean.set_body_decay_ms(300.0);

    KickVoice modulated;
    init(modulated, KickBody::oscillator);
    modulated.set_click_level(0.0);
    modulated.set_body_decay_ms(300.0);
    modulated.set_fm_amount(3.0);
    modulated.set_fm_ratio(3.5);

    const auto a = hit(clean, 1.0f, 24000);
    const auto b = hit(modulated, 1.0f, 24000);
    REQUIRE(high_fraction(b, 500.0) > high_fraction(a, 500.0) * 1.5);
}

TEST_CASE("The sub layer adds energy an octave below the body",
          "[signal][drum][kick]") {
    KickVoice voice;
    init(voice, KickBody::oscillator);
    voice.set_tune_hz(80.0);
    voice.set_pitch_sweep_octaves(0.0);
    voice.set_click_level(0.0);
    voice.set_body_decay_ms(500.0);
    const auto without = hit(voice, 1.0f, 24000);

    voice.set_sub_level(0.8);
    const auto with = hit(voice, 1.0f, 24000);

    // 40 Hz is below the body and above nothing else in the voice.
    REQUIRE(high_fraction(with, 60.0) < high_fraction(without, 60.0));
    REQUIRE(rms(with) > rms(without));
}

// -- Resonant body -----------------------------------------------------------

TEST_CASE("The resonant body rings at its tuning", "[signal][drum][kick]") {
    KickVoice voice;
    init(voice, KickBody::resonant);
    voice.set_tune_hz(70.0);
    voice.set_body_decay_ms(600.0);
    voice.set_click_level(0.0);
    voice.set_velocity_response(VelocityResponse{0.0f, 0.0f, 0.0f, 0.0f});

    const auto y = hit(voice, 1.0f, static_cast<int>(kFs));
    REQUIRE(std::fabs(crossing_rate(y, 4800, 14400) - 70.0) < 5.0);
}

TEST_CASE("The resonant body's decay follows its T60", "[signal][drum][kick]") {
    KickVoice voice;
    init(voice, KickBody::resonant);
    voice.set_tune_hz(70.0);
    voice.set_body_decay_ms(300.0);
    voice.set_click_level(0.0);
    voice.output().set_level(1.0);

    const auto y = hit(voice, 1.0f, static_cast<int>(kFs));
    const double early = rms(y, 480, 1440);            // ~10-30 ms
    const double at_t60 = rms(y, 14400, 15360);        // ~300-320 ms
    const double decay_db = 20.0 * std::log10(at_t60 / (early + 1e-30));
    REQUIRE(decay_db < -50.0);
    REQUIRE(decay_db > -75.0);
}

TEST_CASE("A second strike interferes with the ring rather than replacing it",
          "[signal][drum][kick]") {
    // The resonator is not reset on trigger, so a hit during the tail adds to
    // what is already there. That is what stops a fast pattern sounding like
    // the same sample fired repeatedly.
    KickVoice voice;
    init(voice, KickBody::resonant);
    voice.set_tune_hz(70.0);
    voice.set_body_decay_ms(1200.0);
    voice.set_click_level(0.0);

    const auto single = hit(voice, 1.0f, 24000);

    voice.reset();
    voice.note_on(1.0f);
    std::vector<float> doubled(24000, 0.0f);
    voice.process(doubled.data(), 6000);
    voice.note_on(1.0f);
    voice.process(doubled.data() + 6000, 18000);

    bool differs = false;
    for (std::size_t i = 6000; i < doubled.size(); ++i) {
        if (std::fabs(static_cast<double>(doubled[i] - single[i])) > 1e-4) {
            differs = true;
            break;
        }
    }
    REQUIRE(differs);
}

// -- Circuit body ------------------------------------------------------------

TEST_CASE("The circuit body produces a downward pitch drop with no pitch envelope",
          "[signal][drum][kick][circuit]") {
    // The drop emerges from the network retuning itself as its own ring
    // amplitude falls. Nothing in this voice writes a frequency over time.
    KickVoice voice;
    init(voice, KickBody::circuit);
    voice.set_circuit_feedback(0.95);
    voice.set_click_level(0.0);
    voice.set_noise_level(0.0);

    const auto y = hit(voice, 1.0f, static_cast<int>(kFs));
    REQUIRE(peak(y) > 0.01);

    // The drift is a couple of hertz on a ~50 Hz note -- the real circuit's
    // sigh is subtle, not an octave -- so it is measured by period.
    const double early = period_frequency(y, 480, 9600);
    const double late = period_frequency(y, 24000, 47000);
    REQUIRE(late > 20.0);
    REQUIRE(late < 150.0);
    REQUIRE(early > late * 1.01);
}

TEST_CASE("Disabling the sigh removes the pitch drop",
          "[signal][drum][kick][circuit]") {
    // The negative control for the test above. With the leakage path cut the
    // network cannot retune itself, so the drop must disappear -- which proves
    // the drop came from that mechanism and not from something incidental.
    auto drop_ratio = [](bool sigh) {
        KickVoice voice;
        init(voice, KickBody::circuit);
        voice.set_circuit_feedback(0.95);
        voice.set_circuit_sigh(sigh);
        voice.set_click_level(0.0);
        const auto y = hit(voice, 1.0f, static_cast<int>(kFs));
        return period_frequency(y, 480, 9600) / (period_frequency(y, 24000, 47000) + 1e-9);
    };

    const double with_sigh = drop_ratio(true);
    const double without_sigh = drop_ratio(false);
    REQUIRE(with_sigh > without_sigh);
    REQUIRE(with_sigh > 1.01);
    REQUIRE(std::fabs(without_sigh - 1.0) < 0.002);
}

TEST_CASE("The attack shunt lifts the opening pitch",
          "[signal][drum][kick][circuit]") {
    auto opening_rate = [](double attack_ms) {
        KickVoice voice;
        init(voice, KickBody::circuit);
        voice.set_circuit_feedback(0.9);
        voice.set_circuit_attack_ms(attack_ms);
        voice.set_click_level(0.0);
        const auto y = hit(voice, 1.0f, 24000);
        const auto latency = static_cast<std::size_t>(voice.output().latency_samples());
        // Measured over the shunt's own 20 ms window. Averaging over any longer
        // span mixes the lifted opening with the settled note and understates
        // the effect -- over 50 ms the same lift reads as a few percent.
        return crossing_rate(y, latency, latency + 960);
    };

    // Shorting the collector resistor drops the network's shunt resistance by
    // about a factor of seven, and its centre frequency goes as one over the
    // square root of that -- so the opening should sit over an octave up.
    REQUIRE(opening_rate(20.0) > opening_rate(0.0) * 2.0);
}

TEST_CASE("Circuit feedback lengthens the ring rather than raising its level",
          "[signal][drum][kick][circuit]") {
    auto measure = [](double feedback) {
        KickVoice voice;
    init(voice, KickBody::circuit);
        voice.set_circuit_feedback(feedback);
        voice.set_click_level(0.0);
        const auto y = hit(voice, 1.0f, static_cast<int>(kFs));
        return std::pair<double, double>{peak(y), rms(y, 19200, 24000)};
    };

    const auto low = measure(0.6);
    const auto high = measure(1.0);
    REQUIRE(high.second > low.second * 2.0);
    // The peak is set by the trigger, so it must not move much with feedback.
    REQUIRE(high.first < low.first * 2.5);
}

TEST_CASE("The circuit body preserves its ring across a retrigger",
          "[signal][drum][kick][circuit]") {
    // The anti-machine-gun rule: a trigger re-fires the pulse and the envelopes
    // but leaves the network's stored energy alone, so consecutive hits differ.
    KickVoice voice;
    init(voice, KickBody::circuit);
    voice.set_circuit_feedback(0.95);
    voice.set_click_level(0.0);

    voice.note_on(1.0f);
    std::vector<float> first(12000, 0.0f);
    voice.process(first.data(), static_cast<int>(first.size()));

    voice.note_on(1.0f);
    std::vector<float> second(12000, 0.0f);
    voice.process(second.data(), static_cast<int>(second.size()));

    bool differs = false;
    for (std::size_t i = 0; i < first.size(); ++i) {
        if (std::fabs(static_cast<double>(first[i] - second[i])) > 1e-4) {
            differs = true;
            break;
        }
    }
    REQUIRE(differs);

    // ...whereas an explicit reset between hits must reproduce the first hit
    // exactly, which is what tells the two behaviours apart.
    voice.reset();
    voice.note_on(1.0f);
    std::vector<float> third(12000, 0.0f);
    voice.process(third.data(), static_cast<int>(third.size()));
    REQUIRE(third == first);
}

TEST_CASE("The circuit anti-machine-gun policy is selectable",
          "[signal][drum][kick][circuit][life]") {
    using pulp::signal::drum::HitLifeMode;

    KickVoice voice;
    init(voice, KickBody::circuit);
    voice.set_circuit_feedback(0.95);
    voice.set_click_level(0.0);
    voice.set_noise_level(0.0);
    voice.set_circuit_hit_life(HitLifeMode::fixed_seed);

    const auto first = hit(voice, 1.0f, 12000);
    const auto second = hit(voice, 1.0f, 12000);
    REQUIRE(first == second);

    voice.set_circuit_hit_life(HitLifeMode::preserved_state);
    REQUIRE(voice.circuit_hit_life() == HitLifeMode::preserved_state);
    const auto living = hit(voice, 1.0f, 12000);
    REQUIRE_FALSE(living == first);
}

// -- Output stage and realtime contract --------------------------------------

TEST_CASE("The drum output stage defaults to the exact house x2 latency",
          "[signal][drum][output][latency]") {
    OutputStage output;
    output.prepare(kFs);

    REQUIRE(output.oversampling() == OutputOversampling::x2);
    REQUIRE(output.latency_samples() == 32);

    std::vector<float> impulse(160, 0.0f);
    for (std::size_t i = 0; i < impulse.size(); ++i) {
        impulse[i] = output.process(i == 0 ? 1.0f : 0.0f);
    }
    const auto peak_at = static_cast<std::size_t>(std::distance(
        impulse.begin(), std::max_element(
                             impulse.begin(), impulse.end(),
                             [](float a, float b) { return std::fabs(a) < std::fabs(b); })));
    REQUIRE(peak_at == static_cast<std::size_t>(output.latency_samples()));
    REQUIRE_FALSE(output.has_tail());

    output.set_oversampling(OutputOversampling::x4);
    REQUIRE(output.latency_samples() == 48);
    output.set_oversampling(OutputOversampling::bypass);
    REQUIRE(output.latency_samples() == 0);
}

TEST_CASE("Bypassed clean drum output is sample-exact and deterministic",
          "[signal][drum][output][bypass]") {
    OutputStage output;
    output.prepare(kFs);
    output.set_oversampling(OutputOversampling::bypass);

    std::vector<float> input(4096);
    for (std::size_t i = 0; i < input.size(); ++i) {
        input[i] = 0.35f * std::sin(static_cast<float>(2.0 * kPi * 0.071 *
                                                       static_cast<double>(i)));
    }

    std::vector<float> first(input.size());
    std::vector<float> second(input.size());
    for (std::size_t i = 0; i < input.size(); ++i) first[i] = output.process(input[i]);
    output.reset();
    for (std::size_t i = 0; i < input.size(); ++i) second[i] = output.process(input[i]);

    REQUIRE(first == input);
    REQUIRE(second == first);
    REQUIRE_FALSE(output.has_tail());
}

TEST_CASE("The post-output AHD has exact attack, hold, and decay regions",
          "[signal][drum][output][ahd]") {
    OutputStage output;
    output.prepare(kFs);
    output.set_oversampling(OutputOversampling::bypass);
    output.set_ahd_ms(10.0, 5.0, 20.0);
    output.trigger();

    std::vector<float> y(1800);
    for (auto& sample : y) sample = output.process(1.0f);

    REQUIRE(y[0] == 0.0f);
    REQUIRE(y[240] > 0.49f);
    REQUIRE(y[240] < 0.51f);
    REQUIRE(y[479] > 0.99f);
    REQUIRE(y[480] == 1.0f);
    REQUIRE(y[719] == 1.0f);
    REQUIRE(y[900] < 0.1f);
    REQUIRE(y[1680] == 0.0f);
}

TEST_CASE("The AHD scales saturation output instead of its input",
          "[signal][drum][output][ahd]") {
    OutputStage saturated;
    saturated.prepare(kFs);
    saturated.set_oversampling(OutputOversampling::bypass);
    saturated.set_drive(1.0);
    const double saturated_sample = saturated.process(0.25f);

    OutputStage enveloped;
    enveloped.prepare(kFs);
    enveloped.set_oversampling(OutputOversampling::bypass);
    enveloped.set_drive(1.0);
    enveloped.set_ahd_ms(10.0, 0.0, 100.0);
    enveloped.trigger();

    double halfway = 0.0;
    for (int sample = 0; sample <= 240; ++sample) {
        halfway = enveloped.process(0.25f);
    }

    // At 5 ms the linear attack gain is exactly one half. A pre-saturation
    // envelope would instead change tanh's input and would not equal this.
    REQUIRE(std::fabs(halfway - saturated_sample * 0.5) < 1e-6);
}

TEST_CASE("Clean x2 drum output is deterministic and transparent in band",
          "[signal][drum][output][oversampling]") {
    OutputStage first;
    OutputStage second;
    first.prepare(kFs);
    second.prepare(kFs);

    constexpr int frames = 8192;
    std::vector<float> input(frames, 0.0f);
    std::vector<float> a(frames, 0.0f);
    std::vector<float> b(frames, 0.0f);
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kFs;
        input[static_cast<std::size_t>(i)] = static_cast<float>(
            0.3 * std::sin(2.0 * kPi * 997.0 * t) +
            0.15 * std::sin(2.0 * kPi * 5003.0 * t));
        a[static_cast<std::size_t>(i)] =
            first.process(input[static_cast<std::size_t>(i)]);
        b[static_cast<std::size_t>(i)] =
            second.process(input[static_cast<std::size_t>(i)]);
    }

    REQUIRE(a == b);
    const auto latency = static_cast<std::size_t>(first.latency_samples());
    double max_error = 0.0;
    for (std::size_t i = 512; i < a.size(); ++i) {
        max_error = std::max(
            max_error,
            std::fabs(static_cast<double>(a[i] - input[i - latency])));
    }
    INFO("clean x2 maximum in-band error=" << max_error);
    REQUIRE(max_error < 1e-3);
}

TEST_CASE("Reapplying the active drum quality does not reset its FIR history",
          "[signal][drum][output][latency]") {
    OutputStage reference;
    OutputStage reapplied;
    reference.prepare(kFs);
    reapplied.prepare(kFs);

    std::vector<float> expected(160, 0.0f);
    std::vector<float> actual(160, 0.0f);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const float input = i == 0 ? 1.0f : 0.0f;
        expected[i] = reference.process(input);
        if (i == 16) reapplied.set_oversampling(OutputOversampling::x2);
        actual[i] = reapplied.process(input);
    }

    REQUIRE(actual == expected);
}

TEST_CASE("House oversampling rejects the shared drum drive alias",
          "[signal][drum][output][aliasing]") {
    // 10 kHz driven through tanh creates a 30 kHz third harmonic. At the host
    // rate that folds to a discrete 18 kHz alias; at x2 it exists above the
    // host Nyquist and the house decimator removes it. A clean x2 render is the
    // negative control proving that the 18 kHz detector has a lower floor than
    // the asserted driven result.
    constexpr double fundamental_hz = 10000.0;
    constexpr double alias_hz = 18000.0;
    constexpr int frames = 32768;
    constexpr std::size_t settled = 2048;

    OutputStage host_rate;
    host_rate.prepare(kFs);
    host_rate.set_oversampling(OutputOversampling::bypass);
    host_rate.set_drive(1.0);
    auto aliased = render_output_sine(host_rate, fundamental_hz, 0.35, frames);

    OutputStage house;
    house.prepare(kFs);
    house.set_drive(1.0);
    auto filtered = render_output_sine(house, fundamental_hz, 0.35, frames);

    OutputStage clean_control;
    clean_control.prepare(kFs);
    auto clean = render_output_sine(clean_control, fundamental_hz, 0.35, frames);

    aliased.erase(aliased.begin(), aliased.begin() + static_cast<std::ptrdiff_t>(settled));
    filtered.erase(filtered.begin(), filtered.begin() + static_cast<std::ptrdiff_t>(settled));
    clean.erase(clean.begin(), clean.begin() + static_cast<std::ptrdiff_t>(settled));

    const double host_alias = tone_amplitude(aliased, alias_hz);
    const double house_alias = tone_amplitude(filtered, alias_hz);
    const double detector_floor = tone_amplitude(clean, alias_hz);
    INFO("host alias=" << host_alias << " house alias=" << house_alias
                       << " detector floor=" << detector_floor);
    REQUIRE(host_alias > 1e-3);
    REQUIRE(house_alias < host_alias * 0.05);
    REQUIRE(detector_floor < house_alias * 0.25);
}

TEST_CASE("Drive adds harmonics and bounds the output",
          "[signal][drum][kick]") {
    KickVoice clean;
    init(clean, KickBody::oscillator);
    clean.set_click_level(0.0);
    clean.set_pitch_sweep_octaves(0.0);
    clean.set_tune_hz(60.0);

    KickVoice driven;
    init(driven, KickBody::oscillator);
    driven.set_click_level(0.0);
    driven.set_pitch_sweep_octaves(0.0);
    driven.set_tune_hz(60.0);
    driven.output().set_drive(0.9);

    const auto a = hit(clean, 1.0f, 24000);
    const auto b = hit(driven, 1.0f, 24000);

    // Saturation is odd-order, so the claim is specifically about the third
    // harmonic relative to the fundamental. A whole-band centroid would not
    // show it: driving also compresses the decay, which keeps the fundamental
    // present for longer and moves a band ratio the other way.
    const double clean_third = tone_amplitude(a, 180.0) / (tone_amplitude(a, 60.0) + 1e-20);
    const double driven_third = tone_amplitude(b, 180.0) / (tone_amplitude(b, 60.0) + 1e-20);
    REQUIRE(driven_third > clean_third * 3.0);
    REQUIRE(peak(b) <= 1.0);
}

TEST_CASE("Every body mode stays finite and bounded at extreme settings",
          "[signal][drum][kick]") {
    for (auto body : {KickBody::oscillator, KickBody::resonant, KickBody::circuit}) {
        KickVoice voice;
        init(voice, body);
        voice.set_tune_hz(400.0);
        voice.set_body_decay_ms(4000.0);
        voice.set_pitch_sweep_octaves(6.0);
        voice.set_click_level(2.0);
        voice.set_noise_level(2.0);
        voice.set_sub_level(2.0);
        voice.set_fm_amount(8.0);
        voice.set_circuit_feedback(1.2);
        voice.output().set_drive(1.0);
        voice.output().set_fold(1.0);
        voice.output().set_level(2.0);

        for (int repeat = 0; repeat < 8; ++repeat) {
            const auto y = hit(voice, 1.0f, 12000);
            for (float v : y) REQUIRE(std::isfinite(v));
            REQUIRE(peak(y) < 20.0);
        }
    }
}

TEST_CASE("Rendering a kick allocates nothing on the audio thread",
          "[signal][drum][kick][rt-safety]") {
    KickVoice oscillator;
    init(oscillator, KickBody::oscillator);
    KickVoice resonant;
    init(resonant, KickBody::resonant);
    KickVoice circuit;
    init(circuit, KickBody::circuit);
    for (KickVoice* v : {&oscillator, &resonant, &circuit}) {
        v->set_click_level(0.4);
        v->set_noise_level(0.3);
        v->set_sub_level(0.2);
        v->set_noise_color(NoiseColor::pink);
        v->output().set_drive(0.5);
        v->output().lofi().set_bits(10.0);
        v->output().lofi().set_hold_rate_hz(24000.0);
        v->output().set_ahd_ms(2.0, 5.0, 120.0);
    }

    std::vector<float> buffer(512, 0.0f);
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (KickVoice* v : {&oscillator, &resonant, &circuit}) {
            for (int repeat = 0; repeat < 4; ++repeat) {
                v->note_on(0.6f + 0.1f * static_cast<float>(repeat));
                for (int block = 0; block < 16; ++block) {
                    std::fill(buffer.begin(), buffer.end(), 0.0f);
                    v->process(buffer.data(), static_cast<int>(buffer.size()));
                }
                v->choke(3.0f);
                v->process(buffer.data(), static_cast<int>(buffer.size()));
                v->reset();
            }
        }
        allocations = probe.allocation_count();
    }

    REQUIRE(allocations == 0);
}
