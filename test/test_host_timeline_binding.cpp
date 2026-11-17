#include "harness/rt_allocation_probe.hpp"

#include "../examples/g6-voice-bank-consumer/fixture.hpp"

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/host_timeline_binding.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/trigger_graph.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

// Installed-style four-family consumer of HostTimelineBinding: MIDI/UMP into a
// prepared trigger graph, the graph's own trigger events, voice intent into a
// VoiceBankBinding, and staged parameter records, all driven from one host
// block timeline through gaps, seeks and requested resets.

namespace {
using namespace pulp;
using namespace pulp::host;
namespace g6 = pulp::examples::g6;

constexpr int kGraphMaxFrames = 32;
constexpr std::uint32_t kHostProducer = 3;
constexpr std::uint32_t kSecondProducer = 9;

struct TriggerFixture {
    SignalGraph graph;
    NodeId in = graph.add_input_node(1);
    NodeId out = graph.add_output_node(1);
    NodeId midi_in = graph.add_midi_input_node("timeline MIDI in");
    NodeId midi_out = graph.add_midi_output_node("timeline MIDI out");
    NodeId producer = 0;
    NodeId envelope = 0;
    TriggerFixture() {
        TriggerProducerConfig config;
        config.lane_count = 1;
        config.lanes[0].payload.token = 21;
        producer = graph.add_trigger_producer(config);
        envelope = graph.add_trigger_envelope({0.5f});
        REQUIRE(graph.connect(in, 0, producer, 0));
        REQUIRE(graph.connect(envelope, 0, out, 0));
        REQUIRE(graph.connect_midi(midi_in, midi_out));
        REQUIRE(graph.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted);
        auto candidate = graph.prepare_trigger_plan(48000, kGraphMaxFrames);
        REQUIRE(candidate.result == TriggerGraphResult::Accepted);
        REQUIRE(graph.try_install_prepared_trigger_plan(candidate.plan) ==
                TriggerGraphResult::Accepted);
    }
};

TriggerCounters trigger_clock(SignalGraph& graph) {
    TriggerCounters counters;
    REQUIRE(graph.trigger_counters(counters) == TriggerGraphResult::Accepted);
    return counters;
}

// One host block described by its steady counter, for scripted segments.
struct Segment {
    std::int64_t steady = 0;
    std::size_t frames = 0;
    bool discontinuity = false;
    bool request_reset = false;
};

// Timeline segments: a first span, a forward gap, a seek, and a requested
// reset on a continuous clock.
constexpr std::array<Segment, 4> kSegments{{
    {0, 96, false, false},
    {128, 96, false, false},
    {224, 96, true, false},
    {320, 80, false, true},
}};

enum class Family : std::uint8_t { GraphMidi, Voice, Parameter, Trigger };

// A scripted record, placed by host steady sample. `publish_at` is the steady
// sample of the block that publishes it (a future record is published early).
struct Scripted {
    Family family = Family::GraphMidi;
    std::int64_t steady = 0;
    std::int64_t publish_at = 0;
    std::uint32_t producer = kHostProducer;
    std::uint64_t sequence = 0;
    midi::MidiEvent message{};
    bool ump = false;
    state::ParamID param = 0;
    float value = 0.0f;
};

std::vector<Scripted> script() {
    std::vector<Scripted> s;
    auto midi = [&](std::int64_t steady, std::uint32_t producer, std::uint64_t sequence,
                    midi::MidiEvent message, bool ump) {
        Scripted r;
        r.family = Family::GraphMidi;
        r.steady = r.publish_at = steady;
        r.producer = producer;
        r.sequence = sequence;
        r.message = message;
        r.ump = ump;
        s.push_back(r);
    };
    auto voice = [&](std::int64_t steady, std::uint64_t sequence, midi::MidiEvent message,
                     std::int64_t publish_at = -1) {
        Scripted r;
        r.family = Family::Voice;
        r.steady = steady;
        r.publish_at = publish_at < 0 ? steady : publish_at;
        r.sequence = sequence;
        r.message = message;
        s.push_back(r);
    };
    auto param = [&](std::int64_t steady, std::uint32_t producer, std::uint64_t sequence,
                     state::ParamID id, float value, std::int64_t publish_at = -1) {
        Scripted r;
        r.family = Family::Parameter;
        r.steady = steady;
        r.publish_at = publish_at < 0 ? steady : publish_at;
        r.producer = producer;
        r.sequence = sequence;
        r.param = id;
        r.value = value;
        s.push_back(r);
    };
    // Equal-time MIDI from two producers, published in reverse producer order.
    midi(3, kSecondProducer, 1, midi::MidiEvent::note_on(1, 61, 90), false);
    midi(3, kHostProducer, 1, midi::MidiEvent::note_on(1, 60, 100), true);
    midi(140, kHostProducer, 2, midi::MidiEvent::note_off(1, 60), true);
    midi(230, kHostProducer, 3, midi::MidiEvent::note_on(2, 64, 80), true);
    midi(330, kHostProducer, 4, midi::MidiEvent::cc(1, 74, 33), false);
    voice(5, 1, midi::MidiEvent::note_on(1, 69, 127));
    voice(40, 2, midi::MidiEvent::note_off(1, 69));
    // Future voice record staged ahead of its block.
    voice(60, 3, midi::MidiEvent::note_on(2, 72, 100), 41);
    voice(70, 4, midi::MidiEvent::note_off(2, 72));
    // Sounding across the seek, which must cut it.
    voice(200, 5, midi::MidiEvent::note_on(3, 76, 110));
    voice(330, 6, midi::MidiEvent::note_on(4, 64, 90));
    // Equal-time parameters from two producers, published in reverse order.
    param(10, kSecondProducer, 1, 7, 0.25f);
    param(10, kHostProducer, 1, 7, 0.5f);
    // Future parameter staged ahead of its block.
    param(60, kHostProducer, 2, 8, 0.75f, 20);
    // Staged in the first span for a sample the host gap skips.
    param(110, kHostProducer, 3, 8, 0.1f, 90);
    param(150, kHostProducer, 4, 9, 0.6f);
    // Staged for a sample past the seek; the reset must invalidate it.
    param(300, kHostProducer, 5, 9, 0.9f, 223);
    param(350, kHostProducer, 6, 7, 1.0f);
    return s;
}

// Impulses that make the graph's trigger producer author its own events.
constexpr std::array<std::int64_t, 4> kImpulses{8, 150, 250, 350};

HostEventRecord host_record(std::uint64_t epoch, std::int64_t absolute, const Scripted& r) {
    HostEventRecord record;
    record.epoch = epoch;
    record.absolute_sample = absolute;
    record.producer = r.producer;
    record.producer_sequence = r.sequence;
    record.midi = r.message;
    if (r.ump) {
        record.has_ump = true;
        record.ump.word_count = 2;
        record.ump.words[0] = 0x4090'0000u | (static_cast<std::uint32_t>(r.message.note()) << 8);
        record.ump.words[1] = 0xFEDC'0000u + static_cast<std::uint32_t>(r.sequence);
    }
    return record;
}

struct Trace {
    std::vector<float> graph_audio;
    std::vector<float> voice_left;
    std::vector<float> voice_right;
    // (graph epoch, graph absolute sample, status, data1, ump word 1)
    std::vector<std::array<std::uint64_t, 5>> midi;
    // (epoch, graph sample, sequence, token)
    std::vector<std::array<std::uint64_t, 4>> triggers;
    // (timeline epoch, timeline sample, param id, value bits)
    std::vector<std::array<std::uint64_t, 4>> parameters;
    std::vector<HostBlockWindow> windows;
    HostTimelineBindingCounters counters;
    std::uint64_t final_graph_epoch = 0;
    std::uint64_t voice_epoch_changes = 0;
    std::size_t active_voices_after_seek = 0;
};

std::uint64_t float_bits(float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

struct Mutation {
    // Corrupt one scripted record's sequence, which also feeds its UMP
    // payload word.
    bool payload = false;
};

Trace run(std::initializer_list<std::size_t> partition, Mutation mutation = {}) {
    TriggerFixture fixture;
    VoiceBankBindingResult status;
    auto bank = g6::make_bank(g6::schema(), status);
    REQUIRE(bank);
    HostTimelineBinding binding(&fixture.graph, bank.get());
    auto records = script();
    if (mutation.payload)
        for (auto& r : records)
            if (r.family == Family::GraphMidi && r.steady == 140)
                r.sequence += 0x100;

    Trace trace;
    std::array<float, kGraphMaxFrames> input{};
    std::array<float, kGraphMaxFrames> output{};
    audio::Buffer<float> voice(2, 256);
    midi::MidiBuffer delivered;
    delivered.reserve(64);
    delivered.set_realtime_capacity_limit(true);
    midi::UmpBuffer delivered_ump;
    delivered_ump.reserve(64);
    delivered_ump.set_realtime_capacity_limit(true);
    delivered.attach_ump(&delivered_ump);
    state::ParameterEventQueue parameters;
    std::uint64_t last_voice_epoch = 0;
    const std::vector<std::size_t> sizes(partition);
    std::size_t size_index = 0;

    for (const auto& segment : kSegments) {
        std::size_t done = 0;
        while (done < segment.frames) {
            const auto frames = std::min(sizes[size_index++ % sizes.size()], segment.frames - done);
            HostBlockClock clock;
            clock.steady_sample = segment.steady + static_cast<std::int64_t>(done);
            clock.frames = frames;
            clock.discontinuity = segment.discontinuity && done == 0;
            if (segment.request_reset && done == 0)
                binding.request_reset();
            HostTimelineBlock block;
            REQUIRE(binding.begin_block(clock, block) == HostEventResult::Accepted);
            trace.windows.push_back(block.timeline);
            if (block.voice_epoch != last_voice_epoch)
                ++trace.voice_epoch_changes;
            last_voice_epoch = block.voice_epoch;
            const auto block_steady = clock.steady_sample;
            const auto to_absolute = [&](std::int64_t steady) {
                return static_cast<std::int64_t>(block.timeline.sample_start) +
                       (steady - block_steady);
            };
            for (const auto& r : records) {
                if (r.publish_at < block_steady ||
                    r.publish_at >= block_steady + static_cast<std::int64_t>(frames))
                    continue;
                if (r.family == Family::Parameter) {
                    HostTimelineParameterRecord p;
                    p.epoch = block.timeline.epoch;
                    p.absolute_sample = to_absolute(r.steady);
                    p.producer = r.producer;
                    p.producer_sequence = r.sequence;
                    p.param_id = r.param;
                    p.value = r.value;
                    REQUIRE(binding.publish_parameter(p) == HostEventResult::Accepted);
                    continue;
                }
                auto record = host_record(block.timeline.epoch, to_absolute(r.steady), r);
                if (r.family == Family::GraphMidi)
                    REQUIRE(binding.publish_graph_event(record) == HostEventResult::Accepted);
                else
                    REQUIRE(binding.publish_voice_event(record) == HostEventResult::Accepted);
            }
            parameters.clear();
            REQUIRE(binding.take_parameters(parameters) == HostEventResult::Accepted);
            for (std::size_t i = 0; i < parameters.size(); ++i) {
                const auto& e = parameters.events()[i];
                trace.parameters.push_back({block.timeline.epoch,
                                            block.timeline.sample_start + e.sample_offset,
                                            e.param_id, float_bits(e.value)});
            }
            input.fill(0.0f);
            for (const auto impulse : kImpulses)
                if (impulse >= block_steady && impulse < block_steady + std::int64_t(frames))
                    input[static_cast<std::size_t>(impulse - block_steady)] = 1.0f;
            const float* ip = input.data();
            float* op = output.data();
            audio::BufferView<const float> in(&ip, 1, frames);
            audio::BufferView<float> out(&op, 1, frames);
            auto stereo = voice.view().slice(0, frames);
            const auto rendered = binding.render(&out, &in, &stereo);
            REQUIRE(rendered.graph == TriggerGraphResult::Accepted);
            REQUIRE(rendered.voice == VoiceBankBindingCode::Ok);
            trace.graph_audio.insert(trace.graph_audio.end(), output.begin(),
                                     output.begin() + static_cast<std::ptrdiff_t>(frames));
            trace.voice_left.insert(trace.voice_left.end(), stereo.channel(0).begin(),
                                    stereo.channel(0).end());
            trace.voice_right.insert(trace.voice_right.end(), stereo.channel(1).begin(),
                                     stereo.channel(1).end());
            for (const auto& t : fixture.graph.trigger_trace())
                trace.triggers.push_back({t.epoch, t.sample, t.sequence, t.token});
            delivered.clear();
            delivered_ump.clear();
            REQUIRE(fixture.graph.extract_midi(fixture.midi_out, delivered));
            for (std::size_t i = 0; i < delivered.size(); ++i) {
                const auto& m = delivered[i];
                std::uint64_t word = 0;
                for (std::size_t u = 0; u < delivered_ump.size(); ++u)
                    if (delivered_ump[u].sample_offset == m.sample_offset &&
                        ((delivered_ump[u].packet.words[0] >> 8) & 0x7f) ==
                            static_cast<std::uint32_t>(m.note()))
                        word = delivered_ump[u].packet.words[1];
                trace.midi.push_back(
                    {block.graph_epoch,
                     block.graph_sample_start + static_cast<std::uint64_t>(m.sample_offset),
                     m.data()[0], m.data()[1], word});
            }
            if (segment.discontinuity && done == 0) {
                for (std::uint32_t slot = 0; slot < kVoiceBankSlots; ++slot)
                    trace.active_voices_after_seek +=
                        g6::owner(*bank).allocator().voice(slot).active() ? 1u : 0u;
            }
            done += frames;
        }
    }
    trace.counters = binding.counters();
    trace.final_graph_epoch = trigger_clock(fixture.graph).epoch;
    return trace;
}

void require_same(const Trace& a, const Trace& b) {
    REQUIRE(a.graph_audio == b.graph_audio);
    REQUIRE(a.midi == b.midi);
    REQUIRE(a.triggers == b.triggers);
    REQUIRE(a.parameters == b.parameters);
    REQUIRE(a.voice_left.size() == b.voice_left.size());
    for (std::size_t i = 0; i < a.voice_left.size(); ++i) {
        INFO("sample=" << i);
        REQUIRE(std::abs(a.voice_left[i] - b.voice_left[i]) <= 2e-6f);
        REQUIRE(std::abs(a.voice_right[i] - b.voice_right[i]) <= 2e-6f);
    }
    REQUIRE(a.counters.resets == b.counters.resets);
    REQUIRE(a.counters.parameters_reset == b.counters.parameters_reset);
    REQUIRE(a.counters.parameters_skipped == b.counters.parameters_skipped);
    REQUIRE(a.final_graph_epoch == b.final_graph_epoch);
    REQUIRE(a.voice_epoch_changes == b.voice_epoch_changes);
}

} // namespace

TEST_CASE("Host timeline binding keeps four families partition invariant across resets",
          "[host][timeline][reset][partition]") {
    const auto reference = run({1});
    // Three resets: the first block, the seek and the requested reset. The gap
    // stays in the first timeline epoch.
    CHECK(reference.counters.resets == 3);
    CHECK(reference.counters.refused_resets == 0);
    CHECK(reference.final_graph_epoch == 4);
    CHECK(reference.voice_epoch_changes == 3);
    CHECK(reference.windows.front().epoch == 1);
    CHECK(reference.windows.back().epoch == 3);
    // The parameter staged past the seek was invalidated, the one in the gap
    // was skipped, never retimed.
    CHECK(reference.counters.parameters_reset == 1);
    CHECK(reference.counters.parameters_skipped == 1);
    // The note sounding across the seek was cut by the reset.
    CHECK(reference.active_voices_after_seek == 0);

    // Graph MIDI: equal-time records ordered by producer, the gap compressed
    // onto the graph's rendered-sample clock, the seek and the requested reset
    // opening fresh graph epochs at sample zero, the full UMP packet intact.
    REQUIRE(reference.midi.size() == 5);
    CHECK(reference.midi[0] == std::array<std::uint64_t, 5>{2, 3, 0x91, 60, 0xFEDC'0001u});
    CHECK(reference.midi[1] == std::array<std::uint64_t, 5>{2, 3, 0x91, 61, 0});
    CHECK(reference.midi[2] == std::array<std::uint64_t, 5>{2, 108, 0x81, 60, 0xFEDC'0002u});
    CHECK(reference.midi[3] == std::array<std::uint64_t, 5>{3, 6, 0x92, 64, 0xFEDC'0003u});
    CHECK(reference.midi[4] == std::array<std::uint64_t, 5>{4, 10, 0xB1, 74, 0});

    // Parameters: equal time ordered by producer; the future record delivered
    // at its own sample; timeline coordinates (the gap keeps epoch 1).
    REQUIRE(reference.parameters.size() == 5);
    CHECK(reference.parameters[0] == std::array<std::uint64_t, 4>{1, 10, 7, float_bits(0.5f)});
    CHECK(reference.parameters[1] == std::array<std::uint64_t, 4>{1, 10, 7, float_bits(0.25f)});
    CHECK(reference.parameters[2] == std::array<std::uint64_t, 4>{1, 60, 8, float_bits(0.75f)});
    CHECK(reference.parameters[3] == std::array<std::uint64_t, 4>{1, 150, 9, float_bits(0.6f)});
    CHECK(reference.parameters[4] == std::array<std::uint64_t, 4>{3, 30, 7, float_bits(1.0f)});

    // Graph-authored trigger events follow the graph epoch and clock.
    REQUIRE(reference.triggers.size() == 4);
    CHECK(reference.triggers[0][0] == 2);
    CHECK(reference.triggers[0][1] == 8);
    CHECK(reference.triggers[1][0] == 2);
    CHECK(reference.triggers[1][1] == 118);
    CHECK(reference.triggers[2][0] == 3);
    CHECK(reference.triggers[2][1] == 26);
    CHECK(reference.triggers[3][0] == 4);
    CHECK(reference.triggers[3][1] == 30);

    // The voice bank sounded before the seek and after the requested reset.
    CHECK(std::any_of(reference.voice_left.begin(), reference.voice_left.begin() + 40,
                      [](float v) { return v != 0.0f; }));
    CHECK(std::any_of(reference.voice_left.begin() + 352, reference.voice_left.end(),
                      [](float v) { return v != 0.0f; }));

    for (const auto partition :
         {std::initializer_list<std::size_t>{7}, std::initializer_list<std::size_t>{16},
          std::initializer_list<std::size_t>{32}, std::initializer_list<std::size_t>{32, 5, 11}}) {
        INFO("partition starting " << *partition.begin());
        require_same(reference, run(partition));
    }
}

TEST_CASE("A payload mutation changes only the graph MIDI trace",
          "[host][timeline][reset][negative-control]") {
    const auto reference = run({16});
    const auto mutated = run({16}, Mutation{true});
    CHECK(mutated.midi != reference.midi);
    CHECK(mutated.parameters == reference.parameters);
    CHECK(mutated.triggers == reference.triggers);
    CHECK(mutated.graph_audio == reference.graph_audio);
}

namespace {
struct Rig {
    TriggerFixture fixture;
    std::unique_ptr<VoiceBankBinding> bank;
    std::unique_ptr<HostTimelineBinding> binding;
    std::array<float, kGraphMaxFrames> input{};
    std::array<float, kGraphMaxFrames> output{};
    audio::Buffer<float> voice{2, 256};
    Rig() {
        VoiceBankBindingResult status;
        bank = g6::make_bank(g6::schema(), status);
        REQUIRE(bank);
        binding = std::make_unique<HostTimelineBinding>(&fixture.graph, bank.get());
    }
    HostEventResult begin(std::int64_t steady, std::size_t frames, bool discontinuity,
                          HostTimelineBlock& block) {
        HostBlockClock clock;
        clock.steady_sample = steady;
        clock.frames = frames;
        clock.discontinuity = discontinuity;
        return binding->begin_block(clock, block);
    }
    HostTimelineRenderResult render(std::size_t frames) {
        const float* ip = input.data();
        float* op = output.data();
        audio::BufferView<const float> in(&ip, 1, frames);
        audio::BufferView<float> out(&op, 1, frames);
        auto stereo = voice.view().slice(0, frames);
        return binding->render(&out, &in, &stereo);
    }
    HostEventRecord note(std::uint64_t epoch, std::int64_t sample, std::uint64_t sequence,
                         std::uint8_t channel = 1, std::uint8_t key = 60) {
        HostEventRecord record;
        record.epoch = epoch;
        record.absolute_sample = sample;
        record.producer = kHostProducer;
        record.producer_sequence = sequence;
        record.midi = midi::MidiEvent::note_on(channel, key, 100);
        return record;
    }
};
} // namespace

TEST_CASE("A reset refuses old epochs and generations in every family",
          "[host][timeline][reset][stale]") {
    Rig rig;
    HostTimelineBlock first;
    REQUIRE(rig.begin(0, 8, false, first) == HostEventResult::Accepted);
    REQUIRE(rig.binding->publish_voice_event(rig.note(first.timeline.epoch, 2, 1)) ==
            HostEventResult::Accepted);
    REQUIRE(rig.binding->publish_graph_event(rig.note(first.timeline.epoch, 2, 1)) ==
            HostEventResult::Accepted);
    REQUIRE(rig.render(8));
    midi::MpeNoteState held{};
    bool releasing = false;
    std::uint32_t sounding_slot = kVoiceBankSlots;
    for (std::uint32_t slot = 0; slot < kVoiceBankSlots; ++slot)
        if (g6::owner(*rig.bank).slot_note(slot, held, releasing)) {
            sounding_slot = slot;
            break;
        }
    REQUIRE(sounding_slot < kVoiceBankSlots);
    const auto old_voice_epoch = first.voice_epoch;
    const auto old_graph_epoch = first.graph_epoch;

    HostTimelineBlock seek;
    REQUIRE(rig.begin(500, 8, true, seek) == HostEventResult::Accepted);
    CHECK(seek.reset);
    CHECK(seek.boundary == HostBlockBoundary::NewEpoch);
    CHECK(seek.timeline.epoch == first.timeline.epoch + 1);
    CHECK(seek.timeline.sample_start == 0);
    CHECK(seek.graph_epoch == old_graph_epoch + 1);
    CHECK(seek.graph_sample_start == 0);
    CHECK(seek.voice_epoch != old_voice_epoch);

    // Records still timed against the retired timeline epoch.
    CHECK(rig.binding->publish_graph_event(rig.note(first.timeline.epoch, 1, 2)) ==
          HostEventResult::StaleEpoch);
    CHECK(rig.binding->publish_voice_event(rig.note(first.timeline.epoch, 1, 2)) ==
          HostEventResult::StaleEpoch);
    HostTimelineParameterRecord parameter;
    parameter.epoch = first.timeline.epoch;
    parameter.absolute_sample = 1;
    parameter.producer = kHostProducer;
    CHECK(rig.binding->publish_parameter(parameter) == HostEventResult::StaleEpoch);
    // A deferred voice token minted before the reset.
    CHECK(rig.bank->dispatch_deferred(old_voice_epoch, sounding_slot, held.note_id,
                                      midi::MpeExpressionEvent::Kind::NoteOff,
                                      held) == VoiceBankBindingCode::StaleEpoch);
    // The same token in the new epoch names a generation the reset cut.
    CHECK(rig.bank->dispatch_deferred(seek.voice_epoch, sounding_slot, held.note_id,
                                      midi::MpeExpressionEvent::Kind::NoteOff,
                                      held) == VoiceBankBindingCode::StaleGeneration);
    // Records written straight to the families in their old epochs.
    auto graph_direct = rig.note(old_graph_epoch, 1, 3);
    CHECK(rig.fixture.graph.publish_host_event(graph_direct) == HostEventResult::StaleEpoch);
    auto voice_direct = rig.note(old_voice_epoch, 1, 3);
    CHECK(rig.bank->publish(voice_direct) == HostEventResult::StaleEpoch);
    // Positive control: the same records in the new epoch are accepted.
    CHECK(rig.binding->publish_graph_event(rig.note(seek.timeline.epoch, 1, 4)) ==
          HostEventResult::Accepted);
    CHECK(rig.binding->publish_voice_event(rig.note(seek.timeline.epoch, 1, 4)) ==
          HostEventResult::Accepted);
    parameter.epoch = seek.timeline.epoch;
    CHECK(rig.binding->publish_parameter(parameter) == HostEventResult::Accepted);
    CHECK(rig.render(8));
}

TEST_CASE("Late, future and overflowing records are refused per family",
          "[host][timeline][reset][refusal]") {
    Rig rig;
    HostTimelineBlock block;
    REQUIRE(rig.begin(0, 8, false, block) == HostEventResult::Accepted);
    REQUIRE(rig.render(8));
    REQUIRE(rig.begin(8, 8, false, block) == HostEventResult::Accepted);
    const auto epoch = block.timeline.epoch;
    CHECK(rig.binding->publish_graph_event(rig.note(epoch, 7, 1)) == HostEventResult::Late);
    CHECK(rig.binding->publish_voice_event(rig.note(epoch, 7, 1)) == HostEventResult::Late);
    CHECK(rig.binding->publish_graph_event(rig.note(epoch, -1, 1)) == HostEventResult::OutOfRange);
    auto unbound = rig.note(epoch, 9, 1);
    unbound.producer = 0;
    CHECK(rig.binding->publish_graph_event(unbound) == HostEventResult::Unbound);
    // The graph clock counts rendered samples: a future graph record would be
    // retimed by a later host gap, so only the current block is accepted.
    CHECK(rig.binding->publish_graph_event(rig.note(epoch, 16, 1)) == HostEventResult::OutOfRange);
    CHECK(rig.binding->publish_graph_event(rig.note(epoch, 15, 1)) == HostEventResult::Accepted);
    // The duplicate (producer, sequence) is refused by the graph ingress.
    CHECK(rig.binding->publish_graph_event(rig.note(epoch, 14, 1)) ==
          HostEventResult::InvalidSequence);
    // Voice and parameter families stage future records in timeline samples.
    CHECK(rig.binding->publish_voice_event(rig.note(epoch, 40, 1)) == HostEventResult::Accepted);

    HostTimelineParameterRecord p;
    p.epoch = epoch;
    p.producer = kHostProducer;
    p.absolute_sample = 7;
    CHECK(rig.binding->publish_parameter(p) == HostEventResult::Late);
    for (std::size_t i = 0; i < HostTimelineBinding::kParameterCapacity; ++i) {
        p.absolute_sample = 100 + static_cast<std::int64_t>(i);
        p.producer_sequence = i;
        REQUIRE(rig.binding->publish_parameter(p) == HostEventResult::Accepted);
    }
    p.producer_sequence = 100000;
    CHECK(rig.binding->publish_parameter(p) == HostEventResult::Overflow);
    p.producer_sequence = 3;
    CHECK(rig.binding->publish_parameter(p) == HostEventResult::InvalidSequence);
    CHECK(rig.binding->counters().parameters_overflow == 1);
    CHECK(rig.binding->staged_parameters() == HostTimelineBinding::kParameterCapacity);
    REQUIRE(rig.render(8));

    // Graph ingress overflow through the binding: its own capacity holds.
    REQUIRE(rig.begin(16, 8, false, block) == HostEventResult::Accepted);
    HostEventResult last = HostEventResult::Accepted;
    std::size_t accepted = 0;
    for (std::uint64_t seq = 1; seq <= SignalGraph::kHostEventIngressCapacity + 1; ++seq) {
        last = rig.binding->publish_graph_event(rig.note(block.timeline.epoch, 16, seq));
        if (last == HostEventResult::Accepted)
            ++accepted;
    }
    CHECK(accepted == SignalGraph::kHostEventIngressCapacity);
    CHECK(last == HostEventResult::Overflow);
}

TEST_CASE("Parameter projection refuses a short destination without change",
          "[host][timeline][reset][refusal]") {
    Rig rig;
    HostTimelineBlock block;
    REQUIRE(rig.begin(0, 8, false, block) == HostEventResult::Accepted);
    HostTimelineParameterRecord p;
    p.epoch = block.timeline.epoch;
    p.producer = kHostProducer;
    p.absolute_sample = 4;
    p.param_id = 5;
    REQUIRE(rig.binding->publish_parameter(p) == HostEventResult::Accepted);
    state::ParameterEventQueue full;
    for (std::size_t i = 0; i < full.capacity(); ++i)
        REQUIRE(full.push({}));
    CHECK(rig.binding->take_parameters(full) == HostEventResult::OutputCapacity);
    CHECK(full.size() == full.capacity());
    CHECK(rig.binding->staged_parameters() == 1);
    state::ParameterEventQueue room;
    REQUIRE(rig.binding->take_parameters(room) == HostEventResult::Accepted);
    REQUIRE(room.size() == 1);
    CHECK(room.events()[0].sample_offset == 4);
    CHECK(rig.binding->staged_parameters() == 0);
    REQUIRE(rig.render(8));
}

TEST_CASE("A refused reset leaves every family and the timeline unchanged",
          "[host][timeline][reset][transaction]") {
    SECTION("graph refuses after the voice reservation") {
        Rig rig;
        HostTimelineBlock block;
        REQUIRE(rig.begin(0, 8, false, block) == HostEventResult::Accepted);
        HostTimelineParameterRecord p;
        p.epoch = block.timeline.epoch;
        p.producer = kHostProducer;
        p.absolute_sample = 40;
        REQUIRE(rig.binding->publish_parameter(p) == HostEventResult::Accepted);
        REQUIRE(rig.binding->publish_voice_event(rig.note(block.timeline.epoch, 1, 1)) ==
                HostEventResult::Accepted);
        REQUIRE(rig.render(8));
        const auto before = rig.binding->current();
        const auto graph_before = trigger_clock(rig.fixture.graph);
        const auto voice_before = rig.bank->epoch();

        // Another owner holds the graph's host ingress block open, so the
        // graph reset is refused (Busy).
        REQUIRE(rig.fixture.graph.begin_host_event_block(graph_before.epoch, graph_before.cursor,
                                                         8) == HostEventResult::Accepted);
        HostTimelineBlock refused;
        CHECK(rig.begin(900, 8, true, refused) == HostEventResult::Busy);
        CHECK(rig.binding->counters().refused_resets == 1);
        CHECK(rig.binding->counters().resets == 1);
        CHECK_FALSE(rig.binding->block_open());
        CHECK(rig.binding->current().timeline.epoch == before.timeline.epoch);
        CHECK(rig.binding->current().timeline.sample_start == before.timeline.sample_start);
        CHECK(rig.bank->epoch() == voice_before);
        CHECK(trigger_clock(rig.fixture.graph).epoch == graph_before.epoch);
        CHECK(rig.binding->staged_parameters() == 1);
        bool releasing = false;
        midi::MpeNoteState held{};
        bool sounding = false;
        for (std::uint32_t slot = 0; slot < kVoiceBankSlots; ++slot)
            sounding = g6::owner(*rig.bank).slot_note(slot, held, releasing) || sounding;
        CHECK(sounding);

        // Seal the foreign block, then the same seek succeeds exactly once.
        const float* ip = rig.input.data();
        float* op = rig.output.data();
        audio::BufferView<const float> in(&ip, 1, 8);
        audio::BufferView<float> out(&op, 1, 8);
        REQUIRE(rig.fixture.graph.process_checked(out, in, 8) == TriggerGraphResult::Accepted);
        HostTimelineBlock retried;
        REQUIRE(rig.begin(900, 8, true, retried) == HostEventResult::Accepted);
        CHECK(retried.reset);
        CHECK(retried.timeline.epoch == before.timeline.epoch + 1);
        CHECK(retried.graph_epoch == graph_before.epoch + 1);
        CHECK(rig.bank->epoch() != voice_before);
        CHECK(rig.binding->staged_parameters() == 0);
        CHECK(rig.binding->counters().parameters_reset == 1);
        REQUIRE(rig.render(8));
    }
    SECTION("voice bank refuses before the graph resets") {
        Rig rig;
        HostTimelineBlock block;
        REQUIRE(rig.begin(0, 8, false, block) == HostEventResult::Accepted);
        REQUIRE(rig.render(8));
        const auto graph_before = trigger_clock(rig.fixture.graph);
        const auto voice_before = rig.bank->epoch();
        REQUIRE(rig.bank->begin_block(8, 8) == HostEventResult::Accepted);
        HostTimelineBlock refused;
        rig.binding->request_reset();
        CHECK(rig.begin(8, 8, false, refused) == HostEventResult::Busy);
        CHECK(trigger_clock(rig.fixture.graph).epoch == graph_before.epoch);
        CHECK(trigger_clock(rig.fixture.graph).cursor == graph_before.cursor);
        CHECK(rig.bank->epoch() == voice_before);
        CHECK(rig.binding->current().timeline.epoch == block.timeline.epoch);
        REQUIRE(rig.bank->abort_block() == HostEventResult::Accepted);
        // The request survives the refusal and applies on the retry.
        HostTimelineBlock retried;
        REQUIRE(rig.begin(8, 8, false, retried) == HostEventResult::Accepted);
        CHECK(retried.reset);
        CHECK(retried.timeline.epoch == block.timeline.epoch + 1);
        CHECK(retried.graph_epoch == graph_before.epoch + 1);
        REQUIRE(rig.render(8));
    }
}

TEST_CASE("Voice bank reset tokens are single use and refused inside a block",
          "[host][timeline][reset][voice]") {
    VoiceBankBindingResult status;
    auto bank = g6::make_bank(g6::schema(), status);
    REQUIRE(bank);
    const auto original = bank->epoch();
    std::uint64_t token = 0;
    REQUIRE(bank->reserve_event_reset(token) == HostEventResult::Accepted);
    CHECK(token != original);
    CHECK(bank->epoch() == original);
    CHECK(bank->commit_event_reset(token + 1) == HostEventResult::StaleEpoch);
    CHECK(bank->commit_event_reset(0) == HostEventResult::StaleEpoch);
    CHECK(bank->epoch() == original);
    REQUIRE(bank->commit_event_reset(token) == HostEventResult::Accepted);
    CHECK(bank->epoch() == token);
    CHECK(bank->commit_event_reset(token) == HostEventResult::StaleEpoch);
    REQUIRE(bank->begin_block(0, 4) == HostEventResult::Accepted);
    std::uint64_t inside = 0;
    CHECK(bank->reserve_event_reset(inside) == HostEventResult::Busy);
    REQUIRE(bank->abort_block() == HostEventResult::Accepted);
    CHECK(bank->abort_block() == HostEventResult::StaleEpoch);
    REQUIRE(bank->reserve_event_reset(inside) == HostEventResult::Accepted);
    REQUIRE(bank->begin_block(0, 4) == HostEventResult::Accepted);
    CHECK(bank->commit_event_reset(inside) == HostEventResult::Busy);
    CHECK(bank->epoch() == token);
}

TEST_CASE("A graph reset made elsewhere is remapped, not treated as a binding reset",
          "[host][timeline][reset][remap]") {
    Rig rig;
    HostTimelineBlock block;
    REQUIRE(rig.begin(0, 8, false, block) == HostEventResult::Accepted);
    REQUIRE(rig.render(8));
    const auto voice_before = rig.bank->epoch();
    REQUIRE(rig.fixture.graph.try_reset_events() == TriggerGraphResult::Accepted);
    HostTimelineBlock next;
    REQUIRE(rig.begin(8, 8, false, next) == HostEventResult::Accepted);
    CHECK_FALSE(next.reset);
    CHECK(next.timeline.epoch == block.timeline.epoch);
    CHECK(next.timeline.sample_start == 8);
    CHECK(next.graph_epoch == block.graph_epoch + 1);
    CHECK(next.graph_sample_start == 0);
    CHECK(rig.bank->epoch() == voice_before);
    CHECK(rig.binding->counters().graph_remaps == 1);
    REQUIRE(rig.binding->publish_graph_event(rig.note(next.timeline.epoch, 11, 1)) ==
            HostEventResult::Accepted);
    REQUIRE(rig.render(8));
    midi::MidiBuffer delivered;
    delivered.reserve(4);
    REQUIRE(rig.fixture.graph.extract_midi(rig.fixture.midi_out, delivered));
    REQUIRE(delivered.size() == 1);
    CHECK(delivered[0].sample_offset == 3);
}

TEST_CASE("Binding callbacks and resets do not allocate", "[host][timeline][reset][rt-safety]") {
    REQUIRE(pulp::test::RtAllocationProbe::self_check());
    Rig rig;
    HostTimelineBlock block;
    REQUIRE(rig.begin(0, 8, false, block) == HostEventResult::Accepted);
    REQUIRE(rig.render(8));
    state::ParameterEventQueue parameters;
    const float* ip = rig.input.data();
    float* op = rig.output.data();
    audio::BufferView<const float> in(&ip, 1, 8);
    audio::BufferView<float> out(&op, 1, 8);
    auto stereo = rig.voice.view().slice(0, 8);
    std::size_t allocations = 0;
    bool ok = true;
    {
        pulp::test::RtAllocationProbe probe;
        for (std::int64_t step = 0; step < 6; ++step) {
            HostBlockClock clock;
            // Continuous, gap, seek and requested-reset blocks.
            clock.steady_sample = 8 + step * 16;
            clock.frames = 8;
            clock.discontinuity = step == 3;
            if (step == 4)
                rig.binding->request_reset();
            ok = rig.binding->begin_block(clock, block) == HostEventResult::Accepted && ok;
            auto note = rig.note(
                block.timeline.epoch, static_cast<std::int64_t>(block.timeline.sample_start) + 1,
                static_cast<std::uint64_t>(step + 1), 1, static_cast<std::uint8_t>(60 + step));
            ok = rig.binding->publish_graph_event(note) == HostEventResult::Accepted && ok;
            ok = rig.binding->publish_voice_event(note) == HostEventResult::Accepted && ok;
            HostTimelineParameterRecord p;
            p.epoch = block.timeline.epoch;
            p.absolute_sample = note.absolute_sample;
            p.producer = kHostProducer;
            p.producer_sequence = static_cast<std::uint64_t>(step);
            ok = rig.binding->publish_parameter(p) == HostEventResult::Accepted && ok;
            parameters.clear();
            ok = rig.binding->take_parameters(parameters) == HostEventResult::Accepted && ok;
            ok = static_cast<bool>(rig.binding->render(&out, &in, &stereo)) && ok;
        }
        allocations = probe.allocation_count();
    }
    CHECK(ok);
    CHECK(rig.binding->counters().resets == 3);
    CHECK(allocations == 0);
}

TEST_CASE("MPE derivation rebinds the tracker without allocating",
          "[host][timeline][reset][rt-safety][mpe]") {
    REQUIRE(pulp::test::RtAllocationProbe::self_check());
    midi::MpeVoiceTracker tracker;
    midi::MpeBuffer sidecar;
    sidecar.reserve(64);
    sidecar.set_realtime_capacity_limit(true);
    std::array<HostEventRecord, 2> records{};
    records[0].epoch = records[1].epoch = 1;
    records[0].absolute_sample = 100;
    records[0].producer = records[1].producer = 1;
    records[0].midi = midi::MidiEvent::note_on(1, 60, 100);
    records[1].absolute_sample = 102;
    records[1].producer_sequence = 1;
    records[1].midi = midi::MidiEvent::pitch_bend(1, 10'000);
    // Warm the tracker once so only the per-call binding is measured.
    REQUIRE(derive_mpe(records, 100, tracker, sidecar) == HostEventResult::Accepted);
    sidecar.clear();
    tracker.reset();
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        (void)derive_mpe(records, 100, tracker, sidecar);
        allocations = probe.allocation_count();
    }
    CHECK(allocations == 0);
    REQUIRE(sidecar.size() == 2);
    CHECK(sidecar[0].kind == midi::MpeExpressionEvent::Kind::NoteOn);
    CHECK(sidecar[0].sample_offset == 0);
    CHECK(sidecar[1].kind == midi::MpeExpressionEvent::Kind::PitchBend);
    CHECK(sidecar[1].sample_offset == 2);

    // Positive control: the probe sees a std::function assignment whose
    // capture does not fit inline, which is how a per-block rebinding of the
    // tracker would allocate.
    std::array<void*, 8> wide{};
    std::size_t control = 0;
    {
        pulp::test::RtAllocationProbe probe;
        tracker.on_pitch_bend = [wide](const midi::MpeNoteState&) { (void)wide; };
        control = probe.allocation_count();
    }
    CHECK(control > 0);
}
