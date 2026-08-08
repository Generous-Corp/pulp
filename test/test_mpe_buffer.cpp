#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <pulp/format/adapter_boundary.hpp>
#include <pulp/format/mpe_expression.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/midi/mpe_buffer.hpp>
#include <pulp/midi/mpe_synth_voice.hpp>
#include <array>
#include <vector>

#include "harness/rt_allocation_probe.hpp"

using namespace pulp::midi;
using Kind = MpeExpressionEvent::Kind;
using Catch::Approx;

namespace {
MidiEvent channel_pressure(uint8_t channel, uint8_t value) {
    return {choc::midi::ShortMessage(
        static_cast<uint8_t>(0xD0 | (channel & 0x0F)), value, 0), 0, 0.0};
}

UmpPacket channel_pressure_ump(uint8_t group, uint8_t channel, uint32_t value) {
    UmpPacket p;
    p.word_count = 2;
    p.words[0] = (0x4u << 28)
        | (static_cast<uint32_t>(group & 0x0F) << 24)
        | (static_cast<uint32_t>(0xD0 | (channel & 0x0F)) << 16);
    p.words[1] = value;
    return p;
}

class SidecarProcessor final : public pulp::format::Processor {
public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor d;
        d.name = "MpeSidecarTest";
        d.supports_mpe = true;
        return d;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 MidiBuffer&, MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
};

class SidecarVoice final : public MpeSynthVoice {
public:
    void render(float*, int) override {}
};

template <typename Allocator>
std::size_t held_voice_count(const Allocator& allocator) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < allocator.polyphony(); ++i) {
        const auto& voice = allocator.voice(i);
        if (voice.active() && !voice.releasing()) ++count;
    }
    return count;
}
} // namespace

TEST_CASE("MpeBuffer records expression events from tracker", "[midi][mpe]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);

    offset = 10; tracker.process(MidiEvent::note_on(1, 60, 100));
    offset = 20; tracker.process(MidiEvent::pitch_bend(1, 16383));
    offset = 30; tracker.process(channel_pressure(1, 64));
    offset = 40; tracker.process(MidiEvent::cc(1, 74, 100));
    offset = 50; tracker.process(MidiEvent::note_off(1, 60));

    REQUIRE(buffer.size() == 5);
    REQUIRE(buffer[0].kind == Kind::NoteOn);
    REQUIRE(buffer[0].sample_offset == 10);
    REQUIRE(buffer[0].state.note == 60);

    REQUIRE(buffer[1].kind == Kind::PitchBend);
    REQUIRE(buffer[1].sample_offset == 20);
    REQUIRE(buffer[1].state.pitch_bend_semitones == Approx(48.0f).margin(0.01f));

    REQUIRE(buffer[2].kind == Kind::Pressure);
    REQUIRE(buffer[2].state.pressure > 0.4f);

    REQUIRE(buffer[3].kind == Kind::Timbre);
    REQUIRE(buffer[3].state.timbre == Approx(100.0f / 127.0f).margin(1e-6f));

    REQUIRE(buffer[4].kind == Kind::NoteOff);
    REQUIRE(buffer[4].sample_offset == 50);
}

TEST_CASE("MpeBuffer clear and sort", "[midi][mpe]") {
    MpeBuffer buffer;
    MpeExpressionEvent first{40, Kind::Timbre, {}};
    buffer.add(first);
    buffer.add({30, Kind::Pressure, {}});
    buffer.add({10, Kind::NoteOn, {}});
    buffer.add({20, Kind::PitchBend, {}});
    REQUIRE(buffer.size() == 4);

    buffer.sort();
    REQUIRE(buffer[0].sample_offset == 10);
    REQUIRE(buffer[1].sample_offset == 20);
    REQUIRE(buffer[2].sample_offset == 30);
    REQUIRE(buffer[3].sample_offset == 40);

    buffer.clear();
    REQUIRE(buffer.empty());
}

