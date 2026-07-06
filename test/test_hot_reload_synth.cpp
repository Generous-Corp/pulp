// Headless DSP test for the hot-reload SYNTH demo (examples/hot-reload-synth).
// Proves the instrument makes sound on a MIDI note, both oscillator shapes work
// and differ, and a note-off releases to silence — so the hot-swappable synth
// logic isn't silent/broken before it ships in the M2 pkg.
#include <catch2/catch_test_macros.hpp>

#include "synth_voices.hpp"          // examples/hot-reload-synth (added to include path)
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/state/store.hpp>

#include <cmath>
#include <vector>

using pulp::examples::Osc;
using pulp::examples::PolySynth;

namespace {
// Render `blocks` blocks of `frames` through the synth; return peak |sample|.
float render_peak(PolySynth& synth, pulp::midi::MidiBuffer& first_block_midi,
                  int blocks, std::size_t frames) {
    pulp::audio::Buffer<float> out(2, frames);
    const float* in_ptrs[2] = {nullptr, nullptr};
    pulp::audio::BufferView<const float> in(in_ptrs, 0, frames);   // instrument: no audio in
    float peak = 0.0f;
    for (int b = 0; b < blocks; ++b) {
        for (std::size_t c = 0; c < out.num_channels(); ++c)
            for (std::size_t n = 0; n < frames; ++n) out.channel(c)[n] = 0.0f;
        auto ov = out.view();
        pulp::midi::MidiBuffer mi, mo;
        if (b == 0) mi = first_block_midi;    // deliver the note events on block 0
        synth.process(ov, in, mi, mo, pulp::format::ProcessContext{});
        for (std::size_t n = 0; n < frames; ++n) peak = std::max(peak, std::abs(out.channel(0)[n]));
    }
    return peak;
}

void bind(PolySynth& s, pulp::state::StateStore& store) {
    s.define_parameters(store);
    s.set_state_store(&store);
    pulp::format::PrepareContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.max_buffer_size = 256;
    s.prepare(ctx);
}
}  // namespace

TEST_CASE("hot-reload synth makes sound on a MIDI note", "[examples][hot-reload-synth]") {
    pulp::state::StateStore store;
    PolySynth synth(Osc::Sine);
    bind(synth, store);

    pulp::midi::MidiBuffer note;
    note.add(pulp::midi::MidiEvent::note_on(0, 69, 100));   // A4
    const float peak = render_peak(synth, note, /*blocks=*/8, /*frames=*/256);
    REQUIRE(std::isfinite(peak));
    REQUIRE(peak > 0.01f);                                  // clearly audible, not silence
}

TEST_CASE("hot-reload synth: both oscillator shapes sound + differ", "[examples][hot-reload-synth]") {
    auto peak_for = [](Osc osc, std::vector<float>& capture) {
        pulp::state::StateStore store;
        PolySynth synth(osc);
        synth.define_parameters(store);
        synth.set_state_store(&store);
        pulp::format::PrepareContext ctx; ctx.sample_rate = 48000.0; ctx.max_buffer_size = 256;
        synth.prepare(ctx);
        pulp::audio::Buffer<float> out(2, 256);
        const float* ip[2] = {nullptr, nullptr};
        pulp::audio::BufferView<const float> in(ip, 0, 256);
        pulp::midi::MidiBuffer mi, mo;
        mi.add(pulp::midi::MidiEvent::note_on(0, 69, 100));
        for (int b = 0; b < 8; ++b) {                       // let the attack settle
            auto ov = out.view();
            pulp::midi::MidiBuffer block_mi = (b == 0) ? mi : pulp::midi::MidiBuffer{};
            synth.process(ov, in, block_mi, mo, pulp::format::ProcessContext{});
        }
        capture.assign(out.channel(0).data(), out.channel(0).data() + 256);
    };
    std::vector<float> sine, saw;
    peak_for(Osc::Sine, sine);
    peak_for(Osc::Saw, saw);
    // Both audible; a saw at the same note has a very different waveform than a
    // sine, so the buffers must differ materially (the audible hot-swap).
    double diff = 0.0;
    for (std::size_t i = 0; i < sine.size(); ++i) diff += std::abs(sine[i] - saw[i]);
    REQUIRE(diff > 1.0);
}

TEST_CASE("hot-reload synth releases to silence on note-off", "[examples][hot-reload-synth]") {
    pulp::state::StateStore store;
    PolySynth synth(Osc::Sine);
    bind(synth, store);
    store.set_value(2, 0.05f);                              // short 50ms release

    pulp::midi::MidiBuffer on;  on.add(pulp::midi::MidiEvent::note_on(0, 69, 100));
    REQUIRE(render_peak(synth, on, 8, 256) > 0.01f);        // sounding

    pulp::midi::MidiBuffer off; off.add(pulp::midi::MidiEvent::note_off(0, 69));
    render_peak(synth, off, /*blocks=*/64, /*frames=*/256);        // run past the release window
    pulp::midi::MidiBuffer none;
    const float tail = render_peak(synth, none, /*blocks=*/1, /*frames=*/256);  // a FRESH block now
    REQUIRE(tail < 1e-3f);                                          // fully decayed to silence
}
