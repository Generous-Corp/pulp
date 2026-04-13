#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "pulp_mpe_synth.hpp"

using namespace pulp;
using namespace pulp::examples::mpe_synth;
using Kind = midi::MpeExpressionEvent::Kind;
using Catch::Approx;

TEST_CASE("PulpMpeSynth declares MPE support", "[example][mpe]") {
    Processor p;
    const auto d = p.descriptor();
    REQUIRE(d.supports_mpe);
    REQUIRE(d.accepts_midi);
    REQUIRE(d.category == format::PluginCategory::Instrument);
}

TEST_CASE("PulpMpeSynth allocates voices from an MpeBuffer", "[example][mpe]") {
    Processor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::PrepareContext pctx;
    pctx.sample_rate = 48000;
    pctx.max_buffer_size = 64;
    pctx.output_channels = 2;
    p.prepare(pctx);

    midi::MpeBuffer buf;
    midi::MpeNoteState s{}; s.active = true; s.channel = 1; s.note = 60; s.velocity = 100; s.note_id = 1;
    buf.add({0, Kind::NoteOn, s});
    s.pressure = 0.8f;
    buf.add({0, Kind::Pressure, s});
    p.set_mpe_input(&buf);

    float left[64] = {}, right[64] = {};
    float* channels[2] = {left, right};
    audio::BufferView<float> out(channels, 2, 64);
    audio::BufferView<const float> in(nullptr, 0, 64);
    midi::MidiBuffer midi_in, midi_out;
    format::ProcessContext ctx; ctx.sample_rate = 48000; ctx.num_samples = 64;

    p.process(out, in, midi_in, midi_out, ctx);

    REQUIRE(p.allocator().active_count() == 1);
    // Render should have produced a nonzero signal on both channels
    // after the amp smoother catches up to the pressure target.
    // First block is quiet (amp starts at 0 and ramps) — do a few more.
    for (int i = 0; i < 20; ++i) {
        std::fill_n(left, 64, 0.0f);
        std::fill_n(right, 64, 0.0f);
        p.process(out, in, midi_in, midi_out, ctx);
    }
    float peak = 0.0f;
    for (int i = 0; i < 64; ++i) peak = std::max(peak, std::abs(left[i]));
    REQUIRE(peak > 0.0f);
}
