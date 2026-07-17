// PreparedPiano: one Processor plays a prepared string, and every preparation
// control is physically real. These tests RENDER the plugin through its MIDI +
// parameter surface and MEASURE the result: a buzz preparation adds broadband
// energy above 2 kHz that rises with strike velocity and decays faster than the
// tone; a mute preparation shortens exactly the modes whose antinode sits under
// the felt; a mass preparation shifts exactly those modes' frequencies; the
// un-prepared string is a clean stiff string; and a note lands at its pitch.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "support/modal_analysis.hpp"

#include "prepared_piano.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp;
using pulp::examples::PreparedPiano;
using namespace pulp::examples;
using namespace pulp::test::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kF0 = 82.4069;  // built-in string root, E2 = MIDI 40

struct NoteEvent { int note, velocity; double time_s; };

void bind(PreparedPiano& inst, state::StateStore& store, double fs, int block) {
    inst.define_parameters(store);
    inst.set_state_store(&store);
    format::PrepareContext ctx;
    ctx.sample_rate = fs;
    ctx.max_buffer_size = block;
    inst.prepare(ctx);
}

std::vector<float> render(PreparedPiano& inst, const std::vector<NoteEvent>& notes,
                          double fs, double secs, int block) {
    const int n = static_cast<int>(fs * secs);
    std::vector<float> mono(n, 0.0f);
    audio::Buffer<float> out(2, static_cast<std::size_t>(block));
    const float* in_ptrs[2] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, static_cast<std::size_t>(block));
    for (int i = 0; i < n; i += block) {
        const int c = std::min(block, n - i);
        for (int k = 0; k < c; ++k) { out.channel(0)[k] = 0.0f; out.channel(1)[k] = 0.0f; }
        float* chans[2] = {out.channel(0).data(), out.channel(1).data()};
        audio::BufferView<float> ov(chans, 2, static_cast<std::size_t>(c));
        midi::MidiBuffer mi, mo;
        for (const auto& ev : notes) {
            const int frame = static_cast<int>(ev.time_s * fs);
            if (frame >= i && frame < i + c) {
                auto m = midi::MidiEvent::note_on(0, static_cast<uint8_t>(ev.note),
                                                  static_cast<uint8_t>(ev.velocity));
                m.sample_offset = frame - i;
                mi.add(m);
            }
        }
        format::ProcessContext ctx;
        ctx.sample_rate = fs;
        ctx.num_samples = c;
        inst.process(ov, in, mi, mo, ctx);
        for (int k = 0; k < c; ++k) mono[i + k] = out.channel(0)[k];
    }
    return mono;
}

/// Energy in [lo, hi] Hz over [from_s, to_s], summed on a coarse exact-frequency
/// DFT grid. A band-limited measurement, unlike a leaky Butterworth: the buzz
/// partials above 2 kHz are ~4 orders of magnitude below the fundamental, so a
/// filter's finite stopband would let the fundamental swamp the reading.
double band_energy(const std::vector<float>& x, double fs, double lo, double hi,
                   double from_s, double to_s) {
    const int a = std::max(0, static_cast<int>(from_s * fs));
    const int b = std::min(static_cast<int>(x.size()), static_cast<int>(to_s * fs));
    double e = 0.0;
    for (double f = lo; f < hi; f += 30.0) {
        double re = 0.0, im = 0.0;
        for (int i = a; i < b; ++i) {
            const double ph = -2.0 * kPi * f * i / fs;
            re += x[i] * std::cos(ph);
            im += x[i] * std::sin(ph);
        }
        e += re * re + im * im;
    }
    return e;
}

bool all_finite(const std::vector<float>& x) {
    for (float v : x) if (!std::isfinite(v)) return false;
    return true;
}
double peak_abs(const std::vector<float>& x) {
    double m = 0.0; for (float v : x) m = std::max(m, static_cast<double>(std::fabs(v)));
    return m;
}

/// Frequency of stiff-string mode n on the built-in string (f_n = n f0 sqrt(1+B n^2)).
double base_mode_hz(int n) { return n * kF0 * std::sqrt(1.0 + 2.0e-4 * n * n); }

void set(state::StateStore& s, int id, float v) { s.set_value(id, v); }

}  // namespace

TEST_CASE("PreparedPiano un-prepared string is a clean stiff string", "[prepared-piano][control]") {
    const double fs = 48000.0;
    const int block = 256;
    state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
    // Strength 0 -> no preparation. The render must be a clean, finite stiff
    // string whose inharmonicity B is recoverable near the authored 2e-4.
    auto y = render(inst, {{40, 110, 0.0}}, fs, 3.0, block);
    REQUIRE(all_finite(y));
    REQUIRE(peak_abs(y) > 0.01);
    auto inh = measure_inharmonicity(y, fs, kF0);
    INFO(summarize(inh));
    REQUIRE(inh.ok);
    REQUIRE(inh.found_partials >= 6);
    // A steel string sits around 1e-4..5e-4; recovered B must land in that band.
    REQUIRE(inh.b_coefficient > 0.5e-4);
    REQUIRE(inh.b_coefficient < 5.0e-4);
}

