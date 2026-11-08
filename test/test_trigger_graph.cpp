#include "harness/rt_allocation_probe.hpp"
#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/host/trigger_graph.hpp>
#include <vector>

namespace pulp::host::detail {
struct TriggerTestAccess {
    static void clock(SignalGraph& g, std::uint64_t cursor, std::uint64_t sequence,
                      std::uint64_t epoch, std::uint8_t phase = 0, bool saturate = false) {
        g.trigger_test_clock_(cursor, sequence, epoch, phase, saturate);
    }
    static TriggerEmitResult emit(SignalGraph& g, std::int64_t target, std::uint64_t epoch,
                                  std::uint8_t port = 0) {
        return g.trigger_test_emit_(target, epoch, port);
    }
    static TriggerEmitResult schedule(SignalGraph& g, std::int64_t target, std::uint64_t epoch) {
        return g.trigger_test_schedule_(target, epoch);
    }
    static void gate(SignalGraph& g, bool held) {
        g.trigger_test_gate_(held);
    }
    static bool budget(SignalGraph& g, std::size_t total) {
        return g.trigger_test_budget_(total);
    }
    static void old_event_feedback(SignalGraph& g, NodeId source, NodeId destination) {
        Connection edge{source, 0, destination, 0};
        edge.feedback = edge.midi = true;
        g.connections_.push_back(edge);
    }
    static auto pin(SignalGraph& g) {
        return g.live_slot_.live();
    }
    static void old_render(SignalGraph& g, const auto& pin, audio::BufferView<float>& output,
                           const audio::BufferView<const float>& input, int frames) {
        g.process_snapshot_impl(output, input, frames, nullptr, pin.get());
    }
    static std::size_t host_projection_mpe_size(SignalGraph& g) {
        auto pin = g.live_slot_.live();
        if (!pin)
            return 0;
        return pin->host_event_projection == nullptr ? 0 : pin->host_event_projection->mpe.size();
    }
    static void reserve_host_projection_mpe(SignalGraph& g, std::size_t capacity) {
        auto pin = g.live_slot_.live();
        REQUIRE(pin);
        REQUIRE(pin->host_event_projection != nullptr);
        pin->host_event_projection->mpe.reserve(capacity);
    }
    static void reserve_host_destination_mpe(SignalGraph& g, std::size_t capacity) {
        auto pin = g.live_slot_.live();
        REQUIRE(pin);
        const auto it = pin->runtime.find(pin->host_event_midi_input);
        REQUIRE(it != pin->runtime.end());
        REQUIRE(it->second.midi_input_mailbox);
        it->second.midi_input_mailbox->writer_scratch.mpe.reserve(capacity);
    }
    static pulp::midi::MpeVoiceTracker& host_projection_tracker(SignalGraph& g) {
        auto pin = g.live_slot_.live();
        REQUIRE(pin);
        REQUIRE(pin->host_event_projection != nullptr);
        return pin->host_event_projection->host_event_tracker;
    }
    static std::size_t routed_mpe_size(SignalGraph& g, NodeId id) {
        auto pin = g.live_slot_.live();
        if (!pin)
            return 0;
        for (const auto& output : pin->routed.midi_outputs) {
            if (output.id == id) {
                const auto* mpe = pin->routed.midi.mpe_in(output.plan_index);
                return mpe == nullptr ? 0 : mpe->size();
            }
        }
        const auto runtime = pin->runtime.find(id);
        if (runtime == pin->runtime.end() || !runtime->second.midi_output_mailbox)
            return 0;
        return runtime->second.midi_output_mailbox->consumer_scratch.mpe.size();
    }
};
} // namespace pulp::host::detail
namespace {
using namespace pulp::host;
using Access = detail::TriggerTestAccess;
struct Fixture {
    SignalGraph graph;
    NodeId in = graph.add_input_node(1);
    NodeId out = graph.add_output_node(1);
    NodeId midi_in = 0;
    NodeId midi_out = 0;
    NodeId producer = 0, envelope = 0;
    Fixture(int lanes = 2, int frames = 32, bool with_host_midi = false) {
        if (with_host_midi) {
            midi_in = graph.add_midi_input_node("G5 host MIDI in");
            midi_out = graph.add_midi_output_node("G5 host MIDI out");
        }
        TriggerProducerConfig config;
        config.lane_count = static_cast<std::uint8_t>(lanes);
        for (int i = 0; i < lanes; ++i)
            config.lanes[i].payload.token = i + 10;
        producer = graph.add_trigger_producer(config);
        envelope = graph.add_trigger_envelope({0.5f});
        REQUIRE(graph.connect(in, 0, producer, 0));
        REQUIRE(graph.connect(envelope, 0, out, 0));
        if (with_host_midi)
            REQUIRE(graph.connect_midi(midi_in, midi_out));
        REQUIRE(graph.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted);
        auto candidate = graph.prepare_trigger_plan(48000, frames);
        REQUIRE(candidate.result == TriggerGraphResult::Accepted);
        REQUIRE(graph.try_install_prepared_trigger_plan(candidate.plan) ==
                TriggerGraphResult::Accepted);
        REQUIRE_FALSE(candidate.plan);
    }
};
struct Block {
    std::array<float, 32> input{};
    std::array<float, 32> output{};
    const float* ip = input.data();
    float* op = output.data();
    pulp::audio::BufferView<const float> in{&ip, 1, input.size()};
    pulp::audio::BufferView<float> out{&op, 1, output.size()};
};
TriggerCounters counters(SignalGraph& g) {
    TriggerCounters result;
    REQUIRE(g.trigger_counters(result) == TriggerGraphResult::Accepted);
    return result;
}
} // namespace

