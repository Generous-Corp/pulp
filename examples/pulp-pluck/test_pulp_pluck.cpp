#include <catch2/catch_test_macros.hpp>
#include "pulp_pluck.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <array>
#include <cmath>

using namespace pulp;

TEST_CASE("PulpPluck descriptor", "[examples][pluck]") {
    auto proc = examples::create_pulp_pluck();
    auto desc = proc->descriptor();
    REQUIRE(desc.name == "PulpPluck");
    REQUIRE(desc.category == format::PluginCategory::Instrument);
    REQUIRE(desc.accepts_midi);
    REQUIRE(desc.input_buses.empty());
    REQUIRE(desc.output_buses.size() == 1);
}

TEST_CASE("PulpPluck parameters", "[examples][pluck]") {
    auto proc = examples::create_pulp_pluck();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);
    REQUIRE(store.param_count() == 3);
}

TEST_CASE("PulpPluck produces audio from MIDI", "[examples][pluck]") {
    auto proc = examples::create_pulp_pluck();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);

    format::PrepareContext prep;
    prep.sample_rate = 48000.0;
    prep.max_buffer_size = 512;
    prep.output_channels = 2;
    prep.input_channels = 0;
    proc->prepare(prep);

    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100));

    audio::Buffer<float> output(2, 512);
    audio::BufferView<const float> empty_input(nullptr, 0, 0);
    auto out_view = output.view();

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 512;

    proc->process(out_view, empty_input, midi_in, midi_out, ctx);

    bool has_audio = false;
    for (size_t i = 0; i < 512; ++i) {
        if (std::abs(output.view().channel(0)[i]) > 0.001f) {
            has_audio = true;
            break;
        }
    }
    REQUIRE(has_audio);
}

TEST_CASE("PulpPluck silence without MIDI", "[examples][pluck]") {
    auto proc = examples::create_pulp_pluck();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);

    format::PrepareContext prep;
    prep.sample_rate = 48000.0;
    prep.max_buffer_size = 256;
    prep.output_channels = 2;
    prep.input_channels = 0;
    proc->prepare(prep);

    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    audio::Buffer<float> output(2, 256);
    audio::BufferView<const float> empty_input(nullptr, 0, 0);
    auto out_view = output.view();

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 256;

    proc->process(out_view, empty_input, midi_in, midi_out, ctx);

    float max_val = 0.0f;
    for (size_t i = 0; i < 256; ++i) {
        max_val = std::max(max_val, std::abs(output.view().channel(0)[i]));
    }
    REQUIRE(max_val < 0.0001f);
}

TEST_CASE("PulpPluck prepare clears active voices and permits retrigger",
          "[examples][pluck][lifecycle]") {
    auto proc = examples::create_pulp_pluck();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);

    format::PrepareContext prep;
    prep.sample_rate = 48000.0;
    prep.max_buffer_size = 512;
    prep.output_channels = 2;
    prep.input_channels = 0;
    proc->prepare(prep);

    midi::MidiBuffer midi_out;
    audio::BufferView<const float> empty_input(nullptr, 0, 0);
    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 512;

    auto render_peaks = [&](midi::MidiBuffer& midi_in) {
        audio::Buffer<float> output(2, 512);
        auto out_view = output.view();
        proc->process(out_view, empty_input, midi_in, midi_out, ctx);

        std::array<float, 2> peaks{};
        for (std::size_t ch = 0; ch < peaks.size(); ++ch)
            for (float sample : output.view().channel(ch))
                peaks[ch] = std::max(peaks[ch], std::abs(sample));
        return peaks;
    };

    midi::MidiBuffer first_note;
    first_note.add(midi::MidiEvent::note_on(0, 60, 100));
    const auto active_peaks = render_peaks(first_note);
    REQUIRE(active_peaks[0] > 0.001f);
    REQUIRE(active_peaks[1] > 0.001f);

    proc->prepare(prep);
    midi::MidiBuffer no_midi;
    const auto reset_peaks = render_peaks(no_midi);
    CHECK(reset_peaks[0] == 0.0f);
    CHECK(reset_peaks[1] == 0.0f);

    midi::MidiBuffer second_note;
    second_note.add(midi::MidiEvent::note_on(0, 67, 100));
    const auto retriggered_peaks = render_peaks(second_note);
    CHECK(retriggered_peaks[0] > 0.001f);
    CHECK(retriggered_peaks[1] > 0.001f);
}
