// Coverage for for_each_midi_subblock(): the MIDI counterpart to the
// parameter sub-block splitter. Asserts exact sub-block boundaries and which
// events each callback observes, plus the realtime allocation-free contract.

#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/format/midi_processing.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/midi/message.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace {

using pulp::audio::BufferView;
using pulp::format::for_each_midi_subblock;
using pulp::midi::MidiBuffer;
using pulp::midi::MidiEvent;

// Records one observed sub-block: its leading sample, length, and the notes
// of the events delivered at its boundary.
struct Observation {
    std::size_t start = 0;
    std::size_t length = 0;
    std::vector<int> notes;
};

// Helper: render a block of `frames` samples (single channel) through the
// splitter, capturing every callback's sub-block range + boundary events.
std::vector<Observation> run(const MidiBuffer& midi,
                             std::size_t frames,
                             std::vector<float>& storage) {
    storage.assign(frames == 0 ? 1 : frames, 0.0f);
    float* out_channels[1] = {storage.data()};
    const float* in_channels[1] = {storage.data()};
    BufferView<float> output(out_channels, 1, frames);
    BufferView<const float> input(in_channels, 1, frames);

    std::vector<Observation> observed;
    for_each_midi_subblock(
        output, input, midi,
        [&](BufferView<float>& out,
            const BufferView<const float>& in,
            std::span<const MidiEvent> events) {
            REQUIRE(out.num_samples() == in.num_samples());
            Observation obs;
            obs.start = static_cast<std::size_t>(out.channel_ptr(0) - storage.data());
            obs.length = out.num_samples();
            for (const auto& ev : events) obs.notes.push_back(ev.note());
            observed.push_back(std::move(obs));
        });
    return observed;
}

MidiEvent note_on_at(std::uint8_t note, std::int32_t offset) {
    auto ev = MidiEvent::note_on(0, note, 100);
    ev.sample_offset = offset;
    return ev;
}

} // namespace

TEST_CASE("for_each_midi_subblock with no events emits one full block",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 64, storage);

    REQUIRE(obs.size() == 1);
    REQUIRE(obs[0].start == 0);
    REQUIRE(obs[0].length == 64);
    REQUIRE(obs[0].notes.empty());
}

TEST_CASE("for_each_midi_subblock splits at one mid-block event",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    midi.add(note_on_at(60, 32));
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 64, storage);

    REQUIRE(obs.size() == 2);
    // First sub-block: [0, 32), no events yet.
    REQUIRE(obs[0].start == 0);
    REQUIRE(obs[0].length == 32);
    REQUIRE(obs[0].notes.empty());
    // Second sub-block: [32, 64) carries the event at its leading edge.
    REQUIRE(obs[1].start == 32);
    REQUIRE(obs[1].length == 32);
    REQUIRE(obs[1].notes == std::vector<int>{60});
}

TEST_CASE("for_each_midi_subblock splits at several distinct events",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    midi.add(note_on_at(60, 16));
    midi.add(note_on_at(62, 32));
    midi.add(note_on_at(64, 48));
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 64, storage);

    REQUIRE(obs.size() == 4);
    REQUIRE(obs[0].start == 0);  REQUIRE(obs[0].length == 16); REQUIRE(obs[0].notes.empty());
    REQUIRE(obs[1].start == 16); REQUIRE(obs[1].length == 16); REQUIRE(obs[1].notes == std::vector<int>{60});
    REQUIRE(obs[2].start == 32); REQUIRE(obs[2].length == 16); REQUIRE(obs[2].notes == std::vector<int>{62});
    REQUIRE(obs[3].start == 48); REQUIRE(obs[3].length == 16); REQUIRE(obs[3].notes == std::vector<int>{64});
}

TEST_CASE("for_each_midi_subblock groups coincident-offset events",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    midi.add(note_on_at(60, 32));
    midi.add(note_on_at(62, 32));
    midi.add(note_on_at(64, 32));
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 64, storage);

    // Coincident events do NOT create zero-length sub-blocks: two sub-blocks,
    // and the boundary span carries all three notes (insertion order kept by
    // MidiBuffer::sort()).
    REQUIRE(obs.size() == 2);
    REQUIRE(obs[0].start == 0);  REQUIRE(obs[0].length == 32); REQUIRE(obs[0].notes.empty());
    REQUIRE(obs[1].start == 32); REQUIRE(obs[1].length == 32);
    REQUIRE(obs[1].notes == std::vector<int>{60, 62, 64});
}