TEST_CASE("Typed trigger pair preserves sample and tie order across partitions", "[trigger]") {
    std::vector<float> expected;
    std::vector<std::array<std::uint64_t, 4>> expected_events;
    for (const auto& partitions :
         {std::vector<int>{16}, std::vector<int>(16, 1), std::vector<int>{1, 2, 1, 3, 1, 4, 4}}) {
        Fixture fixture;
        std::array<float, 16> input{0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1};
        std::array<float, 16> audio{};
        std::vector<std::array<std::uint64_t, 4>> events;
        std::size_t offset = 0;
        for (int size : partitions) {
            const float* ip = input.data() + offset;
            float* op = audio.data() + offset;
            pulp::audio::BufferView<const float> in(&ip, 1, size);
            pulp::audio::BufferView<float> out(&op, 1, size);
            REQUIRE(fixture.graph.process_checked(out, in, size) == TriggerGraphResult::Accepted);
            for (auto event : fixture.graph.trigger_trace())
                events.push_back({event.epoch, event.sample, event.sequence, event.token});
            offset += size;
        }
        if (expected.empty()) {
            expected.assign(audio.begin(), audio.end());
            expected_events = events;
            REQUIRE(events.size() == 10);
            REQUIRE(events[0][1] == 1);
            REQUIRE(events[1][3] == 11);
            REQUIRE(audio[1] == 1);
        } else {
            CHECK(std::vector<float>(audio.begin(), audio.end()) == expected);
            CHECK(events == expected_events);
        }
        CHECK(counters(fixture.graph).cursor == 16);
    }
}

TEST_CASE("Host ingress binds to the checked trigger epoch and block cursor",
          "[trigger][g5][host-ingress]") {
    Fixture fixture{2, 32, true};
    Block block;
    HostEventRecord event;
    event.epoch = 1;
    event.absolute_sample = 2;
    event.producer_sequence = 7;
    event.producer = fixture.producer;
    event.midi = pulp::midi::MidiEvent::note_on(1, 60, 100);
    event.has_ump = true;
    event.ump.word_count = 2;
    event.ump.words[0] = 0x4091'3C00u;
    event.ump.words[1] = 0xFFFF'0000u;

    CHECK(fixture.graph.begin_host_event_block(1, 0, 4) == HostEventResult::Accepted);
    CHECK(fixture.graph.publish_host_event(event) == HostEventResult::Accepted);
    auto stale = event;
    stale.epoch = 2;
    CHECK(fixture.graph.publish_host_event(stale) == HostEventResult::StaleEpoch);
    CHECK(fixture.graph.process_checked(block.out, block.in, 4) == TriggerGraphResult::Accepted);
    CHECK(fixture.graph.host_event_trace().size() == 1);

    pulp::midi::MidiBuffer delivered;
    delivered.reserve(4);
    delivered.set_realtime_capacity_limit(true);
    pulp::midi::UmpBuffer delivered_ump;
    delivered_ump.reserve(4);
    delivered_ump.set_realtime_capacity_limit(true);
    delivered.attach_ump(&delivered_ump);
    REQUIRE(fixture.graph.extract_midi(fixture.midi_out, delivered));
    REQUIRE(delivered.size() == 1);
    CHECK(delivered[0].sample_offset == 2);
    CHECK(delivered[0].is_note_on());
    REQUIRE(delivered_ump.size() == 1);
    CHECK(delivered_ump[0].sample_offset == 2);
    CHECK(Access::host_projection_mpe_size(fixture.graph) == 1);
    CHECK(Access::routed_mpe_size(fixture.graph, fixture.midi_out) == 1);

    SignalGraph::HostEventIngressCounters host_counters;
    REQUIRE(fixture.graph.host_event_counters(host_counters) == HostEventResult::Accepted);
    CHECK(host_counters.epoch == 1);
    CHECK(host_counters.accepted == 1);
    CHECK(host_counters.refused == 1);

    CHECK(fixture.graph.begin_host_event_block(1, 0, 1) == HostEventResult::Late);
    CHECK(fixture.graph.begin_host_event_block(1, 5, 1) == HostEventResult::OutOfRange);
    CHECK(fixture.graph.reset_host_event_ingress(1) == HostEventResult::Accepted);
    CHECK(fixture.graph.host_event_trace().empty());
}

TEST_CASE("Host ingress refuses a block whose frames differ from the ingress window",
          "[trigger][g5][host-ingress][negative-control]") {
    // The ingress window is fixed by begin_host_event_block(). Processing a
    // shorter block must not project records past that block's end: they would
    // carry sample_offset >= frames and be delivered early while the cursor
    // advances by fewer frames. The block is refused and the record stays
    // staged for a retry with matching frames.
    Fixture fixture{2, 32, true};
    Block block;
    HostEventRecord event;
    event.epoch = 1;
    event.absolute_sample = 6;
    event.producer_sequence = 1;
    event.producer = fixture.producer;
    event.midi = pulp::midi::MidiEvent::note_on(1, 60, 100);

    REQUIRE(fixture.graph.begin_host_event_block(1, 0, 8) == HostEventResult::Accepted);
    REQUIRE(fixture.graph.publish_host_event(event) == HostEventResult::Accepted);
    CHECK(fixture.graph.process_checked(block.out, block.in, 4) ==
          TriggerGraphResult::ProcessingFailure);
    SignalGraph::HostEventIngressCounters refused;
    REQUIRE(fixture.graph.host_event_counters(refused) == HostEventResult::Accepted);
    CHECK(refused.refused == 1);

    pulp::midi::MidiBuffer delivered;
    delivered.reserve(4);
    delivered.set_realtime_capacity_limit(true);
    REQUIRE(fixture.graph.extract_midi(fixture.midi_out, delivered));
    CHECK(delivered.size() == 0);

    // The refused record is still staged: re-publishing its sequence is a
    // duplicate, and a matching 8-frame block delivers it at offset 6.
    REQUIRE(fixture.graph.begin_host_event_block(1, 0, 8) == HostEventResult::Accepted);
    CHECK(fixture.graph.publish_host_event(event) == HostEventResult::InvalidSequence);
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 8) == TriggerGraphResult::Accepted);
    CHECK(counters(fixture.graph).cursor == 8);
    delivered.clear();
    REQUIRE(fixture.graph.extract_midi(fixture.midi_out, delivered));
    REQUIRE(delivered.size() == 1);
    CHECK(delivered[0].sample_offset == 6);
    CHECK(delivered[0].is_note_on());
}

