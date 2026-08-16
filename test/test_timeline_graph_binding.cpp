#include "../core/host/src/timeline_graph_binding_internal.hpp"
#include "support/timeline_graph_binding_test_support.hpp"

#include <pulp/timeline/schema_registry.hpp>
#include <pulp/timeline/serialize.hpp>

#include <bit>
#include <optional>
#include <tuple>
#include <typeinfo>

namespace {

constexpr std::uint64_t kBasicInstrumentExpectedFullSampleHash = 2'920'126'913'460'893'443ull;

std::shared_ptr<const Project> basic_instrument_project_with_devices(
    const CompiledTempoMap& map, std::vector<DevicePlacement> devices, std::uint8_t pitch = 60) {
    auto event = note(map, 101, 5, 48);
    event.pitch = pitch;
    auto content = take(MidiContent::create({event}));
    auto clip = take(Clip::create({100}, {0}, map.samples_to_ticks({128}) - TickPosition{0},
                                  std::move(content)));
    auto track = take(Track::create(TrackInput{
        .id = {10},
        .name = "basic instrument",
        .clips = {std::move(clip)},
        .device_chain = std::move(devices),
    }));
    auto sequence = take(Sequence::create({2}, "root", std::nullopt, std::nullopt,
                                          std::vector<Track>{std::move(track)}));
    return std::make_shared<const Project>(take(Project::create(
        ProjectInput{{1}, "basic instrument", 1'000, {2}, {}, {std::move(sequence)}})));
}

DeviceConfiguration basic_instrument_configuration() {
    return {
        .position = DeviceChainPosition::PreFader,
        .slot_kind = DeviceSlotKind::EventToAudio,
        .device_kind = DeviceKind::BuiltIn,
        .binding_key = std::string(kBasicInstrumentBindingKey),
    };
}

std::shared_ptr<const Project>
basic_instrument_project(const CompiledTempoMap& map,
                         DeviceConfiguration configuration = basic_instrument_configuration(),
                         std::optional<ContentHash> state_ref = std::nullopt,
                         bool include_device = true, ItemId placement_id = {20},
                         std::uint8_t pitch = 60) {
    std::vector<DevicePlacement> devices;
    if (include_device)
        devices.push_back({placement_id, std::move(configuration), std::move(state_ref)});
    return basic_instrument_project_with_devices(map, std::move(devices), pitch);
}

std::unique_ptr<PluginSlot> reject_timeline_device_factory(const PluginInfo&) {
    return nullptr;
}

bool plain_audio_edge(const Connection& edge, NodeId source, PortIndex source_port) {
    return edge.source_node == source && edge.source_port == source_port && !edge.midi &&
           !edge.feedback && !edge.automation && !edge.audio_rate_modulation && !edge.sidechain;
}

std::uint64_t full_sample_hash(const std::vector<std::vector<float>>& audio) noexcept {
    std::uint64_t hash = 1'469'598'103'934'665'603ull;
    for (const auto& channel : audio) {
        for (const float sample : channel) {
            const auto bits = std::bit_cast<std::uint32_t>(sample);
            for (unsigned shift = 0; shift < 32; shift += 8) {
                hash ^= (bits >> shift) & 0xffu;
                hash *= 1'099'511'628'211ull;
            }
        }
    }
    return hash;
}

struct PluginInfoSignature {
    std::string name;
    std::string manufacturer;
    std::string version;
    std::string path;
    std::string unique_id;
    PluginFormat format = PluginFormat::BuiltIn;
    bool is_instrument = false;
    bool is_effect = false;
    int num_inputs = 0;
    int num_outputs = 0;
    std::string category;
    std::vector<std::string> features;
    std::string description;
    bool has_editor = false;
    bool supports_sidechain = false;
    bool supports_midi_in = false;
    bool supports_midi_out = false;
    bool operator==(const PluginInfoSignature&) const = default;
};

PluginInfoSignature plugin_info_signature(const PluginInfo& info) {
    return {info.name,
            info.manufacturer,
            info.version,
            info.path,
            info.unique_id,
            info.format,
            info.is_instrument,
            info.is_effect,
            info.num_inputs,
            info.num_outputs,
            info.category,
            info.features,
            info.description,
            info.has_editor,
            info.supports_sidechain,
            info.supports_midi_in,
            info.supports_midi_out};
}

struct GraphNodeSignature {
    NodeId id = 0;
    NodeType type = NodeType::Gain;
    std::string name;
    int num_input_ports = 0;
    int num_output_ports = 0;
    const void* plugin = nullptr;
    std::optional<PluginInfoSignature> plugin_info;
    float gain = 0.0f;
    std::string custom_type_id;
    int custom_type_version = 0;
    const void* custom_instance = nullptr;
    std::vector<std::uint8_t> custom_state_blob;
    bool custom_state_pending = false;
    bool transport_sensitive = false;
    bool allow_live_instance_swap = false;
    int live_swap_fade_ms = 0;
    LiveSwapCurve live_swap_curve = LiveSwapCurve::Smoothstep;
    float live_swap_headroom_threshold = 0.0f;
    std::size_t live_swap_max_state_bytes = 0;
    bool has_live_swap_callback = false;
    std::size_t live_swap_callback_type = 0;
    bool hosted_editor_open = false;
    bool operator==(const GraphNodeSignature&) const = default;
};

std::vector<GraphNodeSignature> graph_node_signatures(const SignalGraph& graph) {
    std::vector<GraphNodeSignature> result;
    result.reserve(graph.nodes().size());
    for (const auto& node : graph.nodes()) {
        std::optional<PluginInfoSignature> plugin_info;
        if (node.type == NodeType::Plugin)
            plugin_info = plugin_info_signature(node.plugin_info);
        const auto& policy = node.live_swap_policy;
        result.push_back({
            .id = node.id,
            .type = node.type,
            .name = node.name,
            .num_input_ports = node.num_input_ports,
            .num_output_ports = node.num_output_ports,
            .plugin = node.plugin.get(),
            .plugin_info = std::move(plugin_info),
            .gain = node.gain,
            .custom_type_id = node.custom_type_id,
            .custom_type_version = node.custom_type_version,
            .custom_instance = node.custom_instance.get(),
            .custom_state_blob = node.custom_state_blob,
            .custom_state_pending = node.custom_state_pending,
            .transport_sensitive = node.transport_sensitive,
            .allow_live_instance_swap = policy.allow_live_instance_swap,
            .live_swap_fade_ms = policy.fade_ms,
            .live_swap_curve = policy.curve,
            .live_swap_headroom_threshold = policy.headroom_threshold,
            .live_swap_max_state_bytes = policy.max_state_bytes,
            .has_live_swap_callback = static_cast<bool>(policy.on_instance_swapped),
            .live_swap_callback_type = policy.on_instance_swapped
                                           ? policy.on_instance_swapped.target_type().hash_code()
                                           : 0,
            .hosted_editor_open = node.hosted_editor_open,
        });
    }
    return result;
}

using GraphConnectionSignature =
    std::tuple<NodeId, PortIndex, NodeId, PortIndex, bool, bool, bool, bool, bool, std::uint32_t,
               float, float, float, AutomationMix>;

std::vector<GraphConnectionSignature> graph_connection_signatures(const SignalGraph& graph) {
    std::vector<GraphConnectionSignature> result;
    result.reserve(graph.connections().size());
    for (const auto& edge : graph.connections()) {
        result.emplace_back(edge.source_node, edge.source_port, edge.dest_node, edge.dest_port,
                            edge.feedback, edge.midi, edge.automation, edge.audio_rate_modulation,
                            edge.sidechain, edge.automation_param_id, edge.automation_range_lo,
                            edge.automation_range_hi, edge.automation_smoothing_ms,
                            edge.automation_mix);
    }
    return result;
}

struct TrackedTimelineDeviceLifecycle {
    SignalGraph* graph_for_factory_stale = nullptr;
    NodeId factory_stale_node = 0;
    bool inject_stale_after_factory = false;
    bool fail_prepare = false;
    std::atomic<int> constructs{0};
    std::atomic<int> prepares{0};
    std::atomic<int> releases{0};
    std::atomic<int> destroys{0};
    std::atomic<int> processes{0};
    std::atomic<std::size_t> midi_events{0};
    std::atomic<bool> factory_stale_succeeded{false};
};

class TrackedTimelineDeviceSlot final : public PluginSlot {
  public:
    TrackedTimelineDeviceSlot(std::unique_ptr<PluginSlot> inner,
                              TrackedTimelineDeviceLifecycle& lifecycle)
        : inner_(std::move(inner)), lifecycle_(lifecycle) {
        lifecycle_.constructs.fetch_add(1, std::memory_order_relaxed);
    }
    ~TrackedTimelineDeviceSlot() override {
        lifecycle_.destroys.fetch_add(1, std::memory_order_relaxed);
    }

    const PluginInfo& info() const override {
        return inner_->info();
    }
    bool is_loaded() const override {
        return inner_->is_loaded();
    }
    bool prepare(double sample_rate, int maximum_block_size) override {
        lifecycle_.prepares.fetch_add(1, std::memory_order_relaxed);
        if (lifecycle_.fail_prepare)
            return false;
        if (!inner_->prepare(sample_rate, maximum_block_size))
            return false;
        prepared_ = true;
        return true;
    }
    void release() override {
        lifecycle_.releases.fetch_add(1, std::memory_order_relaxed);
        if (prepared_) {
            inner_->release();
            prepared_ = false;
        }
    }
    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& events, int frames) override {
        lifecycle_.processes.fetch_add(1, std::memory_order_relaxed);
        lifecycle_.midi_events.fetch_add(midi_in.size(), std::memory_order_relaxed);
        inner_->process(output, input, midi_in, midi_out, events, frames);
    }
    std::vector<HostParamInfo> parameters() const override {
        return inner_->parameters();
    }
    float get_parameter(std::uint32_t id) const override {
        return inner_->get_parameter(id);
    }
    void set_parameter(std::uint32_t id, float value) override {
        inner_->set_parameter(id, value);
    }
    void set_bypass(bool bypassed) override {
        inner_->set_bypass(bypassed);
    }
    bool is_bypassed() const override {
        return inner_->is_bypassed();
    }
    std::vector<std::uint8_t> save_state() const override {
        return inner_->save_state();
    }
    bool restore_state(const std::vector<std::uint8_t>& state) override {
        return inner_->restore_state(state);
    }
    bool has_editor() const override {
        return false;
    }
    void* create_editor_view() override {
        return nullptr;
    }
    void destroy_editor_view() override {}
    int latency_samples() const override {
        return inner_->latency_samples();
    }
    int tail_samples() const override {
        return inner_->tail_samples();
    }

  private:
    std::unique_ptr<PluginSlot> inner_;
    TrackedTimelineDeviceLifecycle& lifecycle_;
    bool prepared_ = false;
};

TrackedTimelineDeviceLifecycle* tracked_timeline_device_lifecycle = nullptr;

class ScopedTrackedTimelineDeviceLifecycle {
  public:
    explicit ScopedTrackedTimelineDeviceLifecycle(TrackedTimelineDeviceLifecycle& lifecycle)
        : previous_(tracked_timeline_device_lifecycle) {
        tracked_timeline_device_lifecycle = &lifecycle;
    }
    ~ScopedTrackedTimelineDeviceLifecycle() {
        tracked_timeline_device_lifecycle = previous_;
    }
    void select(TrackedTimelineDeviceLifecycle& lifecycle) noexcept {
        tracked_timeline_device_lifecycle = &lifecycle;
    }

  private:
    TrackedTimelineDeviceLifecycle* previous_ = nullptr;
};

std::unique_ptr<PluginSlot> tracked_timeline_device_factory(const PluginInfo& info) {
    auto inner = load_builtin_plugin(info);
    if (!inner || tracked_timeline_device_lifecycle == nullptr)
        return inner;
    auto& lifecycle = *tracked_timeline_device_lifecycle;
    auto result = std::make_unique<TrackedTimelineDeviceSlot>(std::move(inner), lifecycle);
    if (lifecycle.inject_stale_after_factory && lifecycle.graph_for_factory_stale != nullptr) {
        const auto unchanged_gain =
            lifecycle.graph_for_factory_stale->node_gain(lifecycle.factory_stale_node);
        lifecycle.factory_stale_succeeded.store(lifecycle.graph_for_factory_stale->set_node_gain(
                                                    lifecycle.factory_stale_node, unchanged_gain),
                                                std::memory_order_relaxed);
    }
    return result;
}

struct BindingBoundaryProbe {
    TimelineGraphPlaybackBinding* binding = nullptr;
    const TransportSnapshot* transport = nullptr;
    Buffer* input = nullptr;
    Buffer* output = nullptr;
    TimelineGraphProcessResult result;
    std::uint64_t hash = 0;
    NodeId old_placement_node = 0;
    NodeId new_placement_node = 0;
    RendererProgramKey renderer_key;
};

void process_binding_before_publish(void* opaque) noexcept {
    auto& probe = *static_cast<BindingBoundaryProbe*>(opaque);
    auto output = probe.output->view();
    probe.result = probe.binding->process(output, probe.input->const_view(), *probe.transport);
    probe.hash = full_sample_hash(probe.output->storage);
    probe.old_placement_node = probe.binding->device_node_for({20});
    probe.new_placement_node = probe.binding->device_node_for({21});
    probe.renderer_key = probe.binding->renderer_key_for({10});
}

struct StaleGraphMutationProbe {
    SignalGraph* graph = nullptr;
    NodeId node = 0;
    bool succeeded = false;
};

void preserve_topology_while_advancing_graph_generation(void* opaque) noexcept {
    auto& probe = *static_cast<StaleGraphMutationProbe*>(opaque);
    const auto unchanged_gain = probe.graph->node_gain(probe.node);
    probe.succeeded = probe.graph->set_node_gain(probe.node, unchanged_gain);
}

} // namespace

TEST_CASE("timeline graph parallel audio status cannot downgrade a hard failure") {
    using pulp::host::TimelineGraphProcessCode;
    pulp::host::detail::TimelineGraphSharedBlockState shared;
    constexpr std::array hard_failures{
        TimelineGraphProcessCode::MissingProgram,
        TimelineGraphProcessCode::AudioRenderFailed,
        TimelineGraphProcessCode::RealtimeStretchStateRequired,
        TimelineGraphProcessCode::RealtimeStretchStalePublication,
        TimelineGraphProcessCode::RealtimeStretchImpossibleRatio,
        TimelineGraphProcessCode::RealtimeStretchBackpressure,
        TimelineGraphProcessCode::RealtimeStretchUnderflow,
        TimelineGraphProcessCode::RealtimeStretchUnsupportedScrubbing,
    };
    for (const auto hard_failure : hard_failures) {
        for (std::uint32_t iteration = 0; iteration < 1'000; ++iteration) {
            shared.audio_code.store(TimelineGraphProcessCode::Ok, std::memory_order_relaxed);
            std::thread gap(
                [&] { shared.report_audio_code(TimelineGraphProcessCode::RealtimeStretchGap); });
            std::thread hard([&] { shared.report_audio_code(hard_failure); });
            gap.join();
            hard.join();
            REQUIRE(shared.audio_code.load(std::memory_order_relaxed) == hard_failure);
        }
    }
}

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

TEST_CASE("timeline graph binding routes frozen artifacts after the authored device chain") {
    constexpr std::size_t kFrames = 128;
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(frozen_device_project(kFrames), map,
                     asset_pool(std::vector<float>(kFrames, 1.0f)), 1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);
    const auto* track = pinned->find_track({10});
    REQUIRE(track);
    REQUIRE(track->ordered_device_placement_ids().empty());

    SignalGraph graph;
    const auto output_node = graph.add_output_node(1);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0}};
    REQUIRE(binding.prepare(*pinned, routes, config(1), 48'000.0, 64));

    Buffer input(1, 64);
    Buffer output(1, 64);
    auto output_view = output.view();
    REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 64)));
    REQUIRE(std::all_of(output.storage[0].begin(), output.storage[0].end(),
                        [](float sample) { return sample == 1.0f; }));