TEST_CASE("PreparedPiano note renders at the note's pitch", "[prepared-piano][pitch]") {
    const double fs = 48000.0;
    const int block = 256;
    // The 48-mode stiff string has mode 2 louder than mode 1 at these pickup
    // positions, so a zero-crossing tracker cannot lock the fundamental; the
    // refined-partial inharmonicity fit recovers f0 exactly.
    for (int note : {40, 45, 52, 57, 64}) {
        state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
        auto y = render(inst, {{note, 100, 0.0}}, fs, 2.0, block);
        const double expected = kF0 * std::pow(2.0, (note - 40) / 12.0);
        auto inh = measure_inharmonicity(y, fs, expected);
        REQUIRE(inh.ok);
        const double cents = 1200.0 * std::log2(inh.f0_hz / expected);
        INFO("note " << note << " expected " << expected << " Hz, measured "
                     << inh.f0_hz << " Hz, err " << cents << " cents");
        REQUIRE(std::abs(cents) < 5.0);
    }
}

TEST_CASE("PreparedPiano buzz adds broadband energy above 2 kHz", "[prepared-piano][buzz]") {
    const double fs = 48000.0;
    const int block = 256;
    auto hf = [&](float strength) {
        state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
        set(store, kPrepType, 0.0f);          // Buzz
        set(store, kPrepPosition, 0.28f);
        set(store, kPrepStrength, strength);
        auto y = render(inst, {{40, 110, 0.0}}, fs, 3.0, block);
        REQUIRE(all_finite(y));
        return band_energy(y, fs, 2000.0, 12000.0, 0.1, 0.8);
    };
    const double clean = hf(0.0f);
    const double buzz = hf(80.0f);
    INFO(">2 kHz band energy: clean = " << clean << ", buzz = " << buzz
                                        << " (" << buzz / clean << "x)");
    REQUIRE(clean > 0.0);
    // The rattle transfers energy up the spectrum: materially more above 2 kHz
    // than the clean string. Measured ~2.5x; require a clear margin.
    REQUIRE(buzz > clean * 1.8);
}

TEST_CASE("PreparedPiano buzz rises with strike velocity", "[prepared-piano][buzz]") {
    const double fs = 48000.0;
    const int block = 256;
    // A loose bolt rattles more the harder the string is hit: the absolute
    // above-2 kHz energy of the buzz grows monotonically with velocity.
    auto hf = [&](int vel) {
        state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
        set(store, kPrepType, 0.0f);
        set(store, kPrepStrength, 80.0f);
        auto y = render(inst, {{40, vel, 0.0}}, fs, 3.0, block);
        REQUIRE(all_finite(y));
        return band_energy(y, fs, 2000.0, 12000.0, 0.1, 0.8);
    };
    const double soft = hf(50), mid = hf(90), hard = hf(127);
    INFO("buzz >2 kHz energy: vel50 = " << soft << ", vel90 = " << mid
                                        << ", vel127 = " << hard);
    REQUIRE(mid > soft);
    REQUIRE(hard > mid);
}

TEST_CASE("PreparedPiano buzz decays faster than the tone", "[prepared-piano][buzz]") {
    const double fs = 48000.0;
    const int block = 256;
    state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
    set(store, kPrepType, 0.0f);
    set(store, kPrepStrength, 80.0f);
    auto y = render(inst, {{40, 110, 0.0}}, fs, 3.0, block);
    const double fund_t60 = measure_mode(y, fs, kF0).t60_s;
    ModeAnalysisOptions o; o.search_low_hz = 2000.0; o.search_high_hz = 8000.0;
    auto an = analyze_modes(y, fs, o);
    double buzz_t60 = 0.0, buzz_hz = 0.0;
    for (const auto& m : an.modes)
        if (m.confidence > 0.3) { buzz_t60 = m.t60_s; buzz_hz = m.freq_hz; break; }
    INFO("fundamental T60 = " << fund_t60 << " s; buzz mode " << buzz_hz
                              << " Hz T60 = " << buzz_t60 << " s");
    REQUIRE(fund_t60 > 0.5);
    REQUIRE(buzz_t60 > 0.0);
    // The rattle both brightens and damps the note: its high content dies well
    // before the fundamental. Measured ~0.33 s vs ~3.85 s.
    REQUIRE(buzz_t60 < fund_t60 * 0.6);
}