TEST_CASE("MpeBuffer groups equal sample offsets without dropping events",
          "[midi][mpe][issue-645]") {
    MpeBuffer buffer;
    MpeNoteState first;
    first.note = 60;
    MpeNoteState second;
    second.note = 64;
    MpeNoteState third;
    third.note = 67;

    buffer.add({12, Kind::NoteOn, first});
    buffer.add({12, Kind::Pressure, second});
    buffer.add({20, Kind::Timbre, third});
    buffer.sort();

    REQUIRE(buffer.size() == 3);
    REQUIRE(buffer[0].sample_offset == 12);
    REQUIRE(buffer[1].sample_offset == 12);
    REQUIRE(buffer[2].sample_offset == 20);
    const bool saw_first_equal_offset = buffer[0].state.note == 60
                                     || buffer[1].state.note == 60;
    const bool saw_second_equal_offset = buffer[0].state.note == 64
                                      || buffer[1].state.note == 64;
    REQUIRE(saw_first_equal_offset);
    REQUIRE(saw_second_equal_offset);
    REQUIRE(buffer[2].state.note == 67);
}

TEST_CASE("MpeBuffer accepts moved expression events and supports const iteration",
          "[midi][mpe][issue-645]") {
    MpeBuffer buffer;
    MpeExpressionEvent event;
    event.sample_offset = 7;
    event.kind = Kind::NoteOn;
    event.state.channel = 3;
    event.state.note = 72;
    event.state.velocity = 100;

    buffer.add(std::move(event));

    const MpeBuffer& const_buffer = buffer;
    int seen = 0;
    for (const auto& e : const_buffer) {
        REQUIRE(e.sample_offset == 7);
        REQUIRE(e.kind == Kind::NoteOn);
        REQUIRE(e.state.channel == 3);
        REQUIRE(e.state.note == 72);
        REQUIRE(e.state.velocity == 100);
        ++seen;
    }
    REQUIRE(seen == 1);
}

TEST_CASE("MpeConfig identifies lower and upper zone member boundaries",
          "[midi][mpe][issue-645]") {
    auto cfg = MpeConfig::dual(3, 2);

    REQUIRE(cfg.lower_zone.is_lower());
    REQUIRE(cfg.upper_zone.is_upper());
    REQUIRE(cfg.is_manager_channel(0));
    REQUIRE(cfg.is_manager_channel(15));

    REQUIRE(cfg.zone_for_channel(1) == &cfg.lower_zone);
    REQUIRE(cfg.zone_for_channel(3) == &cfg.lower_zone);
    REQUIRE(cfg.zone_for_channel(4) == nullptr);
    REQUIRE(cfg.zone_for_channel(13) == &cfg.upper_zone);
    REQUIRE(cfg.zone_for_channel(14) == &cfg.upper_zone);
    REQUIRE(cfg.zone_for_channel(12) == nullptr);
}

TEST_CASE("MpeZone rejects disabled and non-standard manager layouts",
          "[midi][mpe]") {
    MpeZone disabled_lower{0, 0};
    REQUIRE(disabled_lower.is_lower());
    REQUIRE_FALSE(disabled_lower.contains_channel(1));

    MpeZone disabled_upper{15, 0};
    REQUIRE(disabled_upper.is_upper());
    REQUIRE_FALSE(disabled_upper.contains_channel(14));

    MpeZone custom_manager{7, 4};
    REQUIRE_FALSE(custom_manager.is_lower());
    REQUIRE_FALSE(custom_manager.is_upper());
    REQUIRE_FALSE(custom_manager.contains_channel(7));
    REQUIRE_FALSE(custom_manager.contains_channel(8));
}

TEST_CASE("MpeVoiceTracker seeds new notes from cached member expression",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};

    REQUIRE(tracker.process(MidiEvent::pitch_bend(2, 16383)));
    REQUIRE(tracker.process(channel_pressure(2, 64)));
    REQUIRE(tracker.process(MidiEvent::cc(2, 74, 100)));
    REQUIRE(tracker.process(MidiEvent::note_on(2, 61, 90)));

    const auto* note = tracker.find(2, 61);
    REQUIRE(note != nullptr);
    REQUIRE(note->pitch_bend_semitones == Approx(48.0f).margin(0.01f));
    REQUIRE(note->pressure == Approx(64.0f / 127.0f).margin(1e-6f));
    REQUIRE(note->timbre == Approx(100.0f / 127.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker manager messages update zone state without notes",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::dual(4, 3)};
    tracker.set_manager_bend_range(2.0f);

    REQUIRE(tracker.process(MidiEvent::pitch_bend(0, 16383)));
    REQUIRE(tracker.process(channel_pressure(15, 127)));
    REQUIRE(tracker.process(MidiEvent::cc(15, 74, 64)));

    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones == Approx(2.0f).margin(0.01f));
    REQUIRE(tracker.upper_zone_state().pressure == Approx(1.0f).margin(1e-6f));
    REQUIRE(tracker.upper_zone_state().timbre == Approx(64.0f / 127.0f).margin(1e-6f));
}