    const std::array stale_devices{TimelineDeviceGraphRoute{{20}, 0}};
    const std::array stale_routes{TimelineTrackGraphRoute{{10}, output_node, 0, 0, stale_devices}};
    const auto rejected = binding.preflight(*pinned, stale_routes, config(1), 64);
    REQUIRE(rejected.code == TimelineGraphAdmissionCode::UnexpectedDevicePlacement);
    REQUIRE(rejected.item == ItemId{10});
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
    programs.publish(audio_project(1.0f, 128), map, asset_pool(std::vector<float>(128, 1.0f)), 1);
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
    const auto midi_destination = graph.add_plugin_node(std::move(counter), 1, 1, "note recorder");
    REQUIRE(graph.prepare(48'000.0, 128));
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node, 0, midi_destination}};
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

TEST_CASE("timeline graph mixer controls hosted instrument output after the device") {
    const auto map = tempo_map();
    ProgramHarness programs;
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    programs.publish(device_project(), map, no_assets, 1);
    auto neutral = programs.store.read();
    REQUIRE(neutral);

    SignalGraph graph;
    auto instrument = std::make_unique<ConstantInstrumentSlot>();
    auto* instrument_ptr = instrument.get();
    const auto instrument_node = graph.add_plugin_node(std::move(instrument), 0, 1, "instrument");
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(instrument_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));
    const std::array devices{TimelineDeviceGraphRoute{{20}, instrument_node}};
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);

        const std::array incomplete{TimelineTrackGraphRoute{
            .track_id = {10},
            .audio_destination = output_node,
            .midi_destination = instrument_node,
            .device_routes = devices,
        }};
        REQUIRE(binding.prepare(*neutral, incomplete, config(1), 48'000.0, 64));

        programs.publish(instrument_mixer_project(*map), map, no_assets, 2);
        auto pinned = programs.store.read();
        REQUIRE(pinned);
        REQUIRE(binding.adopt_program(*pinned).code ==
                TimelineGraphAdmissionCode::MissingPostDeviceRoute);
        REQUIRE(binding.preflight(*pinned, incomplete, config(1), 64).code ==
                TimelineGraphAdmissionCode::MissingPostDeviceRoute);

        const std::array routes{TimelineTrackGraphRoute{
            .track_id = {10},
            .audio_destination = output_node,
            .midi_destination = instrument_node,
            .device_routes = devices,
            .post_device_audio_source = instrument_node,
            .post_mixer_audio_destination = output_node,
        }};
        const auto admission = binding.prepare(*pinned, routes, config(1), 48'000.0, 64);
        INFO("admission code " << static_cast<int>(admission.code) << " actual " << admission.actual
                               << " node " << admission.node);
        REQUIRE(admission);
        REQUIRE(std::none_of(graph.connections().begin(), graph.connections().end(),
                             [&](const Connection& connection) {
                                 return connection.source_node == instrument_node &&
                                        connection.source_port == 0 &&
                                        connection.dest_node == output_node &&
                                        connection.dest_port == 0 && !connection.midi &&
                                        !connection.feedback && !connection.automation &&
                                        !connection.audio_rate_modulation && !connection.sidechain;
                             }));
        Buffer input(1, 64);
        Buffer output(1, 64);
        auto output_view = output.view();
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 64)));
        REQUIRE(instrument_ptr->event_count == 2);
        REQUIRE(std::all_of(output.storage[0].begin(), output.storage[0].end(),
                            [](float sample) { return sample == 0.5f; }));
    }
    REQUIRE(std::any_of(
        graph.connections().begin(), graph.connections().end(), [&](const Connection& connection) {
            return connection.source_node == instrument_node && connection.source_port == 0 &&
                   connection.dest_node == output_node && connection.dest_port == 0 &&
                   !connection.midi && !connection.feedback && !connection.automation &&
                   !connection.audio_rate_modulation && !connection.sidechain;
        }));
}

