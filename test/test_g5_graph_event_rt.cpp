#include "harness/rt_allocation_probe.hpp"
#include "harness/rt_contract_probe.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/host/signal_graph_runtime.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
using namespace pulp::host;

constexpr std::size_t kFrames = 8192;
constexpr std::array<std::pair<std::uint64_t, std::uint16_t>, 7> kStimulus{{
    {2, 11},
    {63, 12},
    {64, 13},
    {64, 14},
    {127, 15},
    {4095, 16},
    {4096, 17},
}};

struct State {
    std::uint64_t producer_cursor = 0;
    std::uint64_t consumer_cursor = 0;
    std::array<GraphAuthoredEvent, 16> events{};
    std::size_t event_count = 0;
    std::size_t emit_failures = 0;
    std::size_t bad_windows = 0;
};

struct Observation {
    std::array<GraphAuthoredEvent, 16> events{};
    std::size_t event_count = 0;
    std::array<float, kFrames> audio{};
    std::size_t allocations = 0;
    std::size_t allocated_bytes = 0;
    std::size_t lock_events = 0;
    std::size_t blocking_events = 0;
    std::size_t emit_failures = 0;
    std::size_t bad_windows = 0;
};

struct PacedObservation {
    std::array<double, 128> callback_us{};
    std::array<double, 128> wake_jitter_us{};
    std::size_t allocations = 0;
    std::size_t lock_events = 0;
    std::size_t blocking_events = 0;
    std::size_t callback_events = 0;
    std::size_t bad_events = 0;
    std::size_t missed_deadlines = 0;
};

void escape_control() {
    // RT_ESCAPE_INJECTION: confirm_failure plants an actual callback escape here.
}

Observation render(std::size_t block_frames, bool routed) {
    State state;
    SignalGraph graph;
    CustomNodeType type;
    type.type_id = "test.g5.graph-event.rt";
    type.num_input_ports = type.num_output_ports = 1;
    type.process_events = [&state](auto& out, const auto& in, int frames,
                                   const CustomNodeEventBlock& event_block) {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
        if (event_block.graph_writer != nullptr) {
            escape_control();
            const auto end = state.producer_cursor + static_cast<std::uint64_t>(frames);
            for (const auto [sample, payload] : kStimulus) {
                if (sample >= state.producer_cursor && sample < end &&
                    event_block.graph_writer->emit_at(sample, payload) !=
                        GraphEventResult::Accepted)
                    ++state.emit_failures;
            }
            state.producer_cursor = end;
        }
        if (event_block.graph_reader != nullptr) {
            const auto end = state.consumer_cursor + static_cast<std::uint64_t>(frames);
            for (const auto& event : event_block.graph_reader->events()) {
                if (event.sample < state.consumer_cursor || event.sample >= end ||
                    state.event_count == state.events.size()) {
                    ++state.bad_windows;
                    continue;
                }
                state.events[state.event_count++] = event;
                out.channel_ptr(0)[event.sample - state.consumer_cursor] =
                    static_cast<float>(event.payload);
            }
            state.consumer_cursor = end;
        }
    };
    REQUIRE(graph.register_custom_node_type(type));
    const auto input = graph.add_input_node(1);
    const auto output = graph.add_output_node(1);
    const auto producer = graph.add_custom_node(type.type_id);
    const auto consumer = graph.add_custom_node(type.type_id);
    REQUIRE(graph.connect(input, 0, producer, 0));
    REQUIRE(graph.connect(producer, 0, consumer, 0));
    REQUIRE(graph.connect(consumer, 0, output, 0));
    REQUIRE(graph.connect_graph_event({producer, consumer}) == GraphEventBindingResult::Accepted);
    graph.set_canonical_executor_routing_enabled(routed);
    REQUIRE(graph.prepare(48000.0, 4096));

    std::array<float, kFrames> input_audio{};
    Observation observed;
    for (std::size_t start = 0; start < kFrames; start += block_frames) {
        const float* input_ptr = input_audio.data() + start;
        float* output_ptr = observed.audio.data() + start;
        pulp::audio::BufferView<const float> input_view(&input_ptr, 1, block_frames);
        pulp::audio::BufferView<float> output_view(&output_ptr, 1, block_frames);
        std::size_t allocations = 0;
        std::size_t bytes = 0;
        std::size_t locks = 0;
        std::size_t blocking = 0;
        {
            pulp::test::RtAllocationProbe allocation_probe;
            pulp::test::RtContractProbe contract_probe;
            graph.process(output_view, input_view, static_cast<int>(block_frames));
            allocations = allocation_probe.allocation_count();
            bytes = allocation_probe.allocated_bytes();
            locks = contract_probe.lock_events();
            blocking = contract_probe.blocking_events();
        }
        observed.allocations += allocations;
        observed.allocated_bytes += bytes;
        observed.lock_events += locks;
        observed.blocking_events += blocking;
    }
    observed.events = state.events;
    observed.event_count = state.event_count;
    observed.emit_failures = state.emit_failures;
    observed.bad_windows = state.bad_windows;
    return observed;
}

