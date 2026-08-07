#include "harness/rt_allocation_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/voice_runtime_facade.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>

using namespace pulp::audio;
using namespace pulp::midi;
using Catch::Matchers::WithinAbs;

namespace {

class FacadeVoice : public SynthesiserVoice {
  public:
    void on_note_on(const SynthesiserNote& note) override {
        SynthesiserVoice::on_note_on(note);
        ++note_on_count;
        render_cursor = 0;
    }
    void on_note_off() override {
        SynthesiserVoice::on_note_off();
        ++note_off_count;
    }
    void on_choke(float fade_ms) override {
        ++choke_count;
        last_choke_fade_ms = fade_ms;
        SynthesiserVoice::on_choke(fade_ms);
    }
    void on_pitch_bend(float semitones) override {
        last_pitch_bend = semitones;
    }
    void on_aftertouch(float pressure) override {
        last_pressure = pressure;
    }
    void on_cc(std::uint8_t controller, std::uint8_t value) override {
        last_controller = controller;
        last_controller_value = value;
    }
    void render(float* output, int frames) override {
        for (int frame = 0; frame < frames; ++frame)
            output[frame] += static_cast<float>(++render_cursor);
    }
    void reset() override {
        SynthesiserVoice::reset();
        render_cursor = 0;
    }

    int note_on_count = 0;
    int note_off_count = 0;
    int choke_count = 0;
    float last_choke_fade_ms = 0.0f;
    float last_pitch_bend = 0.0f;
    float last_pressure = 0.0f;
    std::uint8_t last_controller = 0;
    std::uint8_t last_controller_value = 0;
    std::uint64_t render_cursor = 0;
};

MidiEvent note_event(std::uint8_t status, std::uint8_t note, std::uint8_t velocity, int offset) {
    return {choc::midi::ShortMessage(status, note, velocity), offset, 0.0};
}

template <std::size_t Count>
void prepare_modulation(std::array<VoiceModulationBuffer, Count>& buffers,
                        std::uint32_t frames = 16) {
    for (auto& buffer : buffers) {
        REQUIRE(buffer.prepare({
            .max_lanes = VoiceNoteModulationBridge::lane_count,
            .max_frames = frames,
        }));
    }
}

VoiceNoteModulationInput note_modulation(int note = 60) {
    return {
        .note = note,
        .reference_note = 60,
        .velocity = 64,
        .gate = true,
        .pitch_bend_normalized = 0.5f,
        .bend_range_semitones = 2.0f,
        .pressure = 0.25f,
        .timbre = 0.75f,
        .expression = 0.5f,
    };
}

} // namespace

static_assert(
    std::is_same_v<decltype(VoiceRuntimeFacade(std::declval<Synthesiser<FacadeVoice>&>())),
                   VoiceRuntimeFacade<Synthesiser<FacadeVoice>>>);
static_assert(
    std::is_same_v<decltype(VoiceRuntimeFacade(std::declval<InstrumentVoiceAllocator&>())),
                   VoiceRuntimeFacade<InstrumentVoiceAllocator>>);
static_assert(sizeof(VoiceRuntimeFacade<Synthesiser<FacadeVoice>>) == sizeof(void*));
static_assert(sizeof(VoiceRuntimeFacade<InstrumentVoiceAllocator>) == sizeof(void*));

TEST_CASE("MIDI voice facade preserves pedals choke and deterministic stealing",
          "[audio][voice-runtime][midi][oracle]") {
    Synthesiser<FacadeVoice> synthesiser(2);
    VoiceRuntimeFacade facade(synthesiser);
    REQUIRE(facade.set_steal_strategy(VoiceStealStrategy::Oldest));
    REQUIRE_FALSE(facade.set_steal_strategy(static_cast<VoiceStealStrategy>(255)));
    REQUIRE(facade.steal_strategy() == VoiceStealStrategy::Oldest);
    REQUIRE(facade.set_pitch_bend_range_semitones(12.0f));
    REQUIRE(facade.pitch_bend_range_semitones() == 12.0f);
    REQUIRE_FALSE(facade.set_pitch_bend_range_semitones(0.0f));
    REQUIRE_FALSE(facade.set_pitch_bend_range_semitones(-1.0f));
    REQUIRE_FALSE(facade.set_pitch_bend_range_semitones(std::numeric_limits<float>::infinity()));
    REQUIRE_FALSE(facade.set_pitch_bend_range_semitones(std::numeric_limits<float>::quiet_NaN()));
    REQUIRE(facade.pitch_bend_range_semitones() == 12.0f);

    facade.note_on(0, 60, 100, 0, 4);
    facade.control_change(0, 64, 127);
    facade.note_off(0, 60);
    REQUIRE(facade.voice(0).note().sustained);
    REQUIRE_FALSE(facade.voice(0).releasing());
    facade.control_change(0, 64, 0);
    REQUIRE(facade.voice(0).releasing());

    synthesiser.reset();
    facade.note_on(0, 60, 100, 0, 4);
    facade.control_change(0, 66, 127);
    facade.note_off(0, 60);
    REQUIRE(facade.voice(0).note().sostenuto);
    facade.control_change(0, 66, 0);
    REQUIRE(facade.voice(0).releasing());

    synthesiser.reset();
    facade.control_change(0, 67, 127);
    facade.note_on(0, 42, 100, 0, 7);
    REQUIRE(facade.voice(0).note().soft_pedal);
    facade.note_on(0, 46, 100, 0, 7, true, 3.5f);
    REQUIRE(facade.voice(0).choke_count == 1);
    REQUIRE_THAT(facade.voice(0).last_choke_fade_ms, WithinAbs(3.5f, 1e-6f));

    synthesiser.reset();
    facade.note_on(0, 60, 100);
    facade.note_on(0, 64, 100);
    facade.note_on(0, 67, 100);
    REQUIRE(facade.voice(0).note().note == 67);
    REQUIRE(facade.voice(1).note().note == 64);
    REQUIRE(facade.telemetry().steal_count == 1);
}

