#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/headless.hpp>
#include <pulp/midi/buffer.hpp>

#include "../examples/audio-inspector-demo/audio_inspector_demo_processor.hpp"

#include <cmath>

// The audio-inspector-demo processor is the shared signal source for the live
// window and the offline proof renderer (render_proof_wav). It is an example,
// but it is exercised here so the deterministic audio path the video proofs mux
// is unit-validated, not just compiled.

using namespace pulp;

TEST_CASE("AudioInspectorDemo processor describes an instrument", "[examples][audio-inspector-demo]") {
    auto processor = examples::create_audio_inspector_demo();
    REQUIRE(processor != nullptr);

    const auto descriptor = processor->descriptor();
    CHECK(descriptor.name == "AudioInspectorDemo");
    CHECK(descriptor.manufacturer == "Pulp");
    CHECK(descriptor.category == format::PluginCategory::Instrument);
    REQUIRE(descriptor.output_buses.size() == 1);
    CHECK(descriptor.output_buses[0].default_channels == 2);
    CHECK(descriptor.input_buses.empty());

    const auto view = processor->view_size();
    CHECK(view.preferred_width == 520);
    CHECK(view.preferred_height == 360);
    CHECK(view.min_width == 360);
}

TEST_CASE("AudioInspectorDemo processor renders a deterministic sine at the set level",
          "[examples][audio-inspector-demo]") {
    format::HeadlessHost host(examples::create_audio_inspector_demo);
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlock = 256;
    constexpr int kChannels = 2;
    host.prepare(kSampleRate, kBlock, 0, kChannels);

    // -12 dB target amplitude; 440 Hz.
    host.state().set_value(examples::kFrequency, 440.0f);
    host.state().set_value(examples::kLevelDb, -12.0f);
    const float expected_amp = std::pow(10.0f, -12.0f / 20.0f);

    audio::Buffer<float> output(static_cast<std::size_t>(kChannels),
                                static_cast<std::size_t>(kBlock));
    audio::BufferView<const float> input(nullptr, 0, static_cast<std::size_t>(kBlock));
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    auto view = output.view();
    host.process(view, input, midi_in, midi_out);

    // Both channels are identical (mono signal fanned out), bounded by amplitude,
    // and at least one sample is meaningfully non-zero (the sine is running).
    float peak = 0.0f;
    for (std::size_t i = 0; i < output.num_samples(); ++i) {
        const float left = output.channel(0)[i];
        const float right = output.channel(1)[i];
        CHECK_THAT(right, Catch::Matchers::WithinAbs(left, 1e-6f));
        CHECK(std::abs(left) <= expected_amp + 1e-4f);
        peak = std::max(peak, std::abs(left));
    }
    CHECK(peak > expected_amp * 0.5f);
}

TEST_CASE("AudioInspectorDemo processor clamps an extreme frequency and stays bounded",
          "[examples][audio-inspector-demo]") {
    format::HeadlessHost host(examples::create_audio_inspector_demo);
    host.prepare(0.0 /* falls back to 48k */, 128, 0, 2);
    // Out-of-range values exercise the clamp() branches in process().
    host.state().set_value(examples::kFrequency, 1.0e9f);
    host.state().set_value(examples::kLevelDb, 40.0f);

    audio::Buffer<float> output(2, 128);
    audio::BufferView<const float> input(nullptr, 0, 128);
    midi::MidiBuffer midi_in;
    midi::MidiBuffer midi_out;
    auto view = output.view();
    host.process(view, input, midi_in, midi_out);

    for (std::size_t i = 0; i < output.num_samples(); ++i) {
        CHECK(std::isfinite(output.channel(0)[i]));
        CHECK(std::abs(output.channel(0)[i]) <= 1.0f + 1e-3f);
    }
}
