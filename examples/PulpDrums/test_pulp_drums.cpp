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

// ── Deeper golden cases (#356 golden breadth) ─────────────────────────

namespace {

int count_midi_over_duration(HeadlessHost& host, float tempo_bpm,
                             int total_samples, int block_size) {
    host.state().set_value(kTempo, tempo_bpm);
    host.state().set_value(kDensity, 1.0f);    // every step triggers
    host.state().set_value(kRandomize, 0.0f);  // deterministic

    std::vector<float> in_l(static_cast<std::size_t>(block_size), 0.0f);
    std::vector<float> in_r(static_cast<std::size_t>(block_size), 0.0f);
    std::vector<float> out_l(static_cast<std::size_t>(block_size), 0.0f);
    std::vector<float> out_r(static_cast<std::size_t>(block_size), 0.0f);
    const float* ip[] = {in_l.data(), in_r.data()};
    float* op[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> iv(ip, 2, block_size);
    pulp::audio::BufferView<float> ov(op, 2, block_size);

    int notes = 0;
    pulp::midi::MidiBuffer midi_in, midi_out;
    for (int pos = 0; pos < total_samples; pos += block_size) {
        midi_out.clear();
        host.process(ov, iv, midi_in, midi_out);
        notes += static_cast<int>(midi_out.size());
    }
    return notes;
}

}  // namespace

// Faster tempo ⇒ more steps per fixed wall-clock window, so more MIDI
// events should land in the same number of samples. If the sequencer
// ever ignores the tempo param this test fails hard.
TEST_CASE("PulpDrums golden: tempo change changes MIDI density",
          "[examples][drums][golden][issue-356]") {
    // ~1 second of audio at 48 kHz.
    constexpr int total = 48000;
    constexpr int block = 512;

    HeadlessHost slow(create_pulp_drums);
    slow.prepare(48000, block, 2, 2);
    int slow_notes = count_midi_over_duration(slow, 60.0f, total, block);

    HeadlessHost fast(create_pulp_drums);
    fast.prepare(48000, block, 2, 2);
    int fast_notes = count_midi_over_duration(fast, 240.0f, total, block);

    REQUIRE(slow_notes > 0);
    REQUIRE(fast_notes > slow_notes);
    // 4x tempo ⇒ strictly more notes; allow broad tolerance for
    // quantisation at block boundaries.
    REQUIRE(fast_notes >= slow_notes * 2);
}

// Audio pass-through must remain bit-exact even while the MIDI
// sequencer fires. Regression guard against a future change that
// accidentally reads/writes the audio buffer in the MIDI path.
TEST_CASE("PulpDrums golden: audio is bit-exact pass-through while sequencing",
          "[examples][drums][golden][issue-356]") {
    HeadlessHost host(create_pulp_drums);
    host.prepare(48000, 512, 2, 2);
    host.state().set_value(kTempo, 200.0f);
    host.state().set_value(kDensity, 1.0f);

    constexpr int block = 512;
    std::vector<float> in_l(block), in_r(block);
    for (int i = 0; i < block; ++i) {
        in_l[static_cast<std::size_t>(i)] =
            0.5f * std::sin(static_cast<float>(i) * 0.01f);
        in_r[static_cast<std::size_t>(i)] =
            -0.5f * std::sin(static_cast<float>(i) * 0.01f);
    }
    std::vector<float> out_l(block, 123.0f);  // poison
    std::vector<float> out_r(block, 123.0f);

    const float* ip[] = {in_l.data(), in_r.data()};
    float* op[] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> iv(ip, 2, block);
    pulp::audio::BufferView<float> ov(op, 2, block);
    pulp::midi::MidiBuffer midi_in, midi_out;
    host.process(ov, iv, midi_in, midi_out);

    for (int i = 0; i < block; ++i) {
        REQUIRE(out_l[static_cast<std::size_t>(i)]
                == in_l[static_cast<std::size_t>(i)]);
        REQUIRE(out_r[static_cast<std::size_t>(i)]
                == in_r[static_cast<std::size_t>(i)]);
    }
}
