#include <pulp/host/graph_event_contract.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using pulp::host::GraphAuthoredEvent;
using pulp::host::GraphAuthoredEventLane;
using pulp::host::GraphEventResult;
using pulp::host::HostEventIngress;
using pulp::host::HostEventRecord;
using pulp::host::HostEventResult;
using pulp::host::project_host_event_block;

namespace pulp::host::detail {
struct GraphEventLaneTestAccess {
    template <std::size_t MaxFrames, std::size_t MaxAcceptedPerSample>
    static void set_cursor(GraphAuthoredEventLane<MaxFrames, MaxAcceptedPerSample>& lane,
                           std::uint64_t cursor) {
        lane.test_set_cursor_(cursor);
    }
};
} // namespace pulp::host::detail

namespace {
HostEventRecord midi_event(std::uint64_t epoch, std::int64_t sample, std::uint64_t sequence,
                           std::uint32_t producer, pulp::midi::MidiEvent event) {
    HostEventRecord record;
    record.epoch = epoch;
    record.absolute_sample = sample;
    record.producer_sequence = sequence;
    record.producer = producer;
    record.midi = event;
    return record;
}

std::vector<GraphAuthoredEvent> render_one(GraphAuthoredEventLane<16>& lane,
                                           const std::array<std::size_t, 16>& partitions,
                                           std::size_t partition_count) {
    REQUIRE(lane.reset(1) == GraphEventResult::Accepted);
    std::vector<GraphAuthoredEvent> events;
    std::size_t absolute = 0;
    for (std::size_t block = 0; block < partition_count; ++block) {
        const auto frames = partitions[block];
        REQUIRE(lane.begin_block(1, absolute, frames) == GraphEventResult::Accepted);
        for (std::size_t offset = 0; offset < frames; ++offset) {
            REQUIRE(lane.begin_sample(offset) == GraphEventResult::Accepted);
            if (absolute == 0 || absolute == 1 || absolute == 5) {
                REQUIRE(lane.emit(1, 7, static_cast<std::uint16_t>(absolute), absolute) ==
                        GraphEventResult::Accepted);
            }
            REQUIRE(lane.begin_consumer() == GraphEventResult::Accepted);
            for (const auto& event : lane.received())
                events.push_back(event);
            REQUIRE(lane.end_sample() == GraphEventResult::Accepted);
            ++absolute;
        }
        REQUIRE(lane.end_block() == GraphEventResult::Accepted);
    }
    return events;
}
} // namespace

TEST_CASE("G5 host ingress projects absolute MIDI records without retiming",
          "[host][graph][g5][events]") {
    HostEventIngress<4> ingress;
    REQUIRE(ingress.begin_block(3, 100, 8) == HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(3, 102, 2, 11, pulp::midi::MidiEvent::note_on(1, 62, 90))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(3, 100, 1, 11, pulp::midi::MidiEvent::note_on(1, 60, 90))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(3, 110, 3, 11, pulp::midi::MidiEvent::note_off(1, 60))) ==
            HostEventResult::Accepted);

    REQUIRE(ingress.publish(midi_event(3, 99, 4, 11, pulp::midi::MidiEvent::note_off(1, 60))) ==
            HostEventResult::Late);
    REQUIRE(ingress.publish(midi_event(2, 101, 5, 11, pulp::midi::MidiEvent::note_off(1, 60))) ==
            HostEventResult::StaleEpoch);

    std::array<HostEventRecord, 2> projected{};
    std::size_t count = 0;
    REQUIRE(ingress.project(projected, count) == HostEventResult::Accepted);
    REQUIRE(count == 2);
    REQUIRE(projected[0].absolute_sample == 100);
    REQUIRE(projected[1].absolute_sample == 102);
    REQUIRE(ingress.pending() == 1);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);

    REQUIRE(ingress.begin_block(3, 108, 8) == HostEventResult::Accepted);
    std::array<HostEventRecord, 2> next{};
    REQUIRE(ingress.project(next, count) == HostEventResult::Accepted);
    REQUIRE(count == 1);
    REQUIRE(next[0].absolute_sample == 110);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);
}