TEST_CASE("MIDI voice facade rendering is sample partition deterministic",
          "[audio][voice-runtime][midi][partition]") {
    Synthesiser<FacadeVoice> whole_owner(2);
    Synthesiser<FacadeVoice> split_owner(2);
    VoiceRuntimeFacade whole(whole_owner);
    VoiceRuntimeFacade split(split_owner);

    MidiBuffer whole_events;
    whole_events.add(note_event(0x90, 60, 100, 0));
    whole_events.add(note_event(0x90, 64, 100, 4));
    whole_events.add(note_event(0x80, 60, 0, 8));
    std::array<float, 12> whole_output{};
    whole.process(whole_events, whole_output.data(), 12);

    std::array<float, 12> split_output{};
    for (int partition = 0; partition < 3; ++partition) {
        MidiBuffer events;
        if (partition == 0)
            events.add(note_event(0x90, 60, 100, 0));
        else if (partition == 1)
            events.add(note_event(0x90, 64, 100, 0));
        else
            events.add(note_event(0x80, 60, 0, 0));
        split.process(events, split_output.data() + partition * 4, 4);
    }
    REQUIRE(split_output == whole_output);

    MidiBuffer empty;
    const auto before = split.telemetry();
    split.process(empty, nullptr, 0);
    REQUIRE(split.telemetry().active_voice_count == before.active_voice_count);
}

TEST_CASE("Note modulation bridge writes documented units transactionally",
          "[audio][voice-runtime][modulation][oracle][fault]") {
    VoiceModulationBuffer buffer;
    REQUIRE(buffer.prepare({.max_lanes = 6, .max_frames = 8}));
    const auto input = note_modulation(62);
    const auto result = VoiceNoteModulationBridge::write(buffer, 8, input);
    REQUIRE(result.ok);
    const auto block = buffer.block();
    REQUIRE(block.frame_count == 8);
    REQUIRE(block.lanes.size() == 6);
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::PitchCents, 7), WithinAbs(300.0f, 1e-6f));
    REQUIRE_THAT(block.value_at(VoiceModulationTarget::Gain, 0), WithinAbs(64.0f / 127.0f, 1e-6f));
    REQUIRE(block.value_at(VoiceModulationTarget::Aux0, 0) == 1.0f);
    REQUIRE(block.value_at(VoiceModulationTarget::Pressure, 0) == 0.25f);
    REQUIRE(block.value_at(VoiceModulationTarget::Timbre, 0) == 0.75f);
    REQUIRE(block.value_at(VoiceModulationTarget::Aux1, 0) == 0.5f);

    auto duplicate = VoiceNoteModulationRouting{};
    duplicate.gate = duplicate.velocity;
    REQUIRE(VoiceNoteModulationBridge::write(buffer, 8, input, duplicate).status ==
            VoiceNoteModulationStatus::InvalidRouting);
    REQUIRE(buffer.block().lanes.data() == block.lanes.data());
    REQUIRE(buffer.block().lanes.size() == 6);

    auto invalid_enum = VoiceNoteModulationRouting{};
    invalid_enum.gate = static_cast<VoiceModulationTarget>(255);
    REQUIRE(VoiceNoteModulationBridge::write(buffer, 8, input, invalid_enum).status ==
            VoiceNoteModulationStatus::InvalidRouting);
    REQUIRE(VoiceNoteModulationBridge::write(buffer, 0, input).status ==
            VoiceNoteModulationStatus::InvalidFrameCount);
    REQUIRE(
        VoiceNoteModulationBridge::write(buffer, std::numeric_limits<std::uint32_t>::max(), input)
            .status == VoiceNoteModulationStatus::InvalidFrameCount);
    auto nonfinite = input;
    nonfinite.expression = std::numeric_limits<float>::quiet_NaN();
    REQUIRE(VoiceNoteModulationBridge::write(buffer, 8, nonfinite).status ==
            VoiceNoteModulationStatus::InvalidController);
    nonfinite = input;
    nonfinite.bend_range_semitones = std::numeric_limits<float>::max();
    REQUIRE(VoiceNoteModulationBridge::write(buffer, 8, nonfinite).status ==
            VoiceNoteModulationStatus::InvalidBendRange);
    REQUIRE(buffer.block().lanes.size() == 6);
}

