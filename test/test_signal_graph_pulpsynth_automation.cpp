#include "../examples/PulpSynth/pulp_synth.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/processor_node_adapter.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

using pulp::audio::Buffer;
using pulp::examples::create_pulp_synth;
using pulp::examples::kFilterCutoff;
using pulp::format::ProcessorNodeInstance;
using pulp::host::SignalGraph;

constexpr double kSampleRate = 48'000.0;
constexpr int kBlockSize = 256;

struct RenderedGraph {
    Buffer<float> audio;
};

RenderedGraph render_graph(float modulation, bool connect_automation) {
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "Cutoff modulation");
    const auto midi = graph.add_midi_input_node("MIDI");
    auto synth = ProcessorNodeInstance::create(create_pulp_synth());
    REQUIRE(synth);
    const auto synth_node = graph.add_processor_node(synth, "PulpSynth");
    const auto output = graph.add_output_node(2, "Output");
    REQUIRE(input != 0);
    REQUIRE(midi != 0);
    REQUIRE(synth_node != 0);
    REQUIRE(output != 0);
    REQUIRE(graph.connect(synth_node, 0, output, 0));
    REQUIRE(graph.connect(synth_node, 1, output, 1));
    REQUIRE(graph.connect_midi(midi, synth_node));
    if (connect_automation) {
        REQUIRE(graph.connect_automation(input, 0, synth_node, kFilterCutoff, 800.0f, 12'000.0f));
    }
    REQUIRE(graph.prepare(kSampleRate, kBlockSize));

    auto input_buffer = Buffer<float>(1, kBlockSize);
    std::fill(input_buffer.channel(0).begin(), input_buffer.channel(0).end(), modulation);
    Buffer<float> output_buffer(2, kBlockSize);
    auto note = pulp::midi::MidiBuffer{};
    auto event = pulp::midi::MidiEvent::note_on(0, 60, 110);
    event.sample_offset = 0;
    REQUIRE(note.add(event));
    REQUIRE(graph.inject_midi(midi, note));
    auto output_view = output_buffer.view();
    auto input_view = std::as_const(input_buffer).view();
    graph.process(output_view, input_view, kBlockSize);
    return {std::move(output_buffer)};
}

float rms(const Buffer<float>& audio) {
    double sum = 0.0;
    for (std::size_t channel = 0; channel < audio.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < audio.num_samples(); ++frame) {
            const auto sample = audio.channel(channel)[frame];
            REQUIRE(std::isfinite(sample));
            sum += static_cast<double>(sample) * sample;
        }
    return static_cast<float>(
        std::sqrt(sum / static_cast<double>(audio.num_channels() * audio.num_samples())));
}

void require_different(const Buffer<float>& first, const Buffer<float>& second) {
    REQUIRE(first.num_channels() == second.num_channels());
    REQUIRE(first.num_samples() == second.num_samples());
    bool differs = false;
    for (std::size_t channel = 0; channel < first.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < first.num_samples(); ++frame)
            differs |= first.channel(channel)[frame] != second.channel(channel)[frame];
    REQUIRE(differs);
}

void require_equal(const Buffer<float>& first, const Buffer<float>& second) {
    REQUIRE(first.num_channels() == second.num_channels());
    REQUIRE(first.num_samples() == second.num_samples());
    for (std::size_t channel = 0; channel < first.num_channels(); ++channel)
        for (std::size_t frame = 0; frame < first.num_samples(); ++frame)
            REQUIRE(first.channel(channel)[frame] == second.channel(channel)[frame]);
}

} // namespace

TEST_CASE("PulpSynth graph parameter automation changes filter output",
          "[host][graph][pulpsynth][automation][deterministic]") {
    const auto low_cutoff = render_graph(0.0f, true);
    const auto high_cutoff = render_graph(1.0f, true);
    REQUIRE(rms(low_cutoff.audio) > 1.0e-5f);
    REQUIRE(rms(high_cutoff.audio) > 1.0e-5f);
    require_different(low_cutoff.audio, high_cutoff.audio);
}

TEST_CASE("PulpSynth graph input alone does not alter output without an automation edge",
          "[host][graph][pulpsynth][automation][control]") {
    const auto low_input = render_graph(0.0f, false);
    const auto high_input = render_graph(1.0f, false);
    require_equal(low_input.audio, high_input.audio);
}