TEST_CASE("bind_tracker_to_buffer uses the latest referenced sample offset",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);

    offset = 3;
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 100)));
    offset = 99;
    REQUIRE(tracker.process(MidiEvent::note_off(1, 60)));

    REQUIRE(buffer.size() == 2);
    REQUIRE(buffer[0].sample_offset == 3);
    REQUIRE(buffer[0].kind == Kind::NoteOn);
    REQUIRE(buffer[1].sample_offset == 99);
    REQUIRE(buffer[1].kind == Kind::NoteOff);
}

TEST_CASE("MpeBuffer callback appends are allocation-free after reserve",
          "[midi][mpe][rt-safety]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    buffer.reserve(4);
    buffer.set_realtime_capacity_limit(true);
    const auto prepared_capacity = buffer.capacity();
    REQUIRE(prepared_capacity >= 4);

    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);

    {
        pulp::test::RtAllocationProbe probe;

        offset = 4;
        REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 100)));
        offset = 8;
        REQUIRE(tracker.process(MidiEvent::pitch_bend(1, 16383)));
        offset = 12;
        REQUIRE(tracker.process(channel_pressure(1, 64)));
        offset = 16;
        REQUIRE(tracker.process(MidiEvent::cc(1, 74, 100)));

        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(buffer.size() == 4);
    REQUIRE(buffer.dropped_event_count() == 0);
    REQUIRE(buffer[0].kind == Kind::NoteOn);
    REQUIRE(buffer[0].sample_offset == 4);
    REQUIRE(buffer[3].kind == Kind::Timbre);
    REQUIRE(buffer[3].sample_offset == 16);

    while (buffer.size() < prepared_capacity) {
        REQUIRE(buffer.add({100, Kind::Pressure, {}}));
    }
    REQUIRE(buffer.dropped_event_count() == 0);
    const auto held_id = tracker.find(1, 60)->note_id;

    {
        pulp::test::RtAllocationProbe probe;
        offset = 20;
        REQUIRE(tracker.process(MidiEvent::note_off(1, 60)));
        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(buffer.size() == prepared_capacity);
    REQUIRE(buffer.dropped_event_count() == 1);
    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.pending_note_off_count() == 1);

    REQUIRE(tracker.process(MidiEvent::note_on(1, 62, 100)));
    REQUIRE(tracker.active_count() == 0);
    buffer.clear();
    offset = 0;
    REQUIRE(tracker.flush_pending_note_offs());
    REQUIRE(buffer.size() == 1);
    REQUIRE(buffer[0].kind == Kind::NoteOff);
    REQUIRE(buffer[0].state.note_id == held_id);
    REQUIRE(tracker.process(MidiEvent::note_on(1, 62, 100)));
    REQUIRE(tracker.find(1, 62)->note_id == held_id + 1);
}

