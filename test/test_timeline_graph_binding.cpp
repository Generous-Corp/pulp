#include <pulp/format/process_context.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/model.hpp>

#include "harness/scoped_rt_process_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

using namespace pulp;
using namespace pulp::host;
using namespace pulp::playback;
using namespace pulp::timebase;
using namespace pulp::timeline;

namespace {

template <typename T, typename E> T take(runtime::Result<T, E> result) {
    if (!result)
        std::abort();
    return std::move(result).value();
}

std::shared_ptr<const CompiledTempoMap> tempo_map(RationalRate rate = {48'000, 1}) {
    const std::array points{TempoPoint{{0}, 120.0}};
    return std::make_shared<const CompiledTempoMap>(points, rate);
}

std::shared_ptr<const audio::AudioFileData> audio_data(std::vector<float> mono) {
    auto result = std::make_shared<audio::AudioFileData>();
    result->sample_rate = 48'000;
    result->channels = {std::move(mono)};
    return result;
}

std::shared_ptr<const DecodedAudioAssetPool> asset_pool(std::vector<float> mono) {
    return take(DecodedAudioAssetPool::create({{{3}, audio_data(std::move(mono))}}));
}

Clip audio_clip(float gain = 1.0f, std::uint64_t frames = 512) {
    ClipPlaybackProperties properties;
    properties.gain_linear = gain;
    return take(Clip::create_absolute({100}, {0}, frames, {48'000, 1}, MediaRef{{3}, {0}, frames},
                                      properties));
}

std::shared_ptr<const Project> audio_project(float gain = 1.0f, std::uint64_t frames = 512) {
    auto track = take(Track::create({10}, "audio", {audio_clip(gain, frames)}));
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                          std::vector<Track>{std::move(track)}));
    const auto hash = ContentHash::from_hex(std::string(64, 'a'));
    if (!hash)
        std::abort();
    MediaAsset asset{.id = {3},
                     .name = "tone",
                     .frame_count = frames,
                     .sample_rate = {48'000, 1},
                     .content_hash = *hash};
    ProjectInput input;
    input.id = {1};
    input.name = "binding";
    input.next_item_id = 1'000;
    input.root_sequence_id = {2};
    input.assets = {asset};
    input.sequences = {std::move(sequence)};
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

std::shared_ptr<const Project> parallel_audio_project(std::uint64_t frames = 512,
                                                      bool reverse_tracks = false) {
    ClipPlaybackProperties properties;
    auto first_clip = take(Clip::create_absolute({100}, {0}, frames, {48'000, 1},
                                                 MediaRef{{3}, {0}, frames}, properties));
    properties.gain_linear = 0.5f;
    auto second_clip = take(Clip::create_absolute({101}, {0}, frames, {48'000, 1},
                                                  MediaRef{{3}, {0}, frames}, properties));
    auto first = take(Track::create({10}, "first", {std::move(first_clip)}));
    auto second = take(Track::create({11}, "second", {std::move(second_clip)}));
    std::vector<Track> tracks;
    tracks.push_back(std::move(first));
    tracks.push_back(std::move(second));
    if (reverse_tracks)
        std::reverse(tracks.begin(), tracks.end());
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                          std::move(tracks)));
    const auto hash = ContentHash::from_hex(std::string(64, 'a'));
    if (!hash)
        std::abort();
    MediaAsset asset{.id = {3},
                     .name = "tone",
                     .frame_count = frames,
                     .sample_rate = {48'000, 1},
                     .content_hash = *hash};
    ProjectInput input;
    input.id = {1};
    input.name = "parallel binding";
    input.next_item_id = 1'000;
    input.root_sequence_id = {2};
    input.assets = {asset};
    input.sequences = {std::move(sequence)};
    return std::make_shared<const Project>(take(Project::create(std::move(input))));
}

NoteEvent note(const CompiledTempoMap& map, std::uint64_t id, std::int64_t start_sample,
               std::int64_t end_sample) {
    const auto start = map.samples_to_ticks({start_sample});
    const auto end = map.samples_to_ticks({end_sample});
    return {{id}, start, end - start, 0xffff, 60, 0};
}

std::shared_ptr<const Project> note_project(const CompiledTempoMap& map) {
    auto content = take(NoteContent::create({note(map, 101, 5, 20)}));
    auto clip = take(Clip::create({100}, {0}, map.samples_to_ticks({128}) - TickPosition{0},
                                  std::move(content)));
    auto track = take(Track::create({10}, "notes", {std::move(clip)}));
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                          std::vector<Track>{std::move(track)}));
    return std::make_shared<const Project>(
        take(Project::create(ProjectInput{{1}, "notes", 1'000, {2}, {}, {std::move(sequence)}})));
}

class InlineExecutor final : public CompileExecutor {
  public:
    bool submit(std::unique_ptr<CompileTask> task, std::chrono::steady_clock::time_point) override {
        if (!task)
            return false;
        while (task->run_slice({std::chrono::steady_clock::now() + std::chrono::seconds(1),
                                10'000}) == CompileTaskStatus::Pending) {
        }
        return true;
    }
};

struct ProgramHarness {
    PlaybackProgramStore store;
    InlineExecutor executor;
    PlaybackProgramCompiler compiler{store, executor, std::chrono::microseconds(0)};

    void publish(std::shared_ptr<const Project> project,
                 std::shared_ptr<const CompiledTempoMap> map,
                 std::shared_ptr<const DecodedAudioAssetPool> assets, std::uint64_t revision) {
        ProgramCompileRequest request;
        request.project = std::move(project);
        request.sequence_id = {2};
        request.tempo_map = std::move(map);
        request.document_revision = revision;
        request.dirty.all = true;
        request.audio_assets = std::move(assets);
        REQUIRE(compiler.submit(std::move(request)));
        INFO("compile error " << static_cast<int>(compiler.status().last_error.code));
        REQUIRE_FALSE(compiler.status().has_error);
    }
};

struct Buffer {
    Buffer(std::size_t channels, std::size_t frames, float fill = 0.0f)
        : storage(channels, std::vector<float>(frames, fill)), pointers(channels),
          const_pointers(channels) {
        for (std::size_t channel = 0; channel < channels; ++channel) {
            pointers[channel] = storage[channel].data();
            const_pointers[channel] = storage[channel].data();
        }
    }
    audio::BufferView<float> view() {
        return {pointers.data(), pointers.size(), storage.front().size()};
    }
    audio::BufferView<const float> const_view() const {
        return {const_pointers.data(), const_pointers.size(), storage.front().size()};
    }
    std::vector<std::vector<float>> storage;
    std::vector<float*> pointers;
    std::vector<const float*> const_pointers;
};

TransportSnapshot snapshot(const PlaybackProgram& program, std::uint32_t frames,
                           std::int64_t start = 0) {
    TransportSnapshot result;
    result.tempo_map = &program.tempo_map();
    result.sample_rate = program.tempo_map().sample_rate();
    result.frame_count = frames;
    result.is_playing = true;
    result.range_count = 1;
    result.ranges[0].frame_count = frames;
    result.ranges[0].timeline_sample_start = {start};
    result.ranges[0].timeline_tick_start = program.tempo_map().samples_to_ticks({start});
    result.ranges[0].timeline_tick_end = program.tempo_map().samples_to_ticks({start + frames});
    return result;
}

TimelineGraphBindingConfig config(std::uint32_t channels = 2) {
    TimelineGraphBindingConfig result;
    result.audio_channels = channels;
    result.audio_limits.max_channels = channels;
    result.audio_limits.max_block_frames = 1024;
    return result;
}

} // namespace

static_assert(TimelineGraphPlaybackBinding::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);
static_assert(ArrangementAudioTrackRenderer::process_rt_safety_class ==
              audio::RtSafetyClass::AudioCallbackSafeAfterPrepare);

TEST_CASE("timeline graph binding matches direct audio across varied blocks") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(parallel_audio_project(), map, asset_pool(std::vector<float>(512, 0.25f)), 1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);

    SignalGraph graph;
    graph.set_parallel_routing_enabled(true);
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0},
                            TimelineTrackGraphRoute{{11}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, routes, config(), 48'000.0, 1024));
    REQUIRE(graph.routed_execution_status(1024).strict_routed_ready());
    const auto stable_node = binding.audio_node_for({10});
    const auto second_stable_node = binding.audio_node_for({11});
    REQUIRE(stable_node != 0);
    REQUIRE(second_stable_node != 0);
    REQUIRE(second_stable_node != stable_node);
    REQUIRE(graph.node(stable_node)->transport_sensitive);

    std::int64_t start = 0;
    for (const std::uint32_t frames : {1u, 17u, 64u, 257u}) {
        const auto transport = snapshot(*pinned, frames, start);
        Buffer direct(2, frames, 9.0f);
        REQUIRE(ArrangementAudioRenderer::process(*pinned, transport, direct.view()) ==
                AudioRenderStatus::Rendered);
        Buffer input(2, frames);
        Buffer routed(2, frames, 9.0f);
        auto routed_view = routed.view();
        REQUIRE(binding.process(routed_view, input.const_view(), transport));
        REQUIRE(routed.storage == direct.storage);
        start += frames;
    }
    REQUIRE(binding.audio_node_for({10}) == stable_node);
    REQUIRE(binding.audio_node_for({11}) == second_stable_node);
    REQUIRE(graph.routing_executor_stats().parallel_levels_dispatched >= 1);
    REQUIRE(graph.routed_walk_fallbacks() == 0);
    REQUIRE(binding.prepare(*pinned, routes, config(), 48'000.0, 1024));
    REQUIRE(binding.audio_node_for({10}) == stable_node);
    REQUIRE(binding.audio_node_for({11}) == second_stable_node);
}

TEST_CASE("timeline graph binding uses one exact split transport snapshot") {
    const auto map = tempo_map();
    std::vector<float> ramp(256);
    for (std::size_t index = 0; index < ramp.size(); ++index)
        ramp[index] = static_cast<float>(index);
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, ramp.size()), map, asset_pool(ramp), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    graph.set_parallel_routing_enabled(true);
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 64));

    auto split = snapshot(*pinned, 32, 112);
    split.loop = {true, map->samples_to_ticks({64}), map->samples_to_ticks({128})};
    split.range_count = 2;
    split.ranges[0].frame_count = 16;
    split.ranges[0].timeline_sample_start = {112};
    split.ranges[0].timeline_tick_start = map->samples_to_ticks({112});
    split.ranges[0].timeline_tick_end = map->samples_to_ticks({128});
    split.ranges[1].sample_offset = 16;
    split.ranges[1].frame_count = 16;
    split.ranges[1].timeline_sample_start = {64};
    split.ranges[1].timeline_tick_start = map->samples_to_ticks({64});
    split.ranges[1].timeline_tick_end = map->samples_to_ticks({80});
    split.ranges[1].discontinuity = true;

    Buffer direct(1, 32);
    REQUIRE(ArrangementAudioRenderer::process(*pinned, split, direct.view()) ==
            AudioRenderStatus::Rendered);
    Buffer input(1, 32);
    Buffer routed(1, 32);
    auto routed_view = routed.view();
    REQUIRE(binding.process(routed_view, input.const_view(), split));
    REQUIRE(routed.storage == direct.storage);
    REQUIRE(routed.storage[0][15] == 127.0f);
    REQUIRE(routed.storage[0][16] == 64.0f);
}

TEST_CASE("timeline graph binding projects split transport as one callback context") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map,
                     asset_pool(std::vector<float>(128, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    format::ProcessContext observed;
    bool called = false;
    CustomNodeType recorder;
    recorder.type_id = "timeline.callback-context-recorder";
    recorder.num_input_ports = 1;
    recorder.num_output_ports = 1;
    recorder.process_transport = [&](audio::BufferView<float>& out,
                                     const audio::BufferView<const float>& in, int frames,
                                     const format::ProcessContext& context) {
        observed = context;
        called = true;
        std::copy_n(in.channel_ptr(0), frames, out.channel_ptr(0));
    };
    REQUIRE(graph.register_custom_node_type(std::move(recorder)));
    const auto recorder_node = graph.add_custom_node("timeline.callback-context-recorder");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(recorder_node, 0, output_node, 0));
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, recorder_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 64));

    auto split = snapshot(*pinned, 32, 112);
    split.range_count = 2;
    split.ranges[0].frame_count = 16;
    split.ranges[1].sample_offset = 16;
    split.ranges[1].frame_count = 16;
    split.ranges[1].timeline_sample_start = {64};
    split.ranges[1].timeline_tick_start = map->samples_to_ticks({64});
    split.ranges[1].timeline_tick_end = map->samples_to_ticks({80});
    split.ranges[1].discontinuity = true;
    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), split));
    REQUIRE(called);
    REQUIRE(observed.num_samples == 32);
    REQUIRE(observed.transport_jump);
}