PacedObservation render_paced(std::size_t block_frames, bool routed) {
    struct PacedState {
        std::uint64_t producer_cursor = 0;
        std::uint64_t consumer_cursor = 0;
        std::size_t delivered = 0;
        std::size_t bad = 0;
    } state;
    SignalGraph graph;
    CustomNodeType type;
    type.type_id = "test.g5.graph-event.rt-paced";
    type.num_input_ports = type.num_output_ports = 1;
    type.process_events = [&state](auto& out, const auto& in, int frames,
                                   const CustomNodeEventBlock& event_block) {
        for (int i = 0; i < frames; ++i)
            out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
        if (event_block.graph_writer != nullptr) {
            escape_control();
            if (event_block.graph_writer->emit_at(state.producer_cursor, 17) !=
                GraphEventResult::Accepted)
                ++state.bad;
            state.producer_cursor += static_cast<std::uint64_t>(frames);
        }
        if (event_block.graph_reader != nullptr) {
            const auto events = event_block.graph_reader->events();
            if (events.size() != 1 || events[0].sample != state.consumer_cursor ||
                events[0].payload != 17)
                ++state.bad;
            else {
                out.channel_ptr(0)[0] = static_cast<float>(events[0].payload);
                ++state.delivered;
            }
            state.consumer_cursor += static_cast<std::uint64_t>(frames);
        }
    };
    REQUIRE(graph.register_custom_node_type(type));
    const auto input = graph.add_input_node(1);
    const auto output = graph.add_output_node(1);
    const auto producer = graph.add_custom_node(type.type_id);
    const auto consumer = graph.add_custom_node(type.type_id);
    REQUIRE(graph.connect(input, 0, producer, 0));
    REQUIRE(graph.connect(producer, 0, consumer, 0));
    REQUIRE(graph.connect(consumer, 0, output, 0));
    REQUIRE(graph.connect_graph_event({producer, consumer}) == GraphEventBindingResult::Accepted);
    graph.set_canonical_executor_routing_enabled(routed);
    REQUIRE(graph.prepare(48000.0, 4096));

    std::array<float, 4096> source{};
    std::array<float, 4096> destination{};
    PacedObservation observation;
    std::thread callback_thread([&] {
        using clock = std::chrono::steady_clock;
        const auto period =
            std::chrono::duration<double>(static_cast<double>(block_frames) / 48000.0);
        const auto start = clock::now() + std::chrono::milliseconds(10);
        for (std::size_t i = 0; i < observation.callback_us.size(); ++i) {
            const auto target = start + std::chrono::duration_cast<clock::duration>(period * i);
            std::this_thread::sleep_until(target);
            const auto before = clock::now();
            observation.wake_jitter_us[i] =
                std::chrono::duration<double, std::micro>(before - target).count();
            const float* source_ptr = source.data();
            float* destination_ptr = destination.data();
            pulp::audio::BufferView<const float> in(&source_ptr, 1, block_frames);
            pulp::audio::BufferView<float> out(&destination_ptr, 1, block_frames);
            std::size_t allocations = 0;
            std::size_t locks = 0;
            std::size_t blocking = 0;
            {
                pulp::test::RtAllocationProbe allocation_probe;
                pulp::test::RtContractProbe contract_probe;
                graph.process(out, in, static_cast<int>(block_frames));
                allocations = allocation_probe.allocation_count();
                locks = contract_probe.lock_events();
                blocking = contract_probe.blocking_events();
            }
            const auto after = clock::now();
            observation.callback_us[i] =
                std::chrono::duration<double, std::micro>(after - before).count();
            observation.allocations += allocations;
            observation.lock_events += locks;
            observation.blocking_events += blocking;
            if (after > target + std::chrono::duration_cast<clock::duration>(period))
                ++observation.missed_deadlines;
            if (destination[0] != 17.0f)
                ++state.bad;
        }
    });
    callback_thread.join();
    observation.callback_events = state.delivered;
    observation.bad_events = state.bad;
    return observation;
}

double p99(std::array<double, 128> values) {
    std::sort(values.begin(), values.end());
    return values[126];
}
} // namespace

TEST_CASE("G5 graph event callbacks keep fixed storage across both serial backends",
          "[host][graph][g5][events][rt]") {
    std::array<Observation, 4> baseline{};
    constexpr std::array<std::size_t, 4> sizes{64, 128, 256, 4096};
    for (std::size_t index = 0; index < sizes.size(); ++index) {
        const auto frames = sizes[index];
        const auto reference = render(frames, false);
        const auto routed = render(frames, true);
        INFO("frames=" << frames);
        for (const auto* result : {&reference, &routed}) {
            CHECK(result->allocations == 0);
            CHECK(result->allocated_bytes == 0);
            CHECK(result->lock_events == 0);
            CHECK(result->blocking_events == 0);
            CHECK(result->emit_failures == 0);
            CHECK(result->bad_windows == 0);
            REQUIRE(result->event_count == kStimulus.size());
            for (std::size_t i = 0; i < kStimulus.size(); ++i) {
                CHECK(result->events[i].sample == kStimulus[i].first);
                CHECK(result->events[i].payload == kStimulus[i].second);
                CHECK(result->events[i].sequence == i);
            }
            for (std::size_t sample = 0; sample < kFrames; ++sample) {
                float expected = 0.0f;
                for (const auto [event_sample, payload] : kStimulus)
                    if (event_sample == sample)
                        expected = static_cast<float>(payload);
                CHECK(result->audio[sample] == expected);
            }
        }
        CHECK(reference.events == routed.events);
        CHECK(reference.audio == routed.audio);
        if (index == 0)
            baseline[index] = reference;
        else {
            CHECK(reference.events == baseline[0].events);
            CHECK(reference.audio == baseline[0].audio);
        }
    }
}