TEST_CASE("MpeSidecar drains a deferred release before next-block input",
          "[midi][mpe][lifecycle][sidecar][rt-safety]") {
    pulp::format::boundary::MpeSidecar sidecar;
    SidecarProcessor processor;
    MpeVoiceAllocator<SidecarVoice> allocator{2};
    sidecar.configure(true);
    sidecar.reserve(sidecar.buffer.capacity());

    // Fill block N's production sidecar buffer, then deliver a physical
    // NoteOff that cannot fit and must be retained for the next run().
    std::vector<MidiEvent> first_block;
    first_block.reserve(sidecar.buffer.capacity() + 1);
    first_block.push_back(MidiEvent::note_on(1, 60, 100));
    while (first_block.size() < sidecar.buffer.capacity()) {
        auto bend = MidiEvent::pitch_bend(1, 8192);
        bend.sample_offset = 7;
        first_block.push_back(bend);
    }
    auto physical_off = MidiEvent::note_off(1, 60);
    physical_off.sample_offset = 31;
    first_block.push_back(physical_off);
    REQUIRE(sidecar.run(processor, first_block));

    REQUIRE(processor.mpe_input() == &sidecar.buffer);
    REQUIRE(sidecar.buffer.size() == sidecar.buffer.capacity());
    REQUIRE(sidecar.tracker.pending_note_off_count() == 1);
    const auto retired_generation = sidecar.buffer[0].state.note_id;
    allocator.dispatch_all(sidecar.buffer);
    REQUIRE(held_voice_count(allocator) == 1);

    // Block N+1 starts at the same offset as the deferred release. Production
    // run() must publish retirement first, then the new generation, without an
    // allocation on the audio thread.
    const std::array second_block{MidiEvent::note_on(1, 60, 110)};
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(sidecar.run(processor, second_block));
        allocator.dispatch_all(sidecar.buffer);
        REQUIRE_FALSE(probe.saw_allocation());
    }

    REQUIRE(sidecar.buffer.size() == 2);
    REQUIRE(sidecar.buffer[0].kind == Kind::NoteOff);
    REQUIRE(sidecar.buffer[0].sample_offset == 0);
    REQUIRE(sidecar.buffer[0].state.note_id == retired_generation);
    REQUIRE(sidecar.buffer[1].kind == Kind::NoteOn);
    REQUIRE(sidecar.buffer[1].sample_offset == 0);
    REQUIRE(sidecar.buffer[1].state.note_id == retired_generation + 1);
    REQUIRE(sidecar.tracker.pending_note_off_count() == 0);
    REQUIRE(held_voice_count(allocator) == 1);
    REQUIRE_FALSE(allocator.last_was_glide());
}

TEST_CASE("MpeSidecar reset clears buffer and deferred releases across reactivation cycles",
          "[midi][mpe][lifecycle][sidecar][reset]") {
    pulp::format::boundary::MpeSidecar sidecar;
    SidecarProcessor processor;
    sidecar.configure(true);
    sidecar.reserve(sidecar.buffer.capacity());

    std::vector<MidiEvent> overflowing_block;
    overflowing_block.reserve(sidecar.buffer.capacity() + 1);
    overflowing_block.push_back(MidiEvent::note_on(1, 60, 100));
    while (overflowing_block.size() < sidecar.buffer.capacity()) {
        auto bend = MidiEvent::pitch_bend(1, 8192);
        bend.sample_offset = 4;
        overflowing_block.push_back(bend);
    }
    auto physical_off = MidiEvent::note_off(1, 60);
    physical_off.sample_offset = 15;
    overflowing_block.push_back(physical_off);
    REQUIRE(sidecar.run(processor, overflowing_block));
    REQUIRE(sidecar.tracker.pending_note_off_count() == 1);

    MpeNoteGeneration previous_generation = sidecar.buffer[0].state.note_id;
    for (int cycle = 0; cycle < 3; ++cycle) {
        {
            pulp::test::RtAllocationProbe probe;
            sidecar.reset();
            REQUIRE_FALSE(probe.saw_allocation());
        }
        REQUIRE(sidecar.buffer.empty());
        REQUIRE(sidecar.tracker.active_count() == 0);
        REQUIRE(sidecar.tracker.pending_note_off_count() == 0);
        REQUIRE(sidecar.current_sample_offset == 0);

        auto note_on = MidiEvent::note_on(
            1, static_cast<uint8_t>(61 + cycle), 90);
        note_on.sample_offset = 9;
        const std::array input{note_on};
        REQUIRE(sidecar.run(processor, input));
        REQUIRE(sidecar.buffer.size() == 1);
        REQUIRE(sidecar.buffer[0].kind == Kind::NoteOn);
        REQUIRE(sidecar.buffer[0].sample_offset == 9);
        REQUIRE(sidecar.buffer[0].state.note_id > previous_generation);
        previous_generation = sidecar.buffer[0].state.note_id;
    }
}