TEST_CASE("timeline graph binding adopts live programs without replacing nodes") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, assets, 1);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    Buffer input(1, 64);
    NodeId node = 0;
    RendererProgramKey first_key;
    {
        auto first = programs.store.read();
        REQUIRE(binding.prepare(*first, routes, config(1), 48'000.0, 128));
        node = binding.audio_node_for({10});
        Buffer before(1, 64);
        auto before_view = before.view();
        REQUIRE(binding.process(before_view, input.const_view(), snapshot(*first, 64)));
        REQUIRE(before.storage[0][0] == 1.0f);
        first_key = binding.renderer_key_for({10});
        const auto first_state = binding.renderer_state_for({10});
        REQUIRE(first_key.item_id == ItemId{10});
        REQUIRE(first_key.generation != 0);
        REQUIRE(first_state.valid);
        REQUIRE(first_state.source_sample == SamplePosition{64});
    }

    programs.publish(audio_project(0.5f, 128), map, assets, 2);
    auto next = programs.store.read();
    Buffer after(1, 64);
    auto after_view = after.view();
    REQUIRE(binding.process(after_view, input.const_view(), snapshot(*next, 64, 64)));
    REQUIRE(after.storage[0][0] == 0.5f);
    REQUIRE(binding.audio_node_for({10}) == node);
    const auto next_key = binding.renderer_key_for({10});
    const auto next_state = binding.renderer_state_for({10});
    REQUIRE(next_key.item_id == first_key.item_id);
    REQUIRE(next_key.generation > first_key.generation);
    REQUIRE(next_state.valid);
    REQUIRE(next_state.key == next_key);
    REQUIRE(next_state.source_sample == SamplePosition{128});
}

TEST_CASE("timeline graph binding injects separately rendered notes") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map), map, take(DecodedAudioAssetPool::create({})), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    const auto midi_output = graph.add_midi_output_node();
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, midi_output}};
    REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 128));

    Buffer input(1, 64);
    Buffer output(1, 64);
    auto output_view = output.view();
    const auto result = binding.process(output_view, input.const_view(), snapshot(*pinned, 64));
    REQUIRE(result);
    REQUIRE(result.emitted_note_events == 2);
    midi::MidiBuffer events;
    events.reserve(8);
    midi::UmpBuffer ump_events;
    ump_events.reserve(8);
    events.attach_ump(&ump_events);
    REQUIRE(graph.extract_midi(midi_output, events));
    REQUIRE(events.size() == 2);
    REQUIRE(ump_events.size() == 2);
    REQUIRE(events[0].sample_offset == 5);
    REQUIRE(events[1].sample_offset == 20);
    REQUIRE(graph.routed_walk_fallbacks() == 0);
}

