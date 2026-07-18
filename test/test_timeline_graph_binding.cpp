#include <pulp/format/process_context.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/model.hpp>

#include "harness/scoped_rt_process_probe.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
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

class ReportedLatencySilence final : public PluginSlot {
  public:
    ReportedLatencySilence() {
        info_.name = "timeline PDC anchor";
        info_.unique_id = "pulp.test.timeline-pdc-anchor";
        info_.format = PluginFormat::CLAP;
        info_.is_effect = true;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 const midi::MidiBuffer&, midi::MidiBuffer&,
                 const ParameterEventQueue&, int frames) override {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            std::memset(output.channel_ptr(channel), 0,
                        sizeof(float) * static_cast<std::size_t>(frames));
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return 2; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

  private:
    PluginInfo info_;
};

class MidiCountingSlot final : public PluginSlot {
  public:
    MidiCountingSlot() {
        info_.name = "timeline MIDI counter";
        info_.unique_id = "pulp.test.timeline-midi-counter";
        info_.format = PluginFormat::CLAP;
        info_.is_effect = true;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 const midi::MidiBuffer& events, midi::MidiBuffer&,
                 const ParameterEventQueue&, int) override {
        last_event_count = events.size();
        for (std::size_t index = 0;
             index < std::min(events.size(), last_offsets.size()); ++index) {
            last_offsets[index] = events[index].sample_offset;
        }
        output.clear();
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    std::size_t last_event_count = 0;
    std::array<std::uint32_t, 8> last_offsets{};

  private:
    PluginInfo info_;
};

class DimensionTrackingSlot final : public PluginSlot {
  public:
    DimensionTrackingSlot() {
        info_.name = "timeline dimension tracker";
        info_.unique_id = "pulp.test.timeline-dimension-tracker";
        info_.format = PluginFormat::CLAP;
        info_.is_effect = true;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double sample_rate, int max_block_size) override {
        if (fail_sample_rate.load(std::memory_order_relaxed) == sample_rate)
            return false;
        prepared_sample_rate.store(sample_rate, std::memory_order_relaxed);
        prepared_max_block.store(max_block_size, std::memory_order_relaxed);
        return true;
    }
    void release() override {}
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const midi::MidiBuffer&, midi::MidiBuffer&,
                 const ParameterEventQueue&, int frames) override {
        for (std::size_t channel = 0; channel < output.num_channels(); ++channel)
            std::memcpy(output.channel_ptr(channel), input.channel_ptr(channel),
                        sizeof(float) * static_cast<std::size_t>(frames));
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    std::atomic<double> prepared_sample_rate{0.0};
    std::atomic<int> prepared_max_block{0};
    std::atomic<double> fail_sample_rate{0.0};

  private:
    PluginInfo info_;
};

struct BindingPublishPause {
    static void hook(void* context) noexcept {
        auto& pause = *static_cast<BindingPublishPause*>(context);
        pause.entered.store(true, std::memory_order_release);
        while (!pause.released.load(std::memory_order_acquire))
            std::this_thread::yield();
    }
    bool wait_until_entered() const noexcept {
        for (int spin = 0; spin < 1'000'000; ++spin) {
            if (entered.load(std::memory_order_acquire)) return true;
            std::this_thread::yield();
        }
        return false;
    }
    std::atomic<bool> entered{false};
    std::atomic<bool> released{false};
};

struct BindingCommitFailure {
    static void hook(void* context) noexcept {
        auto& failure = *static_cast<BindingCommitFailure*>(context);
        if (failure.fail_restore)
            failure.plugin->fail_sample_rate.store(48'000.0, std::memory_order_relaxed);
        failure.graph->begin_swap_edit();
    }
    SignalGraph* graph = nullptr;
    DimensionTrackingSlot* plugin = nullptr;
    bool fail_restore = false;
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
    REQUIRE(binding.adopt_latest_program());
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
    auto counter = std::make_unique<MidiCountingSlot>();
    auto* counter_ptr = counter.get();
    const auto midi_destination = graph.add_plugin_node(
        std::move(counter), 1, 1, "note recorder");
    REQUIRE(graph.prepare(48'000.0, 128));
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{
        TimelineTrackGraphRoute{{10}, output_node, 0, midi_destination}};
    REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 128));

    Buffer input(1, 64);
    Buffer output(1, 64);
    auto output_view = output.view();
    const auto result = binding.process(output_view, input.const_view(), snapshot(*pinned, 64));
    REQUIRE(result);
    REQUIRE(result.emitted_note_events == 2);
    REQUIRE(counter_ptr->last_event_count == 2);
    REQUIRE(counter_ptr->last_offsets[0] == 5);
    REQUIRE(counter_ptr->last_offsets[1] == 20);
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
    REQUIRE(output.storage[0] == std::vector<float>(32, 0.0f));
    REQUIRE(binding.prepare_quiesced(*changed, routes, config(1), 44'100.0, 64));
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

TEST_CASE("timeline graph binding pins its exact routed snapshot") {
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
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 32)));
    REQUIRE(output.storage[0] == std::vector<float>(32, 1.0f));
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
    REQUIRE(binding.adopt_latest_program().code == TimelineGraphAdmissionCode::MissingTrack);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 32)));
    REQUIRE(output.storage[0] == std::vector<float>(32, 1.0f));
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
    REQUIRE(binding.adopt_latest_program());
    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*reordered, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);

    programs.publish(audio_project(1.0f, 128), map, assets, 3);
    auto removed = programs.store.read();
    std::fill(output.storage[0].begin(), output.storage[0].end(), 7.0f);
    REQUIRE(binding.adopt_latest_program().code == TimelineGraphAdmissionCode::MissingTrack);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*reordered, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);
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