TEST_CASE("MpeSidecar reconciles tracker state after an expression append drops",
          "[midi][mpe][sidecar][overflow][rt-safety]") {
    pulp::format::boundary::MpeSidecar sidecar;
    SidecarProcessor processor;
    sidecar.configure(true);
    sidecar.reserve(sidecar.buffer.capacity());

    std::vector<MidiEvent> overflowing_block;
    overflowing_block.reserve(sidecar.buffer.capacity() + 1);
    overflowing_block.push_back(MidiEvent::note_on(1, 60, 100));
    while (overflowing_block.size() <= sidecar.buffer.capacity()) {
        overflowing_block.push_back(MidiEvent::pitch_bend(1, 16383));
    }

    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE_FALSE(sidecar.run(processor, overflowing_block));
        REQUIRE_FALSE(probe.saw_allocation());
    }
    REQUIRE(processor.mpe_input() == &sidecar.buffer);
    REQUIRE(sidecar.buffer.empty());
    REQUIRE(sidecar.tracker.active_count() == 0);
    REQUIRE(sidecar.tracker.pending_note_off_count() == 0);

    const std::array next_block{MidiEvent::note_on(1, 62, 90)};
    REQUIRE(sidecar.run(processor, next_block));
    REQUIRE(sidecar.buffer.size() == 1);
    REQUIRE(sidecar.buffer[0].kind == Kind::NoteOn);
    REQUIRE(sidecar.buffer[0].state.note == 62);
}

TEST_CASE("MpeBuffer emits retrigger retirement and replacement atomically",
          "[midi][mpe][lifecycle]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    int32_t offset = 7;
    bind_tracker_to_buffer(tracker, buffer, offset);
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 90)));
    const auto first = tracker.find(1, 60)->note_id;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 110)));
        REQUIRE_FALSE(probe.saw_allocation());
    }
    const auto second = tracker.find(1, 60)->note_id;
    REQUIRE(buffer.size() == 3);
    REQUIRE(buffer[1].kind == Kind::NoteOff);
    REQUIRE(buffer[1].state.note_id == first);
    REQUIRE(buffer[2].kind == Kind::NoteOn);
    REQUIRE(buffer[2].state.note_id == second);
    {
        pulp::test::RtAllocationProbe probe;
        buffer.sort();
        REQUIRE_FALSE(probe.saw_allocation());
    }
    REQUIRE(buffer[1].kind == Kind::NoteOff);
    REQUIRE(buffer[2].kind == Kind::NoteOn);
}

TEST_CASE("MpeBuffer retrigger capacity boundary is atomic",
          "[midi][mpe][lifecycle][overflow]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    buffer.set_realtime_capacity_limit(true);
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 90)));
    const auto before = *tracker.find(1, 60);
    while (buffer.size() + 1 < buffer.capacity()) {
        REQUIRE(buffer.add({0, Kind::Pressure, {}}));
    }
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 110)));
    REQUIRE(buffer.capacity() - buffer.size() == 1);
    REQUIRE(buffer.dropped_event_count() == 2);
    REQUIRE(tracker.find(1, 60)->note_id == before.note_id);
    REQUIRE(tracker.find(1, 60)->velocity == before.velocity);

    buffer.clear();
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 111)));
    REQUIRE(buffer.size() == 2);
    REQUIRE(buffer[0].kind == Kind::NoteOff);
    REQUIRE(buffer[1].kind == Kind::NoteOn);
    REQUIRE(buffer[1].state.note_id == before.note_id + 1);
}

TEST_CASE("MpeBuffer accepts retrigger at exact realtime capacity",
          "[midi][mpe][lifecycle][capacity]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    buffer.set_realtime_capacity_limit(true);
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 90)));
    while (buffer.size() + 2 < buffer.capacity()) {
        REQUIRE(buffer.add({0, Kind::Pressure, {}}));
    }
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 110)));
    REQUIRE(buffer.size() == buffer.capacity());
    REQUIRE(buffer.dropped_event_count() == 0);
    REQUIRE(buffer[buffer.size() - 2].kind == Kind::NoteOff);
    REQUIRE(buffer[buffer.size() - 1].kind == Kind::NoteOn);
}

TEST_CASE("MpeBuffer rejected fresh note-on does not consume generation",
          "[midi][mpe][lifecycle][overflow]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    buffer.set_realtime_capacity_limit(true);
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);
    while (buffer.size() < buffer.capacity()) {
        REQUIRE(buffer.add({0, Kind::Pressure, {}}));
    }
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 90)));
    REQUIRE(tracker.active_count() == 0);
    buffer.clear();
    REQUIRE(tracker.process(MidiEvent::note_on(1, 60, 91)));
    REQUIRE(tracker.find(1, 60)->note_id == 1);
}

