#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include "pulp_pluck.hpp"
#include "pulp_chorus.hpp"
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

using namespace pulp;

// ── PulpPluck Tests ─────────────────────────────────────────────────────

TEST_CASE("web demo build: PulpPluck descriptor", "[examples][pluck]") {
    auto proc = examples::create_pulp_pluck();
    auto desc = proc->descriptor();
    REQUIRE(desc.name == "PulpPluck");
    REQUIRE(desc.category == format::PluginCategory::Instrument);
    REQUIRE(desc.accepts_midi == true);
    REQUIRE(desc.input_buses.empty()); // No audio input
    REQUIRE(desc.output_buses.size() == 1);
}

TEST_CASE("web demo build: PulpPluck parameters", "[examples][pluck]") {
    auto proc = examples::create_pulp_pluck();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);
    REQUIRE(store.param_count() == 3);
}

TEST_CASE("web demo build: PulpPluck produces audio from MIDI", "[examples][pluck]") {
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

    // Send note on
    midi::MidiBuffer midi_in, midi_out;
    midi_in.add(midi::MidiEvent::note_on(0, 60, 100));

    audio::Buffer<float> output(2, 512);
    audio::BufferView<const float> empty_input(nullptr, 0, 0);
    auto out_view = output.view();

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 512;

    proc->process(out_view, empty_input, midi_in, midi_out, ctx);

    // Should have produced non-zero audio
    bool has_audio = false;
    for (size_t i = 0; i < 512; ++i) {
        if (std::abs(output.view().channel(0)[i]) > 0.001f) {
            has_audio = true;
            break;
        }
    }
    REQUIRE(has_audio);
}

TEST_CASE("web demo build: PulpPluck silence without MIDI", "[examples][pluck]") {
    auto proc = examples::create_pulp_pluck();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);

    format::PrepareContext prep;
    prep.sample_rate = 48000.0;
    prep.max_buffer_size = 256;
    prep.output_channels = 2;
    proc->prepare(prep);

    midi::MidiBuffer midi_in, midi_out;
    audio::Buffer<float> output(2, 256);
    audio::BufferView<const float> empty_input(nullptr, 0, 0);
    auto out_view = output.view();

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 256;

    proc->process(out_view, empty_input, midi_in, midi_out, ctx);

    // No MIDI → silence
    float max_val = 0;
    for (size_t i = 0; i < 256; ++i) {
        max_val = std::max(max_val, std::abs(output.view().channel(0)[i]));
    }
    REQUIRE(max_val < 0.0001f);
}

// ── PulpChorus Tests ────────────────────────────────────────────────────

TEST_CASE("PulpChorus descriptor", "[examples][chorus]") {
    auto proc = examples::create_pulp_chorus();
    auto desc = proc->descriptor();
    REQUIRE(desc.name == "PulpChorus");
    REQUIRE(desc.category == format::PluginCategory::Effect);
    REQUIRE(desc.accepts_midi == false);
}

TEST_CASE("PulpChorus parameters", "[examples][chorus]") {
    auto proc = examples::create_pulp_chorus();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);
    REQUIRE(store.param_count() == 3);
}

TEST_CASE("PulpChorus processes audio", "[examples][chorus]") {
    auto proc = examples::create_pulp_chorus();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);

    format::PrepareContext prep;
    prep.sample_rate = 48000.0;
    prep.max_buffer_size = 256;
    prep.input_channels = 2;
    prep.output_channels = 2;
    proc->prepare(prep);

    // Create a test signal: 440Hz sine
    audio::Buffer<float> input(2, 256);
    audio::Buffer<float> output(2, 256);

    for (size_t i = 0; i < 256; ++i) {
        float val = std::sin(2.0f * 3.14159f * 440.0f * i / 48000.0f) * 0.5f;
        input.view().channel(0)[i] = val;
        input.view().channel(1)[i] = val;
    }

    midi::MidiBuffer midi_in, midi_out;
    // Create const view from input buffer channel pointers
    const float* in_ptrs[2] = { &input.view().channel(0)[0], &input.view().channel(1)[0] };
    audio::BufferView<const float> in_view(in_ptrs, 2, 256);
    auto out_view = output.view();

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 256;

    proc->process(out_view, in_view, midi_in, midi_out, ctx);

    // Output should be non-zero
    bool has_output = false;
    for (size_t i = 0; i < 256; ++i) {
        if (std::abs(output.view().channel(0)[i]) > 0.001f) {
            has_output = true;
            break;
        }
    }
    REQUIRE(has_output);
}

TEST_CASE("PulpChorus dry at 0% mix", "[examples][chorus]") {
    auto proc = examples::create_pulp_chorus();
    state::StateStore store;
    proc->set_state_store(&store);
    proc->define_parameters(store);
    store.set_value(examples::kChorusMix, 0.0f); // Fully dry

    format::PrepareContext prep;
    prep.sample_rate = 48000.0;
    prep.max_buffer_size = 256;
    prep.input_channels = 2;
    prep.output_channels = 2;
    proc->prepare(prep);

    audio::Buffer<float> input(2, 256);
    audio::Buffer<float> output(2, 256);

    for (size_t i = 0; i < 256; ++i) {
        input.view().channel(0)[i] = 0.7f;
        input.view().channel(1)[i] = 0.7f;
    }

    midi::MidiBuffer midi_in, midi_out;
    // Create const view from input buffer channel pointers
    const float* in_ptrs[2] = { &input.view().channel(0)[0], &input.view().channel(1)[0] };
    audio::BufferView<const float> in_view(in_ptrs, 2, 256);
    auto out_view = output.view();

    format::ProcessContext ctx;
    ctx.sample_rate = 48000.0;
    ctx.num_samples = 256;

    proc->process(out_view, in_view, midi_in, midi_out, ctx);

    // At 0% mix, output should match input
    for (size_t i = 0; i < 256; ++i) {
        REQUIRE(output.view().channel(0)[i] == Catch::Approx(0.7f).margin(0.001f));
    }
}