TEST_CASE("timeline graph binding reports exact routed capacity axes") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(), map, asset_pool(std::vector<float>(512, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};

    SignalGraph::GraphLimits graph_limits;
    graph_limits.max_nodes = 2;
    graph.set_limits(graph_limits);
    auto result = binding.preflight(*pinned, routes, config(), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::NodeLimitExceeded);
    REQUIRE(result.actual == 3);
    REQUIRE(result.limit == 2);
    graph_limits.max_nodes = 3;
    graph.set_limits(graph_limits);
    REQUIRE(binding.preflight(*pinned, routes, config(), 64));

    graph_limits = {};
    graph_limits.max_connections = 1;
    graph.set_limits(graph_limits);
    result = binding.preflight(*pinned, routes, config(), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::ConnectionLimitExceeded);
    REQUIRE(result.actual == 2);
    REQUIRE(result.limit == 1);
    graph_limits.max_connections = 2;
    graph.set_limits(graph_limits);
    REQUIRE(binding.preflight(*pinned, routes, config(), 64));

    graph_limits = {};
    graph_limits.max_ports = 4;
    graph.set_limits(graph_limits);
    result = binding.preflight(*pinned, routes, config(), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::TotalPortLimitExceeded);
    REQUIRE(result.actual == 5);
    REQUIRE(result.limit == 4);
    graph_limits.max_ports = 5;
    graph.set_limits(graph_limits);
    REQUIRE(binding.preflight(*pinned, routes, config(), 64));
}