TEST_CASE("MIDI 2 UMP retrigger uses retirement-before-start lifecycle",
          "[midi][mpe][ump][lifecycle]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    int32_t offset = 12;
    bind_tracker_to_buffer(tracker, buffer, offset);
    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 2, 67, 0xA000)));
    const auto first = tracker.find(2, 67)->note_id;
    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 2, 67, 0xF000)));
    const auto second = tracker.find(2, 67)->note_id;
    REQUIRE(buffer[1].kind == Kind::NoteOff);
    REQUIRE(buffer[1].state.note_id == first);
    REQUIRE(buffer[2].kind == Kind::NoteOn);
    REQUIRE(buffer[2].state.note_id == second);
    REQUIRE(tracker.process(UmpPacket::note_off_2(0, 2, 67, 0)));
    REQUIRE(buffer[3].kind == Kind::NoteOff);
    REQUIRE(buffer[3].state.note_id == second);
}

TEST_CASE("MpeVoiceTracker UMP manager and member events feed callbacks",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    MpeBuffer buffer;
    int32_t offset = 0;
    bind_tracker_to_buffer(tracker, buffer, offset);

    offset = 5;
    REQUIRE(tracker.process(UmpPacket::pitch_bend_2(0, 0, 0xFFFFFFFFu)));
    REQUIRE(buffer.empty());
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones > 1.9f);

    offset = 10;
    REQUIRE(tracker.process(UmpPacket::note_on_2(0, 3, 67, 0xFFFF)));
    offset = 20;
    REQUIRE(tracker.process(channel_pressure_ump(0, 3, 0xFFFFFFFFu)));
    offset = 30;
    REQUIRE(tracker.process(UmpPacket::registered_per_note_cc(0, 3, 67, 74,
                                                              0xFFFFFFFFu)));

    REQUIRE(buffer.size() == 3);
    REQUIRE(buffer[0].kind == Kind::NoteOn);
    REQUIRE(buffer[0].sample_offset == 10);
    REQUIRE(buffer[0].state.velocity == 127);
    REQUIRE(buffer[1].kind == Kind::Pressure);
    REQUIRE(buffer[1].state.pressure == Approx(1.0f).margin(1e-6f));
    REQUIRE(buffer[2].kind == Kind::Timbre);
    REQUIRE(buffer[2].state.timbre == Approx(1.0f).margin(1e-6f));
}

TEST_CASE("MpeVoiceTracker reset and config changes clear active state",
          "[midi][mpe][issue-645]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(1, 60, 100));
    tracker.process(MidiEvent::pitch_bend(1, 16383));
    REQUIRE(tracker.active_count() == 1);
    REQUIRE(tracker.member_bend_range() == Approx(48.0f));

    tracker.set_member_bend_range(-1.0f);
    tracker.set_manager_bend_range(0.0f);
    REQUIRE(tracker.member_bend_range() == Approx(48.0f));
    REQUIRE(tracker.manager_bend_range() == Approx(2.0f));

    tracker.set_config(MpeConfig::dual(2, 2));
    REQUIRE(tracker.active_count() == 0);
    REQUIRE(tracker.find(1, 60) == nullptr);
    REQUIRE(tracker.config().upper_zone.member_channels == 2);
    REQUIRE(tracker.lower_zone_state().pitch_bend_semitones == Approx(0.0f));
}

TEST_CASE("Processor::supports_mpe defaults to false", "[midi][mpe]") {
    pulp::format::PluginDescriptor desc;
    REQUIRE_FALSE(desc.supports_mpe);
}

// ---------------------------------------------------------------------------
// Shared per-note-expression → MIDI 1.0 synthesis (mpe_expression.hpp).
//
// Each format adapter decodes its own host expression event and then
// synthesizes the same three channel-wide messages through these helpers, so
// the synthesis contract is pinned once here rather than once per format.
// ---------------------------------------------------------------------------

namespace {
/// Recover the 14-bit value from a pitch-bend event's data bytes.
int bend14_of(const MidiEvent& ev) {
    return static_cast<int>(ev.data()[1] & 0x7F)
         | (static_cast<int>(ev.data()[2] & 0x7F) << 7);
}
}  // namespace

