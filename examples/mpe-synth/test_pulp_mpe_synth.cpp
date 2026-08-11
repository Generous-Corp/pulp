#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "pulp_mpe_synth.hpp"
#include <vector>

using namespace pulp;
using namespace pulp::examples::mpe_synth;
using Kind = midi::MpeExpressionEvent::Kind;
using Catch::Approx;

namespace {

std::vector<float> render_a4(double sample_rate) {
    constexpr std::size_t block_size = 256;
    const auto total_samples = static_cast<std::size_t>(sample_rate / 4.0);

    Processor processor;
    state::StateStore store;
    processor.set_state_store(&store);
    processor.define_parameters(store);
    store.set_value(kMasterGainDb, 0.0f);

    format::PrepareContext prepare;
    prepare.sample_rate = sample_rate;
    prepare.max_buffer_size = block_size;
    prepare.output_channels = 2;
    processor.prepare(prepare);

    midi::MpeBuffer note_on;
    midi::MpeNoteState note{};
    note.active = true;
    note.channel = 1;
    note.note = 69;
    note.velocity = 127;
    note.note_id = 1;
    note.pressure = 1.0f;
    note_on.add({0, Kind::NoteOn, note});
    processor.set_mpe_input(&note_on);

    std::vector<float> rendered(total_samples);
    std::vector<float> left(block_size);
    std::vector<float> right(block_size);
    midi::MidiBuffer midi_in, midi_out;

    for (std::size_t offset = 0; offset < total_samples; offset += block_size) {
        const auto frames = std::min(block_size, total_samples - offset);
        float* channels[2] = {left.data(), right.data()};
        audio::BufferView<float> output(channels, 2, frames);
        audio::BufferView<const float> input(nullptr, 0, frames);
        format::ProcessContext process;
        process.sample_rate = sample_rate;
        process.num_samples = frames;

        processor.process(output, input, midi_in, midi_out, process);
        processor.set_mpe_input(nullptr);

        for (std::size_t i = 0; i < frames; ++i) {
            REQUIRE(left[i] == right[i]);
            rendered[offset + i] = left[i];
        }
    }

    return rendered;
}

double positive_crossing_frequency(
    const std::vector<float>& samples, double sample_rate) {
    const auto begin = static_cast<std::size_t>(sample_rate / 20.0);
    std::vector<std::size_t> crossings;
    for (std::size_t i = begin + 1; i < samples.size(); ++i) {
        if (samples[i - 1] <= 0.0f && samples[i] > 0.0f)
            crossings.push_back(i);
    }
    REQUIRE(crossings.size() > 50);
    return static_cast<double>(crossings.size() - 1) * sample_rate /
           static_cast<double>(crossings.back() - crossings.front());
}

} // namespace

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

TEST_CASE("PulpMpeSynth honors the host sample rate for tuning", "[example][mpe]") {
    const auto at_48k = render_a4(48000.0);
    const auto at_96k = render_a4(96000.0);

    const auto hz_48k = positive_crossing_frequency(at_48k, 48000.0);
    const auto hz_96k = positive_crossing_frequency(at_96k, 96000.0);

    REQUIRE(hz_48k == Approx(440.0).margin(0.5));
    REQUIRE(hz_96k == Approx(440.0).margin(0.5));
    REQUIRE(hz_96k / hz_48k == Approx(1.0).margin(0.001));
}

TEST_CASE("PulpMpeSynth clears allocator ownership on release and reset",
          "[example][mpe][lifecycle]") {
    Processor p;
    state::StateStore store;
    p.set_state_store(&store);
    p.define_parameters(store);

    format::PrepareContext pctx;
    pctx.sample_rate = 48000;
    pctx.max_buffer_size = 16;
    pctx.output_channels = 1;
    p.prepare(pctx);

    midi::MpeBuffer buf;
    midi::MpeNoteState note{};
    note.active = true;
    note.channel = 1;
    note.note = 60;
    note.velocity = 100;
    note.note_id = 1;
    REQUIRE(buf.add({0, Kind::NoteOn, note}));
    p.set_mpe_input(&buf);

    float samples[16]{};
    float* channels[1] = {samples};
    audio::BufferView<float> out(channels, 1, 16);
    audio::BufferView<const float> in(nullptr, 0, 16);
    midi::MidiBuffer midi_in, midi_out;
    format::ProcessContext ctx;
    ctx.sample_rate = 48000;
    ctx.num_samples = 16;

    p.process(out, in, midi_in, midi_out, ctx);
    REQUIRE(p.allocator().active_count() == 1);
    p.release();
    REQUIRE(p.allocator().active_count() == 0);

    note.note_id = 2;
    buf.clear();
    REQUIRE(buf.add({0, Kind::NoteOn, note}));
    p.process(out, in, midi_in, midi_out, ctx);
    REQUIRE(p.allocator().active_count() == 1);

    buf.clear();
    ctx.transport_jump = true;
    p.process(out, in, midi_in, midi_out, ctx);
    REQUIRE(p.allocator().active_count() == 1);

    ctx.transport_jump = false;
    ctx.reset_requested = true;
    p.process(out, in, midi_in, midi_out, ctx);
    REQUIRE(p.allocator().active_count() == 0);
}