TEST_CASE("Host ingress projection refusal seals the active block",
          "[trigger][g5][host-ingress][negative-control]") {
    // This generation deliberately has no admitted MidiInput destination. A
    // published host block must refuse as Unbound, then seal the ingress so a
    // later block is not wedged behind Busy.
    Fixture fixture;
    Block block;
    HostEventRecord event;
    event.epoch = 1;
    event.absolute_sample = 0;
    event.producer_sequence = 1;
    event.producer = fixture.producer;
    event.midi = pulp::midi::MidiEvent::note_on(1, 60, 100);

    REQUIRE(fixture.graph.begin_host_event_block(1, 0, 1) == HostEventResult::Accepted);
    REQUIRE(fixture.graph.publish_host_event(event) == HostEventResult::Accepted);
    CHECK(fixture.graph.process_checked(block.out, block.in, 1) ==
          TriggerGraphResult::ProcessingFailure);
    // The refusal seals the active block, but the staged record remains for a
    // retry. Re-publishing its producer sequence is therefore rejected as a
    // duplicate, while begin_host_event_block() is no longer wedged.
    CHECK(fixture.graph.begin_host_event_block(1, 0, 1) == HostEventResult::Accepted);
    CHECK(fixture.graph.publish_host_event(event) == HostEventResult::InvalidSequence);
    CHECK(fixture.graph.process_checked(block.out, block.in, 1) ==
          TriggerGraphResult::ProcessingFailure);
    CHECK(fixture.graph.reset_host_event_ingress(1) == HostEventResult::Accepted);
    CHECK(fixture.graph.process_checked(block.out, block.in, 1) == TriggerGraphResult::Accepted);
}

TEST_CASE("Host ingress preflights MPE fanout and retries without tracker mutation",
          "[trigger][g5][host-ingress][mpe][negative-control]") {
    Fixture fixture{2, 8, true};
    Block block;
    auto& tracker = Access::host_projection_tracker(fixture.graph);
    for (std::uint8_t note = 0; note < pulp::midi::MpeVoiceTracker::kMaxNotes; ++note)
        REQUIRE(tracker.process(pulp::midi::MidiEvent::note_on(1, note, 100)));

    HostEventRecord event;
    event.epoch = 1;
    event.absolute_sample = 0;
    event.producer = fixture.producer;
    event.midi = pulp::midi::MidiEvent::pitch_bend(1, 10'000);
    const auto before_bend = tracker.find(1, 0)->pitch_bend_semitones;
    REQUIRE(fixture.graph.begin_host_event_block(1, 0, 1) == HostEventResult::Accepted);
    for (std::uint64_t sequence = 1; sequence <= 8; ++sequence) {
        event.producer_sequence = sequence;
        REQUIRE(fixture.graph.publish_host_event(event) == HostEventResult::Accepted);
    }
    // Reserve the projection sidecar before the first attempt. The graph-owned
    // destination remains at its smaller default reservation so this refusal
    // exercises the destination-capacity preflight specifically.
    Access::reserve_host_projection_mpe(fixture.graph, 2048);
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 1) ==
            TriggerGraphResult::ProcessingFailure);

    CHECK(tracker.active_count() == pulp::midi::MpeVoiceTracker::kMaxNotes);
    CHECK(tracker.find(1, 0)->pitch_bend_semitones == before_bend);
    CHECK(Access::host_projection_mpe_size(fixture.graph) == 0);

    // Expanding the graph-owned destination lets the same staged records retry
    // without a second publication, proving the destination refusal did not
    // consume them or mutate tracker state.
    Access::reserve_host_destination_mpe(fixture.graph, 2048);
    REQUIRE(fixture.graph.begin_host_event_block(1, 0, 1) == HostEventResult::Accepted);
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 1) == TriggerGraphResult::Accepted);
}

TEST_CASE("Typed render preflight preserves output and clock", "[trigger]") {
    Fixture fixture;
    Block block;
    block.output.fill(91);
    const auto before = counters(fixture.graph);
    for (int frames : {0, -1, 33, 4097}) {
        CHECK(fixture.graph.process_checked(block.out, block.in, frames) ==
              TriggerGraphResult::Frames);
        CHECK(block.output[0] == 91);
        CHECK(counters(fixture.graph).cursor == before.cursor);
        CHECK(counters(fixture.graph).sequence == before.sequence);
    }
    fixture.graph.set_canonical_executor_routing_enabled(false);
    CHECK(fixture.graph.process_checked(block.out, block.in, 1) == TriggerGraphResult::Mode);
    CHECK(block.output[0] == 91);
    fixture.graph.set_canonical_executor_routing_enabled(true);
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    Access::clock(fixture.graph, max, 0, 1);
    CHECK(fixture.graph.process_checked(block.out, block.in, 1) ==
          TriggerGraphResult::ClockExhausted);
    CHECK(block.output[0] == 91);
    Access::clock(fixture.graph, 0, max - 15, 1);
    CHECK(fixture.graph.process_checked(block.out, block.in, 1) ==
          TriggerGraphResult::ClockExhausted);
    CHECK(block.output[0] == 91);
    Access::clock(fixture.graph, 0, 0, max);
    CHECK(fixture.graph.try_reset_events() == TriggerGraphResult::ClockExhausted);
    CHECK(counters(fixture.graph).epoch == max);
    Access::clock(fixture.graph, 0, 0, 1);
    const float* missing = nullptr;
    pulp::audio::BufferView<const float> invalid(&missing, 1, 32);
    CHECK(fixture.graph.process_checked(block.out, invalid, 1) ==
          TriggerGraphResult::InvalidBuffers);
    CHECK(block.output[0] == 91);
    pulp::audio::BufferView<const float> missing_array(nullptr, 1, 32);
    CHECK_FALSE(missing_array.has_valid_channels());
    CHECK_FALSE(invalid.has_valid_channels());
    CHECK(block.in.has_valid_channels());
    CHECK(fixture.graph.process_checked(block.out, missing_array, 1) ==
          TriggerGraphResult::InvalidBuffers);
    CHECK(std::all_of(block.output.begin(), block.output.end(), [](float v) { return v == 91; }));
    CHECK(counters(fixture.graph).cursor == 0);
    CHECK(counters(fixture.graph).sequence == 0);
}