TEST_CASE("timeline graph binding fails closed before an ineligible domain") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(), map, asset_pool(std::vector<float>(512, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    graph.set_canonical_executor_routing_enabled(false);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};

    // Exercise the otherwise unreachable corrupt-enum negative path without
    // changing SignalGraph's public model: preflight must reject it before
    // adding binding nodes or opting the graph into a different domain.
    auto& corrupt = const_cast<GraphNode&>(graph.nodes().front());
    const auto original_type = corrupt.type;
    corrupt.type = static_cast<NodeType>(0xff);
    const auto result = binding.prepare(*pinned, routes, config(), 48'000.0, 64);
    corrupt.type = original_type;

    REQUIRE(result.code == TimelineGraphAdmissionCode::RoutedTopologyIneligible);
    REQUIRE(binding.audio_node_for({10}) == 0);
    REQUIRE(graph.nodes().size() == 1);
    REQUIRE_FALSE(graph.canonical_executor_routing_enabled());
}

TEST_CASE("timeline graph binding process is allocation free") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(), map, asset_pool(std::vector<float>(512, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    graph.set_parallel_routing_enabled(true);
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, routes, config(), 48'000.0, 128));
    const auto transport = snapshot(*pinned, 64);
    Buffer input(2, 64);
    Buffer output(2, 64);
    auto output_view = output.view();
    std::size_t allocations = 1;
    {
        test::ScopedRtProcessProbe probe;
        REQUIRE(binding.process(output_view, input.const_view(), transport));
        allocations = probe.allocation_count();
    }
    REQUIRE(allocations == 0);
}

TEST_CASE("track renderer capacity rejection preserves oversized output") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(), map, asset_pool(std::vector<float>(512, 1.0f)), 1);
    PlaybackProgramBlockLatch latch;
    auto block = latch.begin_block(programs.store);
    ArrangementAudioTrackRenderer renderer({10});
    Buffer oversized(2, 65, 7.0f);
    auto view = oversized.view();

    REQUIRE(renderer.process(block, snapshot(*block.program(), 65), view,
                             {.max_channels = 2, .max_block_frames = 64}) ==
            AudioRenderStatus::CapacityExceeded);
    REQUIRE(oversized.storage[0] == std::vector<float>(65, 7.0f));
    REQUIRE(oversized.storage[1] == std::vector<float>(65, 7.0f));
}