TEST_CASE("timeline graph binding carries renderer and pending MIDI across reprepare") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map,
                     asset_pool(std::vector<float>(128, 1.0f)), 1);
    auto program = programs.store.read();

    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    auto counter = std::make_unique<MidiCountingSlot>();
    auto* counter_ptr = counter.get();
    const auto midi_destination = graph.add_plugin_node(
        std::move(counter), 1, 1, "MIDI counter");
    REQUIRE(graph.prepare(48'000.0, 32));
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{
        TimelineTrackGraphRoute{{10}, output_node, 0, midi_destination}};
    REQUIRE(binding.prepare(*program, routes, config(1), 48'000.0, 32));

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program, 32)));
    const auto stable_audio = binding.audio_node_for({10});
    const auto stable_midi = binding.midi_input_node_for({10});
    const auto carry = binding.renderer_state_for({10});
    REQUIRE(carry.valid);

    midi::MidiBuffer pending;
    pending.reserve(1);
    pending.add(midi::MidiEvent::note_on(0, 64, 100));
    REQUIRE(graph.inject_midi(stable_midi, pending));
    REQUIRE(binding.prepare(*program, routes, config(1), 48'000.0, 32));
    REQUIRE(binding.audio_node_for({10}) == stable_audio);
    REQUIRE(binding.midi_input_node_for({10}) == stable_midi);
    const auto carried = binding.renderer_state_for({10});
    REQUIRE(carried.valid == carry.valid);
    REQUIRE(carried.key == carry.key);
    REQUIRE(carried.event_cursor == carry.event_cursor);
    REQUIRE(carried.source_sample == carry.source_sample);
    REQUIRE(carried.timeline_tick == carry.timeline_tick);
    REQUIRE(carried.loop_iteration == carry.loop_iteration);

    // Drive the graph directly so the binding does not publish a newer empty
    // note batch over the deliberately pending ingress publication.
    graph.process(output_view, input.const_view(), 32);
    REQUIRE(counter_ptr->last_event_count == 1);
    graph.process(output_view, input.const_view(), 32);
    REQUIRE(counter_ptr->last_event_count == 0);
}

TEST_CASE("timeline graph binding transactionally adds and removes PDC tracks") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, assets, 1);

    SignalGraph graph;
    CustomNodeType unrelated;
    unrelated.type_id = "pulp.test.timeline-unrelated-unused";
    unrelated.num_output_ports = 1;
    unrelated.process = [](audio::BufferView<float>& output,
                           const audio::BufferView<const float>&, int) {
        output.clear();
    };
    REQUIRE(graph.register_custom_node_type(std::move(unrelated)));
    const auto input_node = graph.add_input_node(1);
    const auto latency_node = graph.add_plugin_node(
        std::make_unique<ReportedLatencySilence>(), 1, 1, "PDC anchor");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(input_node, 0, latency_node, 0));
    REQUIRE(graph.connect(latency_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 4));
    REQUIRE(graph.custom_node_type_count() == 1);

    std::shared_ptr<const void> pinned_old_snapshot;
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        const std::array one_route{
            TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
        auto one = programs.store.read();
        REQUIRE(binding.prepare(*one, one_route, config(1), 48'000.0, 4));
        const auto stable_audio = binding.audio_node_for({10});
        const auto stable_midi = binding.midi_input_node_for({10});
        REQUIRE(graph.custom_node_type_count() == 2);

        Buffer input(1, 4);
        Buffer output(1, 4);
        auto output_view = output.view();
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*one, 4, 0)));
        REQUIRE(output.storage[0] == std::vector<float>{0.0f, 0.0f, 1.0f, 1.0f});

        programs.publish(parallel_audio_project(128), map, assets, 2);
        auto two = programs.store.read();
        const std::array two_routes{
            TimelineTrackGraphRoute{{10}, output_node, 0, 0},
            TimelineTrackGraphRoute{{11}, output_node, 0, 0}};
        REQUIRE(binding.prepare(*two, two_routes, config(1), 48'000.0, 4));
        REQUIRE(binding.audio_node_for({10}) == stable_audio);
        REQUIRE(binding.midi_input_node_for({10}) == stable_midi);
        REQUIRE(graph.custom_node_type_count() == 3);
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*two, 4, 4)));
        REQUIRE(output.storage[0] == std::vector<float>{1.0f, 1.0f, 1.5f, 1.5f});

        programs.publish(audio_project(1.0f, 128), map, assets, 3);
        auto one_again = programs.store.read();
        REQUIRE(binding.prepare(*one_again, one_route, config(1), 48'000.0, 4));
        REQUIRE(binding.audio_node_for({10}) == stable_audio);
        REQUIRE(binding.midi_input_node_for({10}) == stable_midi);
        REQUIRE(binding.audio_node_for({11}) == 0);
        REQUIRE(graph.custom_node_type_count() == 2);
        REQUIRE(binding.process(output_view, input.const_view(),
                                snapshot(*one_again, 4, 8)));
        REQUIRE(output.storage[0] == std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f});
        pinned_old_snapshot = graph.live_snapshot_handle();
    }

    REQUIRE(graph.nodes().size() == 3);
    REQUIRE(graph.custom_node_type_count() == 1);
    pinned_old_snapshot.reset();
}