TEST_CASE("Typed emission attempts and overflow have distinct budgets", "[trigger]") {
    Fixture fixture;
    Access::clock(fixture.graph, 0, 0, 1, 1);
    CHECK(Access::emit(fixture.graph, 0, 0) == TriggerEmitResult::StaleEpoch);
    for (int i = 0; i < 8; ++i)
        CHECK(Access::emit(fixture.graph, 0, 1) == TriggerEmitResult::Accepted);
    for (int i = 0; i < 7; ++i)
        CHECK(Access::emit(fixture.graph, 0, 1) == TriggerEmitResult::Overflow);
    CHECK(Access::emit(fixture.graph, 0, 1) == TriggerEmitResult::AttemptLimit);
    CHECK(Access::emit(fixture.graph, 0, 1) == TriggerEmitResult::AttemptLimit);
    auto count = counters(fixture.graph);
    CHECK(count.attempts == 16);
    CHECK(count.accepted == 8);
    CHECK(count.sequence == 16);
    CHECK(count.overflow == 7);
    CHECK(count.diagnostic_refusals == 2);
    auto trace = fixture.graph.trigger_trace();
    REQUIRE(trace.size() == 8);
    for (std::size_t i = 0; i < trace.size(); ++i)
        CHECK(trace[i].sequence == i + 1);
    Access::clock(fixture.graph, 1, 0, 1, 1);
    CHECK(Access::emit(fixture.graph, -1, 1) == TriggerEmitResult::OutOfRange);
    CHECK(Access::emit(fixture.graph, 2, 1) == TriggerEmitResult::Future);
    CHECK(Access::emit(fixture.graph, 4097, 1) == TriggerEmitResult::Future);
    CHECK(Access::emit(fixture.graph, 0, 1) == TriggerEmitResult::Late);
    CHECK(Access::emit(fixture.graph, 1, 1, 1) == TriggerEmitResult::Unbound);
    Access::clock(fixture.graph, 0, 0, 1, 2);
    CHECK(Access::emit(fixture.graph, 0, 1) == TriggerEmitResult::InvalidPhase);
    CHECK(counters(fixture.graph).sequence == 0);
}

TEST_CASE("Typed transport rejects future targets and caps one sample",
          "[trigger][negative-control]") {
    Fixture fixture;
    Access::clock(fixture.graph, 0, 0, 1, 1);
    CHECK(Access::schedule(fixture.graph, -1, 1) == TriggerEmitResult::OutOfRange);
    for (int i = 0; i < 8; ++i)
        REQUIRE(Access::schedule(fixture.graph, 0, 1) == TriggerEmitResult::Accepted);
    CHECK(Access::schedule(fixture.graph, 0, 1) == TriggerEmitResult::Overflow);
    CHECK(counters(fixture.graph).accepted == 8);
    CHECK(fixture.graph.trigger_trace().size() == 8);
    CHECK(Access::schedule(fixture.graph, 1, 1) == TriggerEmitResult::Future);
    Access::clock(fixture.graph, 10, 0, 1, 1);
    CHECK(Access::schedule(fixture.graph, 9, 1) == TriggerEmitResult::Late);
}

TEST_CASE("Typed reset replacement and release retire old generation access", "[trigger]") {
    Fixture fixture;
    Block block;
    block.input[0] = 1;
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 1) == TriggerGraphResult::Accepted);
    REQUIRE(fixture.graph.try_reset_events() == TriggerGraphResult::Accepted);
    CHECK(counters(fixture.graph).epoch == 2);
    CHECK(counters(fixture.graph).cursor == 0);
    CHECK(fixture.graph.trigger_trace().empty());
    auto old = Access::pin(fixture.graph);
    auto candidate = fixture.graph.prepare_trigger_plan(48000, 32);
    REQUIRE(candidate.result == TriggerGraphResult::Accepted);
    REQUIRE(fixture.graph.try_install_prepared_trigger_plan(candidate.plan) ==
            TriggerGraphResult::Accepted);
    CHECK(counters(fixture.graph).epoch == 3);
    block.output.fill(91);
    Access::old_render(fixture.graph, old, block.out, block.in, 1);
    CHECK(block.output[0] == 91);
    auto latest = Access::pin(fixture.graph);
    fixture.graph.release();
    Access::old_render(fixture.graph, latest, block.out, block.in, 1);
    CHECK(block.output[0] == 91);
}

