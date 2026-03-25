// PulpSynth test — validates the synth processes audio with MIDI input

#include "pulp_synth.hpp"
#include <pulp/format/headless.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <cmath>

using namespace pulp::examples;
using namespace pulp::format;
using Catch::Matchers::WithinAbs;

TEST_CASE("PulpSynth produces audio on note-on", "[examples][synth]") {
    HeadlessHost host(create_pulp_synth);
    host.prepare(48000, 512, 0, 2);

    // Create buffers
    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<float> output(out_ptrs, 2, 512);
    pulp::audio::BufferView<const float> input(nullptr, 0, 512);

    pulp::midi::MidiBuffer midi_in, midi_out;
    midi_in.add(pulp::midi::MidiEvent::note_on(0, 60, 100));

    host.process(output, input, midi_in, midi_out);

    float sum_sq = 0;
    for (int i = 0; i < 512; ++i)
        sum_sq += out_l[i] * out_l[i];
    float rms = std::sqrt(sum_sq / 512.0f);
    REQUIRE(rms > 0.001f);
}

TEST_CASE("PulpSynth is silent without notes", "[examples][synth]") {
    HeadlessHost host(create_pulp_synth);
    host.prepare(48000, 512, 0, 2);

    std::vector<float> out_l(512, 0), out_r(512, 0);
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<float> output(out_ptrs, 2, 512);
    pulp::audio::BufferView<const float> input(nullptr, 0, 512);

    host.process(output, input);

    float sum_sq = 0;
    for (int i = 0; i < 512; ++i)
        sum_sq += out_l[i] * out_l[i];
    REQUIRE(sum_sq < 0.0001f);
}

TEST_CASE("PulpSynth descriptor", "[examples][synth]") {
    HeadlessHost host(create_pulp_synth);
    auto& desc = host.descriptor();
    REQUIRE(desc.name == "PulpSynth");
    REQUIRE(desc.category == PluginCategory::Instrument);
    REQUIRE(desc.accepts_midi == true);
}

TEST_CASE("PulpSynth state round-trip", "[examples][synth]") {
    HeadlessHost host(create_pulp_synth);
    host.prepare(48000, 512, 0, 2);

    host.state().set_value(kFilterCutoff, 2000.0f);
    auto data = host.save_state();
    REQUIRE(!data.empty());

    host.state().set_value(kFilterCutoff, 5000.0f);
    host.load_state(data);
    REQUIRE_THAT(host.state().get_value(kFilterCutoff), WithinAbs(2000.0, 0.1));
}