TEST_CASE("timeline graph binding capacity rejection preserves oversized output") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(), map, asset_pool(std::vector<float>(512, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, routes, config(), 48'000.0, 64));
    Buffer input(2, 65);
    Buffer output(2, 65, 7.0f);
    auto output_view = output.view();

    const auto result = binding.process(output_view, input.const_view(), snapshot(*pinned, 65));
    REQUIRE(result.code == TimelineGraphProcessCode::CapacityExceeded);
    REQUIRE(output.storage[0] == std::vector<float>(65, 7.0f));
    REQUIRE(output.storage[1] == std::vector<float>(65, 7.0f));
}

TEST_CASE("timeline graph binding uses the SignalGraph executor routed limits") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(), map, asset_pool(std::vector<float>(512, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    for (std::size_t index = 0; index < 509; ++index)
        REQUIRE(graph.add_gain_node() != 0);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};

    REQUIRE(binding.preflight(*pinned, routes, config(), 64));
    REQUIRE(graph.add_gain_node() != 0);
    const auto result = binding.preflight(*pinned, routes, config(), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::NodeLimitExceeded);
    REQUIRE(result.actual == 513);
    REQUIRE(result.limit == graph::GraphRuntimeLimits{}.max_nodes);
}

TEST_CASE("timeline graph binding validates sample rate at prepare and publication") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(note_project(*map), map, take(DecodedAudioAssetPool::create({})), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};

    REQUIRE(binding.prepare(*pinned, routes, config(1), 44'100.0, 64).code ==
            TimelineGraphAdmissionCode::SampleRateMismatch);
    REQUIRE(binding.audio_node_for({10}) == 0);
    REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 64));

    const auto changed_map = tempo_map({44'100, 1});
    programs.publish(note_project(*changed_map), changed_map,
                     take(DecodedAudioAssetPool::create({})), 2);
    auto changed = programs.store.read();
    Buffer input(1, 32);
    Buffer output(1, 32, 7.0f);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*changed, 32)).code ==
            TimelineGraphProcessCode::InvalidTransport);
    REQUIRE(output.storage[0] == std::vector<float>(32, 7.0f));
    REQUIRE(binding.prepare(*changed, routes, config(1), 44'100.0, 64));
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*changed, 32)));
}