TEST_CASE("Typed candidate resource reservations include retained generations", "[trigger]") {
    Fixture fixture(16, 4096);
    auto first = fixture.graph.trigger_resources();
    CHECK(first.total() <= TriggerResourceReport::limit);
    CHECK(first.arena_bytes >= 4096 * 8 * sizeof(TriggerRecord));
    CHECK(Access::budget(fixture.graph, 2097152));
    CHECK_FALSE(Access::budget(fixture.graph, 2097153));
    auto candidate = fixture.graph.prepare_trigger_plan(48000, 4096);
    REQUIRE(candidate.result == TriggerGraphResult::Accepted);
    CHECK(fixture.graph.trigger_resources().generation_bytes > first.generation_bytes);
    candidate.plan.reset();
    CHECK(fixture.graph.trigger_resources().total() == first.total());
}

TEST_CASE("Generic preparation cannot downgrade a typed graph", "[trigger]") {
    Fixture fixture;
    CHECK_FALSE(fixture.graph.prepare(48000, 32));
    auto edit = fixture.graph.begin_prepared_topology_edit();
    CHECK(edit->prepare(48000, 32) != SignalGraph::PreparedTopologyEdit::Result::Prepared);
    fixture.graph.begin_swap_edit();
    CHECK(fixture.graph.prepare_swap(48000, 32) == SignalGraph::SwapResult::NeedsEagerPrepare);
    Block block;
    CHECK(fixture.graph.process_checked(block.out, block.in, 1) == TriggerGraphResult::Accepted);
}

TEST_CASE("Typed admission refuses cardinality payload and contraction cycles", "[trigger]") {
    Fixture fixture;
    CHECK(fixture.graph.connect_trigger({fixture.producer, fixture.envelope}) ==
          TriggerGraphResult::Cardinality);
    CHECK(fixture.graph.connect_trigger(
              {fixture.producer, fixture.envelope, static_cast<TriggerKind>(99)}) ==
          TriggerGraphResult::Payload);
    CHECK(fixture.graph.connect_trigger({fixture.producer, fixture.producer}) ==
          TriggerGraphResult::Feedback);
    CHECK(fixture.graph.connect_trigger({fixture.in, fixture.envelope}) ==
          TriggerGraphResult::UnsupportedProducer);
    auto gain = fixture.graph.add_gain_node();
    REQUIRE(fixture.graph.connect(fixture.envelope, 0, gain, 0));
    REQUIRE(fixture.graph.connect(gain, 0, fixture.producer, 0));
    auto cycle = fixture.graph.prepare_trigger_plan(48000, 32);
    CHECK(cycle.result == TriggerGraphResult::DependencyCycle);
    REQUIRE(fixture.graph.disconnect(gain, 0, fixture.producer, 0));
    CHECK(fixture.graph.add_trigger_producer({}) == 0);
    CHECK(fixture.graph.prepare_trigger_plan(48000, 32).result == TriggerGraphResult::Accepted);
}

TEST_CASE("Typed Busy and failed replacement preserve live candidate ownership", "[trigger]") {
    SignalGraph graph;
    auto in = graph.add_input_node(1), out = graph.add_output_node(1);
    auto producer = graph.add_trigger_producer({});
    auto envelope = graph.add_trigger_envelope({0.5f});
    std::unique_ptr<PreparedTriggerPlan> replacement;
    REQUIRE(graph.connect(in, 0, producer, 0));
    REQUIRE(graph.connect(envelope, 0, out, 0));
    REQUIRE(graph.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted);
    auto initial = graph.prepare_trigger_plan(48000, 32);
    REQUIRE(initial.result == TriggerGraphResult::Accepted);
    REQUIRE(graph.try_install_prepared_trigger_plan(initial.plan) == TriggerGraphResult::Accepted);
    auto prepared = graph.prepare_trigger_plan(48000, 32);
    REQUIRE(prepared.result == TriggerGraphResult::Accepted);
    replacement = std::move(prepared.plan);
    auto* identity = replacement.get();
    Block block;
    block.input[0] = 1;
    Access::gate(graph, true);
    const auto reset = graph.try_reset_events();
    const auto install = graph.try_install_prepared_trigger_plan(replacement);
    const auto render = graph.process_checked(block.out, block.in, 1);
    Access::gate(graph, false);
    CHECK(reset == TriggerGraphResult::Busy);
    CHECK(install == TriggerGraphResult::Busy);
    CHECK(render == TriggerGraphResult::Busy);
    REQUIRE(graph.process_checked(block.out, block.in, 1) == TriggerGraphResult::Accepted);
    CHECK(replacement.get() == identity);
    CHECK(counters(graph).cursor == 1);
    graph.set_parallel_routing_enabled(true);
    CHECK(graph.try_install_prepared_trigger_plan(replacement) == TriggerGraphResult::Mode);
    CHECK(replacement.get() == identity);
    CHECK(counters(graph).cursor == 1);
    graph.set_parallel_routing_enabled(false);
    CHECK(graph.try_install_prepared_trigger_plan(replacement) == TriggerGraphResult::Accepted);
    CHECK(counters(graph).cursor == 0);
    CHECK(counters(graph).epoch == 2);
}

TEST_CASE("Typed maximum-frame render has bounded retained events and no allocation", "[trigger]") {
    Fixture fixture(16, 4096);
    std::vector<float> input(4096), output(4096);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = i % 2 == 0 ? 1 : 0;
    const float* ip = input.data();
    float* op = output.data();
    pulp::audio::BufferView<const float> in(&ip, 1, input.size());
    pulp::audio::BufferView<float> out(&op, 1, output.size());
    TriggerGraphResult result;
    std::size_t allocations = 0;
    {
        pulp::test::RtAllocationProbe probe;
        result = fixture.graph.process_checked(out, in, 4096);
        allocations = probe.allocation_count();
    }
    CHECK(result == TriggerGraphResult::Accepted);
    CHECK(allocations == 0);
    CHECK(fixture.graph.trigger_trace().size() == 2048 * 8);
    CHECK(counters(fixture.graph).sequence == 2048 * 16);
    CHECK(counters(fixture.graph).overflow == 2048 * 8);
    CHECK(counters(fixture.graph).cursor == 4096);
}