TEST_CASE("timeline graph binding publishes coherent state during live reprepare") {
    const auto map = tempo_map();
    constexpr std::size_t kFrames = 128;
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, kFrames), map,
                     asset_pool(std::vector<float>(kFrames, 1.0f)), 1);
    auto program = programs.store.read();

    SignalGraph graph;
    const auto gain_one = graph.add_gain_node("one");
    const auto gain_two = graph.add_gain_node("two");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.set_node_gain(gain_one, 1.0f));
    REQUIRE(graph.set_node_gain(gain_two, 2.0f));
    REQUIRE(graph.connect(gain_one, 0, output_node, 0));
    REQUIRE(graph.connect(gain_two, 0, output_node, 0));
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array route_one{
        TimelineTrackGraphRoute{{10}, gain_one, 0, 0}};
    const std::array route_two{
        TimelineTrackGraphRoute{{10}, gain_two, 0, 0}};
    REQUIRE(binding.prepare(*program, route_one, config(1), 48'000.0, 32));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> one_blocks{0};
    std::atomic<std::uint64_t> two_blocks{0};
    std::atomic<std::uint64_t> invalid_blocks{0};
    std::atomic<std::size_t> allocations{1};
    std::thread audio_thread([&] {
        Buffer input(1, 32);
        Buffer output(1, 32);
        auto output_view = output.view();
        test::ScopedRtProcessProbe probe;
        while (!stop.load(std::memory_order_acquire)) {
            auto transport = snapshot(*program, 32, 0);
            transport.ranges[0].discontinuity = true;
            const auto result = binding.process(output_view, input.const_view(),
                                                transport);
            if (!result) {
                invalid_blocks.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            const float first = output.storage[0][0];
            const bool coherent = std::all_of(
                output.storage[0].begin(), output.storage[0].end(),
                [first](float sample) { return sample == first; });
            if (!coherent) {
                invalid_blocks.fetch_add(1, std::memory_order_relaxed);
            } else if (first == 1.0f) {
                one_blocks.fetch_add(1, std::memory_order_relaxed);
            } else if (first == 2.0f) {
                two_blocks.fetch_add(1, std::memory_order_relaxed);
            } else {
                invalid_blocks.fetch_add(1, std::memory_order_relaxed);
            }
        }
        allocations.store(probe.allocation_count(), std::memory_order_relaxed);
    });

    for (int iteration = 0; iteration < 64; ++iteration) {
        const auto& route = (iteration & 1) == 0 ? route_two : route_one;
        REQUIRE(binding.prepare(*program, route, config(1), 48'000.0, 32));
    }
    for (int spin = 0;
         spin < 10'000 && two_blocks.load(std::memory_order_relaxed) == 0; ++spin) {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    audio_thread.join();

    REQUIRE(one_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(two_blocks.load(std::memory_order_relaxed) > 0);
    REQUIRE(invalid_blocks.load(std::memory_order_relaxed) == 0);
    REQUIRE(allocations.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("timeline graph binding publishes node-set generations atomically") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness two_programs;
    two_programs.publish(parallel_audio_project(128), map, assets, 1);
    auto two = two_programs.store.read();
    ProgramHarness one_programs;
    one_programs.publish(audio_project(1.0f, 128), map, assets, 2);
    auto one = one_programs.store.read();

    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, two_programs.store);
    const std::array two_routes{
        TimelineTrackGraphRoute{{10}, output_node, 0, 0},
        TimelineTrackGraphRoute{{11}, output_node, 0, 0}};
    const std::array one_route{
        TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*two, two_routes, config(1), 48'000.0, 64));

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*two, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);

    BindingPublishPause remove_pause;
    binding.set_before_binding_publish_hook_for_test(&BindingPublishPause::hook,
                                                     &remove_pause);
    std::atomic<TimelineGraphAdmissionCode> remove_code{
        TimelineGraphAdmissionCode::GraphPrepareFailed};
    std::thread remover([&] {
        remove_code.store(binding.prepare(*one, one_route, config(1), 48'000.0, 64).code,
                          std::memory_order_release);
    });
    REQUIRE(remove_pause.wait_until_entered());
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*two, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);
    remove_pause.released.store(true, std::memory_order_release);
    remover.join();
    REQUIRE(remove_code.load(std::memory_order_acquire) ==
            TimelineGraphAdmissionCode::Accepted);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*one, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);

    BindingPublishPause add_pause;
    binding.set_before_binding_publish_hook_for_test(&BindingPublishPause::hook,
                                                     &add_pause);
    std::atomic<TimelineGraphAdmissionCode> add_code{
        TimelineGraphAdmissionCode::GraphPrepareFailed};
    std::thread adder([&] {
        add_code.store(binding.prepare(*two, two_routes, config(1), 48'000.0, 64).code,
                       std::memory_order_release);
    });
    REQUIRE(add_pause.wait_until_entered());
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*one, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);
    add_pause.released.store(true, std::memory_order_release);
    adder.join();
    REQUIRE(add_code.load(std::memory_order_acquire) ==
            TimelineGraphAdmissionCode::Accepted);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*two, 32)));
    REQUIRE(output.storage[0][0] == 1.5f);
    binding.set_before_binding_publish_hook_for_test(nullptr);
}

TEST_CASE("timeline graph binding publishes content generations atomically") {
    const auto map = tempo_map();
    auto assets = asset_pool(std::vector<float>(128, 1.0f));
    ProgramHarness programs;
    programs.publish(audio_project(1.0f, 128), map, assets, 1);
    auto first = programs.store.read();
    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*first, routes, config(1), 48'000.0, 64));

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*first, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);

    programs.publish(audio_project(0.5f, 128), map, assets, 2);
    auto second = programs.store.read();
    BindingPublishPause pause;
    binding.set_before_binding_publish_hook_for_test(&BindingPublishPause::hook, &pause);
    std::atomic<TimelineGraphAdmissionCode> adoption_code{
        TimelineGraphAdmissionCode::GraphPrepareFailed};
    std::thread adopter([&] {
        adoption_code.store(binding.adopt_program(*second).code,
                            std::memory_order_release);
    });
    REQUIRE(pause.wait_until_entered());
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*first, 32, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);
    pause.released.store(true, std::memory_order_release);
    adopter.join();
    REQUIRE(adoption_code.load(std::memory_order_acquire) ==
            TimelineGraphAdmissionCode::Accepted);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*second, 32, 64)));
    REQUIRE(output.storage[0][0] == 0.5f);
    binding.set_before_binding_publish_hook_for_test(nullptr);
}