TEST_CASE("timeline graph binding compares fractional rates in its double API domain") {
    const auto map = tempo_map({48'000, 1'001});
    const double projected_rate = static_cast<double>(map->sample_rate().as_long_double());
    ProgramHarness programs;
    programs.publish(note_project(*map), map, take(DecodedAudioAssetPool::create({})), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};

    REQUIRE(binding.prepare(*pinned, routes, config(1), projected_rate, 64));
    REQUIRE(binding.prepare(*pinned, routes, config(1),
                            std::nextafter(projected_rate,
                                           std::numeric_limits<double>::infinity()),
                            64)
                .code == TimelineGraphAdmissionCode::SampleRateMismatch);
    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 32)));
}

TEST_CASE("SignalGraph routed status rejects a build-invalid live snapshot") {
    SignalGraph graph;
    graph.set_canonical_executor_routing_enabled(true);
    const auto input_node = graph.add_input_node(1);
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(input_node, 0, output_node, 0));
    for (std::size_t index = 0; index < 510; ++index)
        REQUIRE(graph.add_gain_node() != 0);

    graph.acquire_routed_only_execution();
    REQUIRE(graph.prepare(48'000.0, 64));
    const auto exact = graph.routed_execution_status(64);
    REQUIRE(exact.serial_snapshot_valid);
    REQUIRE(exact.serial_pool_fits);
    REQUIRE(exact.strict_routed_ready());

    REQUIRE(graph.add_gain_node() != 0);
    REQUIRE(graph.prepare(48'000.0, 64));
    const auto above_bound = graph.routed_execution_status(64);
    REQUIRE(above_bound.prepared);
    REQUIRE_FALSE(above_bound.serial_snapshot_valid);
    REQUIRE_FALSE(above_bound.strict_routed_ready());
    REQUIRE_FALSE(above_bound.reference_walk_permitted);

    Buffer input(1, 32, 1.0f);
    Buffer output(1, 32, 7.0f);
    auto output_view = output.view();
    graph.process(output_view, input.const_view(), 32);
    REQUIRE(output.storage[0] == std::vector<float>(32, 0.0f));
    REQUIRE(graph.routed_only_execution_failures() == 1);

    graph.release_routed_only_execution();
    graph.process(output_view, input.const_view(), 32);
    REQUIRE(output.storage[0] == std::vector<float>(32, 1.0f));
}

TEST_CASE("timeline graph binding reports loss of its prepared routed snapshot") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map,
                     asset_pool(std::vector<float>(128, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 64));

    REQUIRE(graph.add_gain_node() != 0);
    Buffer input(1, 32);
    Buffer output(1, 32, 7.0f);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 32)).code ==
            TimelineGraphProcessCode::RoutedDispatchFailed);
    REQUIRE(output.storage[0] == std::vector<float>(32, 0.0f));
}