TEST_CASE("Typed factory identity cannot be replaced by a custom callback", "[trigger]") {
    Fixture fixture;
    CustomNodeType fake;
    fake.type_id = "pulp.trigger.onset";
    fake.num_input_ports = 1;
    fake.process = [](auto&, const auto&, int) {};
    REQUIRE(fixture.graph.register_custom_node_type(fake));
    CHECK(fixture.graph.prepare_trigger_plan(48000, 32).result ==
          TriggerGraphResult::UnsupportedProducer);
}

TEST_CASE("Typed diagnostic counters saturate instead of wrapping", "[trigger]") {
    Fixture fixture;
    Access::clock(fixture.graph, 0, 0, 1, 1, true);
    CHECK(Access::emit(fixture.graph, 0, 0) == TriggerEmitResult::StaleEpoch);
    for (int i = 0; i < 8; ++i)
        REQUIRE(Access::emit(fixture.graph, 0, 1) == TriggerEmitResult::Accepted);
    CHECK(Access::emit(fixture.graph, 0, 1) == TriggerEmitResult::Overflow);
    CHECK(counters(fixture.graph).overflow == std::numeric_limits<std::uint64_t>::max());
    CHECK(counters(fixture.graph).diagnostic_refusals == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("Typed plans reject legacy Event feedback before replacing live state", "[trigger]") {
    Fixture fixture;
    Access::old_event_feedback(fixture.graph, fixture.in, fixture.out);
    CHECK(fixture.graph.prepare_trigger_plan(48000, 32).result == TriggerGraphResult::Feedback);
    Block block;
    block.input[0] = 1;
    CHECK(fixture.graph.process_checked(block.out, block.in, 1) == TriggerGraphResult::Accepted);
    CHECK(block.output[0] == 1);
}

TEST_CASE("Typed reset clears previous-block audio feedback on a fixed block schedule",
          "[trigger]") {
    Fixture fixture;
    REQUIRE(fixture.graph.disconnect(fixture.envelope, 0, fixture.out, 0));
    auto gain = fixture.graph.add_gain_node();
    REQUIRE(fixture.graph.connect(fixture.envelope, 0, gain, 0));
    REQUIRE(fixture.graph.connect_feedback(gain, 0, gain, 0));
    REQUIRE(fixture.graph.connect(gain, 0, fixture.out, 0));
    auto prepared = fixture.graph.prepare_trigger_plan(48000, 32);
    REQUIRE(prepared.result == TriggerGraphResult::Accepted);
    REQUIRE(fixture.graph.try_install_prepared_trigger_plan(prepared.plan) ==
            TriggerGraphResult::Accepted);
    Block block;
    block.input[0] = 1;
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 2) == TriggerGraphResult::Accepted);
    CHECK(block.output[0] == 1);
    CHECK(block.output[1] == 0.5f);
    block.input[0] = 0;
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 2) == TriggerGraphResult::Accepted);
    CHECK(block.output[0] == 1.25f);
    CHECK(block.output[1] == 0.625f);
    REQUIRE(fixture.graph.try_reset_events() == TriggerGraphResult::Accepted);
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 2) == TriggerGraphResult::Accepted);
    CHECK(block.output[0] == 0);
    CHECK(block.output[1] == 0);
    CHECK(fixture.graph.trigger_trace().empty());
}

TEST_CASE("Removing typed factories restores ordinary graph preparation", "[trigger]") {
    Fixture fixture;
    REQUIRE(fixture.graph.remove_node(fixture.producer));
    REQUIRE(fixture.graph.remove_node(fixture.envelope));
    CHECK(fixture.graph.trigger_edges().empty());
    REQUIRE(fixture.graph.connect(fixture.in, 0, fixture.out, 0));
    REQUIRE(fixture.graph.prepare(48000, 32));
    Block block;
    block.input[0] = 0.75f;
    fixture.graph.process(block.out, block.in, 32);
    CHECK(block.output[0] == 0.75f);
}

TEST_CASE("Typed process silences after live snapshot invalidation", "[trigger]") {
    Fixture fixture;
    Block block;
    block.output.fill(91);
    REQUIRE(fixture.graph.add_gain_node() != 0);

    // Authoring invalidates the published generation while retaining the typed
    // authoring marker. The ordinary null-snapshot path must still clear output.
    fixture.graph.process(block.out, block.in, 1);
    CHECK(block.output[0] == 0);
}

TEST_CASE("Typed contraction preserves both members external audio ports", "[trigger]") {
    SignalGraph graph;
    auto input = graph.add_input_node(2);
    auto output = graph.add_output_node(2);
    auto producer = graph.add_trigger_producer({});
    auto envelope = graph.add_trigger_envelope({0.5f});
    REQUIRE(graph.connect(input, 0, producer, 0));
    REQUIRE(graph.connect(input, 1, envelope, 0));
    REQUIRE(graph.connect(producer, 0, output, 1));
    REQUIRE(graph.connect(envelope, 0, output, 0));
    REQUIRE(graph.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted);
    auto plan = graph.prepare_trigger_plan(48000, 4);
    REQUIRE(plan.result == TriggerGraphResult::Accepted);
    REQUIRE(graph.try_install_prepared_trigger_plan(plan.plan) == TriggerGraphResult::Accepted);
    std::array<float, 4> source{0, 1, 0, 0}, carrier{0.25f, 0.25f, 0.25f, 0.25f};
    std::array<float, 4> scaled{}, passthrough{};
    const float* ip[]{source.data(), carrier.data()};
    float* op[]{scaled.data(), passthrough.data()};
    pulp::audio::BufferView<const float> in(ip, 2, 4);
    pulp::audio::BufferView<float> out(op, 2, 4);
    REQUIRE(graph.process_checked(out, in, 4) == TriggerGraphResult::Accepted);
    CHECK(passthrough == source);
    CHECK(scaled == std::array<float, 4>{0, 0.25f, 0.125f, 0.0625f});
}

