// Compiled into the existing trigger-graph test unit: the graph runtime is already
// included there, so this fixture adds no redundant heavy-header consumer.
#include "../harness/rt_allocation_probe.hpp"
#include "../harness/rt_contract_probe.hpp"
#include "../harness/paced_timing_cell.hpp"

#include <catch2/catch_test_macros.hpp>
#include <pulp/format/process_context.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/runtime/build_info.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>
#include <utility>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace g5_rt_fixture {
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

template <class Callback>
void for_each_fixed_origin_callback(std::size_t block_frames, double sample_rate, std::size_t count,
                                    Callback&& callback) {
    using clock = std::chrono::steady_clock;
    const auto period =
        std::chrono::duration<double>(static_cast<double>(block_frames) / sample_rate);
    const auto origin = clock::now() + std::chrono::milliseconds(10);
    for (std::size_t i = 0; i < count; ++i) {
        const auto target = origin + std::chrono::duration_cast<clock::duration>(period * i);
        std::this_thread::sleep_until(target);
        callback(i, target, clock::now());
    }
}

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

struct PacedState {
    std::uint64_t producer_cursor = 0;
    std::uint64_t consumer_cursor = 0;
    std::size_t delivered = 0;
    std::size_t bad = 0;
};

// Producer -> consumer graph-event chain shared by the paced measurement and
// the paced-timing matrix cell. Every block emits one event at its first
// sample; the consumer writes the payload (17) to output sample zero.
void build_paced_graph(SignalGraph& graph, PacedState& state, bool routed) {
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
}