TEST_CASE("G5 host ingress rejects output-capacity and sequence mutations",
          "[host][graph][g5][events][negative-control]") {
    HostEventIngress<2> ingress;
    REQUIRE(ingress.begin_block(1, 0, 4) == HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 0, 1, 9, pulp::midi::MidiEvent::note_on(0, 60, 1))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 1, 1, 9, pulp::midi::MidiEvent::note_on(0, 61, 1))) ==
            HostEventResult::InvalidSequence);
    REQUIRE(ingress.publish(midi_event(1, 1, 2, 9, pulp::midi::MidiEvent::note_on(0, 61, 1))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 2, 3, 9, pulp::midi::MidiEvent::note_on(0, 62, 1))) ==
            HostEventResult::Overflow);
    std::array<HostEventRecord, 1> too_small{};
    std::size_t count = 0;
    REQUIRE(ingress.project(too_small, count) == HostEventResult::OutputCapacity);
    REQUIRE(count == 0);
    REQUIRE(ingress.pending() == 2);
}

TEST_CASE("G5 host ingress drops staged records at an epoch boundary",
          "[host][graph][g5][events][negative-control]") {
    HostEventIngress<4> ingress;
    REQUIRE(ingress.begin_block(1, 100, 4) == HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 110, 1, 9, pulp::midi::MidiEvent::note_on(0, 60, 1))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.pending() == 1);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);

    REQUIRE(ingress.begin_block(2, 100, 4) == HostEventResult::Accepted);
    std::array<HostEventRecord, 1> projected{};
    std::size_t count = 0;
    REQUIRE(ingress.project(projected, count) == HostEventResult::Accepted);
    REQUIRE(count == 0);
    REQUIRE(ingress.pending() == 0);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);
}

TEST_CASE("G5 host ingress evicts staged records passed by a forward sample jump",
          "[host][graph][g5][events][negative-control]") {
    HostEventIngress<4> ingress;
    REQUIRE(ingress.begin_block(1, 100, 4) == HostEventResult::Accepted);
    REQUIRE(ingress.publish(midi_event(1, 110, 1, 9, pulp::midi::MidiEvent::note_on(0, 60, 1))) ==
            HostEventResult::Accepted);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);

    // Seek/loop: the next block starts past the staged record.
    REQUIRE(ingress.begin_block(1, 200, 4) == HostEventResult::Accepted);
    REQUIRE(ingress.pending() == 0);
    // Same (producer, sequence) is reusable and full capacity is available.
    for (std::uint64_t i = 1; i <= 4; ++i)
        REQUIRE(ingress.publish(midi_event(1, 200 + static_cast<std::int64_t>(i) - 1, i, 9,
                                           pulp::midi::MidiEvent::note_on(0, 60, 1))) ==
                HostEventResult::Accepted);
    std::array<HostEventRecord, 4> projected{};
    std::size_t count = 0;
    REQUIRE(ingress.project(projected, count) == HostEventResult::Accepted);
    REQUIRE(count == 4);
    REQUIRE(ingress.pending() == 0);
    REQUIRE(ingress.end_block() == HostEventResult::Accepted);
}