TEST_CASE("PreparedPiano mute shortens exactly the modes under the felt",
          "[prepared-piano][mute]") {
    const double fs = 48000.0;
    const int block = 256;
    const double f2 = base_mode_hz(2);  // ~164.9 Hz, antinode at 0.25, node at 0.5
    auto t60_at = [&](float type, float pos, float strength) {
        state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
        set(store, kPrepType, type);
        set(store, kPrepPosition, pos);
        set(store, kPrepStrength, strength);
        auto y = render(inst, {{40, 110, 0.0}}, fs, 3.0, block);
        return measure_mode(y, fs, f2).t60_s;
    };
    const double clean = t60_at(0.0f, 0.28f, 0.0f);           // Buzz type, strength 0 = clean
    const double antinode = t60_at(1.0f, 0.25f, 70.0f);       // Mute at mode-2 antinode
    const double node = t60_at(1.0f, 0.50f, 70.0f);           // Mute at mode-2 node
    INFO("mode 2 (" << f2 << " Hz) T60: clean = " << clean << ", mute@antinode = "
                    << antinode << ", mute@node = " << node);
    REQUIRE(clean > 1.0);
    // Felt under the antinode shortens the mode hard; under the node it is inert.
    REQUIRE(antinode < clean * 0.4);
    REQUIRE(node > clean * 0.8);
}

TEST_CASE("PreparedPiano mass shifts exactly the modes under it",
          "[prepared-piano][mass]") {
    const double fs = 48000.0;
    const int block = 256;
    const double f2 = base_mode_hz(2);  // ~164.9 Hz
    auto freq_at = [&](float pos, double guess) {
        state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
        set(store, kPrepType, 2.0f);      // Mass
        set(store, kPrepPosition, pos);
        set(store, kPrepStrength, 80.0f);
        auto y = render(inst, {{40, 110, 0.0}}, fs, 3.0, block);
        return static_cast<double>(measure_mode(y, fs, guess).freq_hz);
    };
    const double antinode = freq_at(0.25f, f2 * 0.9);  // shifted down, search below
    const double node = freq_at(0.50f, f2);            // unshifted
    INFO("mode 2 base = " << f2 << " Hz; mass@antinode = " << antinode
                          << " Hz, mass@node = " << node << " Hz");
    // A point mass under the antinode lowers the mode by several Hz; under the
    // node it does not move it. Measured ~-12 Hz vs 0.
    REQUIRE(antinode < f2 - 3.0);
    REQUIRE(std::abs(node - f2) < 1.5);
}

TEST_CASE("PreparedPiano stays bounded under a hard preparation", "[prepared-piano][stability]") {
    const double fs = 48000.0;
    const int block = 256;
    state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
    set(store, kPrepType, 0.0f);
    set(store, kPrepStrength, 100.0f);   // bolt pressed hard against the string
    // Three max-velocity strikes into the same string over several seconds.
    auto y = render(inst, {{40, 127, 0.0}, {40, 127, 0.7}, {40, 127, 1.4}}, fs, 4.0, block);
    INFO("hard-strike peak = " << peak_abs(y));
    REQUIRE(all_finite(y));
    REQUIRE(peak_abs(y) < 20.0);
    REQUIRE(peak_abs(y) > 0.0);
}

TEST_CASE("PreparedPiano rapid repeats superpose rather than resetting",
          "[prepared-piano][polyphony]") {
    const double fs = 48000.0;
    const int block = 256;
    auto energy = [&](const std::vector<NoteEvent>& notes) {
        state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
        auto y = render(inst, notes, fs, 0.4, block);
        double s = 0.0; const int a = static_cast<int>(0.10 * fs), b = static_cast<int>(0.15 * fs);
        for (int i = a; i < b; ++i) s += static_cast<double>(y[i]) * y[i];
        return std::sqrt(s / (b - a));
    };
    const double single = energy({{40, 100, 0.0}});
    const double doubled = energy({{40, 100, 0.0}, {40, 100, 0.05}});
    INFO("single-strike RMS " << single << ", double-strike RMS " << doubled);
    REQUIRE(single > 0.0);
    // A note-on adds energy to a still-ringing string; it never resets it.
    REQUIRE(doubled > single * 1.2);
}

TEST_CASE("PreparedPiano process() allocates nothing on the audio thread",
          "[prepared-piano][rt-safety]") {
    const double fs = 48000.0;
    const int block = 256;
    state::StateStore store; PreparedPiano inst; bind(inst, store, fs, block);
    set(store, kPrepType, 0.0f);
    set(store, kPrepStrength, 80.0f);   // exercise the collision path too

    audio::Buffer<float> out(2, static_cast<std::size_t>(block));
    const float* in_ptrs[2] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, static_cast<std::size_t>(block));
    float* chans[2] = {out.channel(0).data(), out.channel(1).data()};

    midi::MidiBuffer with_note, empty, mo;
    with_note.add(midi::MidiEvent::note_on(0, 40, 110));

    format::ProcessContext ctx;
    ctx.sample_rate = fs;
    ctx.num_samples = block;

    std::size_t allocations = 0, allocated_bytes = 0;
    {
        pulp::test::RtAllocationProbe probe;
        for (int b = 0; b < 32; ++b) {
            audio::BufferView<float> ov(chans, 2, static_cast<std::size_t>(block));
            midi::MidiBuffer& mi = (b == 0) ? with_note : empty;
            inst.process(ov, in, mi, mo, ctx);
        }
        allocations = probe.allocation_count();
        allocated_bytes = probe.allocated_bytes();
    }
    INFO("allocations in process(): " << allocations << " (" << allocated_bytes << " bytes)");
    REQUIRE(allocations == 0);
}
