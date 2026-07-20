#include "support/timeline_graph_binding_test_support.hpp"

namespace {

template <typename T, typename E> T checked_take(runtime::Result<T, E> result) {
    REQUIRE(result);
    return std::move(result).value();
}

class PdcProbeSlot final : public PluginSlot {
  public:
    explicit PdcProbeSlot(int latency,
                          LatencyQuery query = LatencyQuery::Available)
        : latency_(latency), query_(query) {
        info_.name = "timeline PDC probe";
        info_.num_inputs = 1;
        info_.num_outputs = 1;
        parameter_.id = 7;
        parameter_.name = "probe";
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer&,
                 const ParameterEventQueue& params, int frames) override {
        capture(midi_in, params);
        if (!input.empty()) {
            std::copy_n(input.channel_ptr(0), frames, output.channel_ptr(0));
        } else {
            output.clear();
        }
    }
    void process(format::ProcessBuffers& buffers,
                 const midi::MidiBuffer& midi_in, midi::MidiBuffer& midi_out,
                 const ParameterEventQueue& params, int frames,
                 const format::ProcessContext& context) override {
        position_samples = context.position_samples;
        PluginSlot::process(buffers, midi_in, midi_out, params, frames);
    }
    std::vector<HostParamInfo> parameters() const override { return {parameter_}; }
    float get_parameter(std::uint32_t) const override { return 0.0f; }
    void set_parameter(std::uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<std::uint8_t> save_state() const override { return {}; }
    bool restore_state(const std::vector<std::uint8_t>&) override { return true; }
    int latency_samples() const override { return latency_; }
    int tail_samples() const override { return 0; }
    LatencyQuery latency_query() const override { return query_; }
    LatencyReport latency_report() const override { return {query_, latency_}; }
    bool wants_transport() const override { return true; }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

    void capture(const midi::MidiBuffer& midi_in,
                 const ParameterEventQueue& params) {
        midi_offsets.clear();
        for (const auto& event : midi_in) midi_offsets.push_back(event.sample_offset);
        parameter_offsets.clear();
        for (const auto& event : params) parameter_offsets.push_back(event.sample_offset);
    }

    std::vector<std::uint32_t> midi_offsets;
    std::vector<std::uint32_t> parameter_offsets;
    std::int64_t position_samples = -1;

  private:
    PluginInfo info_;
    HostParamInfo parameter_;
    int latency_ = 0;
    LatencyQuery query_ = LatencyQuery::Available;
};

std::shared_ptr<const Project> pdc_project(
    const CompiledTempoMap& map, ItemId automated_device,
    std::int64_t automation_sample) {
    auto note_content = checked_take(NoteContent::create(
        {note(map, 101, 13, 14)}));
    auto note_clip = checked_take(Clip::create(
        {102}, {0}, map.samples_to_ticks({128}) - TickPosition{0},
        std::move(note_content)));
    auto curve = checked_take(AutomationCurve::create({AutomationPoint{
        {41}, map.samples_to_ticks({automation_sample}), 0.75f,
        AutomationInterpolation::Hold}}));
    auto lane = checked_take(AutomationLane::create(
        {31}, DeviceParameterTarget{automated_device, 7}, std::move(curve)));
    auto audio_track = checked_take(
        Track::create({10}, "PDC audio", {audio_clip(1.0f, 128)}));
    auto event_track = checked_take(Track::create(TrackInput{
        .id = {11},
        .name = "PDC integration",
        .clips = {std::move(note_clip)},
        .device_chain = {{{21}}, {{20}}},
        .automation_lanes = {std::move(lane)},
    }));
    auto sequence = checked_take(Sequence::create(
        {2}, "root", std::nullopt, std::nullopt,
        std::vector<Track>{std::move(audio_track), std::move(event_track)}));
    const auto hash = ContentHash::from_hex(std::string(64, 'b'));
    REQUIRE(hash);
    MediaAsset asset{.id = {3},
                     .name = "ramp",
                     .frame_count = 128,
                     .sample_rate = {48'000, 1},
                     .content_hash = *hash};
    return std::make_shared<const Project>(checked_take(Project::create(ProjectInput{
        {1}, "PDC integration", 1'000, {2}, {asset}, {std::move(sequence)}})));
}

} // namespace

TEST_CASE("timeline binding applies sealed PDC schedules without shifting events") {
    const auto map = tempo_map();
    std::vector<float> ramp(128);
    for (std::size_t index = 0; index < ramp.size(); ++index)
        ramp[index] = static_cast<float>(index);
    ProgramHarness programs;
    programs.publish(pdc_project(*map, {20}, 8), map, asset_pool(ramp), 1);
    auto first_program = programs.store.read();

    SignalGraph graph;
    auto first = std::make_unique<PdcProbeSlot>(3);
    auto* first_probe = first.get();
    const auto first_node = graph.add_plugin_node(std::move(first), 1, 1);
    auto middle = std::make_unique<PdcProbeSlot>(5);
    auto* middle_probe = middle.get();
    const auto middle_node = graph.add_plugin_node(std::move(middle), 1, 1);
    auto last = std::make_unique<PdcProbeSlot>(7);
    auto* last_probe = last.get();
    const auto last_node = graph.add_plugin_node(std::move(last), 1, 1);
    const auto unavailable = graph.add_plugin_node(
        std::make_unique<PdcProbeSlot>(0, PluginSlot::LatencyQuery::Unsupported),
        1, 1);
    const auto output_node = graph.add_output_node(1);
    REQUIRE(graph.connect(first_node, 0, middle_node, 0));
    REQUIRE(graph.connect(middle_node, 0, last_node, 0));
    REQUIRE(graph.connect(last_node, 0, output_node, 0));
    REQUIRE(graph.prepare(48'000.0, 64));

    const std::array devices{
        TimelineDeviceGraphRoute{{20}, last_node},
        TimelineDeviceGraphRoute{{21}, first_node},
    };
    const std::array routes{
        TimelineTrackGraphRoute{{10}, first_node, 0, 0},
        TimelineTrackGraphRoute{{11}, first_node, 0, middle_node, devices},
    };
    TimelineGraphPlaybackBinding binding(graph, programs.store);
    REQUIRE(binding.prepare(*first_program, routes, config(1), 48'000.0, 64));

    Buffer input(1, 16);
    Buffer output(1, 16);
    auto output_view = output.view();
    auto first_transport = snapshot(*first_program, 16, 0);
    first_transport.block_index = 0;
    REQUIRE(binding.process(output_view, input.const_view(), first_transport));
    REQUIRE(output.storage[0][0] == 15.0f);
    REQUIRE(std::find(middle_probe->midi_offsets.begin(),
                      middle_probe->midi_offsets.end(), 1) !=
            middle_probe->midi_offsets.end());
    REQUIRE(std::find(last_probe->parameter_offsets.begin(),
                      last_probe->parameter_offsets.end(), 1) !=
            last_probe->parameter_offsets.end());
    REQUIRE(middle_probe->position_samples == 0);
    REQUIRE(first_probe->parameter_offsets.empty());

    programs.publish(pdc_project(*map, {21}, 32), map, asset_pool(ramp), 2);
    auto second_program = programs.store.read();
    REQUIRE(binding.adopt_latest_program());
    auto second_transport = snapshot(*second_program, 16, 16);
    second_transport.block_index = 1;
    const auto second_result =
        binding.process(output_view, input.const_view(), second_transport);
    INFO("second process code " << static_cast<int>(second_result.code));
    REQUIRE(second_result);
    REQUIRE(std::find(first_probe->parameter_offsets.begin(),
                      first_probe->parameter_offsets.end(), 1) !=
            first_probe->parameter_offsets.end());
    REQUIRE(last_probe->parameter_offsets.empty());

    const std::array unavailable_devices{
        TimelineDeviceGraphRoute{{20}, unavailable},
        TimelineDeviceGraphRoute{{21}, first_node},
    };
    const std::array unavailable_routes{
        TimelineTrackGraphRoute{{10}, first_node, 0, 0},
        TimelineTrackGraphRoute{{11}, first_node, 0, middle_node,
                                unavailable_devices},
    };
    const auto failed = binding.prepare(
        *second_program, unavailable_routes, config(1), 48'000.0, 64);
    REQUIRE(failed.code == TimelineGraphAdmissionCode::LatencyNoOutputPath);
    REQUIRE(binding.audio_node_for({10}) != 0);
    auto third_transport = snapshot(*second_program, 16, 32);
    third_transport.block_index = 2;
    REQUIRE(binding.process(output_view, input.const_view(), third_transport));
}
