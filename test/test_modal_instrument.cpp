// ModalInstrument: one spec-driven Processor plays any modal instrument, and
// the audible controls are physically real. These tests RENDER the plugin
// through its MIDI + parameter surface and MEASURE the result — a note lands
// at the pitch it names, two notes ring independently, and strike position
// silences the modes whose nodes it lands on.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "harness/rt_allocation_probe.hpp"
#include "support/modal_analysis.hpp"

#include "modal_instrument.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

using namespace pulp;
using pulp::examples::ModalInstrument;
using namespace pulp::test::audio;

namespace {

constexpr double kPi = 3.14159265358979323846;

struct NoteEvent {
    int note;
    int velocity;
    double time_s;
};

/// Bind an instrument to a fresh store and prepare it.
void bind(ModalInstrument& inst, state::StateStore& store, double fs, int block) {
    inst.define_parameters(store);
    inst.set_state_store(&store);
    format::PrepareContext ctx;
    ctx.sample_rate = fs;
    ctx.max_buffer_size = block;
    inst.prepare(ctx);
}

/// Render `secs` of mono output, delivering each note at its sample-accurate
/// offset. Blocks are `block` samples; a note whose time falls inside a block
/// is stamped at its offset within that block.
std::vector<float> render(ModalInstrument& inst, const std::vector<NoteEvent>& notes,
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

double rms(const std::vector<float>& x, double from_s, double to_s, double fs) {
    const int a = std::max(0, static_cast<int>(from_s * fs));
    const int b = std::min(static_cast<int>(x.size()), static_cast<int>(to_s * fs));
    if (b <= a) return 0.0;
    double sum = 0.0;
    for (int i = a; i < b; ++i) sum += static_cast<double>(x[i]) * x[i];
    return std::sqrt(sum / (b - a));
}

/// An ideal string built in code (matches examples/modal-specs/ideal-string-a2.json),
/// carrying analytic strike/pickup maps phi_m(x) = sin(m*pi*x) so position
/// actually weights each mode. A2 = 110 Hz = MIDI 45.
signal::ModalSpec ideal_string_spec() {
    signal::ModalSpec s;
    s.name = "ideal-string-a2";
    s.modes = {{110.0f, 3.2f, 1.0f}, {220.0f, 2.4f, 0.5f},
               {330.0f, 1.7f, 0.3333f}, {440.0f, 1.2f, 0.25f}};
    s.excitation.contact_s = 0.0004;
    s.excitation.velocity = 1.0;
    constexpr int G = 9;
    s.strike_map.grid_points = G;
    s.pickup_map.grid_points = G;
    for (int m = 1; m <= 4; ++m)
        for (int g = 0; g < G; ++g) {
            const float x = static_cast<float>(g) / (G - 1);
            const float w = static_cast<float>(std::sin(m * kPi * x));
            s.strike_map.weights.push_back(w);
            s.pickup_map.weights.push_back(w);
        }
    return s;
}

double note_hz(int note, int root, double root_hz) {
    return root_hz * std::pow(2.0, (note - root) / 12.0);
}

}  // namespace

TEST_CASE("ModalInstrument renders a MIDI note at the note's pitch", "[modal][pitch]") {
    const double fs = 48000.0;
    const int block = 256;
    // The marimba default is A3 = 220 Hz = MIDI 57. A note-on at N must render
    // at 220 * 2^((N-57)/12). Measured with the calibrated cycle tracker.
    for (int note : {45, 57, 60, 69, 72}) {
        state::StateStore store;
        ModalInstrument inst;
        bind(inst, store, fs, block);

        auto y = render(inst, {{note, 100, 0.0}}, fs, 2.0, block);
        CycleTrack t = track_cycles(y, fs);
        INFO(summarize(t));
        REQUIRE(t.ok);
        // A clean single-fundamental instrument: the tracker should be locked.
        REQUIRE(t.monocomponent_confidence > 0.8);

        const double expected = note_hz(note, 57, 220.0);
        const double measured = t.mean_f0(0.1, 0.5);
        const double cents = 1200.0 * std::log2(measured / expected);
        INFO("note " << note << " expected " << expected << " Hz, measured "
                     << measured << " Hz, err " << cents << " cents");
        REQUIRE(std::abs(cents) < 5.0);
    }
}

TEST_CASE("ModalInstrument plays two notes without either cutting the other",
          "[modal][polyphony]") {
    const double fs = 48000.0;
    const int block = 256;
    const int note_a = 45;   // A2, 110 Hz
    const int note_b = 68;   // G#4, 415.3 Hz — no partial collision with A2
    const double fa = note_hz(note_a, 57, 220.0);
    const double fb = note_hz(note_b, 57, 220.0);

    // A alone.
    double a_alone_amp;
    {
        state::StateStore store; ModalInstrument inst; bind(inst, store, fs, block);
        auto y = render(inst, {{note_a, 100, 0.0}}, fs, 2.0, block);
        a_alone_amp = measure_mode(y, fs, fa).amplitude;
        REQUIRE(a_alone_amp > 0.0);
    }

    // A struck at t=0, B struck 0.2 s later. Both must be present, and A's
    // fundamental must not have been cut when B started (independent voices).
    state::StateStore store; ModalInstrument inst; bind(inst, store, fs, block);
    auto y = render(inst, {{note_a, 100, 0.0}, {note_b, 100, 0.2}}, fs, 2.0, block);

    auto ma = measure_mode(y, fs, fa);
    auto mb = measure_mode(y, fs, fb);
    INFO("A@" << fa << " amp " << ma.amplitude << " (alone " << a_alone_amp
              << "), B@" << fb << " amp " << mb.amplitude);
    REQUIRE(ma.amplitude > 0.0);
    REQUIRE(mb.amplitude > 0.0);
    // Both partials clearly present.
    REQUIRE(mb.amplitude > 0.1 * ma.amplitude);
    // The second note did not steal or reset the first voice.
    REQUIRE(ma.amplitude > 0.8 * a_alone_amp);
}

TEST_CASE("ModalInstrument strike position silences the modes whose nodes it hits",
          "[modal][timbre]") {
    const double fs = 48000.0;
    const int block = 256;
    // Ideal string, note A2 = 110 Hz. Mode 2 (220 Hz) has a node at x = 0.5 and
    // an antinode at x = 0.25. Pickup is pinned at 0.25 (an antinode of mode 2)
    // in both renders so only the strike side changes.
    auto measure_mode2 = [&](double strike_pos) {
        state::StateStore store;
        ModalInstrument inst;
        inst.set_spec(ideal_string_spec());
        inst.set_root_note(45);
        inst.define_parameters(store);
        inst.set_state_store(&store);
        store.set_value(examples::kModalStrikePosition, static_cast<float>(strike_pos));
        store.set_value(examples::kModalPickupPosition, 0.25f);
        format::PrepareContext ctx; ctx.sample_rate = fs; ctx.max_buffer_size = block;
        inst.prepare(ctx);
        auto y = render(inst, {{45, 110, 0.0}}, fs, 2.0, block);
        return measure_mode(y, fs, 220.0).amplitude;
    };

    const double antinode = measure_mode2(0.25);  // mode 2 fully excited
    const double node = measure_mode2(0.5);        // mode 2 at its node
    INFO("mode 2 amplitude: antinode(0.25) = " << antinode
                                               << ", node(0.5) = " << node);
    REQUIRE(antinode > 0.0);
    // Striking the node kills the mode: at least a 20x drop. (Measured ~190x.)
    REQUIRE(node < antinode * 0.05);
}

TEST_CASE("ModalInstrument rapid repeats superpose rather than resetting",
          "[modal][polyphony]") {
    const double fs = 48000.0;
    const int block = 256;

    // One strike; ringing energy shortly after 0.1 s.
    double single;
    {
        state::StateStore store; ModalInstrument inst; bind(inst, store, fs, block);
        auto y = render(inst, {{57, 100, 0.0}}, fs, 0.4, block);
        single = rms(y, 0.10, 0.15, fs);
        REQUIRE(single > 0.0);
    }

    // Same note struck twice in quick succession. If a note-on reset the body,
    // the first ring would be gone and the energy at 0.1 s would be no larger
    // than a single strike. Additive strikes make it larger.
    state::StateStore store; ModalInstrument inst; bind(inst, store, fs, block);
    auto y = render(inst, {{57, 100, 0.0}, {57, 100, 0.05}}, fs, 0.4, block);
    const double doubled = rms(y, 0.10, 0.15, fs);
    INFO("single-strike RMS " << single << ", double-strike RMS " << doubled);
    REQUIRE(doubled > single * 1.2);
}

TEST_CASE("ModalInstrument process() allocates nothing on the audio thread",
          "[modal][rt-safety]") {
    const double fs = 48000.0;
    const int block = 256;
    state::StateStore store;
    ModalInstrument inst;
    bind(inst, store, fs, block);

    audio::Buffer<float> out(2, static_cast<std::size_t>(block));
    const float* in_ptrs[2] = {nullptr, nullptr};
    audio::BufferView<const float> in(in_ptrs, 0, static_cast<std::size_t>(block));
    float* chans[2] = {out.channel(0).data(), out.channel(1).data()};

    // Build the MIDI buffers OUTSIDE the probed region; a note-on on the first
    // block exercises the allocation-prone paths (retune, set_modes, strike).
    midi::MidiBuffer with_note, empty, mo;
    with_note.add(midi::MidiEvent::note_on(0, 57, 100));

    format::ProcessContext ctx;
    ctx.sample_rate = fs;
    ctx.num_samples = block;

    // Read the counters into locals INSIDE the probe scope but before any
    // Catch2 macro runs: INFO/REQUIRE build a stringstream that itself
    // allocates, and the probe is a global new hook that would count it.
    std::size_t allocations = 0;
    std::size_t allocated_bytes = 0;
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
    INFO("allocations in process(): " << allocations << " (" << allocated_bytes
                                      << " bytes)");
    REQUIRE(allocations == 0);
}