TEST_CASE("for_each_midi_subblock delivers a frame-0 event on the first block",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    midi.add(note_on_at(60, 0));
    midi.add(note_on_at(62, 0));
    midi.add(note_on_at(64, 32));
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 64, storage);

    REQUIRE(obs.size() == 2);
    // First sub-block starts at 0 and already carries the frame-0 events.
    REQUIRE(obs[0].start == 0);  REQUIRE(obs[0].length == 32);
    REQUIRE(obs[0].notes == std::vector<int>{60, 62});
    REQUIRE(obs[1].start == 32); REQUIRE(obs[1].length == 32);
    REQUIRE(obs[1].notes == std::vector<int>{64});
}

TEST_CASE("for_each_midi_subblock groups negative offsets onto the first block",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    auto stale = note_on_at(50, -4);  // offset clamped onto the start boundary
    midi.add(stale);
    midi.add(note_on_at(60, 0));
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 32, storage);

    REQUIRE(obs.size() == 1);
    REQUIRE(obs[0].start == 0);
    REQUIRE(obs[0].length == 32);
    REQUIRE(obs[0].notes == std::vector<int>{50, 60});
}

TEST_CASE("for_each_midi_subblock ignores events at or past block end",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    midi.add(note_on_at(60, 32));   // in range
    midi.add(note_on_at(62, 64));   // exactly at block end: ignored
    midi.add(note_on_at(64, 100));  // past block end: ignored
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 64, storage);

    REQUIRE(obs.size() == 2);
    REQUIRE(obs[0].start == 0);  REQUIRE(obs[0].length == 32); REQUIRE(obs[0].notes.empty());
    REQUIRE(obs[1].start == 32); REQUIRE(obs[1].length == 32);
    REQUIRE(obs[1].notes == std::vector<int>{60});
}

TEST_CASE("for_each_midi_subblock with only out-of-range events emits full block",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    midi.add(note_on_at(60, 64));   // at end
    midi.add(note_on_at(62, 200));  // past end
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 64, storage);

    REQUIRE(obs.size() == 1);
    REQUIRE(obs[0].start == 0);
    REQUIRE(obs[0].length == 64);
    REQUIRE(obs[0].notes.empty());
}

TEST_CASE("for_each_midi_subblock skips a zero-frame block",
          "[format][midi][subblock]") {
    MidiBuffer midi;
    midi.add(note_on_at(60, 0));
    midi.sort();
    std::vector<float> storage;
    auto obs = run(midi, 0, storage);

    REQUIRE(obs.empty());
}

TEST_CASE("for_each_midi_subblock does not allocate on the audio thread",
          "[format][midi][subblock][rt]") {
    MidiBuffer midi;
    midi.reserve(8);
    midi.set_realtime_capacity_limit(true);
    midi.add(note_on_at(60, 0));
    midi.add(note_on_at(62, 16));
    midi.add(note_on_at(64, 16));
    midi.add(note_on_at(65, 48));
    midi.sort();

    std::array<float, 64> samples{};
    float* out_channels[1] = {samples.data()};
    const float* in_channels[1] = {samples.data()};
    BufferView<float> output(out_channels, 1, samples.size());
    BufferView<const float> input(in_channels, 1, samples.size());

    int calls = 0;
    int events_seen = 0;
    {
        pulp::runtime::ScopedNoAlloc guard;
        for_each_midi_subblock(
            output, input, midi,
            [&](BufferView<float>&,
                const BufferView<const float>&,
                std::span<const MidiEvent> events) {
                ++calls;
                events_seen += static_cast<int>(events.size());
            });
    }

    // [0,16) carries the frame-0 event; [16,48) carries the two @16 events;
    // [48,64) carries the @48 event => 3 sub-blocks, 4 events delivered.
    REQUIRE(calls == 3);
    REQUIRE(events_seen == 4);
}