TEST_CASE("timeline graph binding owns the authored basic instrument topology") {
    constexpr std::size_t kFrames = 64;
    const auto map = tempo_map();
    const auto authored = basic_instrument_project(*map);
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    ProgramHarness programs;
    programs.publish(authored, map, no_assets, 1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    const auto caller_node_count = graph.nodes().size();
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        // The caller supplies only the track's final destination. The authored
        // placement is the durable provenance for the owned plugin node, MIDI
        // edge, and stereo audio edges.
        const std::array routes{TimelineTrackGraphRoute{
            .track_id = {10},
            .audio_destination = output_node,
        }};
        REQUIRE(binding.prepare(*pinned, routes, config(2), 48'000.0, kFrames));
        const auto device_node = binding.device_node_for({20});
        REQUIRE(device_node != 0);
        const auto* device = graph.node(device_node);
        REQUIRE(device);
        REQUIRE(device->type == NodeType::Plugin);
        REQUIRE(device->plugin_info.format == PluginFormat::BuiltIn);
        REQUIRE(device->plugin_info.unique_id == kBasicInstrumentBindingKey);
        REQUIRE(device->plugin_info.path.empty());
        REQUIRE(std::count_if(graph.nodes().begin(), graph.nodes().end(), [](const auto& node) {
                    return node.type == NodeType::Plugin &&
                           node.plugin_info.format == PluginFormat::BuiltIn;
                }) == 1);

        const auto midi_node = binding.midi_input_node_for({10});
        REQUIRE(std::count_if(graph.connections().begin(), graph.connections().end(),
                              [&](const Connection& edge) {
                                  return edge.source_node == midi_node &&
                                         edge.dest_node == device_node && edge.midi;
                              }) == 1);
        const auto left = std::find_if(
            graph.connections().begin(), graph.connections().end(),
            [&](const Connection& edge) { return plain_audio_edge(edge, device_node, 0); });
        const auto right = std::find_if(
            graph.connections().begin(), graph.connections().end(),
            [&](const Connection& edge) { return plain_audio_edge(edge, device_node, 1); });
        REQUIRE(left != graph.connections().end());
        REQUIRE(right != graph.connections().end());
        REQUIRE(left->dest_node == right->dest_node);
        REQUIRE(left->dest_port == 0);
        REQUIRE(right->dest_port == 1);
        REQUIRE(graph.node(left->dest_node)->type == NodeType::Custom);
        REQUIRE(std::none_of(
            graph.connections().begin(), graph.connections().end(), [&](const Connection& edge) {
                return edge.dest_node == device_node && !edge.midi && !edge.feedback &&
                       !edge.automation && !edge.audio_rate_modulation && !edge.sidechain;
            }));
        REQUIRE(std::count_if(graph.connections().begin(), graph.connections().end(),
                              [&](const Connection& edge) {
                                  return edge.source_node == device_node ||
                                         edge.dest_node == device_node;
                              }) == 3);
        REQUIRE(binding.prepare(*pinned, routes, config(2), 48'000.0, kFrames));
        REQUIRE(binding.device_node_for({20}) == device_node);
    }
    REQUIRE(graph.nodes().size() == caller_node_count);
    REQUIRE(graph.node(output_node) != nullptr);
    REQUIRE(graph.connections().empty());
}

TEST_CASE("timeline graph binding basic-instrument ownership control is independent of MIDI") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map), map, take(DecodedAudioAssetPool::create({})),
                     1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);

    TrackedTimelineDeviceLifecycle lifecycle;
    ScopedTrackedTimelineDeviceLifecycle selected(lifecycle);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        binding.set_timeline_device_factory_for_test(&tracked_timeline_device_factory);
        REQUIRE(binding.prepare(*pinned, routes, config(2), 48'000.0, 64));
        const auto device_node = binding.device_node_for({20});
        REQUIRE(device_node != 0);
        const auto* device = graph.node(device_node);
        REQUIRE(device);
        REQUIRE(device->type == NodeType::Plugin);
        REQUIRE(device->plugin_info.format == PluginFormat::BuiltIn);
        REQUIRE(device->plugin_info.unique_id == kBasicInstrumentBindingKey);
        REQUIRE(lifecycle.constructs.load(std::memory_order_relaxed) == 1);
        REQUIRE(lifecycle.prepares.load(std::memory_order_relaxed) == 1);
        REQUIRE(lifecycle.processes.load(std::memory_order_relaxed) == 0);
    }
    REQUIRE(lifecycle.releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle.destroys.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("timeline graph binding basic-instrument MIDI delivery control ignores audio output") {
    constexpr std::size_t kFrames = 64;
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map), map, take(DecodedAudioAssetPool::create({})),
                     1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);

    TrackedTimelineDeviceLifecycle lifecycle;
    ScopedTrackedTimelineDeviceLifecycle selected(lifecycle);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        binding.set_timeline_device_factory_for_test(&tracked_timeline_device_factory);
        REQUIRE(binding.prepare(*pinned, routes, config(2), 48'000.0, kFrames));
        REQUIRE(binding.device_node_for({20}) != 0);
        Buffer input(2, kFrames);
        Buffer ignored_output(2, kFrames, 7.0f);
        auto output_view = ignored_output.view();
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, kFrames)));
        REQUIRE(lifecycle.constructs.load(std::memory_order_relaxed) == 1);
        REQUIRE(lifecycle.prepares.load(std::memory_order_relaxed) == 1);
        REQUIRE(lifecycle.processes.load(std::memory_order_relaxed) == 1);
        REQUIRE(lifecycle.midi_events.load(std::memory_order_relaxed) == 2);
    }
    REQUIRE(lifecycle.releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle.destroys.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("timeline graph binding basic-instrument sound survives canonical reopen") {
    constexpr std::size_t kFrames = 64;
    const auto map = tempo_map();
    const auto authored = basic_instrument_project(*map);
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    const auto render = [&](const std::shared_ptr<const Project>& project, std::uint64_t revision,
                            std::size_t preseed_nodes) {
        ProgramHarness programs;
        programs.publish(project, map, no_assets, revision);
        auto pinned = programs.store.read();
        REQUIRE(pinned);
        SignalGraph graph;
        for (std::size_t index = 0; index < preseed_nodes; ++index)
            REQUIRE(graph.add_gain_node("preseed " + std::to_string(index)) != 0);
        const auto output_node = graph.add_output_node(2);
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
        REQUIRE(binding.prepare(*pinned, routes, config(2), 48'000.0, kFrames));
        const auto device_node = binding.device_node_for({20});
        REQUIRE(device_node != 0);
        Buffer input(2, kFrames);
        Buffer output(2, kFrames);
        auto output_view = output.view();
        const auto transport = snapshot(*pinned, kFrames);
        std::size_t allocations = 1;
        {
            test::ScopedRtProcessProbe probe;
            REQUIRE(binding.process(output_view, input.const_view(), transport));
            allocations = probe.allocation_count();
        }
        REQUIRE(allocations == 0);
        double energy = 0.0;
        for (const auto& channel : output.storage)
            for (const float sample : channel)
                energy += static_cast<double>(sample) * sample;
        REQUIRE(energy > 0.0);
        return std::pair{device_node, std::move(output.storage)};
    };

    const auto fresh = render(authored, 1, 0);
    const auto fresh_hash = full_sample_hash(fresh.second);
    INFO("full sample hash " << fresh_hash);
    REQUIRE(fresh_hash != 0);
    REQUIRE(fresh_hash == kBasicInstrumentExpectedFullSampleHash);
    const auto registry = take(make_builtin_timeline_registry());
    const auto saved = take(serialize_project(*authored, registry));
    const auto reopened = take(deserialize_project(saved.json, registry));
    const auto reopened_saved = take(serialize_project(reopened, registry));
    REQUIRE(reopened_saved.json == saved.json);
    REQUIRE(saved.json.find("\"device_kind\":\"built_in\"") != std::string::npos);
    REQUIRE(saved.json.find("\"binding_key\":\"pulp.instrument.basic\"") != std::string::npos);
    for (const std::string_view forbidden :
         {"\"format\"", "\"path\"", "\"state_b64\"", "\"nodes\"", "\"connections\"", "\"node_id\"",
          "\"plugin_node\"", "\"plugin_info\"", "\"plugin_state\"", "\"state_blob\"",
          "\"last_path\""}) {
        INFO("forbidden host token " << forbidden);
        REQUIRE(saved.json.find(forbidden) == std::string::npos);
    }
    const auto reopened_render = render(std::make_shared<const Project>(std::move(reopened)), 2, 3);
    REQUIRE(reopened_render.first != fresh.first);
    REQUIRE(reopened_render.second == fresh.second);
    REQUIRE(full_sample_hash(reopened_render.second) == kBasicInstrumentExpectedFullSampleHash);
}

TEST_CASE("timeline graph binding admits exactly one basic-instrument node and three edges") {
    const auto map = tempo_map();
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map, {}, std::nullopt, false), map, no_assets, 1);
    auto no_device = programs.store.read();
    REQUIRE(no_device);

    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{
        .track_id = {10},
        .audio_destination = output_node,
    }};
    SignalGraph::GraphLimits limits;
    limits.max_nodes = 4;
    limits.max_connections = 4;
    graph.set_limits(limits);
    const auto caller_nodes = graph.nodes().size();
    const auto caller_connections = graph.connections();
    REQUIRE(binding.preflight(*no_device, routes, config(2), 64));

    programs.publish(basic_instrument_project(*map), map, no_assets, 2);
    auto with_device = programs.store.read();
    REQUIRE(with_device);
    auto result = binding.preflight(*with_device, routes, config(2), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::NodeLimitExceeded);
    REQUIRE(result.actual == 5);
    REQUIRE(result.limit == 4);
    REQUIRE(graph.nodes().size() == caller_nodes);
    REQUIRE(graph.connections() == caller_connections);
    REQUIRE(binding.audio_node_for({10}) == 0);
    REQUIRE(binding.device_node_for({20}) == 0);

    limits.max_nodes = 5;
    limits.max_connections = 6;
    graph.set_limits(limits);
    result = binding.preflight(*with_device, routes, config(2), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::ConnectionLimitExceeded);
    REQUIRE(result.actual == 7);
    REQUIRE(result.limit == 6);
    REQUIRE(graph.nodes().size() == caller_nodes);
    REQUIRE(graph.connections() == caller_connections);
    REQUIRE(binding.audio_node_for({10}) == 0);
    REQUIRE(binding.device_node_for({20}) == 0);

    limits.max_connections = 7;
    graph.set_limits(limits);
    REQUIRE(binding.preflight(*with_device, routes, config(2), 64));
}

TEST_CASE("timeline graph binding prepares an exact pinned basic-instrument program") {
    constexpr std::size_t kFrames = 64;
    const auto map = tempo_map();
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map), map, no_assets, 1);
    auto pinned_a = programs.store.read();
    REQUIRE(pinned_a);

    auto retargeted = DeviceConfiguration{
        .position = DeviceChainPosition::PreFader,
        .slot_kind = DeviceSlotKind::EventToAudio,
        .device_kind = DeviceKind::BuiltIn,
        .binding_key = "pulp.instrument.retargeted",
    };
    programs.publish(basic_instrument_project(*map, std::move(retargeted)), map, no_assets, 2);
    auto pinned_b = programs.store.read();
    REQUIRE(pinned_b);
    REQUIRE(pinned_b->generation() > pinned_a->generation());

    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const std::array routes{TimelineTrackGraphRoute{
        .track_id = {10},
        .audio_destination = output_node,
    }};

    // Preparing A after B is already latest must use A's exact authored
    // declaration rather than consulting PlaybackProgramStore independently.
    REQUIRE(binding.prepare(*pinned_a, routes, config(2), 48'000.0, kFrames));
    const auto device_node = binding.device_node_for({20});
    REQUIRE(device_node != 0);
    REQUIRE(graph.node(device_node)->plugin_info.unique_id == kBasicInstrumentBindingKey);
    Buffer input(2, kFrames);
    Buffer before(2, kFrames);
    auto before_view = before.view();
    REQUIRE(binding.process(before_view, input.const_view(), snapshot(*pinned_a, kFrames)));
    REQUIRE(std::any_of(before.storage[0].begin(), before.storage[0].end(),
                        [](float sample) { return sample != 0.0f; }));

    REQUIRE(binding.adopt_program(*pinned_b).code ==
            TimelineGraphAdmissionCode::UnsupportedDeviceChain);
    const auto nodes_before_replacement = graph.nodes().size();
    const auto connections_before_replacement = graph.connections();
    REQUIRE(binding.prepare(*pinned_b, routes, config(2), 48'000.0, kFrames).code ==
            TimelineGraphAdmissionCode::UnsupportedDeviceBinding);
    REQUIRE(binding.device_node_for({20}) == device_node);
    REQUIRE(graph.node(device_node)->plugin_info.unique_id == kBasicInstrumentBindingKey);
    REQUIRE(graph.nodes().size() == nodes_before_replacement);
    REQUIRE(graph.connections() == connections_before_replacement);

    Buffer after(2, kFrames);
    auto after_view = after.view();
    REQUIRE(binding.process(after_view, input.const_view(), snapshot(*pinned_a, kFrames)));
    REQUIRE(after.storage == before.storage);

    auto event_stage = DeviceConfiguration{
        .position = DeviceChainPosition::PreFader,
        .slot_kind = DeviceSlotKind::EventToEvent,
        .device_kind = DeviceKind::BuiltIn,
        .binding_key = std::string(kBasicInstrumentBindingKey),
    };
    auto audio_stage = event_stage;
    audio_stage.slot_kind = DeviceSlotKind::EventToAudio;
    std::vector<DevicePlacement> chain{
        {{20}, std::move(event_stage), std::nullopt},
        {{21}, std::move(audio_stage), std::nullopt},
    };
    programs.publish(basic_instrument_project_with_devices(*map, std::move(chain)), map, no_assets,
                     3);
    auto multi = programs.store.read();
    REQUIRE(multi);
    REQUIRE(binding.adopt_program(*multi).code ==
            TimelineGraphAdmissionCode::UnsupportedDeviceChain);
    REQUIRE(binding.device_node_for({20}) == device_node);
    REQUIRE(binding.process(after_view, input.const_view(), snapshot(*pinned_a, kFrames)));
    REQUIRE(after.storage == before.storage);

    programs.publish(basic_instrument_project(*map, {}, std::nullopt, false), map, no_assets, 4);
    auto removed = programs.store.read();
    REQUIRE(removed);
    const auto stable_track_node = binding.audio_node_for({10});
    REQUIRE(binding.prepare(*removed, routes, config(2), 48'000.0, kFrames));
    REQUIRE(binding.audio_node_for({10}) == stable_track_node);
    REQUIRE(binding.device_node_for({20}) == 0);
    REQUIRE(graph.node(device_node) == nullptr);
    Buffer silent(2, kFrames, 7.0f);
    auto silent_view = silent.view();
    REQUIRE(binding.process(silent_view, input.const_view(), snapshot(*removed, kFrames)));
    REQUIRE(std::all_of(silent.storage.begin(), silent.storage.end(), [](const auto& channel) {
        return std::all_of(channel.begin(), channel.end(),
                           [](float sample) { return sample == 0.0f; });
    }));
}

TEST_CASE("timeline graph binding replaces a canonical placement at an exact block boundary") {
    constexpr std::size_t kFrames = 64;
    const auto map = tempo_map();
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map), map, no_assets, 1);
    auto pinned_a = programs.store.read();
    REQUIRE(pinned_a);

    TrackedTimelineDeviceLifecycle lifecycle_a;
    TrackedTimelineDeviceLifecycle lifecycle_b;
    ScopedTrackedTimelineDeviceLifecycle selected(lifecycle_a);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        binding.set_timeline_device_factory_for_test(&tracked_timeline_device_factory);
        REQUIRE(binding.prepare(*pinned_a, routes, config(2), 48'000.0, kFrames));
        const auto old_node = binding.device_node_for({20});
        REQUIRE(old_node != 0);
        REQUIRE(lifecycle_a.prepares.load(std::memory_order_relaxed) == 1);

        Buffer input(2, kFrames);
        Buffer before(2, kFrames);
        auto before_view = before.view();
        REQUIRE(binding.process(before_view, input.const_view(), snapshot(*pinned_a, kFrames)));
        const auto old_hash = full_sample_hash(before.storage);
        REQUIRE(old_hash != 0);

        const auto replacement_project = basic_instrument_project(
            *map, basic_instrument_configuration(), std::nullopt, true, {21}, 96);
        programs.publish(replacement_project, map, no_assets, 2);
        auto pinned_b = programs.store.read();
        REQUIRE(pinned_b);
        REQUIRE(pinned_b->generation() > pinned_a->generation());
        selected.select(lifecycle_b);

        Buffer boundary_input(2, kFrames);
        Buffer boundary_output(2, kFrames, 7.0f);
        const auto boundary_transport = snapshot(*pinned_a, kFrames);
        BindingBoundaryProbe boundary{
            .binding = &binding,
            .transport = &boundary_transport,
            .input = &boundary_input,
            .output = &boundary_output,
        };
        binding.set_before_binding_publish_hook_for_test(&process_binding_before_publish,
                                                         &boundary);
        REQUIRE(binding.prepare(*pinned_b, routes, config(2), 48'000.0, kFrames));
        binding.set_before_binding_publish_hook_for_test(nullptr);

        REQUIRE(boundary.result.code == TimelineGraphProcessCode::Ok);
        REQUIRE(boundary.hash == old_hash);
        REQUIRE(boundary.old_placement_node == old_node);
        REQUIRE(boundary.new_placement_node == 0);
        REQUIRE(boundary.renderer_key.generation == pinned_a->generation());
        REQUIRE(binding.device_node_for({20}) == 0);
        const auto new_node = binding.device_node_for({21});
        REQUIRE(new_node != 0);
        REQUIRE(new_node != old_node);
        REQUIRE(lifecycle_b.prepares.load(std::memory_order_relaxed) == 1);

        Buffer after(2, kFrames);
        auto after_view = after.view();
        REQUIRE(binding.process(after_view, input.const_view(), snapshot(*pinned_b, kFrames)));
        const auto new_hash = full_sample_hash(after.storage);
        REQUIRE(new_hash != 0);
        REQUIRE(new_hash != old_hash);
    }
    REQUIRE(lifecycle_a.prepares.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_a.releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_a.destroys.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_b.prepares.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_b.releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_b.destroys.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("timeline graph binding removes a canonical placement at an exact block boundary") {
    constexpr std::size_t kFrames = 64;
    const auto map = tempo_map();
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map), map, no_assets, 1);
    auto pinned_a = programs.store.read();
    REQUIRE(pinned_a);

    TrackedTimelineDeviceLifecycle lifecycle_a;
    ScopedTrackedTimelineDeviceLifecycle selected(lifecycle_a);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        binding.set_timeline_device_factory_for_test(&tracked_timeline_device_factory);
        REQUIRE(binding.prepare(*pinned_a, routes, config(2), 48'000.0, kFrames));
        const auto old_node = binding.device_node_for({20});
        REQUIRE(old_node != 0);
        const auto stable_track_node = binding.audio_node_for({10});
        Buffer input(2, kFrames);
        Buffer before(2, kFrames);
        auto before_view = before.view();
        REQUIRE(binding.process(before_view, input.const_view(), snapshot(*pinned_a, kFrames)));
        const auto old_hash = full_sample_hash(before.storage);
        REQUIRE(old_hash == kBasicInstrumentExpectedFullSampleHash);

        programs.publish(basic_instrument_project(*map, {}, std::nullopt, false), map, no_assets,
                         2);
        auto removed = programs.store.read();
        REQUIRE(removed);
        REQUIRE(removed->generation() > pinned_a->generation());

        Buffer boundary_input(2, kFrames);
        Buffer boundary_output(2, kFrames, 7.0f);
        const auto boundary_transport = snapshot(*pinned_a, kFrames);
        BindingBoundaryProbe boundary{
            .binding = &binding,
            .transport = &boundary_transport,
            .input = &boundary_input,
            .output = &boundary_output,
        };
        binding.set_before_binding_publish_hook_for_test(&process_binding_before_publish,
                                                         &boundary);
        REQUIRE(binding.prepare(*removed, routes, config(2), 48'000.0, kFrames));
        binding.set_before_binding_publish_hook_for_test(nullptr);

        REQUIRE(boundary.result.code == TimelineGraphProcessCode::Ok);
        REQUIRE(boundary.hash == old_hash);
        REQUIRE(boundary.old_placement_node == old_node);
        REQUIRE(boundary.renderer_key.generation == pinned_a->generation());
        REQUIRE(binding.device_node_for({20}) == 0);
        REQUIRE(binding.audio_node_for({10}) == stable_track_node);
        REQUIRE(graph.node(old_node) == nullptr);

        Buffer silent(2, kFrames, 7.0f);
        auto silent_view = silent.view();
        REQUIRE(binding.process(silent_view, input.const_view(), snapshot(*removed, kFrames)));
        REQUIRE(std::all_of(silent.storage.begin(), silent.storage.end(), [](const auto& channel) {
            return std::all_of(channel.begin(), channel.end(),
                               [](float sample) { return sample == 0.0f; });
        }));
    }
    REQUIRE(lifecycle_a.constructs.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_a.prepares.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_a.releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_a.destroys.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("timeline graph binding rejected candidates preserve the exact audible publication") {
    constexpr std::size_t kFrames = 64;
    const auto map = tempo_map();
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map), map, no_assets, 1);
    auto pinned_a = programs.store.read();
    REQUIRE(pinned_a);

    TrackedTimelineDeviceLifecycle lifecycle_a;
    TrackedTimelineDeviceLifecycle rejected_lifecycle;
    ScopedTrackedTimelineDeviceLifecycle selected(lifecycle_a);
    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    const std::array routes{TimelineTrackGraphRoute{{10}, output_node}};
    {
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        binding.set_timeline_device_factory_for_test(&tracked_timeline_device_factory);
        REQUIRE(binding.prepare(*pinned_a, routes, config(2), 48'000.0, kFrames));
        const auto device_node = binding.device_node_for({20});
        REQUIRE(device_node != 0);
        Buffer input(2, kFrames);
        Buffer before(2, kFrames);
        auto before_view = before.view();
        REQUIRE(binding.process(before_view, input.const_view(), snapshot(*pinned_a, kFrames)));
        const auto audible_hash = full_sample_hash(before.storage);
        REQUIRE(audible_hash == kBasicInstrumentExpectedFullSampleHash);
        const auto nodes_before = graph_node_signatures(graph);
        const auto connections_before = graph_connection_signatures(graph);
        const auto key_before = binding.renderer_key_for({10});
        REQUIRE(key_before.generation == pinned_a->generation());

        const auto replacement_project = basic_instrument_project(
            *map, basic_instrument_configuration(), std::nullopt, true, {21}, 96);
        programs.publish(replacement_project, map, no_assets, 2);
        auto pinned_b = programs.store.read();
        REQUIRE(pinned_b);
        selected.select(rejected_lifecycle);

        SignalGraph::PreparedTopologyEdit::Result expected_graph_result =
            SignalGraph::PreparedTopologyEdit::Result::NotPrepared;
        StaleGraphMutationProbe stale_mutation{.graph = &graph, .node = output_node};
        int expected_prepares = 1;
        int expected_releases = 1;
        bool expect_precommit_mutation = false;
        SECTION("slot prepare failure") {
            rejected_lifecycle.fail_prepare = true;
            expected_graph_result =
                SignalGraph::PreparedTopologyEdit::Result::ExternalPluginReprepareRequired;
        }
        SECTION("stale graph base after factory construction and before slot prepare") {
            rejected_lifecycle.inject_stale_after_factory = true;
            rejected_lifecycle.graph_for_factory_stale = &graph;
            rejected_lifecycle.factory_stale_node = output_node;
            expected_graph_result = SignalGraph::PreparedTopologyEdit::Result::StaleBase;
            expected_prepares = 0;
            expected_releases = 0;
        }
        SECTION("stale graph base after candidate prepare") {
            binding.set_before_graph_commit_hook_for_test(
                &preserve_topology_while_advancing_graph_generation, &stale_mutation);
            expected_graph_result = SignalGraph::PreparedTopologyEdit::Result::StaleBase;
            expect_precommit_mutation = true;
        }

        const auto rejected = binding.prepare(*pinned_b, routes, config(2), 48'000.0, kFrames);
        REQUIRE(rejected.code == TimelineGraphAdmissionCode::GraphPrepareFailed);
        REQUIRE(rejected.actual == static_cast<std::uint64_t>(expected_graph_result));
        REQUIRE(graph_node_signatures(graph) == nodes_before);
        REQUIRE(graph_connection_signatures(graph) == connections_before);
        REQUIRE(binding.device_node_for({20}) == device_node);
        REQUIRE(binding.device_node_for({21}) == 0);
        REQUIRE(binding.renderer_key_for({10}) == key_before);

        Buffer after(2, kFrames, 7.0f);
        auto after_view = after.view();
        REQUIRE(binding.process(after_view, input.const_view(), snapshot(*pinned_a, kFrames)));
        REQUIRE(full_sample_hash(after.storage) == audible_hash);
        REQUIRE(rejected_lifecycle.constructs.load(std::memory_order_relaxed) == 1);
        REQUIRE(rejected_lifecycle.prepares.load(std::memory_order_relaxed) == expected_prepares);
        REQUIRE(rejected_lifecycle.releases.load(std::memory_order_relaxed) == expected_releases);
        REQUIRE(rejected_lifecycle.destroys.load(std::memory_order_relaxed) == 1);
        if (rejected_lifecycle.inject_stale_after_factory)
            REQUIRE(rejected_lifecycle.factory_stale_succeeded.load(std::memory_order_relaxed));
        if (expect_precommit_mutation)
            REQUIRE(stale_mutation.succeeded);
    }
    REQUIRE(lifecycle_a.prepares.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_a.releases.load(std::memory_order_relaxed) == 1);
    REQUIRE(lifecycle_a.destroys.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("timeline graph binding rejects mixed basic-instrument ownership atomically") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map), map, take(DecodedAudioAssetPool::create({})),
                     1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);

    SignalGraph graph;
    const auto caller_device =
        graph.add_plugin_node(std::make_unique<ConstantInstrumentSlot>(), 0, 1, "caller device");
    const auto output_node = graph.add_output_node(2);
    REQUIRE(graph.prepare(48'000.0, 64));
    const auto nodes_before = graph.nodes().size();
    const auto connections_before = graph.connections();
    const std::array device_routes{TimelineDeviceGraphRoute{{20}, caller_device}};
    const std::array routes{TimelineTrackGraphRoute{
        .track_id = {10},
        .audio_destination = output_node,
        .device_routes = device_routes,
    }};

    TimelineGraphPlaybackBinding binding(graph, programs.store);
    const auto rejected = binding.prepare(*pinned, routes, config(2), 48'000.0, 64);
    REQUIRE(rejected.code == TimelineGraphAdmissionCode::MixedDeviceOwnership);
    REQUIRE(rejected.item == ItemId{20});
    REQUIRE(graph.nodes().size() == nodes_before);
    REQUIRE(graph.connections() == connections_before);
    REQUIRE(binding.audio_node_for({10}) == 0);
    REQUIRE(binding.device_node_for({20}) == 0);
}

TEST_CASE("timeline graph binding basic-instrument factory failure is atomic") {
    const auto map = tempo_map();
    ProgramHarness programs;
    programs.publish(basic_instrument_project(*map), map, take(DecodedAudioAssetPool::create({})),
                     1);
    auto pinned = programs.store.read();
    REQUIRE(pinned);

    SignalGraph graph;
    const auto output_node = graph.add_output_node(2);
    const auto nodes_before = graph.nodes().size();
    const auto connections_before = graph.connections();
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    binding.set_timeline_device_factory_for_test(&reject_timeline_device_factory);
    const std::array routes{TimelineTrackGraphRoute{
        .track_id = {10},
        .audio_destination = output_node,
    }};

    const auto rejected = binding.prepare(*pinned, routes, config(2), 48'000.0, 64);
    REQUIRE(rejected.code == TimelineGraphAdmissionCode::DeviceFactoryFailed);
    REQUIRE(rejected.item == ItemId{20});
    REQUIRE(graph.nodes().size() == nodes_before);
    REQUIRE(graph.connections() == connections_before);
    REQUIRE(binding.audio_node_for({10}) == 0);
    REQUIRE(binding.device_node_for({20}) == 0);
}

TEST_CASE("timeline graph binding rejects unsupported basic-instrument declaration axes") {
    const auto map = tempo_map();
    const auto no_assets = take(DecodedAudioAssetPool::create({}));
    const auto admit = [&](const std::shared_ptr<const Project>& project) {
        ProgramHarness programs;
        programs.publish(project, map, no_assets, 1);
        auto pinned = programs.store.read();
        SignalGraph graph;
        const auto output_node = graph.add_output_node(2);
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        const std::array routes{TimelineTrackGraphRoute{
            .track_id = {10},
            .audio_destination = output_node,
        }};
        const auto result = binding.prepare(*pinned, routes, config(2), 48'000.0, 64);
        REQUIRE(graph.nodes().size() == 1);
        REQUIRE(graph.connections().empty());
        return result;
    };
    const auto basic = DeviceConfiguration{
        .position = DeviceChainPosition::PreFader,
        .slot_kind = DeviceSlotKind::EventToAudio,
        .device_kind = DeviceKind::BuiltIn,
        .binding_key = std::string(kBasicInstrumentBindingKey),
    };

    SECTION("adjacent no-device route remains renderable") {
        ProgramHarness programs;
        programs.publish(audio_project(1.0f, 128), map, asset_pool(std::vector<float>(128, 1.0f)),
                         1);
        auto pinned = programs.store.read();
        REQUIRE(pinned);
        SignalGraph graph;
        const auto output_node = graph.add_output_node(2);
        TimelineGraphPlaybackBinding binding(graph, programs.store);
        const std::array routes{TimelineTrackGraphRoute{
            .track_id = {10},
            .audio_destination = output_node,
        }};
        REQUIRE(binding.prepare(*pinned, routes, config(2), 48'000.0, 64));
        Buffer input(2, 64);
        Buffer output(2, 64, 7.0f);
        auto output_view = output.view();
        REQUIRE(binding.process(output_view, input.const_view(), snapshot(*pinned, 64)));
        REQUIRE(binding.device_node_for({20}) == 0);
        REQUIRE(std::all_of(output.storage.begin(), output.storage.end(), [](const auto& channel) {
            return std::all_of(channel.begin(), channel.end(),
                               [](float sample) { return sample == 1.0f; });
        }));
    }
    SECTION("position") {
        auto declaration = basic;
        declaration.position = DeviceChainPosition::PostFader;
        declaration.slot_kind = DeviceSlotKind::AudioToAudio;
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::UnsupportedDevicePosition);
    }

    SECTION("slot kind") {
        auto declaration = basic;
        declaration.slot_kind = DeviceSlotKind::AudioToAudio;
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::UnsupportedDeviceSlotKind);
    }
    SECTION("standalone event-to-event slot kind") {
        auto declaration = basic;
        declaration.slot_kind = DeviceSlotKind::EventToEvent;
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::UnsupportedDeviceSlotKind);
    }
    SECTION("binding") {
        auto declaration = basic;
        declaration.binding_key = "pulp.instrument.unknown";
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::UnsupportedDeviceBinding);
    }
    SECTION("external device kind remains caller-owned") {
        auto declaration = basic;
        declaration.device_kind = DeviceKind::External;
        // External devices remain caller-owned. Without a caller route the
        // structured failure is a missing placement, before BuiltIn resolver
        // declaration validation can run.
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::MissingDevicePlacement);
    }
    SECTION("unresolved device kind remains caller-owned") {
        auto declaration = basic;
        declaration.device_kind = DeviceKind::Unresolved;
        declaration.binding_key.clear();
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::MissingDevicePlacement);
    }
    SECTION("generated device kind remains caller-owned") {
        auto declaration = basic;
        declaration.device_kind = DeviceKind::Generated;
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::MissingDevicePlacement);
    }
    SECTION("bypass") {
        auto declaration = basic;
        declaration.bypassed = true;
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::UnsupportedDeviceBypass);
    }
    SECTION("wet dry") {
        auto declaration = basic;
        declaration.wet_dry_bits = std::bit_cast<std::uint32_t>(0.5f);
        REQUIRE(admit(basic_instrument_project(*map, std::move(declaration))).code ==
                TimelineGraphAdmissionCode::UnsupportedDeviceWetDry);
    }
    SECTION("state") {
        const auto state = ContentHash::from_hex(std::string(64, 'd'));
        REQUIRE(state);
        REQUIRE(admit(basic_instrument_project(*map, basic, *state)).code ==
                TimelineGraphAdmissionCode::UnsupportedDeviceState);
    }
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

    const auto node_count = graph.nodes().size();
    const auto connection_count = graph.connections().size();
    const auto custom_type_count = graph.custom_node_type_count();
    REQUIRE(binding.preflight(*pinned, routes, config(), 64));
    REQUIRE(graph.nodes().size() == node_count);
    REQUIRE(graph.connections().size() == connection_count);
    REQUIRE(graph.custom_node_type_count() == custom_type_count);
    REQUIRE(binding.audio_node_for({10}) == 0);

    SignalGraph::GraphLimits graph_limits;
    graph_limits.max_nodes = 2;
    graph.set_limits(graph_limits);
    auto result = binding.preflight(*pinned, routes, config(), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::NodeLimitExceeded);
    REQUIRE(result.actual == 4);
    REQUIRE(result.limit == 2);
    graph_limits.max_nodes = 4;
    graph.set_limits(graph_limits);
    REQUIRE(binding.preflight(*pinned, routes, config(), 64));

    graph_limits = {};
    graph_limits.max_connections = 1;
    graph.set_limits(graph_limits);
    result = binding.preflight(*pinned, routes, config(), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::ConnectionLimitExceeded);
    REQUIRE(result.actual == 4);
    REQUIRE(result.limit == 1);
    graph_limits.max_connections = 4;
    graph.set_limits(graph_limits);
    REQUIRE(binding.preflight(*pinned, routes, config(), 64));

    graph_limits = {};
    graph_limits.max_ports = 4;
    graph.set_limits(graph_limits);
    result = binding.preflight(*pinned, routes, config(), 64);
    REQUIRE(result.code == TimelineGraphAdmissionCode::TotalPortLimitExceeded);
    REQUIRE(result.actual == 9);
    REQUIRE(result.limit == 4);
    graph_limits.max_ports = 9;
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
    for (std::size_t index = 0; index < 508; ++index)
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
    REQUIRE(binding
                .prepare(*pinned, routes, config(1),
                         std::nextafter(projected_rate, std::numeric_limits<double>::infinity()),
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
    programs.publish(audio_project(1.0f, 128), map, asset_pool(std::vector<float>(128, 1.0f)), 1);
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
