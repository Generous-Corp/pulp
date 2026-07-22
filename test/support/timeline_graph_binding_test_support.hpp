#pragma once

#include <pulp/format/process_context.hpp>
#include <pulp/host/timeline_graph_binding.hpp>
#include <pulp/playback/program_compiler.hpp>
#include <pulp/timeline/model.hpp>

#include "../harness/scoped_rt_process_probe.hpp"

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
    return std::make_shared<const CompiledTempoMap>(
        take(CompiledTempoMap::compile(points, rate)));
}

std::shared_ptr<const audio::AudioFileData> audio_data(std::vector<float> mono) {
    auto result = std::make_shared<audio::AudioFileData>();
    result->sample_rate = 48'000;
    result->channels = {std::move(mono)};
    return result;
}

[[maybe_unused]] std::shared_ptr<const DecodedAudioAssetPool>
asset_pool(std::vector<float> mono) {
    return take(DecodedAudioAssetPool::create({{{3}, audio_data(std::move(mono))}}));
}

Clip audio_clip(float gain = 1.0f, std::uint64_t frames = 512) {
    ClipPlaybackProperties properties;
    properties.gain_linear = gain;
    return take(Clip::create_absolute({100}, {0}, frames, {48'000, 1}, MediaRef{{3}, {0}, frames},
                                      properties));
}

[[maybe_unused]] std::shared_ptr<const Project>
audio_project(float gain = 1.0f, std::uint64_t frames = 512) {
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

[[maybe_unused]] std::shared_ptr<const Project>
parallel_audio_project(std::uint64_t frames = 512,
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

[[maybe_unused]] std::shared_ptr<const Project> automation_project(
    const CompiledTempoMap& map, float first_value = 0.25f,
    float second_value = 0.75f, std::uint32_t parameter = 7) {
    auto curve = take(AutomationCurve::create({
        {{41}, map.samples_to_ticks({0}), first_value},
        {{42}, map.samples_to_ticks({16}), second_value},
    }));
    auto lane = take(AutomationLane::create(
        {31}, DeviceParameterTarget{{20}, parameter}, std::move(curve)));
    auto track = take(Track::create(TrackInput{
        .id = {10},
        .name = "automation",
        .device_chain = {{{20}}},
        .automation_lanes = {std::move(lane)},
    }));
    auto sequence = take(Sequence::create(
        {2}, "root", std::nullopt, std::nullopt,
        std::vector<Track>{std::move(track)}));
    return std::make_shared<const Project>(take(Project::create(
        ProjectInput{{1}, "automation", 1'000, {2}, {}, {std::move(sequence)}})));
}

[[maybe_unused]] std::shared_ptr<const Project> device_project() {
    auto track = take(Track::create(TrackInput{
        .id = {10}, .name = "device", .device_chain = {{{20}}},
    }));
    auto sequence = take(Sequence::create(
        {2}, "root", std::nullopt, std::nullopt,
        std::vector<Track>{std::move(track)}));
    return std::make_shared<const Project>(take(Project::create(
        ProjectInput{{1}, "device", 1'000, {2}, {}, {std::move(sequence)}})));
}

[[maybe_unused]] std::shared_ptr<const Project> two_device_automation_project(
    const CompiledTempoMap& map) {
    const auto make_lane = [&](ItemId lane_id, ItemId point_id,
                               ItemId placement, std::uint32_t parameter) {
        auto curve = take(AutomationCurve::create(
            {AutomationPoint{point_id, map.samples_to_ticks({0}), 0.5f}}));
        return take(AutomationLane::create(
            lane_id, DeviceParameterTarget{placement, parameter},
            std::move(curve)));
    };
    auto track = take(Track::create(TrackInput{
        .id = {10},
        .name = "two devices",
        .device_chain = {{{20}}, {{21}}},
        .automation_lanes = {
            make_lane({31}, {41}, {20}, 7),
            make_lane({32}, {42}, {21}, 7),
        },
    }));
    auto sequence = take(Sequence::create(
        {2}, "root", std::nullopt, std::nullopt,
        std::vector<Track>{std::move(track)}));
    return std::make_shared<const Project>(take(Project::create(
        ProjectInput{{1}, "two devices", 1'000, {2}, {},
                     {std::move(sequence)}})));
}

NoteEvent note(const CompiledTempoMap& map, std::uint64_t id, std::int64_t start_sample,
               std::int64_t end_sample) {
    const auto start = map.samples_to_ticks({start_sample});
    const auto end = map.samples_to_ticks({end_sample});
    return {{id}, start, end - start, 0xffff, 60, 0};
}

[[maybe_unused]] std::shared_ptr<const Project> note_project(const CompiledTempoMap& map) {
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

class AutomationRecordingSlot final : public PluginSlot {
  public:
    explicit AutomationRecordingSlot(std::uint32_t parameter = 7,
                                     ParamFlags flags = {}) {
        info_.name = "timeline automation recorder";
        info_.unique_id = "pulp.test.timeline-automation-recorder";
        info_.format = PluginFormat::CLAP;
        info_.is_effect = true;
        info_.num_inputs = 1;
        info_.num_outputs = 1;
        param_.id = parameter;
        param_.name = "gain";
        param_.flags = flags;
    }
    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return loaded; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>&,
                 const midi::MidiBuffer&, midi::MidiBuffer&,
                 const ParameterEventQueue& events, int) override {
        const auto call = process_count.fetch_add(1, std::memory_order_relaxed);
        if (call < call_event_counts.size()) {
            call_event_counts[call].store(events.size(), std::memory_order_relaxed);
        }
        event_count = std::min(events.size(), received.size());
        std::copy_n(events.begin(), event_count, received.begin());
        if (call == 0 && block_first.load(std::memory_order_acquire)) {
            first_entered.store(true, std::memory_order_release);
            while (!release_first.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        output.clear();
    }
    std::vector<HostParamInfo> parameters() const override { return {param_}; }
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

    PluginInfo info_;
    HostParamInfo param_;
    std::array<state::ParameterEvent, ParameterEventQueue::kCapacity> received{};
    std::size_t event_count = 0;
    std::atomic<std::size_t> process_count{0};
    std::array<std::atomic<std::size_t>, 2> call_event_counts{};
    std::atomic<bool> block_first{false};
    std::atomic<bool> first_entered{false};
    std::atomic<bool> release_first{false};
    bool loaded = true;
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

// Exact-generation injection must stay gated by its passkey: callable only with
// a SnapshotParameterIngressPasskey, never through this passkey-free form. The
// generic inject_parameter_events(node, events) remains public by design.
template <typename T>
concept HasPublicExactParameterInjection = requires(
    const T& snapshot_handle, const ParameterEventQueue& events) {
    snapshot_handle.inject_exact_parameter_events(NodeId{1}, events);
};

static_assert(!HasPublicExactParameterInjection<SignalGraph::ExecutionSnapshot>);
