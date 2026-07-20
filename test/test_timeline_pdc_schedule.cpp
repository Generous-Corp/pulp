#include "timeline_pdc_schedule.hpp"

#include "timebase_test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <memory>

using namespace pulp;
using namespace pulp::host;
using namespace pulp::host::detail;
using namespace pulp::playback;
using namespace pulp::timebase;

namespace {

class LatencySlot final : public PluginSlot {
  public:
    LatencySlot(int latency, LatencyQuery query)
        : latency_(latency), query_(query) {
        info_.name = "PDC schedule fixture";
        info_.num_inputs = 1;
        info_.num_outputs = 1;
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}
    void process(audio::BufferView<float>& output,
                 const audio::BufferView<const float>& input,
                 const midi::MidiBuffer&, midi::MidiBuffer&,
                 const ParameterEventQueue&, int samples) override {
        std::copy_n(input.channel_ptr(0), samples, output.channel_ptr(0));
    }
    std::vector<HostParamInfo> parameters() const override { return {}; }
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
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}

  private:
    PluginInfo info_;
    int latency_ = 0;
    LatencyQuery query_ = LatencyQuery::Available;
};

CustomNodeType source_type() {
    CustomNodeType type;
    type.type_id = "test.timeline-pdc-source";
    type.version = 1;
    type.num_output_ports = 1;
    type.process = [](audio::BufferView<float>& output,
                      const audio::BufferView<const float>&, int) {
        output.clear();
    };
    return type;
}

TimelinePdcTrackRequirement requirement(NodeId source, NodeId midi,
                                        NodeId device) {
    TimelinePdcTrackRequirement result;
    result.track_id = {10};
    result.audio_source = source;
    result.midi_destination = midi;
    result.audio_slot = std::make_shared<TimelinePdcAudioTransportSlot>();
    if (device != 0) {
        result.devices.push_back({{20}, device});
        result.automation_device_ids.push_back({20});
    }
    return result;
}

