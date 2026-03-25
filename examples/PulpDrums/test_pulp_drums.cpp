// PulpDrums test — validates MIDI output from drum sequencer

#include "pulp_drums.hpp"
#include <pulp/format/headless.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace pulp::examples;
using namespace pulp::format;

TEST_CASE("PulpDrums generates MIDI output", "[examples][drums]") {
    HeadlessHost host(create_pulp_drums);
    host.prepare(48000, 512, 2, 2);

    std::vector<float> in_l(512, 0), in_r(512, 0);
    std::vector<float> out_l(512, 0), out_r(512, 0);
    const float* in_ptrs[] = {in_l.data(), in_r.data()};
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> input(in_ptrs, 2, 512);
    pulp::audio::BufferView<float> output(out_ptrs, 2, 512);

    pulp::midi::MidiBuffer midi_in, midi_out;

    // Process several blocks to accumulate MIDI output
    int total_notes = 0;
    for (int block = 0; block < 20; ++block) {
        midi_out.clear();
        host.process(output, input, midi_in, midi_out);
        total_notes += static_cast<int>(midi_out.size());
    }

    // At 120 BPM, 16th notes = 8 per second
    // 20 blocks * 512 samples / 48000 = 0.213 seconds ≈ 1-2 steps
    // Should have generated some MIDI notes
    REQUIRE(total_notes > 0);
}

TEST_CASE("PulpDrums passes audio through", "[examples][drums]") {
    HeadlessHost host(create_pulp_drums);
    host.prepare(48000, 512, 2, 2);

    std::vector<float> in_l(512), in_r(512);
    std::vector<float> out_l(512, 0), out_r(512, 0);
    // Fill with test signal
    for (int i = 0; i < 512; ++i) {
        in_l[i] = 0.5f;
        in_r[i] = -0.5f;
    }
    const float* in_ptrs[] = {in_l.data(), in_r.data()};
    float* out_ptrs[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> input(in_ptrs, 2, 512);
    pulp::audio::BufferView<float> output(out_ptrs, 2, 512);

    host.process(output, input);

    // Audio should pass through unchanged
    REQUIRE(out_l[100] > 0.49f);
    REQUIRE(out_r[100] < -0.49f);
}

TEST_CASE("PulpDrums descriptor", "[examples][drums]") {
    HeadlessHost host(create_pulp_drums);
    auto& desc = host.descriptor();

    REQUIRE(desc.name == "PulpDrums");
    REQUIRE(desc.produces_midi == true);
    REQUIRE(desc.accepts_midi == true);
}

TEST_CASE("PulpDrums state round-trip", "[examples][drums]") {
    HeadlessHost host(create_pulp_drums);
    host.prepare(48000, 512, 2, 2);

    host.state().set_value(kTempo, 140.0f);
    host.state().set_value(kSwing, 0.3f);
    auto data = host.save_state();
    REQUIRE(!data.empty());

    host.state().set_value(kTempo, 120.0f);
    host.load_state(data);

    REQUIRE(host.state().get_value(kTempo) > 139.0f);
    REQUIRE(host.state().get_value(kSwing) > 0.29f);
}