TEST_CASE("Typed contraction rejects external dependencies between pair members", "[trigger]") {
    for (bool reverse : {false, true}) {
        Fixture fixture;
        auto gain = fixture.graph.add_gain_node();
        REQUIRE(fixture.graph.connect(reverse ? fixture.envelope : fixture.producer, 0, gain, 0));
        REQUIRE(fixture.graph.connect(gain, 0, reverse ? fixture.producer : fixture.envelope, 0));
        CHECK(fixture.graph.prepare_trigger_plan(48000, 32).result ==
              TriggerGraphResult::DependencyCycle);
    }
}

TEST_CASE("Typed preflight rejects insufficient and unbounded channels without mutation",
          "[trigger]") {
    SignalGraph graph;
    auto input = graph.add_input_node(2);
    auto output = graph.add_output_node(1);
    TriggerProducerConfig config;
    config.input_channels = 2;
    config.lanes[0].input_channel = 1;
    auto producer = graph.add_trigger_producer(config);
    auto envelope = graph.add_trigger_envelope({0.5f});
    REQUIRE(graph.connect(input, 0, producer, 0));
    REQUIRE(graph.connect(input, 1, producer, 1));
    REQUIRE(graph.connect(envelope, 0, output, 0));
    REQUIRE(graph.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted);
    auto plan = graph.prepare_trigger_plan(48000, 32);
    REQUIRE(plan.result == TriggerGraphResult::Accepted);
    REQUIRE(graph.try_install_prepared_trigger_plan(plan.plan) == TriggerGraphResult::Accepted);
    Block block;
    block.output.fill(91);
    for (std::size_t channels : {1u, 65u}) {
        pulp::audio::BufferView<const float> invalid(&block.ip, channels, 32);
        CHECK(graph.process_checked(block.out, invalid, 1) == TriggerGraphResult::InvalidBuffers);
        CHECK(block.output[0] == 91);
        CHECK(counters(graph).cursor == 0);
        CHECK(counters(graph).sequence == 0);
        CHECK(graph.trigger_trace().empty());
    }
}

TEST_CASE("Typed generation reservation bounds measured maximum-frame preparation", "[trigger]") {
    Fixture fixture(16, 4096);
    const auto initial = fixture.graph.trigger_resources();
    std::vector<std::unique_ptr<PreparedTriggerPlan>> retained;
    retained.reserve(16);
    std::size_t generation_bytes = 0;
    bool refused = false;
    for (int attempt = 0; attempt < 16; ++attempt) {
        TriggerPreparation candidate;
        std::size_t allocated = 0;
        const auto before = fixture.graph.trigger_resources();
        {
            pulp::test::RtAllocationProbe probe;
            candidate = fixture.graph.prepare_trigger_plan(48000, 4096);
            allocated = probe.allocated_bytes();
        }
        const auto after = fixture.graph.trigger_resources();
        INFO("measured preparation bytes=" << allocated);
        if (candidate.result == TriggerGraphResult::Budget) {
            refused = true;
            CHECK_FALSE(candidate.plan);
            CHECK(allocated == 0);
            CHECK(after.total() == before.total());
            CHECK(before.total() + generation_bytes > TriggerResourceReport::limit);
            break;
        }
        REQUIRE(candidate.result == TriggerGraphResult::Accepted);
        generation_bytes = after.generation_bytes - before.generation_bytes;
        INFO("charged generation bytes=" << generation_bytes);
        CHECK(allocated <= generation_bytes);
        CHECK(after.total() <= TriggerResourceReport::limit);
        retained.push_back(std::move(candidate.plan));
    }
    REQUIRE(refused);
    Block block;
    block.input[0] = 1;
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 1) == TriggerGraphResult::Accepted);
    CHECK(block.output[0] == 1);
    retained.clear();
    CHECK(fixture.graph.trigger_resources().total() == initial.total());
}

TEST_CASE("Typed plans reject opaque Custom state", "[trigger]") {
    Fixture fixture;
    CustomNodeType opaque;
    opaque.type_id = "test.opaque";
    opaque.num_input_ports = opaque.num_output_ports = 1;
    opaque.process = [](auto&, const auto&, int) {};
    REQUIRE(fixture.graph.register_custom_node_type(opaque));
    REQUIRE(fixture.graph.add_custom_node(opaque.type_id) != 0);
    CHECK(fixture.graph.prepare_trigger_plan(48000, 32).result ==
          TriggerGraphResult::UnsupportedProducer);
}