PacedObservation render_paced(std::size_t block_frames, bool routed) {
    PacedState state;
    SignalGraph graph;
    build_paced_graph(graph, state, routed);

    std::array<float, 4096> source{};
    std::array<float, 4096> destination{};
    PacedObservation observation;
    std::thread callback_thread([&] {
        using clock = std::chrono::steady_clock;
        const auto period =
            std::chrono::duration<double>(static_cast<double>(block_frames) / 48000.0);
        for_each_fixed_origin_callback(
            block_frames, 48000.0, observation.callback_us.size(),
            [&](std::size_t i, clock::time_point target, clock::time_point before) {
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
            });
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
} // namespace g5_rt_fixture

using namespace g5_rt_fixture;

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
        for (const bool reset_requested : {false, true}) {
            struct ResetState {
                std::size_t producer_calls = 0;
                std::size_t delivered = 0;
                std::size_t emit_failures = 0;
                std::size_t bad_events = 0;
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
                if (event_block.graph_reader != nullptr) {
                    for (const auto& event : event_block.graph_reader->events()) {
                        ++state.delivered;
                        if (event.sample != 2 || event.payload != 17)
                            ++state.bad_events;
                        out.channel_ptr(0)[0] = static_cast<float>(event.payload);
                    }
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
            REQUIRE(graph.connect_graph_event({producer, consumer}) ==
                    GraphEventBindingResult::Accepted);
            graph.set_canonical_executor_routing_enabled(routed);
            REQUIRE(graph.prepare(48000.0, 2));
            std::array<float, 2> source{}, destination{};
            const float* source_ptr = source.data();
            float* destination_ptr = destination.data();
            pulp::audio::BufferView<const float> in(&source_ptr, 1, 1);
            pulp::audio::BufferView<float> out(&destination_ptr, 1, 1);
            pulp::format::ProcessContext transport;
            transport.reset_requested = reset_requested;
            transport.position_samples = 0;
            // Both paths reach sample 2: reset covers [0,1), [0,1), [1,2), [2,3);
            // no reset covers [0,1), [1,2), [2,3), [3,4). Only the first call emits.
            for (std::size_t callback = 0; callback < 4; ++callback) {
                std::size_t allocations = 0, bytes = 0, locks = 0, blocking = 0;
                {
                    pulp::test::RtAllocationProbe allocation_probe;
                    pulp::test::RtContractProbe contract_probe;
                    if (callback == 1)
                        graph.process(out, in, 1, transport);
                    else
                        graph.process(out, in, 1);
                    allocations = allocation_probe.allocation_count();
                    bytes = allocation_probe.allocated_bytes();
                    locks = contract_probe.lock_events();
                    blocking = contract_probe.blocking_events();
                }
                INFO("routed=" << routed << " reset_requested=" << reset_requested
                               << " callback=" << callback);
                CHECK(allocations == 0);
                CHECK(bytes == 0);
                CHECK(locks == 0);
                CHECK(blocking == 0);
                CHECK(state.emit_failures == 0);
                CHECK(state.bad_events == 0);
                CHECK(state.producer_calls == callback + 1);
                CHECK(state.delivered == ((!reset_requested && callback >= 2) ? 1 : 0));
                CHECK(destination[0] == ((!reset_requested && callback == 2) ? 17.0f : 0.0f));
            }
        }
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

// Paced-timing matrix cell: the same producer/consumer graph-event chain under
// a configurable fixed-origin callback count. Timing runs carry no scoped
// resource probes; the oracle is the delivered payload on every block.
TEST_CASE("G5 graph event paced matrix cell", "[.][host][graph][g5][events][paced-matrix]") {
    const auto config = pulp::test::paced::read_config("routed", 128);
    REQUIRE((config.variant == "direct" || config.variant == "routed"));
    REQUIRE(config.rate == 48000);
    REQUIRE(config.frames <= 4096);
    PacedState state;
    SignalGraph graph;
    build_paced_graph(graph, state, config.variant == "routed");
    std::array<float, 4096> source{};
    std::array<float, 4096> destination{};
    const auto frames = static_cast<int>(config.frames);
    const float* source_ptr = source.data();
    float* destination_ptr = destination.data();
    pulp::audio::BufferView<const float> in(&source_ptr, 1, config.frames);
    pulp::audio::BufferView<float> out(&destination_ptr, 1, config.frames);
    std::size_t bad_before = 0;
    const auto rows = pulp::test::paced::run_cell(
        config,
        [&](std::size_t, std::uint64_t) {
            bad_before = state.bad;
            graph.process(out, in, frames);
        },
        [&](std::size_t index, std::uint64_t) {
            return destination[0] == 17.0f && state.bad == bad_before &&
                   state.delivered == index + 1;
        });
    CHECK(pulp::test::paced::print_cell("g5-graph-event", config, rows) == 0);
    CHECK(state.delivered == config.callbacks);
}

namespace g12_activation_fixture {
using namespace pulp::host;
using clock = std::chrono::steady_clock;

#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#define G12_RTSAN_BOUNDARY [[clang::nonblocking]]
#define G12_HAS_RTSAN 1
#endif
#endif
#ifndef G12_RTSAN_BOUNDARY
#define G12_RTSAN_BOUNDARY
#endif

constexpr std::size_t kBlockFrames = 128;
constexpr std::size_t kCallbacks = 128;
constexpr double kRate = 48000.0;
constexpr float kInput = 0.5f;
constexpr float kOldOutput = 0.25f;
constexpr float kNewOutput = 0.5f;

enum class AudioClass : std::uint8_t { Old, New, Mixed, Silent, Wrong };

AudioClass classify(const float* samples, std::size_t frames) {
    const bool old =
        std::all_of(samples, samples + frames, [](float x) { return x == kOldOutput; });
    const bool fresh =
        std::all_of(samples, samples + frames, [](float x) { return x == kNewOutput; });
    if (old)
        return AudioClass::Old;
    if (fresh)
        return AudioClass::New;
    if (std::all_of(samples, samples + frames, [](float x) { return x == 0.0f; }))
        return AudioClass::Silent;
    if (std::all_of(samples, samples + frames,
                    [](float x) { return x == kOldOutput || x == kNewOutput; }))
        return AudioClass::Mixed;
    return AudioClass::Wrong;
}

std::int64_t ns(clock::time_point at) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(at.time_since_epoch()).count();
}

struct CallbackRow {
    std::size_t index = 0;
    std::uint64_t first_sample = 0;
    std::int64_t target_ns = 0;
    std::int64_t start_ns = 0;
    std::int64_t end_ns = 0;
    AudioClass audio = AudioClass::Wrong;
    std::size_t allocations = 0;
    std::size_t locks = 0;
    std::size_t blocking = 0;
};

#if defined(G12_HAS_RTSAN)
enum class AnalyzerControl { Clean, Malloc, Mutex, Sleep, Write };

AnalyzerControl analyzer_control() {
    const auto* requested = std::getenv("G12_RTSAN_CONTROL");
    if (!requested)
        return AnalyzerControl::Clean;
    const std::string_view name(requested);
    if (name == "malloc")
        return AnalyzerControl::Malloc;
    if (name == "mutex")
        return AnalyzerControl::Mutex;
    if (name == "sleep")
        return AnalyzerControl::Sleep;
    if (name == "write")
        return AnalyzerControl::Write;
    REQUIRE(name == "clean");
    return AnalyzerControl::Clean;
}

// Real intercepted operations, never probe counters. The volatile pointer keeps
// Release optimization from deleting the malloc/free pair used as a control.
void inject_analyzer_control(AnalyzerControl control) {
    switch (control) {
    case AnalyzerControl::Clean:
        break;
    case AnalyzerControl::Malloc: {
        void* (*volatile allocate)(std::size_t) = &std::malloc;
        std::free(allocate(16));
        break;
    }
    case AnalyzerControl::Mutex: {
        std::mutex mutex;
        mutex.lock();
        mutex.unlock();
        break;
    }
    case AnalyzerControl::Sleep:
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        break;
    case AnalyzerControl::Write: {
        constexpr char message[] = "G12 RTSan write control\n";
        (void)::write(STDERR_FILENO, message, sizeof(message) - 1);
        break;
    }
    }
}
#else
enum class AnalyzerControl { Clean };
AnalyzerControl analyzer_control() { return AnalyzerControl::Clean; }
#endif

// The annotation marks only the actual graph callback. Scheduling, probes,
// clock reads and evidence writes stay outside the RTSan realtime boundary.
void graph_callback_boundary(SignalGraph& graph, pulp::audio::BufferView<float>& output,
                             const pulp::audio::BufferView<const float>& input,
                             AnalyzerControl control, std::size_t frames = kBlockFrames) G12_RTSAN_BOUNDARY {
#if defined(G12_HAS_RTSAN)
    inject_analyzer_control(control);
#else
    (void)control;
#endif
    graph.process(output, input, static_cast<int>(frames));
}

struct Observation {
    std::vector<CallbackRow> rows = std::vector<CallbackRow>(kCallbacks);
    std::vector<std::uint32_t> audio_bits;
    std::size_t frames = kBlockFrames;
    pulp::format::GraphRuntimeExecutorStats executor_stats{};
    std::uint64_t walk_fallbacks = 0;
    std::uint64_t execution_failures = 0;
    bool parallel_enabled = false;
    std::int64_t prepare_start_ns = 0;
    std::int64_t prepare_end_ns = 0;
    std::int64_t request_ns = 0;
    std::int64_t return_ns = 0;
    SignalGraph::PreparedTopologyEdit::Result commit_result =
        SignalGraph::PreparedTopologyEdit::Result::NotPrepared;
    int latency_samples = -1;
    SignalGraph::PreparedLatencySnapshot latency{};
    SignalGraph::PreparedStats prepared{};
    std::size_t first_new = kCallbacks;
    std::size_t missed_deadlines = 0;
};

bool exact_transition(const Observation& observation, std::size_t expected_first_new) {
    if (expected_first_new < 8 || expected_first_new >= observation.rows.size() ||
        observation.first_new != expected_first_new)
        return false;
    for (std::size_t i = 0; i < observation.rows.size(); ++i) {
        const auto& row = observation.rows[i];
        if (row.index != i || row.first_sample != i * observation.frames ||
            row.start_ns < row.target_ns || row.end_ns < row.start_ns ||
            row.audio != (i < expected_first_new ? AudioClass::Old : AudioClass::New))
            return false;
    }
    return observation.rows[expected_first_new].end_ns >= observation.request_ns;
}

bool meets_qualification_thresholds(const Observation& observation, double sample_rate,
                                    std::size_t frames) {
    const auto budget_ns = 1000000000.0 * static_cast<double>(frames) / sample_rate;
    const auto wake_limit_ns = 100000000.0 / sample_rate;
    std::vector<double> durations(observation.rows.size());
    std::vector<double> wake_errors(observation.rows.size());
    for (std::size_t i = 0; i < observation.rows.size(); ++i) {
        const auto& row = observation.rows[i];
        if (row.end_ns < row.start_ns || row.end_ns > row.target_ns + budget_ns)
            return false;
        durations[i] = static_cast<double>(row.end_ns - row.start_ns);
        wake_errors[i] = std::fabs(static_cast<double>(row.start_ns - row.target_ns));
    }
    std::sort(durations.begin(), durations.end());
    std::sort(wake_errors.begin(), wake_errors.end());
    const auto rank = static_cast<std::size_t>(std::ceil(observation.rows.size() * 0.99)) - 1;
    return durations[rank] < 0.7 * budget_ns && wake_errors[rank] < wake_limit_ns;
}

AudioClass render_one(SignalGraph& graph) {
    std::array<float, kBlockFrames> source{};
    std::array<float, kBlockFrames> destination{};
    source.fill(kInput);
    const float* source_ptr = source.data();
    float* destination_ptr = destination.data();
    pulp::audio::BufferView<const float> in(&source_ptr, 1, kBlockFrames);
    pulp::audio::BufferView<float> out(&destination_ptr, 1, kBlockFrames);
    graph.process(out, in, static_cast<int>(kBlockFrames));
    return classify(destination.data(), kBlockFrames);
}

NodeId prepare_gain_graph(SignalGraph& graph, bool routed, double rate = kRate,
                          std::size_t frames = kBlockFrames) {
    const auto input = graph.add_input_node(1);
    const auto gain = graph.add_gain_node("G12 activation gain");
    const auto output = graph.add_output_node(1);
    REQUIRE(input != 0);
    REQUIRE(gain != 0);
    REQUIRE(output != 0);
    REQUIRE(graph.connect(input, 0, gain, 0));
    REQUIRE(graph.connect(gain, 0, output, 0));
    REQUIRE(graph.set_node_gain(gain, 0.5f));
    graph.set_canonical_executor_routing_enabled(routed);
    REQUIRE(graph.prepare(rate, static_cast<int>(frames)));
    return gain;
}

Observation run(bool routed, double rate = kRate, std::size_t frames = kBlockFrames,
                std::size_t callbacks = kCallbacks, bool probes = true,
                std::string_view fault = {}) {
    const auto control = analyzer_control();
    SignalGraph graph;
    const auto gain = prepare_gain_graph(graph, routed, rate, frames);

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->set_node_gain(gain, fault == "wrong_gain" ? 0.75f : 1.0f));
    Observation observation;
    observation.frames = frames;
    observation.rows = std::vector<CallbackRow>(callbacks);
    observation.audio_bits.resize(callbacks * frames);
    observation.first_new = callbacks;
    observation.prepare_start_ns = ns(clock::now());
    REQUIRE(edit->prepare(rate, static_cast<int>(frames)) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    observation.prepare_end_ns = ns(clock::now());

    std::vector<float> source(frames, kInput);
    std::vector<float> destination(frames);
    std::atomic<std::size_t> completed{0};
    std::thread callback_thread([&] {
        for_each_fixed_origin_callback(
            frames, rate, callbacks,
            [&](std::size_t i, clock::time_point target, clock::time_point before) {
                const float* source_ptr = source.data();
                float* destination_ptr = destination.data();
                pulp::audio::BufferView<const float> in(&source_ptr, 1, frames);
                pulp::audio::BufferView<float> out(&destination_ptr, 1, frames);
                CallbackRow row;
                row.index = i;
                row.first_sample = i * frames;
                row.target_ns = ns(target);
                row.start_ns = ns(before);
                if (probes) {
                    pulp::test::RtAllocationProbe allocations;
                    pulp::test::RtContractProbe contract;
                    graph_callback_boundary(graph, out, in, control, frames);
                    row.allocations = allocations.allocation_count();
                    row.locks = contract.lock_events();
                    row.blocking = contract.blocking_events();
                } else {
                    graph_callback_boundary(graph, out, in, control, frames);
                }
                const auto after = clock::now();
                row.end_ns = ns(after);
                row.audio = classify(destination.data(), frames);
                observation.rows[i] = row;
                for (std::size_t f = 0; f < frames; ++f)
                    observation.audio_bits[i * frames + f] = std::bit_cast<std::uint32_t>(destination[f]);
                completed.store(i + 1, std::memory_order_release);
            });
    });
    const auto wait_deadline = clock::now() + std::chrono::seconds(2);
    while (completed.load(std::memory_order_acquire) < 8 && clock::now() < wait_deadline)
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    if (completed.load(std::memory_order_acquire) >= 8 && fault != "omit_commit") {
        observation.request_ns = ns(clock::now());
        observation.commit_result = edit->commit();
        observation.return_ns = ns(clock::now());
    }
    callback_thread.join();
    observation.latency_samples = graph.latency_samples();
    const auto latency = graph.prepared_latency_snapshot();
    REQUIRE(latency);
    observation.latency = *latency;
    observation.prepared = graph.prepared_stats();
    observation.executor_stats = graph.routing_executor_stats();
    observation.walk_fallbacks = graph.routed_walk_fallbacks();
    observation.execution_failures = graph.routed_only_execution_failures();
    observation.parallel_enabled = graph.parallel_routing_enabled();
    const auto period_ns =
        static_cast<std::int64_t>(1000000000.0 * static_cast<double>(frames) / rate);
    for (std::size_t i = 0; i < callbacks; ++i) {
        const auto& row = observation.rows[i];
        if (row.audio == AudioClass::New && observation.first_new == callbacks)
            observation.first_new = i;
        if (row.end_ns > row.target_ns + period_ns)
            ++observation.missed_deadlines;
    }
    return observation;
}
} // namespace g12_activation_fixture

#undef G12_RTSAN_BOUNDARY
#undef G12_HAS_RTSAN

TEST_CASE("G12 prepared gain activation has one exact audio transition under paced callbacks",
          "[host][graph][g12][activation][rt]") {
    using namespace g12_activation_fixture;
    for (const bool routed : {false, true}) {
#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
        if (const auto* backend = std::getenv("G12_RTSAN_BACKEND")) {
            const std::string_view requested(backend);
            REQUIRE((requested == "direct" || requested == "routed"));
            if (routed != (requested == "routed"))
                continue;
        }
#endif
#endif
        const auto observed = run(routed);
        INFO("routed=" << routed);
        CHECK(observed.executor_stats.blocks_processed == (routed ? kCallbacks : 0));
        CHECK(observed.executor_stats.parallel_levels_dispatched == 0);
        CHECK_FALSE(observed.parallel_enabled);
        CHECK(observed.walk_fallbacks == 0);
        CHECK(observed.execution_failures == 0);
        REQUIRE(observed.commit_result == SignalGraph::PreparedTopologyEdit::Result::Committed);
        REQUIRE(observed.first_new < kCallbacks);
        CHECK(exact_transition(observed, observed.first_new));
        CHECK(observed.prepare_end_ns >= observed.prepare_start_ns);
        CHECK(observed.prepare_end_ns < observed.request_ns);
        CHECK(observed.return_ns >= observed.request_ns);
        CHECK(observed.rows[observed.first_new].end_ns >= observed.request_ns);
        CHECK(observed.latency_samples == 0);
        CHECK(observed.latency.total_latency_samples == 0);
        CHECK(observed.latency.nodes.size() == 3);
        CHECK(observed.latency.connections.size() == 2);
        for (const auto& node : observed.latency.nodes)
            CHECK(node.artifact_latency_samples == 0);
        for (const auto& connection : observed.latency.connections)
            CHECK(connection.delay_samples == 0);
        CHECK(observed.prepared.node_count == 3);
        CHECK(observed.prepared.connection_count == 2);
        CHECK(observed.prepared.total_prepared_buffer_bytes > 0);
        for (const auto& row : observed.rows) {
            CHECK(row.allocations == 0);
            CHECK(row.locks == 0);
            CHECK(row.blocking == 0);
        }
        std::cout << "G12 activation routed=" << routed << " callbacks=" << kCallbacks
                  << " frames=" << kBlockFrames
                  << " prepare_duration_ns=" << observed.prepare_end_ns - observed.prepare_start_ns
                  << " commit_duration_ns=" << observed.return_ns - observed.request_ns
                  << " request_to_first_changed_output_ns="
                  << observed.rows[observed.first_new].end_ns - observed.request_ns
                  << " first_new_index=" << observed.first_new
                  << " first_new_sample=" << observed.rows[observed.first_new].first_sample
                  << " latency_samples=" << observed.latency_samples
                  << " prepared_buffer_bytes=" << observed.prepared.total_prepared_buffer_bytes
                  << " harness_payload_bytes=" << observed.rows.capacity() * sizeof(CallbackRow) +
                                             observed.audio_bits.capacity() * sizeof(std::uint32_t) +
                                             2 * kBlockFrames * sizeof(float)
                  << " missed_deadlines=" << observed.missed_deadlines << '\n';
        for (const auto& row : observed.rows)
            std::cout << "G12 raw routed=" << routed << " index=" << row.index
                      << " sample=" << row.first_sample << " target_ns=" << row.target_ns
                      << " start_ns=" << row.start_ns << " end_ns=" << row.end_ns
                      << " audio_class=" << static_cast<int>(row.audio)
                      << " allocations=" << row.allocations << " locks=" << row.locks
                      << " blocking=" << row.blocking << '\n';
    }
}

TEST_CASE("G12 activation oracle rejects wrong and mixed audio or timing",
          "[host][graph][g12][activation]") {
    using namespace g12_activation_fixture;
    std::array<float, 4> old{kOldOutput, kOldOutput, kOldOutput, kOldOutput};
    std::array<float, 4> fresh{kNewOutput, kNewOutput, kNewOutput, kNewOutput};
    std::array<float, 4> mixed{kOldOutput, kNewOutput, kOldOutput, kNewOutput};
    std::array<float, 4> silent{};
    std::array<float, 4> wrong{0.375f, 0.375f, 0.375f, 0.375f};
    CHECK(classify(old.data(), old.size()) == AudioClass::Old);
    CHECK(classify(fresh.data(), fresh.size()) == AudioClass::New);
    CHECK(classify(mixed.data(), mixed.size()) == AudioClass::Mixed);
    CHECK(classify(silent.data(), silent.size()) == AudioClass::Silent);
    CHECK(classify(wrong.data(), wrong.size()) == AudioClass::Wrong);

    g12_activation_fixture::Observation valid;
    valid.request_ns = 100;
    valid.first_new = 8;
    for (std::size_t i = 0; i < kCallbacks; ++i) {
        valid.rows[i] = {
            i, i * kBlockFrames, 10, 10, 110, i < 8 ? AudioClass::Old : AudioClass::New, 0, 0, 0};
    }
    REQUIRE(exact_transition(valid, 8));
    CHECK(meets_qualification_thresholds(valid, kRate, kBlockFrames));
    auto broken = valid;
    broken.rows[8].first_sample += 1;
    CHECK_FALSE(exact_transition(broken, 8));
    broken = valid;
    broken.rows[8].index += 1;
    CHECK_FALSE(exact_transition(broken, 8));
    broken = valid;
    broken.rows[9].audio = AudioClass::Old;
    CHECK_FALSE(exact_transition(broken, 8));
    broken = valid;
    broken.rows[8].end_ns = 99;
    CHECK_FALSE(exact_transition(broken, 8));
    CHECK_FALSE(exact_transition(valid, 9));
    broken = valid;
    broken.first_new = 9;
    CHECK_FALSE(exact_transition(broken, 8));
    broken = valid;
    const auto duration_limit = static_cast<std::int64_t>(
        std::ceil(0.7 * 1000000000.0 * static_cast<double>(kBlockFrames) / kRate));
    for (const auto i : {126, 127})
        broken.rows[i].end_ns = broken.rows[i].start_ns + duration_limit;
    CHECK_FALSE(meets_qualification_thresholds(broken, kRate, kBlockFrames));
    broken = valid;
    const auto wake_limit = static_cast<std::int64_t>(std::ceil(100000000.0 / kRate));
    for (const auto i : {126, 127}) {
        broken.rows[i].start_ns = broken.rows[i].target_ns + wake_limit;
        broken.rows[i].end_ns = broken.rows[i].start_ns + 100;
    }
    CHECK_FALSE(meets_qualification_thresholds(broken, kRate, kBlockFrames));
    broken = valid;
    broken.rows[8].end_ns = broken.rows[8].target_ns +
                            static_cast<std::int64_t>(std::ceil(
                                1000000000.0 * static_cast<double>(kBlockFrames) / kRate)) +
                            1;
    CHECK_FALSE(meets_qualification_thresholds(broken, kRate, kBlockFrames));
}

TEST_CASE("G12 omitted and refused commits preserve audio and stale commit cannot revert it",
          "[host][graph][g12][activation]") {
    using namespace g12_activation_fixture;
    for (const bool routed : {false, true}) {
        SignalGraph graph;
        const auto gain = prepare_gain_graph(graph, routed);
        REQUIRE(render_one(graph) == AudioClass::Old);

        auto uncommitted = graph.begin_prepared_topology_edit();
        REQUIRE(uncommitted);
        REQUIRE(uncommitted->set_node_gain(gain, 1.0f));
        REQUIRE(uncommitted->prepare(kRate, static_cast<int>(kBlockFrames)) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared);
        for (int i = 0; i < 8; ++i)
            CHECK(render_one(graph) == AudioClass::Old);

        auto refused = graph.begin_prepared_topology_edit();
        REQUIRE(refused);
        REQUIRE(refused->set_node_gain(gain, 1.0f));
        CHECK(refused->commit() == SignalGraph::PreparedTopologyEdit::Result::NotPrepared);
        CHECK(render_one(graph) == AudioClass::Old);

        auto winner = graph.begin_prepared_topology_edit();
        REQUIRE(winner);
        REQUIRE(winner->set_node_gain(gain, 1.0f));
        REQUIRE(winner->prepare(kRate, static_cast<int>(kBlockFrames)) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(winner->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
        CHECK(render_one(graph) == AudioClass::New);
        CHECK(uncommitted->commit() == SignalGraph::PreparedTopologyEdit::Result::StaleBase);
        CHECK(render_one(graph) == AudioClass::New);
    }
}

TEST_CASE("G12 paced activation receipt cell", "[.][host][graph][g12][activation][receipt]") {
    using namespace g12_activation_fixture;
    const auto integer = [](const char* key, std::size_t fallback) {
        const auto* text = std::getenv(key);
        if (!text) return fallback;
        char* end = nullptr;
        const auto value = std::strtoull(text, &end, 10);
        REQUIRE((end != text && *end == '\0'));
        return static_cast<std::size_t>(value);
    };
    const auto rate = integer("G12_ACTIVATION_RATE", 48000);
    const auto frames = integer("G12_ACTIVATION_FRAMES", 128);
    const auto callbacks = integer("G12_ACTIVATION_CALLBACKS", 32);
    REQUIRE((rate == 44100 || rate == 48000 || rate == 96000));
    REQUIRE((frames == 32 || frames == 64 || frames == 128 || frames == 256));
    REQUIRE((callbacks >= 16 && callbacks <= 100000));
    const std::string_view backend = std::getenv("G12_ACTIVATION_BACKEND")
        ? std::getenv("G12_ACTIVATION_BACKEND") : "direct";
    REQUIRE((backend == "direct" || backend == "routed"));
    const std::string_view fault = std::getenv("G12_ACTIVATION_FAULT")
        ? std::getenv("G12_ACTIVATION_FAULT") : "";
    REQUIRE((fault.empty() || fault == "omit_commit" || fault == "wrong_gain"));
    const auto observed = run(backend == "routed", static_cast<double>(rate), frames,
                              callbacks, false, fault);
    const auto& p = observed.prepared;
    const auto row_bytes = observed.rows.capacity() * sizeof(CallbackRow);
    const auto audio_bytes = observed.audio_bits.capacity() * sizeof(std::uint32_t);
    const auto io_bytes = 2 * frames * sizeof(float);
    std::cout << "g12_paced_activation_v1\nG12_CELL rate=" << rate << " frames=" << frames
              << " backend=" << backend << " callbacks=" << callbacks
              << " compiled_source=" << pulp::runtime::kGitSha
              << " build_type=" << pulp::runtime::kBuildType
              << " build_dirty=" << pulp::runtime::kGitDirty
              << " prepare_start_ns=" << observed.prepare_start_ns
              << " prepare_end_ns=" << observed.prepare_end_ns
              << " request_ns=" << observed.request_ns << " return_ns=" << observed.return_ns
              << " committed=" << (observed.commit_result == SignalGraph::PreparedTopologyEdit::Result::Committed)
              << " first_new=" << observed.first_new << " first_new_sample=" << observed.first_new * frames
              << " latency_samples=" << observed.latency_samples
              << " snapshot_latency=" << observed.latency.total_latency_samples
              << " snapshot_nodes=" << observed.latency.nodes.size()
              << " snapshot_edges=" << observed.latency.connections.size()
              << " node_latencies=";
    for (std::size_t i = 0; i < observed.latency.nodes.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << observed.latency.nodes[i].artifact_latency_samples;
    }
    std::cout << " edge_delays=";
    for (std::size_t i = 0; i < observed.latency.connections.size(); ++i) {
        if (i) std::cout << ',';
        std::cout << observed.latency.connections[i].delay_samples;
    }
    std::cout << " nodes=" << p.node_count << " edges=" << p.connection_count
              << " executor_blocks=" << observed.executor_stats.blocks_processed
              << " parallel_levels=" << observed.executor_stats.parallel_levels_dispatched
              << " parallel_enabled=" << observed.parallel_enabled
              << " walk_fallbacks=" << observed.walk_fallbacks
              << " execution_failures=" << observed.execution_failures
              << " voices=0 grains=0 probes=0"
              << " node_audio_bytes=" << p.node_audio_buffer_bytes
              << " automation_bytes=" << p.automation_buffer_bytes
              << " delay_bytes=" << p.delay_buffer_bytes
              << " spectral_bytes=" << p.spectral_buffer_bytes
              << " prepared_bytes=" << p.total_prepared_buffer_bytes
              << " row_stride=" << sizeof(CallbackRow) << " row_bytes=" << row_bytes
              << " audio_bytes=" << audio_bytes << " io_bytes=" << io_bytes
              << " harness_payload_bytes=" << row_bytes + audio_bytes + io_bytes
              << " misses=" << observed.missed_deadlines << '\n';
    for (const auto& row : observed.rows) {
        std::cout << "G12_ROW index=" << row.index << " sample=" << row.first_sample
                  << " target_ns=" << row.target_ns << " start_ns=" << row.start_ns
                  << " end_ns=" << row.end_ns << " audio_class=" << static_cast<int>(row.audio)
                  << " bits=";
        // Lossless per-block float32 RLE keeps every sample independently checkable.
        for (std::size_t f = 0; f < frames;) {
            const auto bits = observed.audio_bits[row.index * frames + f];
            std::size_t end = f + 1;
            while (end < frames && observed.audio_bits[row.index * frames + end] == bits) ++end;
            if (f) std::cout << ',';
            std::cout << std::hex << bits << std::dec << ':' << end - f;
            f = end;
        }
        std::cout << '\n';
    }
    REQUIRE(observed.commit_result == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(exact_transition(observed, observed.first_new));
}