TEST_CASE("G5 MPE sidecar preserves projected sample offsets", "[host][graph][g5][mpe]") {
    HostEventIngress<4> ingress;
    REQUIRE(ingress.begin_block(1, 40, 8) == HostEventResult::Accepted);
    auto note = midi_event(1, 43, 1, 3, pulp::midi::MidiEvent::note_on(1, 60, 100));
    REQUIRE(ingress.publish(note) == HostEventResult::Accepted);
    HostEventRecord bend = note;
    bend.absolute_sample = 45;
    bend.producer_sequence = 2;
    bend.midi = pulp::midi::MidiEvent::pitch_bend(1, 10'000);
    REQUIRE(ingress.publish(bend) == HostEventResult::Accepted);
    std::array<HostEventRecord, 4> projected{};
    std::size_t count = 0;
    REQUIRE(ingress.project(projected, count) == HostEventResult::Accepted);
    REQUIRE(count == 2);

    pulp::midi::MpeVoiceTracker tracker;
    pulp::midi::MpeBuffer sidecar;
    sidecar.reserve(4);
    sidecar.set_realtime_capacity_limit(true);
    REQUIRE(pulp::host::derive_mpe(std::span<const HostEventRecord>(projected.data(), count), 40,
                                   tracker, sidecar) == HostEventResult::Accepted);
    REQUIRE(sidecar.size() == 2);
    REQUIRE(sidecar[0].sample_offset == 3);
    REQUIRE(sidecar[0].kind == pulp::midi::MpeExpressionEvent::Kind::NoteOn);
    REQUIRE(sidecar[1].sample_offset == 5);
    REQUIRE(sidecar[1].kind == pulp::midi::MpeExpressionEvent::Kind::PitchBend);
}

TEST_CASE("G5 host projection preserves absolute offsets and sidecars",
          "[host][graph][g5][events][mpe][ump]") {
    std::array<HostEventRecord, 3> records{};
    records[0] = midi_event(1, 43, 1, 3, pulp::midi::MidiEvent::note_on(1, 60, 100));
    records[1] = midi_event(1, 45, 2, 3, pulp::midi::MidiEvent::pitch_bend(1, 10'000));
    records[2] = midi_event(1, 46, 3, 3, pulp::midi::MidiEvent::cc(1, 74, 90));
    records[2].has_ump = true;
    records[2].ump.word_count = 2;
    records[2].ump.words[0] = 0x4010'3C00u;
    records[2].ump.words[1] = 0xFFFF'0000u;

    pulp::midi::MidiBuffer midi;
    pulp::midi::UmpBuffer ump;
    pulp::midi::MpeBuffer mpe;
    midi.reserve(4);
    midi.set_realtime_capacity_limit(true);
    ump.reserve(4);
    ump.set_realtime_capacity_limit(true);
    midi.attach_ump(&ump);
    mpe.reserve(512);
    mpe.set_realtime_capacity_limit(true);
    pulp::midi::MpeVoiceTracker tracker;

    REQUIRE(project_host_event_block(records, 40, 8, midi, &tracker, &mpe) ==
            HostEventResult::Accepted);
    REQUIRE(midi.size() == 3);
    REQUIRE(midi[0].sample_offset == 3);
    REQUIRE(midi[1].sample_offset == 5);
    REQUIRE(midi[2].sample_offset == 6);
    REQUIRE(ump.size() == 1);
    REQUIRE(ump[0].sample_offset == 6);
    REQUIRE(mpe.size() == 2);
    REQUIRE(mpe[0].sample_offset == 3);
    REQUIRE(mpe[1].sample_offset == 5);
}

TEST_CASE("G5 host projection refuses retiming and preserves destinations",
          "[host][graph][g5][events][negative-control]") {
    std::array<HostEventRecord, 1> late{
        midi_event(1, 39, 1, 3, pulp::midi::MidiEvent::note_on(1, 60, 100))};
    pulp::midi::MidiBuffer midi;
    midi.reserve(1);
    midi.set_realtime_capacity_limit(true);
    REQUIRE(project_host_event_block(late, 40, 8, midi) == HostEventResult::Late);
    REQUIRE(midi.empty());

    std::array<HostEventRecord, 1> accepted{
        midi_event(1, 40, 1, 3, pulp::midi::MidiEvent::note_on(1, 60, 100))};
    REQUIRE(midi.add(pulp::midi::MidiEvent::note_off(1, 60)));
    REQUIRE(project_host_event_block(accepted, 40, 8, midi) == HostEventResult::OutputCapacity);
    REQUIRE(midi.size() == 1);
}

TEST_CASE("G5 host projection preflights bounded MPE fanout",
          "[host][graph][g5][mpe][negative-control]") {
    pulp::midi::MpeVoiceTracker tracker;
    for (std::uint8_t note = 0; note < pulp::midi::MpeVoiceTracker::kMaxNotes; ++note)
        REQUIRE(tracker.process(pulp::midi::MidiEvent::note_on(1, note, 100)));

    std::array<HostEventRecord, 1> bend{
        midi_event(1, 0, 1, 3, pulp::midi::MidiEvent::pitch_bend(1, 10'000))};
    pulp::midi::MidiBuffer midi;
    pulp::midi::MpeBuffer mpe;
    midi.reserve(1);
    midi.set_realtime_capacity_limit(true);
    mpe.reserve(pulp::midi::MpeVoiceTracker::kMaxNotes - 1);
    mpe.set_realtime_capacity_limit(true);

    REQUIRE(project_host_event_block(bend, 0, 1, midi, &tracker, &mpe) ==
            HostEventResult::OutputCapacity);
    REQUIRE(midi.empty());
    REQUIRE(mpe.empty());
}

TEST_CASE("G5 host projection preflights pending MPE release on an empty block",
          "[host][graph][g5][mpe][negative-control]") {
    pulp::midi::MpeVoiceTracker tracker;
    REQUIRE(tracker.process(pulp::midi::MidiEvent::note_on(1, 60, 100)));
    tracker.on_note_lifecycle = [](const pulp::midi::MpeNoteState*,
                                   const pulp::midi::MpeNoteState*) { return false; };
    REQUIRE(tracker.process(pulp::midi::MidiEvent::note_off(1, 60)));
    REQUIRE(tracker.pending_note_off_count() == 1);

    pulp::midi::MidiBuffer midi;
    pulp::midi::MpeBuffer mpe(false);
    midi.reserve(1);
    midi.set_realtime_capacity_limit(true);
    mpe.set_realtime_capacity_limit(true);
    REQUIRE(project_host_event_block(std::span<const HostEventRecord>{}, 0, 1, midi,
                                     &tracker, &mpe) == HostEventResult::OutputCapacity);
    REQUIRE(midi.empty());
    REQUIRE(mpe.empty());
    REQUIRE(mpe.dropped_event_count() == 0);
    REQUIRE(tracker.pending_note_off_count() == 1);
}

TEST_CASE("G5 host projection preflights pending release plus event bound",
          "[host][graph][g5][mpe][negative-control]") {
    pulp::midi::MpeVoiceTracker tracker;
    REQUIRE(tracker.process(pulp::midi::MidiEvent::note_on(1, 60, 100)));
    tracker.on_note_lifecycle = [](const pulp::midi::MpeNoteState*,
                                   const pulp::midi::MpeNoteState*) { return false; };
    REQUIRE(tracker.process(pulp::midi::MidiEvent::note_off(1, 60)));
    REQUIRE(tracker.pending_note_off_count() == 1);

    std::array<HostEventRecord, 1> records{
        midi_event(1, 0, 1, 3, pulp::midi::MidiEvent::note_on(1, 62, 100))};
    pulp::midi::MidiBuffer midi;
    pulp::midi::MpeBuffer mpe;
    midi.reserve(1);
    midi.set_realtime_capacity_limit(true);
    constexpr auto event_bound = pulp::midi::MpeVoiceTracker::kMaxNotes + 2;
    mpe.reserve(event_bound);
    mpe.set_realtime_capacity_limit(true);
    REQUIRE(mpe.capacity() == event_bound);
    REQUIRE(project_host_event_block(records, 0, 1, midi, &tracker, &mpe) ==
            HostEventResult::OutputCapacity);
    REQUIRE(midi.empty());
    REQUIRE(mpe.empty());
    REQUIRE(mpe.dropped_event_count() == 0);
    REQUIRE(tracker.pending_note_off_count() == 1);
}

TEST_CASE("G5 host projection flushes a pending MPE release at full capacity",
          "[host][graph][g5][mpe]") {
    pulp::midi::MpeVoiceTracker tracker;
    REQUIRE(tracker.process(pulp::midi::MidiEvent::note_on(1, 60, 100)));
    tracker.on_note_lifecycle = [](const pulp::midi::MpeNoteState*,
                                   const pulp::midi::MpeNoteState*) { return false; };
    REQUIRE(tracker.process(pulp::midi::MidiEvent::note_off(1, 60)));
    REQUIRE(tracker.pending_note_off_count() == 1);

    pulp::midi::MidiBuffer midi;
    pulp::midi::MpeBuffer mpe(false);
    midi.reserve(1);
    midi.set_realtime_capacity_limit(true);
    mpe.reserve(1);
    mpe.set_realtime_capacity_limit(true);
    REQUIRE(project_host_event_block(std::span<const HostEventRecord>{}, 0, 1, midi,
                                     &tracker, &mpe) == HostEventResult::Accepted);
    REQUIRE(tracker.pending_note_off_count() == 0);
    REQUIRE(mpe.size() == 1);
    REQUIRE(mpe[0].sample_offset == 0);
    REQUIRE(mpe[0].kind == pulp::midi::MpeExpressionEvent::Kind::NoteOff);
    REQUIRE(mpe[0].state.note == 60);
    REQUIRE(mpe[0].state.channel == 1);
    REQUIRE(mpe[0].state.note_id != 0);
}

TEST_CASE("G5 host projection preserves all destinations on pending MPE rejection",
          "[host][graph][g5][mpe][ump][negative-control]") {
    pulp::midi::MpeVoiceTracker tracker;
    REQUIRE(tracker.process(pulp::midi::MidiEvent::note_on(1, 60, 100)));
    tracker.on_note_lifecycle = [](const pulp::midi::MpeNoteState*,
                                   const pulp::midi::MpeNoteState*) { return false; };
    REQUIRE(tracker.process(pulp::midi::MidiEvent::note_off(1, 60)));
    REQUIRE(tracker.pending_note_off_count() == 1);

    pulp::midi::MidiBuffer midi;
    pulp::midi::UmpBuffer ump;
    pulp::midi::MpeBuffer mpe(false);
    midi.reserve(2);
    midi.set_realtime_capacity_limit(true);
    ump.reserve(2);
    ump.set_realtime_capacity_limit(true);
    midi.attach_ump(&ump);
    mpe.reserve(pulp::midi::MpeVoiceTracker::kMaxNotes + 2);
    mpe.set_realtime_capacity_limit(true);

    auto midi_sentinel = pulp::midi::MidiEvent::note_off(2, 30);
    midi_sentinel.sample_offset = 7;
    REQUIRE(midi.add(midi_sentinel));
    const auto ump_sentinel = pulp::midi::UmpPacket::midi1_note_on(0, 2, 64, 90);
    REQUIRE(ump.add(ump_sentinel, 9));
    pulp::midi::MpeExpressionEvent mpe_sentinel{};
    mpe_sentinel.sample_offset = 11;
    mpe_sentinel.kind = pulp::midi::MpeExpressionEvent::Kind::Pressure;
    mpe_sentinel.state.channel = 3;
    mpe_sentinel.state.note = 42;
    mpe_sentinel.state.note_id = 17;
    REQUIRE(mpe.add(mpe_sentinel));

    std::array<HostEventRecord, 1> records{
        midi_event(1, 0, 1, 3, pulp::midi::MidiEvent::note_on(1, 62, 100))};
    records[0].has_ump = true;
    records[0].ump = pulp::midi::UmpPacket::midi1_note_on(0, 1, 62, 100);
    REQUIRE(mpe.capacity() == pulp::midi::MpeVoiceTracker::kMaxNotes + 2);
    REQUIRE(project_host_event_block(records, 0, 1, midi, &tracker, &mpe) ==
            HostEventResult::OutputCapacity);
    REQUIRE(midi.size() == 1);
    REQUIRE(midi[0].sample_offset == 7);
    REQUIRE(midi[0].is_note_off());
    REQUIRE(ump.size() == 1);
    REQUIRE(ump[0].sample_offset == 9);
    REQUIRE(ump[0].packet.words[0] == ump_sentinel.words[0]);
    REQUIRE(ump[0].packet.words[1] == ump_sentinel.words[1]);
    REQUIRE(mpe.size() == 1);
    REQUIRE(mpe[0].sample_offset == 11);
    REQUIRE(mpe[0].kind == pulp::midi::MpeExpressionEvent::Kind::Pressure);
    REQUIRE(mpe[0].state.channel == 3);
    REQUIRE(mpe[0].state.note == 42);
    REQUIRE(mpe[0].state.note_id == 17);
    REQUIRE(mpe.dropped_event_count() == 0);
    REQUIRE(tracker.pending_note_off_count() == 1);
}

TEST_CASE("G5 graph-authored event lane is partition invariant", "[host][graph][g5][events]") {
    GraphAuthoredEventLane<16> whole;
    GraphAuthoredEventLane<16> split;
    const std::array<std::size_t, 16> one{8};
    const std::array<std::size_t, 16> parts{1, 1, 3, 1, 2};
    const auto all = render_one(whole, one, 1);
    const auto divided = render_one(split, parts, 5);
    REQUIRE(all == divided);
    REQUIRE(all.size() == 3);
    REQUIRE(all[0].sample == 0);
    REQUIRE(all[1].sample == 1);
    REQUIRE(all[2].sample == 5);
    REQUIRE(all[0].sequence == 0);
}

TEST_CASE("G5 graph-authored event lane has checked refusal and attempt budget",
          "[host][graph][g5][events][negative-control]") {
    GraphAuthoredEventLane<4> lane;
    REQUIRE(lane.reset(1) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_block(1, 8, 1) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_sample(0) == GraphEventResult::Accepted);
    REQUIRE(lane.emit(2, 7, 1, 8) == GraphEventResult::StaleEpoch);
    REQUIRE(lane.emit(1, 7, 1, 9) == GraphEventResult::Future);
    REQUIRE(lane.emit(1, 7, 1, 7) == GraphEventResult::Late);
    for (std::uint16_t i = 0; i < 8; ++i)
        REQUIRE(lane.emit(1, 7, i, 8) == GraphEventResult::Accepted);
    REQUIRE(lane.emit(1, 7, 99, 8) == GraphEventResult::Overflow);
    // Three refused attempts + eight accepted records + five overflows
    // exhaust the fixed 16-attempt sample budget.
    for (int i = 0; i < 4; ++i)
        REQUIRE(lane.emit(1, 7, 100, 8) == GraphEventResult::Overflow);
    REQUIRE(lane.emit(1, 7, 101, 8) == GraphEventResult::AttemptLimit);
    REQUIRE(lane.counters().attempts == 16);
    REQUIRE(lane.counters().accepted == 8);
    REQUIRE(lane.counters().sequence == 16);
    REQUIRE(lane.counters().overflow == 5);
    REQUIRE(lane.counters().diagnostic_refusals == 3);
    REQUIRE(lane.begin_consumer() == GraphEventResult::Accepted);
    REQUIRE(lane.received().size() == 8);
    REQUIRE(lane.end_sample() == GraphEventResult::Accepted);
    REQUIRE(lane.end_block() == GraphEventResult::Accepted);
    REQUIRE(lane.reset(2) == GraphEventResult::Accepted);
}

TEST_CASE("G5 graph-authored lane recovers after cursor exhaustion",
          "[host][graph][g5][events][negative-control]") {
    GraphAuthoredEventLane<1> lane;
    REQUIRE(lane.reset(1) == GraphEventResult::Accepted);
    pulp::host::detail::GraphEventLaneTestAccess::set_cursor(
        lane, std::numeric_limits<std::uint64_t>::max());
    REQUIRE(lane.begin_block(1, 0, 1) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_sample(0) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_consumer() == GraphEventResult::Accepted);
    REQUIRE(lane.end_sample() == GraphEventResult::Accepted);
    REQUIRE(lane.end_block() == GraphEventResult::OutOfRange);
    REQUIRE_FALSE(lane.active());
    REQUIRE(lane.reset(2) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_block(2, 0, 1) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_sample(0) == GraphEventResult::Accepted);
    REQUIRE(lane.begin_consumer() == GraphEventResult::Accepted);
    REQUIRE(lane.end_sample() == GraphEventResult::Accepted);
    REQUIRE(lane.end_block() == GraphEventResult::Accepted);
    REQUIRE(lane.counters().cursor == 1);
}