TEST_CASE("timeline graph binding rejects MIDI capacity before graph mutation") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, asset_pool(std::vector<float>(128, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    auto exact = config(1);
    exact.maximum_note_events_per_track_per_block = 1024;
    REQUIRE(binding.preflight(*pinned, routes, exact, 64));
    auto oversized = exact;
    oversized.maximum_note_events_per_track_per_block = 1025;
    const auto result = binding.prepare(*pinned, routes, oversized, 48'000.0, 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::NoteCapacityExceeded);
    REQUIRE(result.actual == 1025);
    REQUIRE(result.limit == 1024);
    REQUIRE(graph.nodes().size() == 1);
    REQUIRE(binding.audio_node_for({10}) == 0);
}

TEST_CASE("timeline graph binding shape and topology guards preserve caller output") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, assets, 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 64));

    Buffer wrong_channels(2, 32);
    Buffer output(1, 32, 7.0f);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, wrong_channels.const_view(), snapshot(*pinned, 32)).code ==
            TimelineGraphProcessCode::InputShapeMismatch);
    REQUIRE(output.storage[0] == std::vector<float>(32, 7.0f));
    Buffer wrong_frames(1, 31);
    REQUIRE(binding.process(output_view, wrong_frames.const_view(), snapshot(*pinned, 32)).code ==
            TimelineGraphProcessCode::InputShapeMismatch);
    REQUIRE(output.storage[0] == std::vector<float>(32, 7.0f));

    programs.publish(parallel_audio_project(128), map, assets, 2);
    auto added = programs.store.read();
    Buffer input(1, 32);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*added, 32)).code ==
            TimelineGraphProcessCode::TopologyChanged);
    REQUIRE(output.storage[0] == std::vector<float>(32, 7.0f));
}

TEST_CASE("timeline graph binding topology fingerprint ignores track ordering") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness programs;
    programs.publish(parallel_audio_project(128), map, assets, 1);
    auto initial = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0},
                            TimelineTrackGraphRoute{{11}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*initial, routes, config(1), 48'000.0, 64));

    programs.publish(parallel_audio_project(128, true), map, assets, 2);
    auto reordered = programs.store.read();
    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*reordered, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);

    programs.publish(audio_project(1.0f, 128), map, assets, 3);
    auto removed = programs.store.read();
    std::fill(output.storage[0].begin(), output.storage[0].end(), 7.0f);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*removed, 32)).code ==
            TimelineGraphProcessCode::TopologyChanged);
    REQUIRE(output.storage[0] == std::vector<float>(32, 7.0f));
}

TEST_CASE("timeline graph binding preserves prepared state after a later route fails") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, assets, 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array good{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, good, config(1), 48'000.0, 64));
    const auto stable_node = binding.audio_node_for({10});

    programs.publish(parallel_audio_project(128), map, assets, 2);
    auto two_tracks = programs.store.read();
    const std::array bad{TimelineTrackGraphRoute{{10}, output_node, 0, 0},
                         TimelineTrackGraphRoute{{11}, 999'999, 0, 0}};
    REQUIRE(binding.prepare(*two_tracks, bad, config(1), 48'000.0, 64).code ==
            TimelineGraphAdmissionCode::MissingDestination);
    REQUIRE(binding.audio_node_for({10}) == stable_node);
    REQUIRE(binding.audio_node_for({11}) == 0);

    programs.publish(audio_project(1.0f, 128), map, assets, 3);
    auto restored = programs.store.read();
    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*restored, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);
}

TEST_CASE("timeline graph binding can churn the same ItemId without registry retention") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, asset_pool(std::vector<float>(128, 1.0f)), 1);
    auto pinned = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    for (int iteration = 0; iteration < 8; ++iteration) {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 64));
        Buffer input(1, 32);
        Buffer output(1, 32);
        auto output_view = output.view();
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 32)));
    }
    REQUIRE(graph.nodes().size() == 1);
}