CompiledTempoMap tempo_map() {
    const std::array points{TempoPoint{{0}, 60.0}};
    return require_compiled_tempo_map(points, RationalRate{48'000, 1});
}

} // namespace

TEST_CASE("timeline PDC schedule seals exact source and sink leads") {
    SignalGraph graph;
    REQUIRE(graph.register_custom_node_type(source_type()));
    const auto source = graph.add_custom_node("test.timeline-pdc-source");
    const auto plugin = graph.add_plugin_node(
        std::make_unique<LatencySlot>(11, PluginSlot::LatencyQuery::Available),
        1, 1);
    const auto output = graph.add_output_node(1);
    const auto non_automated_input = graph.add_input_node(1);
    const auto non_automated = graph.add_plugin_node(
        std::make_unique<LatencySlot>(7, PluginSlot::LatencyQuery::Available),
        1, 1);
    const auto non_automated_output = graph.add_output_node(1);
    REQUIRE(graph.connect(source, 0, plugin, 0));
    REQUIRE(graph.connect(plugin, 0, output, 0));
    REQUIRE(graph.connect(non_automated_input, 0, non_automated, 0));
    REQUIRE(graph.connect(non_automated, 0, non_automated_output, 0));

    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->prepare_quiesced(48'000.0, 64) ==
            SignalGraph::PreparedTopologyEdit::Result::Prepared);
    auto track_requirement = requirement(source, plugin, plugin);
    track_requirement.devices.push_back({{30}, non_automated});
    const std::array requirements{track_requirement};
    auto built = build_timeline_pdc_schedule(*edit, requirements);
    REQUIRE(built);
    auto plan = std::move(built).value();
    REQUIRE(plan.tracks.size() == 1);
    REQUIRE(plan.tracks[0].audio_lead_samples == 11);
    REQUIRE(plan.tracks[0].midi_lead_samples == 11);
    REQUIRE(plan.tracks[0].devices.size() == 2);
    REQUIRE(plan.tracks[0].devices[0].lead_samples == 11);
    REQUIRE(plan.tracks[0].devices[1].lead_samples == 7);
    REQUIRE(plan.tracks[0].device_views.size() == 1);
    REQUIRE(plan.tracks[0].device_views[0].device_placement_id == timeline::ItemId{20});

    const auto map = tempo_map();
    MasterTransport transport;
    MasterTransportConfig config;
    config.max_buffer_size = 64;
    config.initially_playing = true;
    REQUIRE(transport.prepare(map, config) == TransportError::None);
    TransportSnapshot base;
    REQUIRE(transport.begin_block(32, base) == TransportError::None);
    REQUIRE(project_timeline_pdc_schedule(plan, base) ==
            TimelineGraphProcessCode::Ok);
    REQUIRE(plan.tracks[0].audio_projection.ranges[0].timeline_sample_start ==
            SamplePosition{11});
    REQUIRE(requirements[0].audio_slot->transport.load() ==
            &plan.tracks[0].audio_projection);
    clear_timeline_pdc_audio_slots(plan);
    REQUIRE(requirements[0].audio_slot->transport.load() == nullptr);

    auto clone = clone_timeline_pdc_schedule(plan);
    REQUIRE(clone.tracks[0].device_views[0].transport ==
            &clone.tracks[0].devices[0].projected);
    REQUIRE(clone.tracks[0].device_views[0].transport !=
            plan.tracks[0].device_views[0].transport);
}

TEST_CASE("timeline PDC schedule maps every unavailable latency status") {
    using Admission = TimelineGraphAdmissionCode;
    using Query = PluginSlot::LatencyQuery;

    SECTION("no compiled snapshot") {
        SignalGraph graph;
        REQUIRE(graph.register_custom_node_type(source_type()));
        const auto source = graph.add_custom_node("test.timeline-pdc-source");
        auto edit = graph.begin_prepared_topology_edit();
        const std::array requirements{requirement(source, 0, 0)};
        const auto built = build_timeline_pdc_schedule(*edit, requirements);
        REQUIRE_FALSE(built);
        REQUIRE(built.error().code == Admission::LatencyNoCompiledSnapshot);
    }

    for (const auto [query, expected] :
         {std::pair{Query::Unsupported, Admission::LatencyUnsupported},
          std::pair{Query::QueryFailed, Admission::LatencyQueryFailed}}) {
        SignalGraph graph;
        REQUIRE(graph.register_custom_node_type(source_type()));
        const auto source = graph.add_custom_node("test.timeline-pdc-source");
        const auto unavailable = graph.add_plugin_node(
            std::make_unique<LatencySlot>(0, query), 1, 1);
        const auto output = graph.add_output_node(1);
        REQUIRE(graph.connect(source, 0, output, 0));
        REQUIRE(graph.connect(unavailable, 0, output, 0));
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(48'000.0, 64) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared);
        const std::array requirements{requirement(source, 0, unavailable)};
        const auto built = build_timeline_pdc_schedule(*edit, requirements);
        REQUIRE_FALSE(built);
        REQUIRE(built.error().code == expected);
        REQUIRE(built.error().node == unavailable);
    }

    SECTION("no output path") {
        SignalGraph graph;
        REQUIRE(graph.register_custom_node_type(source_type()));
        const auto source = graph.add_custom_node("test.timeline-pdc-source");
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(48'000.0, 64) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared);
        const std::array requirements{requirement(source, 0, 0)};
        const auto built = build_timeline_pdc_schedule(*edit, requirements);
        REQUIRE_FALSE(built);
        REQUIRE(built.error().code == Admission::LatencyNoOutputPath);
    }

    SECTION("unknown node") {
        SignalGraph graph;
        const auto output = graph.add_output_node(1);
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(48'000.0, 64) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared);
        const std::array requirements{requirement(999'999, 0, 0)};
        const auto built = build_timeline_pdc_schedule(*edit, requirements);
        REQUIRE_FALSE(built);
        REQUIRE(built.error().code == Admission::LatencyUnknownNode);
        REQUIRE(built.error().node == 999'999);
        REQUIRE(output != 0);
    }

    SECTION("ambiguous output") {
        SignalGraph graph;
        const auto branch = graph.add_plugin_node(
            std::make_unique<LatencySlot>(0, Query::Available), 1, 1);
        const auto ten = graph.add_plugin_node(
            std::make_unique<LatencySlot>(10, Query::Available), 1, 1);
        const auto twenty = graph.add_plugin_node(
            std::make_unique<LatencySlot>(20, Query::Available), 1, 1);
        const auto output_a = graph.add_output_node(1);
        const auto output_b = graph.add_output_node(1);
        REQUIRE(graph.connect(branch, 0, ten, 0));
        REQUIRE(graph.connect(ten, 0, output_a, 0));
        REQUIRE(graph.connect(branch, 0, twenty, 0));
        REQUIRE(graph.connect(twenty, 0, output_b, 0));
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->prepare_quiesced(48'000.0, 64) ==
                SignalGraph::PreparedTopologyEdit::Result::Prepared);
        const std::array requirements{requirement(branch, 0, 0)};
        const auto built = build_timeline_pdc_schedule(*edit, requirements);
        REQUIRE_FALSE(built);
        REQUIRE(built.error().code == Admission::LatencyAmbiguousOutput);
    }
}