TEST_CASE("Allocator facade preserves termination reasons and clears reused slots",
          "[audio][voice-runtime][allocator][steal][choke]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(2));
    VoiceRuntimeFacade facade(allocator);
    REQUIRE(facade.set_steal_policy(VoiceStealPolicy::PreferSameVoiceGroupOldest));
    REQUIRE_FALSE(facade.set_steal_policy(static_cast<VoiceStealPolicy>(255)));
    REQUIRE(facade.steal_policy() == VoiceStealPolicy::PreferSameVoiceGroupOldest);
    facade.set_termination_fade_frames(std::numeric_limits<std::uint32_t>::max());
    std::array<VoiceModulationBuffer, 2> modulation;
    prepare_modulation(modulation);
    std::array<VoiceTermination, 2> terminations{};

    const auto first =
        facade.trigger({.note = 60, .sample_id = 1, .voice_group = 9}, terminations, modulation);
    REQUIRE(first.ok());
    REQUIRE(VoiceNoteModulationBridge::write(modulation[first.allocation.voice_index], 8,
                                             note_modulation(60))
                .ok);
    const auto second =
        facade.trigger({.note = 62, .sample_id = 2, .voice_group = 9}, terminations, modulation);
    REQUIRE(second.ok());
    REQUIRE(VoiceNoteModulationBridge::write(modulation[second.allocation.voice_index], 8,
                                             note_modulation(62))
                .ok);

    const auto stolen =
        facade.trigger({.note = 64, .sample_id = 3, .voice_group = 9}, terminations, modulation);
    REQUIRE(stolen.ok());
    REQUIRE(stolen.allocation.stolen);
    REQUIRE(stolen.allocation.stolen_voice_id == first.allocation.voice_id);
    REQUIRE(terminations[0].reason == VoiceTerminationReason::Stolen);
    REQUIRE(terminations[0].fade_out_frames == std::numeric_limits<std::uint32_t>::max());
    REQUIRE(modulation[stolen.allocation.voice_index].block().empty());
    REQUIRE_FALSE(modulation[second.allocation.voice_index].block().empty());

    allocator.reset();
    for (auto& buffer : modulation)
        buffer.reset();
    const auto open_hat =
        facade.trigger({.note = 46, .sample_id = 4, .choke_group = 7}, terminations, modulation);
    REQUIRE(open_hat.ok());
    REQUIRE(VoiceNoteModulationBridge::write(modulation[open_hat.allocation.voice_index], 4,
                                             note_modulation(46))
                .ok);
    const auto closed_hat =
        facade.trigger({.note = 42, .sample_id = 5, .choke_group = 7}, terminations, modulation);
    REQUIRE(closed_hat.ok());
    REQUIRE(closed_hat.allocation.choked_count == 1);
    REQUIRE(terminations[0].reason == VoiceTerminationReason::Choked);
    REQUIRE(terminations[0].voice_id == open_hat.allocation.voice_id);
    REQUIRE(modulation[open_hat.allocation.voice_index].block().empty());
}