TEST_CASE("timeline graph binding quiescently reprepares plugin dimensions") {
    auto map48 = tempo_map({48'000, 1});
    auto assets = asset_pool(std::vector<float>(512, 1.0f));
    ProgramHarness programs48;
    programs48.publish(audio_project(1.0f, 512), map48, assets, 1);
    auto program48 = programs48.store.read();

    SignalGraph graph;
    auto plugin = std::make_unique<DimensionTrackingSlot>();
    auto* plugin_ptr = plugin.get();
    const auto plugin_node = graph.add_plugin_node(std::move(plugin), 1, 1,
                                                   "dimension tracker");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(plugin_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));
    TimelineGraphPlaybackBinding binding(graph, programs48.store);
    const std::array routes{
        TimelineTrackGraphRoute{{10}, plugin_node, 0, 0}};
    REQUIRE(binding.prepare(*program48, routes, config(1), 48'000.0, 64));

    auto map44 = tempo_map({44'100, 1});
    ProgramHarness programs44;
    programs44.publish(audio_project(1.0f, 512), map44, assets, 2);
    auto program44 = programs44.store.read();
    REQUIRE(binding.prepare_quiesced(*program44, routes, config(1), 44'100.0, 128));
    REQUIRE(plugin_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 44'100.0);
    REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 128);
    REQUIRE(binding.prepare_quiesced(*program44, routes, config(1), 44'100.0, 256));
    REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 256);

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program44, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);

    auto map32 = tempo_map({32'000, 1});
    ProgramHarness programs32;
    programs32.publish(audio_project(1.0f, 512), map32, assets, 3);
    auto program32 = programs32.store.read();
    plugin_ptr->fail_sample_rate.store(32'000.0, std::memory_order_relaxed);
    REQUIRE(binding.prepare_quiesced(*program32, routes, config(1), 32'000.0, 256).code ==
            TimelineGraphAdmissionCode::GraphPrepareFailed);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program44, 32, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);
    REQUIRE(plugin_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 44'100.0);
    REQUIRE(plugin_ptr->prepared_max_block.load(std::memory_order_relaxed) == 256);
}

TEST_CASE("timeline graph binding restores shared lifecycles after quiesced failure") {
    auto map48 = tempo_map({48'000, 1});
    auto assets = asset_pool(std::vector<float>(512, 1.0f));
    ProgramHarness programs48;
    programs48.publish(audio_project(1.0f, 512), map48, assets, 1);
    auto program48 = programs48.store.read();

    SignalGraph graph;
    auto first = std::make_unique<DimensionTrackingSlot>();
    auto* first_ptr = first.get();
    const auto first_node = graph.add_plugin_node(std::move(first), 1, 1, "first tracker");
    auto second = std::make_unique<DimensionTrackingSlot>();
    auto* second_ptr = second.get();
    const auto second_node = graph.add_plugin_node(std::move(second), 1, 1, "second tracker");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(first_node, 0, second_node, 0));
    REQUIRE(graph.connect(second_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));
    TimelineGraphPlaybackBinding binding(graph, programs48.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, first_node, 0, 0}};
    REQUIRE(binding.prepare(*program48, routes, config(1), 48'000.0, 64));

    auto map32 = tempo_map({32'000, 1});
    ProgramHarness programs32;
    programs32.publish(audio_project(1.0f, 512), map32, assets, 2);
    auto program32 = programs32.store.read();
    second_ptr->fail_sample_rate.store(32'000.0, std::memory_order_relaxed);
    REQUIRE(binding.prepare_quiesced(*program32, routes, config(1), 32'000.0, 128).code ==
            TimelineGraphAdmissionCode::GraphPrepareFailed);
    REQUIRE(first_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 48'000.0);
    REQUIRE(first_ptr->prepared_max_block.load(std::memory_order_relaxed) == 64);
    REQUIRE(second_ptr->prepared_sample_rate.load(std::memory_order_relaxed) == 48'000.0);
    REQUIRE(second_ptr->prepared_max_block.load(std::memory_order_relaxed) == 64);

    Buffer input(1, 32);
    Buffer output(1, 32);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program48, 32)));
    REQUIRE(output.storage[0][0] == 1.0f);

    // If restoring even one already-touched shared instance fails, the binding
    // revokes its publication instead of exposing a partially re-prepared graph.
    first_ptr->fail_sample_rate.store(48'000.0, std::memory_order_relaxed);
    REQUIRE(binding.prepare_quiesced(*program32, routes, config(1), 32'000.0, 128).code ==
            TimelineGraphAdmissionCode::GraphPrepareFailed);
    std::fill(output.storage[0].begin(), output.storage[0].end(), 7.0f);
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program48, 32)).code ==
            TimelineGraphProcessCode::MissingProgram);
    REQUIRE(output.storage[0] == std::vector<float>(32, 0.0f));
}

TEST_CASE("timeline graph binding revokes publication when commit rollback fails") {
    auto map48 = tempo_map({48'000, 1});
    auto assets = asset_pool(std::vector<float>(512, 1.0f));
    ProgramHarness programs48;
    programs48.publish(audio_project(1.0f, 512), map48, assets, 1);
    auto program48 = programs48.store.read();

    SignalGraph graph;
    auto plugin = std::make_unique<DimensionTrackingSlot>();
    auto* plugin_ptr = plugin.get();
    const auto plugin_node =
        graph.add_plugin_node(std::move(plugin), 1, 1, "dimension tracker");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(plugin_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));
    TimelineGraphPlaybackBinding binding(graph, programs48.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, plugin_node, 0, 0}};
    REQUIRE(binding.prepare(*program48, routes, config(1), 48'000.0, 64));

    auto map44 = tempo_map({44'100, 1});
    ProgramHarness programs44;
    programs44.publish(audio_project(1.0f, 512), map44, assets, 2);
    auto program44 = programs44.store.read();
    BindingCommitFailure failure{&graph, plugin_ptr, true};
    binding.set_before_graph_commit_hook_for_test(&BindingCommitFailure::hook, &failure);
    REQUIRE(binding.prepare_quiesced(*program44, routes, config(1), 44'100.0, 128).code ==
            TimelineGraphAdmissionCode::GraphPrepareFailed);
    REQUIRE_FALSE(graph.is_prepared());

    Buffer input(1, 32);
    Buffer output(1, 32);
    std::fill(output.storage[0].begin(), output.storage[0].end(), 7.0f);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*program48, 32)).code ==
            TimelineGraphProcessCode::MissingProgram);
    REQUIRE(output.storage[0] == std::vector<float>(32, 0.0f));
}