TEST_CASE("Typed epoch exhaustion preserves replacement ownership", "[trigger]") {
    Fixture fixture;
    auto candidate = fixture.graph.prepare_trigger_plan(48000, 32);
    REQUIRE(candidate.result == TriggerGraphResult::Accepted);
    auto* identity = candidate.plan.get();
    Access::clock(fixture.graph, 0, 0, std::numeric_limits<std::uint64_t>::max());
    CHECK(fixture.graph.try_install_prepared_trigger_plan(candidate.plan) ==
          TriggerGraphResult::ClockExhausted);
    CHECK(candidate.plan.get() == identity);
    CHECK(counters(fixture.graph).epoch == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("Typed bounded audio chain preparation is charged and resets filter state", "[trigger]") {
    Fixture fixture(16, 4096);
    REQUIRE(fixture.graph.disconnect(fixture.envelope, 0, fixture.out, 0));
    auto modulation = fixture.graph.add_trigger_modulation();
    auto lowpass = fixture.graph.add_trigger_lowpass(0.5f);
    REQUIRE(fixture.graph.connect(fixture.envelope, 0, modulation, 0));
    REQUIRE(fixture.graph.connect(fixture.in, 0, modulation, 1));
    REQUIRE(fixture.graph.connect(modulation, 0, lowpass, 0));
    REQUIRE(fixture.graph.connect(lowpass, 0, fixture.out, 0));
    const auto before = fixture.graph.trigger_resources();
    TriggerPreparation candidate;
    std::size_t allocated = 0;
    {
        pulp::test::RtAllocationProbe probe;
        candidate = fixture.graph.prepare_trigger_plan(48000, 4096);
        allocated = probe.allocated_bytes();
    }
    REQUIRE(candidate.result == TriggerGraphResult::Accepted);
    const auto after = fixture.graph.trigger_resources();
    INFO("measured=" << allocated
                     << " charged=" << after.generation_bytes - before.generation_bytes);
    CHECK(allocated <= after.generation_bytes - before.generation_bytes);
    REQUIRE(fixture.graph.try_install_prepared_trigger_plan(candidate.plan) ==
            TriggerGraphResult::Accepted);
    Block block;
    block.input[0] = 1;
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 2) == TriggerGraphResult::Accepted);
    CHECK(block.output[0] == 0.5f);
    CHECK(block.output[1] == 0.25f);
    REQUIRE(fixture.graph.try_reset_events() == TriggerGraphResult::Accepted);
    REQUIRE(fixture.graph.process_checked(block.out, block.in, 2) == TriggerGraphResult::Accepted);
    CHECK(block.output[0] == 0.5f);
    CHECK(block.output[1] == 0.25f);
}

TEST_CASE("Typed retired generation releases its reservation only after the last owner",
          "[trigger]") {
    Fixture fixture;
    auto old = Access::pin(fixture.graph);
    const auto first = fixture.graph.trigger_resources();
    auto candidate = fixture.graph.prepare_trigger_plan(48000, 32);
    REQUIRE(candidate.result == TriggerGraphResult::Accepted);
    const auto overlap = fixture.graph.trigger_resources();
    REQUIRE(fixture.graph.try_install_prepared_trigger_plan(candidate.plan) ==
            TriggerGraphResult::Accepted);
    CHECK(fixture.graph.trigger_resources().total() == overlap.total());
    old.reset();
    CHECK(fixture.graph.trigger_resources().generation_bytes == first.generation_bytes);
}

TEST_CASE("Typed framework audio vector is charged through its maximum admitted count",
          "[trigger]") {
    Fixture fixture;
    std::size_t accepted_count = 0;
    bool refused = false;
    for (std::size_t count = 1; count <= 64; ++count) {
        REQUIRE(fixture.graph.add_trigger_lowpass(0.5f) != 0);
        const auto before = fixture.graph.trigger_resources();
        TriggerPreparation candidate;
        std::size_t allocated = 0;
        {
            pulp::test::RtAllocationProbe probe;
            candidate = fixture.graph.prepare_trigger_plan(48000, 1);
            allocated = probe.allocated_bytes();
        }
        const auto after = fixture.graph.trigger_resources();
        INFO("audio nodes=" << count << " measured=" << allocated
                            << " charged=" << after.generation_bytes - before.generation_bytes);
        if (candidate.result == TriggerGraphResult::Budget) {
            CHECK(allocated == 0);
            CHECK(after.total() == before.total());
            refused = true;
            break;
        }
        REQUIRE(candidate.result == TriggerGraphResult::Accepted);
        CHECK(allocated <= after.generation_bytes - before.generation_bytes);
        CHECK(after.total() <= TriggerResourceReport::limit);
        accepted_count = count;
    }
    INFO("maximum admitted framework audio count=" << accepted_count);
    CHECK(accepted_count > 0);
    CHECK(refused);
}

TEST_CASE("Typed and spectral preparation remain separate admission lanes", "[trigger]") {
    auto identity = SpectralIdentity::make(256, 63, 1, 48000);
    REQUIRE(identity);
    SignalGraph spectral_first;
    REQUIRE(spectral_first.add_spectral_analysis(*identity.value, "analysis"));
    std::array<NodeId, 4> refused_nodes{};
    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        refused_nodes = {
            spectral_first.add_trigger_producer({}), spectral_first.add_trigger_envelope({}),
            spectral_first.add_trigger_modulation(), spectral_first.add_trigger_lowpass(0.5f)};
        allocation_count = probe.allocation_count();
    }
    CHECK(allocation_count == 0);
    for (const auto id : refused_nodes)
        CHECK(id == 0);

    SignalGraph trigger_first;
    auto producer = trigger_first.add_trigger_producer({});
    auto envelope = trigger_first.add_trigger_envelope({});
    REQUIRE(producer != 0);
    REQUIRE(envelope != 0);
    REQUIRE(trigger_first.connect_trigger({producer, envelope}) == TriggerGraphResult::Accepted);
    REQUIRE(trigger_first.add_spectral_analysis(*identity.value, "analysis"));
    TriggerPreparation refused_plan;
    bool generic_prepared = true;
    {
        pulp::test::RtAllocationProbe probe;
        refused_plan = trigger_first.prepare_trigger_plan(48000, 32);
        generic_prepared = trigger_first.prepare(48000, 32);
        allocation_count = probe.allocation_count();
    }
    CHECK(allocation_count == 0);
    CHECK(refused_plan.result == TriggerGraphResult::Mode);
    CHECK_FALSE(generic_prepared);
}