TEST_CASE("Allocator facade rejects borrowed capacity faults without mutation",
          "[audio][voice-runtime][allocator][fault]") {
    InstrumentVoiceAllocator allocator;
    VoiceRuntimeFacade facade(allocator);
    std::array<VoiceModulationBuffer, 2> modulation;
    prepare_modulation(modulation);
    std::array<VoiceTermination, 2> terminations{};

    REQUIRE(facade.trigger({.note = 60, .sample_id = 1}, terminations, modulation).status ==
            VoiceRuntimeFacadeStatus::OwnerNotPrepared);
    REQUIRE(allocator.prepare(2));
    REQUIRE(facade
                .trigger({.note = 60, .sample_id = 1},
                         std::span<VoiceTermination>{terminations}.first(1), modulation)
                .status == VoiceRuntimeFacadeStatus::InvalidTerminationCapacity);
    REQUIRE(facade
                .trigger({.note = 60, .sample_id = 1}, terminations,
                         std::span<VoiceModulationBuffer>{modulation}.first(1))
                .status == VoiceRuntimeFacadeStatus::InvalidModulationCapacity);
    REQUIRE(facade.trigger({.note = -1, .sample_id = 1}, terminations, modulation).status ==
            VoiceRuntimeFacadeStatus::OwnerRejected);
    REQUIRE(allocator.allocated_voice_count() == 0);

    const auto allocated = facade.trigger({.note = 60, .sample_id = 1}, terminations, modulation);
    REQUIRE(allocated.ok());
    REQUIRE(VoiceNoteModulationBridge::write(modulation[allocated.allocation.voice_index], 4,
                                             note_modulation())
                .ok);
    REQUIRE(facade.release_voice(allocated.allocation.voice_index));
    REQUIRE(facade.finish_voice(allocated.allocation.voice_index, modulation));
    REQUIRE(modulation[allocated.allocation.voice_index].block().empty());
    REQUIRE_FALSE(facade.finish_voice(std::numeric_limits<std::uint32_t>::max(), modulation));

    const auto live = facade.trigger({.note = 62, .sample_id = 2}, terminations, modulation);
    REQUIRE(live.ok());
    REQUIRE(VoiceNoteModulationBridge::write(modulation[live.allocation.voice_index], 4,
                                             note_modulation(62))
                .ok);
    REQUIRE(facade.reset(std::span<VoiceModulationBuffer>{modulation}.first(1)) ==
            VoiceRuntimeFacadeStatus::InvalidModulationCapacity);
    REQUIRE(allocator.allocated_voice_count() == 1);
    REQUIRE_FALSE(modulation[live.allocation.voice_index].block().empty());

    const auto other_index = 1U - live.allocation.voice_index;
    modulation[other_index].release();
    REQUIRE(facade.reset(modulation) == VoiceRuntimeFacadeStatus::InvalidModulationCapacity);
    REQUIRE(allocator.allocated_voice_count() == 1);
    REQUIRE_FALSE(modulation[live.allocation.voice_index].block().empty());
    REQUIRE(modulation[other_index].prepare({.max_lanes = 6, .max_frames = 16}));
    REQUIRE(facade.reset(modulation) == VoiceRuntimeFacadeStatus::Ok);
    REQUIRE(allocator.allocated_voice_count() == 0);
    REQUIRE(modulation[live.allocation.voice_index].block().empty());
}

TEST_CASE("Voice runtime hot bridge allocation and sum paths allocate nothing",
          "[audio][voice-runtime][rt]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(2));
    VoiceRuntimeFacade facade(allocator);
    std::array<VoiceModulationBuffer, 2> modulation;
    prepare_modulation(modulation, 8);
    std::array<VoiceTermination, 2> terminations{};
    std::array<float, 8> source_a{};
    std::array<float, 8> source_b{};
    source_a.fill(1.0f);
    source_b.fill(2.0f);
    std::array<float, 8> destination{};
    const float* source_a_channels[]{source_a.data()};
    const float* source_b_channels[]{source_b.data()};
    float* destination_channels[]{destination.data()};
    const std::array<VoiceSumInput, 2> inputs{{
        {BufferView<const float>{source_a_channels, 1, source_a.size()}, 1.0f, true},
        {BufferView<const float>{source_b_channels, 1, source_b.size()}, 0.5f, true},
    }};

    AllocatedVoiceRuntimeTriggerResult triggered;
    VoiceNoteModulationResult bridged;
    VoiceSumResult mixed;
    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        triggered = facade.trigger({.note = 60, .sample_id = 1}, terminations, modulation);
        bridged = VoiceNoteModulationBridge::write(modulation[triggered.allocation.voice_index], 8,
                                                   note_modulation());
        mixed = facade.mix(inputs, BufferView<float>{destination_channels, 1, destination.size()},
                           std::numeric_limits<std::uint64_t>::max(), {.accumulate = false});
        allocation_count = probe.allocation_count();
    }
    REQUIRE(triggered.ok());
    REQUIRE(bridged.ok);
    REQUIRE(mixed.frames_mixed == destination.size());
    REQUIRE(mixed.inputs_mixed == 2);
    REQUIRE(destination[0] == 2.0f);
    REQUIRE(allocation_count == 0);

    destination.fill(9.0f);
    mixed = facade.mix(inputs, BufferView<float>{destination_channels, 1, destination.size()}, 0,
                       {.accumulate = false});
    REQUIRE(mixed.frames_mixed == 0);
    REQUIRE(destination[0] == 9.0f);
}