TEST_CASE("MPE expression to_7bit rounds and clamps the normalized axis",
          "[midi][mpe][parity]") {
    REQUIRE(pulp::format::mpe::to_7bit(0.0) == 0);
    REQUIRE(pulp::format::mpe::to_7bit(1.0) == 127);
    // Round-to-nearest, not truncation: 0.5 * 127 = 63.5 → 64.
    REQUIRE(pulp::format::mpe::to_7bit(0.5) == 64);
    // Out-of-range input must not produce a malformed data byte.
    REQUIRE(pulp::format::mpe::to_7bit(-1.0) == 0);
    REQUIRE(pulp::format::mpe::to_7bit(2.0) == 127);
}

TEST_CASE("MPE expression pitch maps semitones onto the member bend range",
          "[midi][mpe][parity]") {
    auto center = pulp::format::mpe::pitch_bend_from_semitones(3, 0.0);
    REQUIRE(center.is_pitch_bend());
    REQUIRE(center.channel() == 3);
    REQUIRE(bend14_of(center) == 8192);
    REQUIRE(center.sample_offset == 0);

    // A full member-range detune reaches the 14-bit ends.
    const double full = MpeVoiceTracker::kDefaultMemberBendSemitones;
    REQUIRE(bend14_of(pulp::format::mpe::pitch_bend_from_semitones(0, full))
            == 16383);
    REQUIRE(bend14_of(pulp::format::mpe::pitch_bend_from_semitones(0, -full))
            == 1);

    // Beyond the member range saturates rather than wrapping.
    REQUIRE(bend14_of(pulp::format::mpe::pitch_bend_from_semitones(0, full * 10.0))
            == 16383);
    REQUIRE(bend14_of(pulp::format::mpe::pitch_bend_from_semitones(0, -full * 10.0))
            == 1);

    // Half the member range lands halfway through the bend span.
    REQUIRE(bend14_of(pulp::format::mpe::pitch_bend_from_semitones(0, full * 0.5))
            == 12288);
}

TEST_CASE("MPE expression pressure emits channel pressure on the right channel",
          "[midi][mpe][parity]") {
    auto ev = pulp::format::mpe::channel_pressure(5, 1.0);
    REQUIRE(ev.data()[0] == static_cast<uint8_t>(0xD0 | 5));
    REQUIRE(ev.data()[1] == 127);
    REQUIRE(ev.sample_offset == 0);
    REQUIRE(pulp::format::mpe::channel_pressure(0, 0.0).data()[1] == 0);
    // The channel is masked to the low nibble.
    REQUIRE(pulp::format::mpe::channel_pressure(0x1F, 0.0).data()[0]
            == static_cast<uint8_t>(0xD0 | 0x0F));
}

TEST_CASE("MPE expression timbre emits CC 74", "[midi][mpe][parity]") {
    auto ev = pulp::format::mpe::timbre_cc(2, 1.0);
    REQUIRE(ev.is_cc());
    REQUIRE(ev.channel() == 2);
    REQUIRE(pulp::format::mpe::kTimbreController == 74);
    REQUIRE(ev.cc_number() == 74);
    REQUIRE(ev.cc_value() == 127);
    REQUIRE(ev.sample_offset == 0);
}

TEST_CASE("MPE expression pitch round-trips through the voice tracker",
          "[midi][mpe][parity]") {
    // The adapters synthesize a channel-wide bend from a semitone request and
    // rely on the tracker's inverse expansion to recover it per note.
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(1, 60, 100));
    const double request = MpeVoiceTracker::kDefaultMemberBendSemitones * 0.5;
    tracker.process(pulp::format::mpe::pitch_bend_from_semitones(1, request));
    const auto* voice = tracker.find(1, 60);
    REQUIRE(voice != nullptr);
    REQUIRE(voice->pitch_bend_semitones
            == Approx(static_cast<float>(request)).margin(0.01));
}

TEST_CASE("MPE expression pressure and timbre reach the tracked note",
          "[midi][mpe][parity]") {
    MpeVoiceTracker tracker{MpeConfig::standard_lower(15)};
    tracker.process(MidiEvent::note_on(1, 60, 100));
    tracker.process(pulp::format::mpe::channel_pressure(1, 1.0));
    tracker.process(pulp::format::mpe::timbre_cc(1, 0.0));
    const auto* voice = tracker.find(1, 60);
    REQUIRE(voice != nullptr);
    REQUIRE(voice->pressure == Approx(1.0f).margin(0.01));
    REQUIRE(voice->timbre == Approx(0.0f).margin(0.01));
}