TEST_CASE("G5 callback reset retires a queued graph event without allocating",
          "[host][graph][g5][events][rt]") {
    for (const bool routed : {false, true}) {
        struct ResetState {
            std::size_t producer_calls = 0;
            std::size_t delivered = 0;
            std::size_t emit_failures = 0;
        } state;
        SignalGraph graph;
        CustomNodeType type;
        type.type_id = "test.g5.graph-event.rt-reset";
        type.num_input_ports = type.num_output_ports = 1;
        type.process_events = [&state](auto& out, const auto& in, int frames,
                                       const CustomNodeEventBlock& event_block) {
            for (int i = 0; i < frames; ++i)
                out.channel_ptr(0)[i] = in.channel_ptr(0)[i];
            if (event_block.graph_writer != nullptr && state.producer_calls++ == 0) {
                escape_control();
                if (event_block.graph_writer->emit_at(2, 17) != GraphEventResult::Accepted)
                    ++state.emit_failures;
            }
            if (event_block.graph_reader != nullptr)
                state.delivered += event_block.graph_reader->events().size();
        };
        REQUIRE(graph.register_custom_node_type(type));
        const auto input = graph.add_input_node(1);
        const auto output = graph.add_output_node(1);
        const auto producer = graph.add_custom_node(type.type_id);
        const auto consumer = graph.add_custom_node(type.type_id);
        REQUIRE(graph.connect(input, 0, producer, 0));
        REQUIRE(graph.connect(producer, 0, consumer, 0));
        REQUIRE(graph.connect(consumer, 0, output, 0));
        REQUIRE(graph.connect_graph_event({producer, consumer}) ==
                GraphEventBindingResult::Accepted);
        graph.set_canonical_executor_routing_enabled(routed);
        REQUIRE(graph.prepare(48000.0, 2));
        std::array<float, 2> source{}, destination{};
        const float* source_ptr = source.data();
        float* destination_ptr = destination.data();
        pulp::audio::BufferView<const float> in(&source_ptr, 1, 1);
        pulp::audio::BufferView<float> out(&destination_ptr, 1, 1);
        std::size_t allocations = 0, locks = 0, blocking = 0;
        {
            pulp::test::RtAllocationProbe allocation_probe;
            pulp::test::RtContractProbe contract_probe;
            graph.process(out, in, 1);
            allocations += allocation_probe.allocation_count();
            locks += contract_probe.lock_events();
            blocking += contract_probe.blocking_events();
        }
        pulp::format::ProcessContext reset;
        reset.reset_requested = true;
        reset.position_samples = 0;
        {
            pulp::test::RtAllocationProbe allocation_probe;
            pulp::test::RtContractProbe contract_probe;
            graph.process(out, in, 1, reset);
            allocations += allocation_probe.allocation_count();
            locks += contract_probe.lock_events();
            blocking += contract_probe.blocking_events();
        }
        INFO("routed=" << routed);
        CHECK(allocations == 0);
        CHECK(locks == 0);
        CHECK(blocking == 0);
        CHECK(state.emit_failures == 0);
        CHECK(state.delivered == 0);
        CHECK(destination[0] == 0.0f);
    }
}

TEST_CASE("G5 graph event callback paced measurement", "[.][host][graph][g5][events][rt][paced]") {
    constexpr std::array<std::size_t, 4> sizes{64, 128, 256, 4096};
    for (const auto frames : sizes) {
        for (const bool routed : {false, true}) {
            const auto measured = render_paced(frames, routed);
            INFO("frames=" << frames << " routed=" << routed);
            CHECK(measured.callback_events == 128);
            CHECK(measured.bad_events == 0);
            CHECK(measured.allocations == 0);
            CHECK(measured.lock_events == 0);
            CHECK(measured.blocking_events == 0);
            const auto budget_us = 1000000.0 * static_cast<double>(frames) / 48000.0;
            std::cout << "G5 paced frames=" << frames << " routed=" << routed
                      << " callbacks=128 budget_us=" << budget_us
                      << " p99_callback_us=" << p99(measured.callback_us)
                      << " p99_wake_jitter_us=" << p99(measured.wake_jitter_us)
                      << " missed_deadlines=" << measured.missed_deadlines << '\n';
        }
    }
}
