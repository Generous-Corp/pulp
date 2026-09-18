// GraphSerializer round-trip tests.
//
// Verifies that a populated SignalGraph survives to_json / from_json with
// topology, connections, editor layout, and plugin identity preserved.
// Plugin state (save_state blobs) is covered only in shape here — a full
// state round-trip requires a real loaded plugin and lives in the
// integration lane gated on PULP_TEST_CLAP_PATH.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/buffer.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/sample_region_authoring.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::host;

namespace {

PluginInfo make_fake_plugin_info(const std::string& name,
                                 const std::string& uid,
                                 PluginFormat fmt = PluginFormat::CLAP,
                                 int inputs = 2,
                                 int outputs = 2) {
    PluginInfo info;
    info.name = name;
    info.manufacturer = "PulpTest";
    info.version = "1.0.0";
    info.path = "/nonexistent/" + name + ".clap";
    info.unique_id = uid;
    info.format = fmt;
    info.is_effect = true;
    info.num_inputs = inputs;
    info.num_outputs = outputs;
    return info;
}

class SerializerSlot final : public PluginSlot {
public:
    static constexpr uint32_t kParamId = 42;

    explicit SerializerSlot(PluginInfo info,
                            std::vector<uint8_t> state = {},
                            ParamRate rate = ParamRate::ControlRate)
        : info_(std::move(info)), state_(std::move(state)), rate_(rate) {}

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { return true; }
    void release() override {}

    void process(pulp::audio::BufferView<float>&,
                 const pulp::audio::BufferView<const float>&,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const ParameterEventQueue&,
                 int) override {}

    std::vector<HostParamInfo> parameters() const override {
        HostParamInfo p;
        p.id = kParamId;
        p.name = "Drive";
        p.min_value = -1.0f;
        p.max_value = 1.0f;
        p.flags.automatable = true;
        p.rate = rate_;
        return {p};
    }

    float get_parameter(uint32_t) const override { return 0.0f; }
    void set_parameter(uint32_t, float) override {}
    void set_bypass(bool) override {}
    bool is_bypassed() const override { return false; }
    std::vector<uint8_t> save_state() const override { return state_; }
    bool restore_state(const std::vector<uint8_t>& data) override {
        state_ = data;
        return true;
    }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

private:
    PluginInfo info_;
    std::vector<uint8_t> state_;
    ParamRate rate_ = ParamRate::ControlRate;
};

bool missing_plugins_contain(const GraphSerializer::LoadResult& result,
                             const std::string& needle) {
    for (const auto& entry : result.missing_plugins) {
        if (entry.find(needle) != std::string::npos) return true;
    }
    return false;
}

bool missing_custom_types_contain(const GraphSerializer::LoadResult& result,
                                  const std::string& needle) {
    for (const auto& entry : result.missing_custom_node_types) {
        if (entry.find(needle) != std::string::npos) return true;
    }
    return false;
}

const GraphNode* find_node_named(const SignalGraph& graph,
                                 const std::string& name) {
    for (const auto& node : graph.nodes()) {
        if (node.name == name) return &node;
    }
    return nullptr;
}

struct PersistedRegionFixture {
    SignalGraph graph;
    std::unique_ptr<SampleRegionParameterOwner> parameters;
};

void make_persisted_region(PersistedRegionFixture& fixture,
                           std::size_t extra_region_output_connections = 0) {
    const auto input = fixture.graph.add_input_node(1, "Input");
    const auto output = fixture.graph.add_output_node(1, "Output");
    REQUIRE(fixture.graph.connect(input, 0, output, 0));
    REQUIRE(fixture.graph.prepare(48000.0, 16));

    auto edit = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->disconnect(input, 0, output, 0));
    REQUIRE(register_builtin_sample_region_types(*edit));
    const auto region_input = edit->add_custom_node("pulp.core.sample-region.input");
    const auto add = edit->add_custom_node("pulp.core.sample-region.add");
    const auto multiply = edit->add_custom_node("pulp.core.sample-region.multiply");
    const auto delay = edit->add_custom_node("pulp.core.unit-delay");
    const auto region_output = edit->add_custom_node("pulp.core.sample-region.output");
    const auto parameter = edit->add_custom_node("pulp.core.sample-region.parameter");
    REQUIRE((region_input && add && multiply && delay && region_output && parameter));
    REQUIRE(edit->connect(input, 0, region_input, 0));
    REQUIRE(edit->connect(region_output, 0, output, 0));
    if (extra_region_output_connections != 0) {
        const auto extra_output = edit->add_output_node(
            static_cast<int>(extra_region_output_connections), "Extra Region Outputs");
        REQUIRE(extra_output != 0);
        for (std::size_t port = 0; port < extra_region_output_connections; ++port) {
            REQUIRE(edit->connect(region_output, 0, extra_output, static_cast<PortIndex>(port)));
        }
    }

    constexpr pulp::state::ParamID kParam = 771;
    SampleRegionDefinition definition;
    definition.region_id = 771;
    definition.members = {
        {region_input,
         "pulp.core.sample-region.input",
         1,
         {SampleKernelConfigKind::BoundaryIndex, 0, 0.0f}},
        {add, "pulp.core.sample-region.add", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
        {multiply, "pulp.core.sample-region.multiply", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
        {delay, "pulp.core.unit-delay", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
        {region_output,
         "pulp.core.sample-region.output",
         1,
         {SampleKernelConfigKind::BoundaryIndex, 0, 0.0f}},
        {parameter,
         "pulp.core.sample-region.parameter",
         1,
         {SampleKernelConfigKind::PromotedParameterId, kParam, 0.0f}},
    };
    definition.input_boundaries = {region_input};
    definition.output_boundaries = {region_output};
    SampleRegionPromotedParameter promoted;
    promoted.param_id = kParam;
    promoted.key = "amount";
    promoted.name = "Amount";
    promoted.unit = "ratio";
    promoted.range = pulp::state::ParamRange::linear(0.0f, 1.0f, 0.5f);
    promoted.bound_node_id = parameter;
    definition.promoted_parameters = {promoted};
    definition.limits.max_member_nodes = 63;
    definition.limits.max_internal_connections = 124;
    definition.limits.max_input_boundaries = 7;
    definition.limits.max_output_boundaries = 7;
    definition.limits.max_delay_nodes = 31;
    definition.limits.max_promoted_parameters = 15;
    definition.limits.max_state_bytes = 127;
    definition.limits.max_logical_boundary_bytes = 1'048'575;
    definition.limits.max_work_per_frame = 190;
    definition.limits.max_work_per_block = 3'129'343;
    REQUIRE(edit->declare_sample_region(definition).accepted);
    REQUIRE(edit->connect_in_sample_region(771, region_input, 0, add, 0).accepted);
    REQUIRE(edit->connect_in_sample_region(771, delay, 0, multiply, 0).accepted);
    REQUIRE(edit->connect_in_sample_region(771, parameter, 0, multiply, 1).accepted);
    REQUIRE(edit->connect_in_sample_region(771, multiply, 0, add, 1).accepted);
    REQUIRE(edit->connect_in_sample_region(771, add, 0, delay, 0).accepted);
    REQUIRE(edit->connect_in_sample_region(771, delay, 0, region_output, 0).accepted);
    REQUIRE(edit->prove_sample_region(definition.region_id).accepted);
    fixture.parameters =
        SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
    REQUIRE(fixture.parameters);
    REQUIRE(edit->bind_sample_region_parameters(fixture.parameters->binding()).accepted);
    REQUIRE(edit->prepare(48000.0, 16) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
}

CustomNodeType make_custom_node_type(std::string type_id,
                                     int version,
                                     int inputs,
                                     int outputs,
                                     std::string name) {
    CustomNodeType type;
    type.type_id = std::move(type_id);
    type.version = version;
    type.num_input_ports = inputs;
    type.num_output_ports = outputs;
    type.default_name = std::move(name);
    return type;
}

} // namespace

TEST_CASE("GraphSerializer round-trips an empty graph", "[host][serializer]") {
    SignalGraph src;
    const auto json = GraphSerializer::to_json(src);
    REQUIRE_FALSE(json.empty());

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(result.missing_plugins.empty());
    REQUIRE(dst.nodes().empty());
    REQUIRE(dst.connections().empty());

    // The no-region form is deliberately still v2 and must remain byte stable.
    REQUIRE(json.find("\"format_version\": 2") != std::string::npos);
    REQUIRE(json.find("sample_regions") == std::string::npos);
    REQUIRE(GraphSerializer::to_json(dst) == json);
}

TEST_CASE("GraphSerializer persists an executable sample region",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    const auto json = GraphSerializer::to_json(fixture.graph);
    REQUIRE(json.find("\"format_version\": 3") != std::string::npos);
    REQUIRE(json.find("sample_regions") != std::string::npos);
    REQUIRE(json.find("\"promoted_parameters\"") != std::string::npos);

    SignalGraph loaded;
    REQUIRE(register_builtin_sample_region_types(loaded));
    const auto result = GraphSerializer::from_json(loaded, json);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(loaded.sample_regions().size() == 1);
    REQUIRE(loaded.sample_regions().front().members.size() ==
            fixture.graph.sample_regions().front().members.size());
    REQUIRE(loaded.sample_regions().front().promoted_parameters.size() == 1);
    REQUIRE(loaded.sample_regions().front().promoted_parameters.front().key == "amount");
    REQUIRE(loaded.sample_regions().front().limits.max_work_per_block == 3'129'343);
    REQUIRE(GraphSerializer::to_json(loaded) == json);

    auto edit = loaded.begin_prepared_topology_edit();
    REQUIRE(edit);
    auto owner = SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
    REQUIRE(owner);
    REQUIRE(edit->bind_sample_region_parameters(owner->binding()).accepted);
    REQUIRE(edit->prepare(48000.0, 16) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    REQUIRE(loaded.prove_sample_region(771).accepted);
}

TEST_CASE("GraphSerializer preserves unresolved region type versions and refuses futures",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    auto json = GraphSerializer::to_json(fixture.graph);
    auto rewrite_one = [](std::string& text, std::size_t start) {
        const auto type_pos = text.find("pulp.core.unit-delay", start);
        REQUIRE(type_pos != std::string::npos);
        text.replace(type_pos, std::string("pulp.core.unit-delay").size(),
                     "pulp.future.unit-delay");
        const auto version_pos = text.find("\"version\": 1", type_pos);
        REQUIRE(version_pos != std::string::npos);
        text.replace(version_pos, std::string("\"version\": 1").size(), "\"version\": 77");
        return version_pos;
    };
    const auto first_version = rewrite_one(json, 0);
    rewrite_one(json, first_version + 1);

    SignalGraph unresolved;
    REQUIRE(register_builtin_sample_region_types(unresolved));
    const auto loaded = GraphSerializer::from_json(unresolved, json);
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    REQUIRE_FALSE(loaded.missing_custom_node_types.empty());
    REQUIRE(GraphSerializer::to_json(unresolved) == json);
    REQUIRE(unresolved.prove_sample_region(771).reason ==
            SampleRegionRefusalReason::UnresolvedSampleKernel);
    REQUIRE_FALSE(unresolved.prepare(48000.0, 16));

    auto future = GraphSerializer::to_json(fixture.graph);
    const auto version_pos = future.find("\"format_version\": 3");
    REQUIRE(version_pos != std::string::npos);
    future.replace(version_pos, std::string("\"format_version\": 3").size(),
                   "\"format_version\": 4");
    SignalGraph rejected;
    const auto refusal = GraphSerializer::from_json(rejected, future);
    REQUIRE_FALSE(refusal.ok);
    REQUIRE(refusal.error.find("unsupported graph format_version 4") != std::string::npos);
    REQUIRE(rejected.nodes().empty());
}

TEST_CASE("GraphSerializer validates unresolved region topology",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    const auto definition = fixture.graph.sample_regions().front();
    const auto input_boundary = definition.input_boundaries.front();
    const auto ordinary =
        std::find_if(definition.members.begin(), definition.members.end(), [](const auto& member) {
            return member.type_id == "pulp.core.sample-region.add";
        })->node;

    auto unresolved_json = GraphSerializer::to_json(fixture.graph);
    std::size_t type = 0;
    while ((type = unresolved_json.find("pulp.core.unit-delay", type)) != std::string::npos) {
        unresolved_json.replace(type, std::string("pulp.core.unit-delay").size(),
                                "pulp.future.unit-delay");
        const auto version = unresolved_json.find("\"version\": 1", type);
        REQUIRE(version != std::string::npos);
        unresolved_json.replace(version, std::string("\"version\": 1").size(), "\"version\": 77");
        type = version + 1;
    }
    const auto require_topology_rejected = [](const std::string& json) {
        SignalGraph loaded;
        REQUIRE(register_builtin_sample_region_types(loaded));
        const auto result = GraphSerializer::from_json(loaded, json);
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error == "invalid persisted sample region topology");
        REQUIRE(loaded.nodes().empty());
    };

    SECTION("exterior edge enters an ordinary member") {
        auto json = unresolved_json;
        const auto boundary = "\"dest_node\": " + std::to_string(input_boundary);
        const auto pos = json.find(boundary);
        REQUIRE(pos != std::string::npos);
        json.replace(pos, boundary.size(), "\"dest_node\": " + std::to_string(ordinary));
        require_topology_rejected(json);
    }

    SECTION("internal connection count exceeds the authored limit") {
        auto json = unresolved_json;
        const auto limit = json.find("\"max_internal_connections\": 124");
        REQUIRE(limit != std::string::npos);
        json.replace(limit, std::string("\"max_internal_connections\": 124").size(),
                     "\"max_internal_connections\": 1");
        require_topology_rejected(json);
    }

    SECTION("zero-input source is disconnected from every output") {
        const auto parameter =
            std::find_if(definition.members.begin(), definition.members.end(),
                         [](const auto& member) {
                             return member.type_id == "pulp.core.sample-region.parameter";
                         })
                ->node;
        auto json = unresolved_json;
        const auto source = "\"source_node\": " + std::to_string(parameter);
        const auto pos = json.find(source);
        REQUIRE(pos != std::string::npos);
        json.replace(pos, source.size(), "\"source_node\": " + std::to_string(input_boundary));
        require_topology_rejected(json);
    }
}

TEST_CASE("GraphSerializer rejects invalid unrelated connections in a region graph",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    const auto unrelated_source = fixture.graph.add_input_node(1, "Unrelated Source");
    const auto unrelated_destination = fixture.graph.add_output_node(1, "Unrelated Destination");
    REQUIRE(fixture.graph.connect(unrelated_source, 0, unrelated_destination, 0));

    auto json = GraphSerializer::to_json(fixture.graph);
    const auto source = json.find("\"source_node\": " + std::to_string(unrelated_source));
    REQUIRE(source != std::string::npos);
    const auto port = json.find("\"source_port\": 0", source);
    REQUIRE(port != std::string::npos);
    json.replace(port, std::string("\"source_port\": 0").size(), "\"source_port\": 99");

    SignalGraph loaded;
    REQUIRE(register_builtin_sample_region_types(loaded));
    const auto result = GraphSerializer::from_json(loaded, json);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error == "connection failed validation");
    REQUIRE(loaded.nodes().empty());
    REQUIRE(loaded.connections().empty());
}

TEST_CASE("GraphSerializer installs normalized unrelated MIDI ports in a region graph",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    const auto midi_input = fixture.graph.add_midi_input_node();
    const auto midi_output = fixture.graph.add_midi_output_node();
    REQUIRE(fixture.graph.connect_midi(midi_input, midi_output));

    auto json = GraphSerializer::to_json(fixture.graph);
    const auto source = json.find("\"source_node\": " + std::to_string(midi_input));
    REQUIRE(source != std::string::npos);
    const auto source_port = json.find("\"source_port\": 0", source);
    const auto dest_port = json.find("\"dest_port\": 0", source_port);
    REQUIRE((source_port != std::string::npos && dest_port != std::string::npos));
    json.replace(source_port, std::string("\"source_port\": 0").size(), "\"source_port\": 7");
    json.replace(dest_port, std::string("\"dest_port\": 0").size(), "\"dest_port\": 9");

    SignalGraph loaded;
    REQUIRE(register_builtin_sample_region_types(loaded));
    const auto result = GraphSerializer::from_json(loaded, json);
    INFO(result.error);
    REQUIRE(result.ok);
    const auto midi = std::find_if(loaded.connections().begin(), loaded.connections().end(),
                                   [](const auto& connection) { return connection.midi; });
    REQUIRE(midi != loaded.connections().end());
    REQUIRE(midi->source_port == 0);
    REQUIRE(midi->dest_port == 0);
}

TEST_CASE("GraphSerializer rejects invalid persisted region metadata",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    const auto definitions = fixture.graph.sample_regions();
    const auto& definition = definitions.front();
    const auto input = definition.input_boundaries.front();
    const auto non_input =
        std::find_if(definition.members.begin(), definition.members.end(), [&](const auto& member) {
            return member.node != input && member.type_id != "pulp.core.sample-region.input";
        })->node;
    const auto original = GraphSerializer::to_json(fixture.graph);

    const auto replace_boundary_list = [&](std::string& json, const std::string& replacement) {
        const auto key = json.find("\"input_boundaries\"");
        REQUIRE(key != std::string::npos);
        const auto begin = json.find('[', key);
        const auto end = json.find(']', begin);
        REQUIRE((begin != std::string::npos && end != std::string::npos));
        json.replace(begin + 1, end - begin - 1, replacement);
    };
    const auto require_rejected = [&](const std::string& json) {
        SignalGraph loaded;
        REQUIRE(register_builtin_sample_region_types(loaded));
        const auto result = GraphSerializer::from_json(loaded, json);
        REQUIRE_FALSE(result.ok);
        REQUIRE_FALSE(result.error.empty());
        REQUIRE(loaded.nodes().empty());
    };

    SECTION("duplicate boundary") {
        auto json = original;
        replace_boundary_list(json, std::to_string(input) + "," + std::to_string(input));
        require_rejected(json);
    }
    SECTION("zero member identity") {
        auto json = original;
        const auto regions = json.find("\"sample_regions\"");
        const auto member =
            json.find("\"node\": " + std::to_string(definition.members.front().node), regions);
        REQUIRE(member != std::string::npos);
        json.replace(
            member,
            std::string("\"node\": " + std::to_string(definition.members.front().node)).size(),
            "\"node\": 0");
        require_rejected(json);
    }
    SECTION("zero serialized node identity") {
        auto json = original;
        const auto node = json.find("\"id\": " + std::to_string(definition.members.front().node));
        REQUIRE(node != std::string::npos);
        json.replace(
            node, std::string("\"id\": " + std::to_string(definition.members.front().node)).size(),
            "\"id\": 0");
        require_rejected(json);
    }
    SECTION("zero connection endpoint") {
        auto json = original;
        const auto source = json.find("\"source_node\": ");
        REQUIRE(source != std::string::npos);
        const auto value = json.find(':', source);
        const auto end = json.find(',', value);
        REQUIRE((value != std::string::npos && end != std::string::npos));
        json.replace(value + 1, end - value - 1, " 0");
        require_rejected(json);
    }
    SECTION("zero boundary identity") {
        auto json = original;
        replace_boundary_list(json, "0");
        require_rejected(json);
    }
    SECTION("zero promoted binding identity") {
        auto json = original;
        const auto promoted = json.find("\"promoted_parameters\"");
        const auto binding =
            json.find("\"bound_node\": " +
                          std::to_string(definition.promoted_parameters.front().bound_node_id),
                      promoted);
        REQUIRE(binding != std::string::npos);
        const auto value = json.find(':', binding);
        const auto end = json.find(',', value);
        REQUIRE((value != std::string::npos && end != std::string::npos));
        json.replace(value + 1, end - value - 1, " 0");
        require_rejected(json);
    }
    SECTION("boundary names the wrong member type") {
        auto json = original;
        replace_boundary_list(json, std::to_string(non_input));
        require_rejected(json);
    }
    SECTION("duplicate promoted parameter id") {
        auto json = original;
        const auto key = json.find("\"promoted_parameters\"");
        REQUIRE(key != std::string::npos);
        const auto object_begin = json.find('{', key);
        const auto object_end = json.find('}', object_begin);
        REQUIRE((object_begin != std::string::npos && object_end != std::string::npos));
        const auto parameter = json.substr(object_begin, object_end - object_begin + 1);
        json.insert(object_end + 1, "," + parameter);
        require_rejected(json);
    }
    SECTION("authored limits exceed v1") {
        auto json = original;
        const auto limit = json.find("\"max_member_nodes\": 63");
        REQUIRE(limit != std::string::npos);
        json.replace(limit, std::string("\"max_member_nodes\": 63").size(),
                     "\"max_member_nodes\": 65");
        require_rejected(json);
    }
}

TEST_CASE("GraphSerializer rejects fully resolved region proof failures",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    const auto original = GraphSerializer::to_json(fixture.graph);
    const auto require_region_rejected = [&](const std::string& json) {
        SignalGraph loaded;
        REQUIRE(register_builtin_sample_region_types(loaded));
        const auto result = GraphSerializer::from_json(loaded, json);
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error.find("sample region") != std::string::npos);
        REQUIRE(loaded.nodes().empty());
    };

    SECTION("instantaneous cycle") {
        auto json = original;
        const auto first_type = json.find("pulp.core.unit-delay");
        REQUIRE(first_type != std::string::npos);
        const auto input_count = json.rfind("\"num_input_ports\": 1", first_type);
        REQUIRE(input_count != std::string::npos);
        json.replace(input_count, std::string("\"num_input_ports\": 1").size(),
                     "\"num_input_ports\": 2");
        std::size_t type = 0;
        while ((type = json.find("pulp.core.unit-delay", type)) != std::string::npos) {
            json.replace(type, std::string("pulp.core.unit-delay").size(),
                         "pulp.core.sample-region.add");
            type += std::string("pulp.core.sample-region.add").size();
        }
        require_region_rejected(json);
    }

    SECTION("authored internal connection limit") {
        auto json = original;
        const auto limit = json.find("\"max_internal_connections\": 124");
        REQUIRE(limit != std::string::npos);
        json.replace(limit, std::string("\"max_internal_connections\": 124").size(),
                     "\"max_internal_connections\": 1");
        require_region_rejected(json);
    }

    SECTION("registered scalar kernel outside every region") {
        REQUIRE(fixture.graph.add_custom_node("pulp.core.sample-region.add") != 0);
        const auto json = GraphSerializer::to_json(fixture.graph);
        REQUIRE_FALSE(json.empty());
        require_region_rejected(json);
    }
}

TEST_CASE("GraphSerializer rejects non-boolean optional connection flags in a region graph",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    const auto original = GraphSerializer::to_json(fixture.graph);

    for (const std::string field : {"audio_rate_modulation", "sidechain"}) {
        DYNAMIC_SECTION(field) {
            auto json = original;
            const auto flag = json.find("\"" + field + "\": false");
            REQUIRE(flag != std::string::npos);
            json.replace(flag, std::string("\"" + field + "\": false").size(),
                         "\"" + field + "\": 1");

            SignalGraph loaded;
            REQUIRE(register_builtin_sample_region_types(loaded));
            const auto result = GraphSerializer::from_json(loaded, json);
            REQUIRE_FALSE(result.ok);
            REQUIRE(result.error == "invalid connection flags");
            REQUIRE(loaded.nodes().empty());
        }
    }
}

TEST_CASE("GraphSerializer drops unresolved plugin automation from a region graph",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);
    const auto source = fixture.graph.add_input_node(1, "Automation Source");
    auto info = make_fake_plugin_info("Missing Automation", "pulp.test.region.missing-auto",
                                      PluginFormat::CLAP, 1, 1);
    const auto plugin = fixture.graph.add_plugin_node(std::make_unique<SerializerSlot>(info), 1, 1,
                                                      "Missing Automation");
    REQUIRE(
        fixture.graph.connect_automation(source, 0, plugin, SerializerSlot::kParamId, -1.0f, 1.0f));
    const auto json = GraphSerializer::to_json(fixture.graph);

    SECTION("valid source preserves partial load") {
        SignalGraph loaded;
        REQUIRE(register_builtin_sample_region_types(loaded));
        const auto result = GraphSerializer::from_json(loaded, json);
        INFO(result.error);
        REQUIRE(result.ok);
        REQUIRE(missing_plugins_contain(result, "clap:pulp.test.region.missing-auto"));
        REQUIRE(loaded.sample_regions().size() == 1);
        REQUIRE(std::none_of(loaded.connections().begin(), loaded.connections().end(),
                             [](const auto& connection) {
                                 return connection.automation || connection.audio_rate_modulation;
                             }));
    }

    SECTION("invalid source port remains fatal") {
        auto malformed = json;
        const auto edge = malformed.find("\"source_node\": " + std::to_string(source));
        REQUIRE(edge != std::string::npos);
        const auto port = malformed.find("\"source_port\": 0", edge);
        REQUIRE(port != std::string::npos);
        malformed.replace(port, std::string("\"source_port\": 0").size(), "\"source_port\": 99");

        SignalGraph loaded;
        REQUIRE(register_builtin_sample_region_types(loaded));
        const auto result = GraphSerializer::from_json(loaded, malformed);
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error == "connection failed validation");
        REQUIRE(loaded.nodes().empty());
    }
}

TEST_CASE("GraphSerializer refuses region graphs above its persisted connection ceiling",
          "[host][serializer][sample-region][p2]") {
    PersistedRegionFixture fixture;
    make_persisted_region(fixture);

    constexpr std::size_t kPersistedConnectionLimit = 2'048;
    REQUIRE(fixture.graph.connections().size() < kPersistedConnectionLimit);
    const auto remaining = kPersistedConnectionLimit - fixture.graph.connections().size();
    const auto source =
        fixture.graph.add_input_node(static_cast<int>(remaining + 1), "Ceiling Source");
    const auto destination =
        fixture.graph.add_output_node(static_cast<int>(remaining + 1), "Ceiling Destination");
    for (std::size_t port = 0; port < remaining; ++port) {
        REQUIRE(fixture.graph.connect(source, static_cast<PortIndex>(port), destination,
                                      static_cast<PortIndex>(port)));
    }

    REQUIRE(fixture.graph.connections().size() == kPersistedConnectionLimit);
    REQUIRE_FALSE(GraphSerializer::to_json(fixture.graph).empty());
    REQUIRE(fixture.graph.connect(source, static_cast<PortIndex>(remaining), destination,
                                  static_cast<PortIndex>(remaining)));
    REQUIRE(fixture.graph.connections().size() == kPersistedConnectionLimit + 1);
    REQUIRE(GraphSerializer::to_json(fixture.graph).empty());
}

TEST_CASE("GraphSerializer refuses region graphs above the per-region parser ceiling",
          "[host][serializer][sample-region][p2]") {
    const auto touching_connections = [](const PersistedRegionFixture& fixture) {
        const auto definitions = fixture.graph.sample_regions();
        const auto& region = definitions.front();
        const auto touches_region = [&](const auto& connection) {
            const auto touches = [&](NodeId id) {
                return std::any_of(region.members.begin(), region.members.end(),
                                   [&](const auto& member) { return member.node == id; });
            };
            return touches(connection.source_node) || touches(connection.dest_node);
        };
        return static_cast<std::size_t>(std::count_if(fixture.graph.connections().begin(),
                                                      fixture.graph.connections().end(),
                                                      touches_region));
    };

    PersistedRegionFixture baseline;
    make_persisted_region(baseline);
    const auto touching = touching_connections(baseline);
    constexpr std::size_t kPerRegionConnectionLimit = 128;
    REQUIRE(touching < kPerRegionConnectionLimit);

    PersistedRegionFixture at_limit;
    make_persisted_region(at_limit, kPerRegionConnectionLimit - touching);
    REQUIRE(touching_connections(at_limit) == kPerRegionConnectionLimit);
    REQUIRE_FALSE(GraphSerializer::to_json(at_limit.graph).empty());

    PersistedRegionFixture above_limit;
    make_persisted_region(above_limit, kPerRegionConnectionLimit - touching + 1);
    REQUIRE(touching_connections(above_limit) == kPerRegionConnectionLimit + 1);
    REQUIRE(GraphSerializer::to_json(above_limit.graph).empty());
}

TEST_CASE("GraphSerializer round-trips topology with gain and I/O nodes", "[host][serializer]") {
    SignalGraph src;
    auto input = src.add_input_node(2, "Input");
    auto gain = src.add_gain_node("Gain");
    auto output = src.add_output_node(2, "Output");
    REQUIRE(src.connect(input, 0, gain, 0));
    REQUIRE(src.connect(gain, 0, output, 0));

    const auto json = GraphSerializer::to_json(src);
    REQUIRE_FALSE(json.empty());

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.empty());

    REQUIRE(dst.nodes().size() == 3);
    REQUIRE(dst.connections().size() == 2);

    // Node types preserved
    const auto& dst_nodes = dst.nodes();
    std::array<int, 7> type_counts{};
    for (const auto& n : dst_nodes) type_counts[static_cast<int>(n.type)]++;
    REQUIRE(type_counts[static_cast<int>(NodeType::AudioInput)] == 1);
    REQUIRE(type_counts[static_cast<int>(NodeType::Gain)] == 1);
    REQUIRE(type_counts[static_cast<int>(NodeType::AudioOutput)] == 1);
}

TEST_CASE("GraphSerializer reports missing plugins when they can't re-resolve",
          "[host][serializer]") {
    SignalGraph src;
    auto input = src.add_input_node(2);
    auto plugin = src.add_plugin_node(make_fake_plugin_info("Ghost", "com.pulp.test.ghost"));
    auto output = src.add_output_node(2);
    REQUIRE(src.connect(input, 0, plugin, 0));
    REQUIRE(src.connect(plugin, 0, output, 0));

    const auto json = GraphSerializer::to_json(src);

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    // Loading succeeds even though the fake plugin can't be resolved.
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.missing_plugins.empty());

    // The missing-plugin entry should reference the identity tuple we
    // serialized (name or unique_id, depending on serializer formatting).
    bool mentions_ghost = false;
    for (const auto& entry : result.missing_plugins) {
        if (entry.find("Ghost") != std::string::npos ||
            entry.find("com.pulp.test.ghost") != std::string::npos) {
            mentions_ghost = true;
            break;
        }
    }
    REQUIRE(mentions_ghost);

    // The plugin node still exists in the loaded graph, just with a null
    // PluginSlot — topology survives resolution failure.
    int plugin_node_count = 0;
    for (const auto& n : dst.nodes()) {
        if (n.type == NodeType::Plugin) plugin_node_count++;
    }
    REQUIRE(plugin_node_count == 1);
}

TEST_CASE("GraphSerializer round-trips editor layout", "[host][serializer]") {
    SignalGraph src;
    auto input = src.add_input_node(2);
    auto gain = src.add_gain_node("Mid");
    auto output = src.add_output_node(2);

    std::unordered_map<NodeId, std::pair<float, float>> layout;
    layout[input] = {10.0f, 20.0f};
    layout[gain] = {100.5f, 200.25f};
    layout[output] = {300.0f, 400.0f};

    const auto json = GraphSerializer::to_json(src, layout);

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(result.editor_layout.size() == 3);

    // NodeIds are assigned by the destination graph's own next_id_, so
    // they may not match the source's IDs. Verify by matching values.
    bool saw_input_pos = false, saw_gain_pos = false, saw_output_pos = false;
    for (const auto& [id, pos] : result.editor_layout) {
        if (pos.first == 10.0f && pos.second == 20.0f) saw_input_pos = true;
        if (pos.first == 100.5f && pos.second == 200.25f) saw_gain_pos = true;
        if (pos.first == 300.0f && pos.second == 400.0f) saw_output_pos = true;
    }
    REQUIRE(saw_input_pos);
    REQUIRE(saw_gain_pos);
    REQUIRE(saw_output_pos);
}

TEST_CASE("GraphSerializer surfaces malformed JSON as a clean error",
          "[host][serializer]") {
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, "{ this is not json }");
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("GraphSerializer rejects non-object roots and malformed field types",
          "[host][serializer][issue-643]") {
    SignalGraph root_dst;
    auto root_result = GraphSerializer::from_json(root_dst, "[]");
    REQUIRE_FALSE(root_result.ok);
    REQUIRE(root_result.error == "root is not an object");
    REQUIRE(root_dst.nodes().empty());

    SignalGraph field_dst;
    auto field_result = GraphSerializer::from_json(field_dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": "not-an-integer",
      "type": "gain",
      "name": "Bad",
      "num_input_ports": 2,
      "num_output_ports": 2
    }
  ],
  "connections": []
})");
    REQUIRE_FALSE(field_result.ok);
    REQUIRE(field_result.error.find("field deserialization failed") != std::string::npos);
    REQUIRE(field_dst.nodes().empty());
}

TEST_CASE("GraphSerializer fails closed before loading missing or future graph versions",
          "[host][serializer][migration]") {
    SignalGraph current_src;
    const auto current_json = GraphSerializer::to_json(current_src);
    REQUIRE(current_json.find("\"format_version\": 2") != std::string::npos);

    SignalGraph v1_graph;
    auto v1_result = GraphSerializer::from_json(v1_graph, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "audio_in",
      "name": "Input",
      "num_input_ports": 0,
      "num_output_ports": 1,
      "gain": 1
    },
    {
      "id": 2,
      "type": "audio_out",
      "name": "Output",
      "num_input_ports": 1,
      "num_output_ports": 0,
      "gain": 1
    }
  ],
  "connections": [
    {
      "source_node": 1,
      "source_port": 0,
      "dest_node": 2,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    }
  ]
})");
    REQUIRE(v1_result.ok);
    REQUIRE(v1_graph.nodes().size() == 2);
    REQUIRE(v1_graph.connections().size() == 1);

    SignalGraph missing_version;
    auto missing_result = GraphSerializer::from_json(missing_version, R"({
  "nodes": [
    {
      "id": 1,
      "type": "gain",
      "name": "Missing Version",
      "num_input_ports": 2,
      "num_output_ports": 2
    }
  ],
  "connections": []
})");
    REQUIRE_FALSE(missing_result.ok);
    REQUIRE(missing_result.error.find("missing graph format_version")
            != std::string::npos);
    REQUIRE(missing_version.nodes().empty());

    SignalGraph future_version;
    auto future_result = GraphSerializer::from_json(future_version, R"({
  "format_version": 4,
  "nodes": [
    {
      "id": 1,
      "type": "gain",
      "name": "Future Version",
      "num_input_ports": 2,
      "num_output_ports": 2
    }
  ],
  "connections": []
})");
    REQUIRE_FALSE(future_result.ok);
    REQUIRE(future_result.error.find("unsupported graph format_version 4") != std::string::npos);
    REQUIRE(future_version.nodes().empty());
}

TEST_CASE("GraphSerializer dispatches graph format migrations before materializing nodes",
          "[host][serializer][migration]") {
    const std::string versioned_json = R"({
  "format_version": 0,
  "nodes": [
    {
      "id": 7,
      "type": "gain",
      "name": "Migrated Gain",
      "num_input_ports": 2,
      "num_output_ports": 2,
      "gain": 0.5
    }
  ],
  "connections": []
})";

    SignalGraph rejected;
    auto rejected_result = GraphSerializer::from_json(rejected, versioned_json);
    REQUIRE_FALSE(rejected_result.ok);
    REQUIRE(rejected_result.error.find("unsupported graph format_version 0")
            != std::string::npos);
    REQUIRE(rejected.nodes().empty());

    REQUIRE(GraphSerializer::register_migration(
        0, GraphSerializer::current_format_version(),
        [](const std::string& source_json, std::string& migrated_json) {
            migrated_json = source_json;
            const auto pos = migrated_json.find("\"format_version\": 0");
            if (pos == std::string::npos) return false;
            migrated_json.replace(pos, std::string("\"format_version\": 0").size(),
                                  "\"format_version\": 3");
            return true;
        }));

    SignalGraph migrated;
    auto result = GraphSerializer::from_json(migrated, versioned_json);
    REQUIRE(result.ok);
    REQUIRE(migrated.nodes().size() == 1);
    REQUIRE(migrated.nodes().front().name == "Migrated Gain");
}

TEST_CASE("GraphSerializer rejects graph migrations that do not advance versions",
          "[host][serializer][migration]") {
    const std::string versioned_json = R"({
  "format_version": -1,
  "nodes": [],
  "connections": []
})";

    REQUIRE(GraphSerializer::register_migration(
        -1, GraphSerializer::current_format_version(),
        [](const std::string& source_json, std::string& migrated_json) {
            migrated_json = source_json;
            return true;
        }));

    SignalGraph graph;
    auto result = GraphSerializer::from_json(graph, versioned_json);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("expected format_version") != std::string::npos);
}

TEST_CASE("GraphSerializer rejects invalid graph migration registrations",
          "[host][serializer][migration]") {
    REQUIRE(GraphSerializer::current_format_version() == 3);
    REQUIRE_FALSE(GraphSerializer::register_migration(
        2, 2,
        [](const std::string& source_json, std::string& migrated_json) {
            migrated_json = source_json;
            return true;
        }));
    REQUIRE_FALSE(GraphSerializer::register_migration(
        0, GraphSerializer::current_format_version() + 1,
        [](const std::string& source_json, std::string& migrated_json) {
            migrated_json = source_json;
            return true;
        }));
    REQUIRE_FALSE(GraphSerializer::register_migration(
        -10, GraphSerializer::current_format_version(), {}));
    REQUIRE_FALSE(GraphSerializer::register_migration(
        1, GraphSerializer::current_format_version(),
        [](const std::string& source_json, std::string& migrated_json) {
            migrated_json = source_json;
            return true;
        }));
}

TEST_CASE("GraphSerializer rejects non-integer and out-of-range graph versions",
          "[host][serializer][migration]") {
    SignalGraph string_version;
    auto string_result = GraphSerializer::from_json(string_version, R"({
  "format_version": "2",
  "nodes": [],
  "connections": []
})");
    REQUIRE_FALSE(string_result.ok);
    REQUIRE(string_result.error == "format_version is not an integer");
    REQUIRE(string_version.nodes().empty());

    SignalGraph overflow_version;
    auto overflow_result = GraphSerializer::from_json(overflow_version, R"({
  "format_version": 2147483648,
  "nodes": [],
  "connections": []
})");
    REQUIRE_FALSE(overflow_result.ok);
    REQUIRE(overflow_result.error == "format_version is not an integer");
    REQUIRE(overflow_version.nodes().empty());
}

TEST_CASE("GraphSerializer reports broken graph migration outputs",
          "[host][serializer][migration]") {
    REQUIRE(GraphSerializer::register_migration(
        -20, GraphSerializer::current_format_version(),
        [](const std::string&, std::string&) {
            return true;
        }));
    SignalGraph empty_output;
    auto empty_result = GraphSerializer::from_json(empty_output, R"({
  "format_version": -20,
  "nodes": [],
  "connections": []
})");
    REQUIRE_FALSE(empty_result.ok);
    REQUIRE(empty_result.error == "graph migration failed");

    REQUIRE(GraphSerializer::register_migration(
        -21, GraphSerializer::current_format_version(),
        [](const std::string&, std::string& migrated_json) {
            migrated_json = "{ not json }";
            return true;
        }));
    SignalGraph invalid_json;
    auto invalid_result = GraphSerializer::from_json(invalid_json, R"({
  "format_version": -21,
  "nodes": [],
  "connections": []
})");
    REQUIRE_FALSE(invalid_result.ok);
    REQUIRE(invalid_result.error.find("graph migration produced invalid JSON")
            != std::string::npos);

    REQUIRE(GraphSerializer::register_migration(
        -22, GraphSerializer::current_format_version(),
        [](const std::string&, std::string& migrated_json) {
            migrated_json = "[]";
            return true;
        }));
    SignalGraph array_output;
    auto array_result = GraphSerializer::from_json(array_output, R"({
  "format_version": -22,
  "nodes": [],
  "connections": []
})");
    REQUIRE_FALSE(array_result.ok);
    REQUIRE(array_result.error == "graph migration did not produce expected format_version");

    REQUIRE(GraphSerializer::register_migration(
        -23, GraphSerializer::current_format_version(),
        [](const std::string&, std::string& migrated_json) {
            migrated_json = R"({"format_version": 1, "nodes": [], "connections": []})";
            return true;
        }));
    SignalGraph wrong_version;
    auto wrong_result = GraphSerializer::from_json(wrong_version, R"({
  "format_version": -23,
  "nodes": [],
  "connections": []
})");
    REQUIRE_FALSE(wrong_result.ok);
    REQUIRE(wrong_result.error == "graph migration did not produce expected format_version");
}

TEST_CASE("GraphSerializer clears partially loaded graphs after connection field errors",
          "[host][serializer][issue-493]") {
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "audio_in",
      "name": "Input",
      "num_input_ports": 0,
      "num_output_ports": 1,
      "gain": 1
    },
    {
      "id": 2,
      "type": "audio_out",
      "name": "Output",
      "num_input_ports": 1,
      "num_output_ports": 0,
      "gain": 1
    }
  ],
  "connections": [
    {
      "source_node": "not-an-integer",
      "source_port": 0,
      "dest_node": 2,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    }
  ]
})");

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("invalid source_node") != std::string::npos);
    REQUIRE(dst.nodes().empty());
    REQUIRE(dst.connections().empty());
}

TEST_CASE("GraphSerializer tolerates missing arrays and skips stale connection ids",
          "[host][serializer][issue-493]") {
    SignalGraph empty;
    auto empty_result = GraphSerializer::from_json(empty, R"({
  "format_version": 1
})");
    REQUIRE(empty_result.ok);
    REQUIRE(empty.nodes().empty());
    REQUIRE(empty.connections().empty());

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 10,
      "type": "audio_in",
      "name": "Input",
      "num_input_ports": 0,
      "num_output_ports": 1,
      "gain": 1
    },
    {
      "id": 11,
      "type": "audio_out",
      "name": "Output",
      "num_input_ports": 1,
      "num_output_ports": 0,
      "gain": 1
    }
  ],
  "connections": [
    {
      "source_node": 99,
      "source_port": 0,
      "dest_node": 11,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 10,
      "source_port": 0,
      "dest_node": 98,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 10,
      "source_port": 0,
      "dest_node": 11,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    }
  ]
})");

    REQUIRE(result.ok);
    REQUIRE(dst.nodes().size() == 2);
    REQUIRE(dst.connections().size() == 1);
    REQUIRE_FALSE(dst.connections().front().feedback);
    REQUIRE_FALSE(dst.connections().front().midi);
    REQUIRE_FALSE(dst.connections().front().automation);
}

TEST_CASE("GraphSerializer generated topology fuzz rejects unsafe imported edges",
          "[host][serializer][generated][fuzz]") {
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 10,
      "type": "audio_in",
      "name": "Input",
      "num_input_ports": 0,
      "num_output_ports": 1,
      "gain": 1
    },
    {
      "id": 11,
      "type": "gain",
      "name": "A",
      "num_input_ports": 2,
      "num_output_ports": 2,
      "gain": 1
    },
    {
      "id": 12,
      "type": "gain",
      "name": "B",
      "num_input_ports": 2,
      "num_output_ports": 2,
      "gain": 1
    },
    {
      "id": 13,
      "type": "audio_out",
      "name": "Output",
      "num_input_ports": 1,
      "num_output_ports": 0,
      "gain": 1
    }
  ],
  "connections": [
    {
      "source_node": 99,
      "source_port": 0,
      "dest_node": 11,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 10,
      "source_port": 0,
      "dest_node": 98,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 10,
      "source_port": 1,
      "dest_node": 11,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 10,
      "source_port": 0,
      "dest_node": 11,
      "dest_port": 99,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 10,
      "source_port": 0,
      "dest_node": 11,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 11,
      "source_port": 0,
      "dest_node": 12,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 12,
      "source_port": 0,
      "dest_node": 11,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 12,
      "source_port": 0,
      "dest_node": 13,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    }
  ]
})");

    REQUIRE(result.ok);
    REQUIRE(dst.nodes().size() == 4);
    REQUIRE(dst.connections().size() == 3);

    const auto* a = find_node_named(dst, "A");
    const auto* b = find_node_named(dst, "B");
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    REQUIRE_FALSE(dst.would_create_cycle(a->id, b->id));
    REQUIRE(dst.would_create_cycle(b->id, a->id));

    for (const auto& connection : dst.connections()) {
        REQUIRE_FALSE((connection.source_node == b->id
                       && connection.dest_node == a->id));
        REQUIRE_FALSE(connection.feedback);
        REQUIRE_FALSE(connection.midi);
        REQUIRE_FALSE(connection.automation);
    }

    REQUIRE(dst.validate_generated_graph(16).accepted);
    REQUIRE(dst.prepare(48000.0, 16));
}

TEST_CASE("GraphSerializer decodes fallback wire values deterministically",
          "[host][serializer][issue-493]") {
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "future_node_type",
      "name": "Future sink",
      "num_input_ports": 1,
      "num_output_ports": 0,
      "gain": 1
    },
    {
      "id": 2,
      "type": "plugin",
      "name": "Future plugin",
      "num_input_ports": 1,
      "num_output_ports": 1,
      "gain": 1,
      "plugin": {
        "format": "future_format",
        "unique_id": "pulp.test.future",
        "name": "Future plugin",
        "manufacturer": "PulpTest",
        "version": "1.0.0",
        "last_path": "/missing/future.plugin"
      }
    }
  ],
  "connections": []
})");

    REQUIRE(result.ok);
    REQUIRE(dst.nodes().size() == 2);
    REQUIRE(missing_custom_types_contain(result, "future_node_type@1"));
    REQUIRE(missing_plugins_contain(result, "lv2:pulp.test.future"));

    bool saw_custom_fallback = false;
    for (const auto& node : dst.nodes()) {
        if (node.name == "Future sink") {
            saw_custom_fallback = true;
            REQUIRE(node.type == NodeType::Custom);
            REQUIRE(node.custom_type_id == "future_node_type");
            REQUIRE(node.custom_type_version == 1);
            REQUIRE(node.num_input_ports == 1);
            REQUIRE(node.num_output_ports == 0);
        }
    }
    REQUIRE(saw_custom_fallback);
}

TEST_CASE("GraphSerializer round-trips registered custom node identity",
          "[host][serializer][node-abi]") {
    SignalGraph src;
    REQUIRE(src.register_custom_node_type(make_custom_node_type(
        "pulp.test.custom-filter", 2, 1, 1, "Custom Filter")));
    auto input = src.add_input_node(1, "Input");
    auto custom = src.add_custom_node("pulp.test.custom-filter", "Filter A");
    auto output = src.add_output_node(1, "Output");
    REQUIRE(custom != 0);
    REQUIRE(src.connect(input, 0, custom, 0));
    REQUIRE(src.connect(custom, 0, output, 0));

    const auto json = GraphSerializer::to_json(src);
    REQUIRE(json.find("\"type\": \"custom\"") != std::string::npos);
    REQUIRE(json.find("\"type_id\": \"pulp.test.custom-filter\"") !=
            std::string::npos);
    REQUIRE(json.find("\"version\": 2") != std::string::npos);

    SignalGraph dst;
    REQUIRE(dst.register_custom_node_type(make_custom_node_type(
        "pulp.test.custom-filter", 2, 1, 1, "Custom Filter")));
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(result.missing_custom_node_types.empty());
    REQUIRE(dst.nodes().size() == 3);
    REQUIRE(dst.connections().size() == 2);

    bool saw_custom = false;
    for (const auto& node : dst.nodes()) {
        if (node.name != "Filter A") continue;
        saw_custom = true;
        REQUIRE(node.type == NodeType::Custom);
        REQUIRE(node.custom_type_id == "pulp.test.custom-filter");
        REQUIRE(node.custom_type_version == 2);
        REQUIRE(node.num_input_ports == 1);
        REQUIRE(node.num_output_ports == 1);
    }
    REQUIRE(saw_custom);
}

namespace {
// A stateful custom type: instance holds a `level` serialized as 4 little-endian
// bytes via save/load_state.
struct SerLevel {
    float level = 1.0f;
};
pulp::host::CustomNodeType make_stateful_level_type(const char* id, int ver) {
    pulp::host::CustomNodeType t;
    t.type_id = id;
    t.version = ver;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.default_name = "Level";
    t.create = []() -> void* { return new SerLevel(); };
    t.destroy = [](void* p) { delete static_cast<SerLevel*>(p); };
    t.process_instance = [](void* p, pulp::audio::BufferView<float>& o,
                            const pulp::audio::BufferView<const float>& i, int n) {
        const float lvl = static_cast<SerLevel*>(p)->level;
        for (int s = 0; s < n; ++s) o.channel_ptr(0)[s] = i.channel_ptr(0)[s] * lvl;
    };
    t.save_state = [](void* p) {
        const float lvl = static_cast<SerLevel*>(p)->level;
        std::vector<uint8_t> b(sizeof(float));
        std::memcpy(b.data(), &lvl, sizeof(float));
        return b;
    };
    t.load_state = [](void* p, const std::vector<uint8_t>& b) {
        if (b.size() != sizeof(float)) return false;
        std::memcpy(&static_cast<SerLevel*>(p)->level, b.data(), sizeof(float));
        return true;
    };
    return t;
}
std::vector<uint8_t> ser_level_bytes(float lvl) {
    std::vector<uint8_t> b(sizeof(float));
    std::memcpy(b.data(), &lvl, sizeof(float));
    return b;
}
}  // namespace

TEST_CASE("GraphSerializer round-trips opaque custom-node state",
          "[host][serializer][node-abi]") {
    SignalGraph src;
    REQUIRE(src.register_custom_node_type(
        make_stateful_level_type("pulp.test.level", 1)));
    auto node = src.add_custom_node("pulp.test.level", "Level A");
    REQUIRE(node != 0);
    REQUIRE(src.set_custom_node_state(node, ser_level_bytes(2.5f)));

    const auto json = GraphSerializer::to_json(src);
    REQUIRE(json.find("\"state_b64\"") != std::string::npos);

    SignalGraph dst;
    REQUIRE(dst.register_custom_node_type(
        make_stateful_level_type("pulp.test.level", 1)));
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(result.missing_custom_node_types.empty());
    REQUIRE(dst.nodes().size() == 1);
    const NodeId dst_node = dst.nodes()[0].id;
    REQUIRE(dst.custom_node_state(dst_node) == ser_level_bytes(2.5f));

    // After prepare(), the live instance carries the restored state.
    REQUIRE(dst.prepare(48000.0, 8));
    REQUIRE(dst.custom_node_state(dst_node) == ser_level_bytes(2.5f));
}

TEST_CASE("GraphSerializer preserves opaque state for unresolved custom nodes",
          "[host][serializer][node-abi]") {
    SignalGraph src;
    REQUIRE(src.register_custom_node_type(
        make_stateful_level_type("pulp.test.level", 1)));
    auto node = src.add_custom_node("pulp.test.level", "Level A");
    REQUIRE(src.set_custom_node_state(node, ser_level_bytes(1.75f)));
    const auto json = GraphSerializer::to_json(src);

    // Load into a graph WITHOUT the type — node becomes an unresolved placeholder.
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.missing_custom_node_types.empty());
    REQUIRE(dst.nodes().size() == 1);
    const NodeId dst_node = dst.nodes()[0].id;
    // State survives even though no instance exists.
    REQUIRE(dst.custom_node_state(dst_node) == ser_level_bytes(1.75f));

    // Re-serializing preserves the blob (save-load-save keeps the state).
    const auto json2 = GraphSerializer::to_json(dst);
    REQUIRE(json2.find("\"state_b64\"") != std::string::npos);

    SignalGraph reloaded;
    auto reloaded_result = GraphSerializer::from_json(reloaded, json2);
    REQUIRE(reloaded_result.ok);
    REQUIRE(missing_custom_types_contain(reloaded_result, "pulp.test.level@1"));
    REQUIRE(reloaded.nodes().size() == 1);
    REQUIRE(reloaded.custom_node_state(reloaded.nodes().front().id) ==
            ser_level_bytes(1.75f));
}

TEST_CASE("GraphSerializer generated custom-state fuzz validates state payloads",
          "[host][serializer][generated][state][fuzz]") {
    SECTION("malformed base64 fails closed") {
        SignalGraph dst;
        auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "custom",
      "name": "GeneratedState",
      "num_input_ports": 1,
      "num_output_ports": 1,
      "gain": 1,
      "custom": {
        "type_id": "pulp.generated.state",
        "version": 1,
        "state_b64": "AA#="
      }
    }
  ],
  "connections": []
})");

        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error == "invalid custom state_b64");
        REQUIRE(dst.nodes().empty());
        REQUIRE(dst.connections().empty());
    }

    SECTION("valid padded base64 tolerates whitespace") {
        SignalGraph dst;
        auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "custom",
      "name": "GeneratedState",
      "num_input_ports": 1,
      "num_output_ports": 1,
      "gain": 1,
      "custom": {
        "type_id": "pulp.generated.state",
        "version": 1,
        "state_b64": "AAEC/ w=="
      }
    }
  ],
  "connections": []
})");

        REQUIRE(result.ok);
        REQUIRE(dst.nodes().size() == 1);
        REQUIRE(dst.custom_node_state(dst.nodes().front().id)
                == std::vector<uint8_t>{0x00, 0x01, 0x02, 0xff});
    }
}

TEST_CASE("GraphSerializer generated node type validation rejects invalid shapes",
          "[host][serializer][generated][type]") {
    SECTION("audio input cannot declare input ports") {
        SignalGraph dst;
        auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "audio_in",
      "name": "BadInput",
      "num_input_ports": 1,
      "num_output_ports": 1,
      "gain": 1
    }
  ],
  "connections": []
})");

        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error == "invalid generated node shape for audio_in");
        REQUIRE(dst.nodes().empty());
    }

    SECTION("gain nodes must use the built-in stereo utility shape") {
        SignalGraph dst;
        auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "gain",
      "name": "BadGain",
      "num_input_ports": 1,
      "num_output_ports": 1,
      "gain": 1
    }
  ],
  "connections": []
})");

        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error == "invalid generated node shape for gain");
        REQUIRE(dst.nodes().empty());
    }

    SECTION("generated custom nodes cannot declare negative ports") {
        SignalGraph dst;
        auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "custom",
      "name": "BadCustom",
      "num_input_ports": -1,
      "num_output_ports": 1,
      "gain": 1,
      "custom": {
        "type_id": "pulp.generated.bad",
        "version": 1
      }
    }
  ],
  "connections": []
})");

        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error
                == "invalid generated node port count for custom");
        REQUIRE(dst.nodes().empty());
    }
}

TEST_CASE("GraphSerializer preserves unresolved custom node identity",
          "[host][serializer][node-abi]") {
    SignalGraph src;
    auto input = src.add_input_node(1, "Input");
    auto custom = src.add_unresolved_custom_node(
        "pulp.test.future-node", 4, 2, 1, "Future Node");
    auto output = src.add_output_node(1, "Output");
    REQUIRE(input != 0);
    REQUIRE(custom != 0);
    REQUIRE(output != 0);
    REQUIRE(src.connect(input, 0, custom, 0));
    REQUIRE(src.connect(custom, 0, output, 0));

    const auto json = GraphSerializer::to_json(src);
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(missing_custom_types_contain(result, "pulp.test.future-node@4"));
    REQUIRE(dst.nodes().size() == 3);
    REQUIRE(dst.connections().size() == 2);

    bool saw_custom = false;
    for (const auto& node : dst.nodes()) {
        if (node.type != NodeType::Custom) continue;
        saw_custom = true;
        REQUIRE(node.custom_type_id == "pulp.test.future-node");
        REQUIRE(node.custom_type_version == 4);
        REQUIRE(node.num_input_ports == 2);
        REQUIRE(node.num_output_ports == 1);
    }
    REQUIRE(saw_custom);

    REQUIRE(dst.prepare(48000.0, 4));
    float in_samples[4] = {0.25f, 0.5f, 0.75f, 1.0f};
    float out_samples[4] = {-1.0f, -1.0f, -1.0f, -1.0f};
    const float* in_ptrs[1] = {in_samples};
    float* out_ptrs[1] = {out_samples};
    pulp::audio::BufferView<const float> in_view(in_ptrs, 1, 4);
    pulp::audio::BufferView<float> out_view(out_ptrs, 1, 4);
    dst.process(out_view, in_view, 4);
    for (int i = 0; i < 4; ++i) REQUIRE(out_samples[i] == in_samples[i]);

    const auto second_json = GraphSerializer::to_json(dst);
    REQUIRE(second_json.find("\"type_id\": \"pulp.test.future-node\"") !=
            std::string::npos);
    REQUIRE(second_json.find("\"version\": 4") != std::string::npos);
    REQUIRE(second_json.find("\"source_node\"") != std::string::npos);

    SignalGraph reloaded;
    auto reloaded_result = GraphSerializer::from_json(reloaded, second_json);
    REQUIRE(reloaded_result.ok);
    REQUIRE(missing_custom_types_contain(reloaded_result, "pulp.test.future-node@4"));
    REQUIRE(reloaded.nodes().size() == 3);
    REQUIRE(reloaded.connections().size() == 2);
    const auto* reloaded_custom = find_node_named(reloaded, "Future Node");
    REQUIRE(reloaded_custom != nullptr);
    REQUIRE(reloaded_custom->type == NodeType::Custom);
    REQUIRE(reloaded_custom->custom_type_id == "pulp.test.future-node");
    REQUIRE(reloaded_custom->custom_type_version == 4);
    REQUIRE(reloaded_custom->num_input_ports == 2);
    REQUIRE(reloaded_custom->num_output_ports == 1);
}

TEST_CASE("GraphSerializer resolves custom nodes by exact registry version",
          "[host][serializer][node-abi]") {
    SignalGraph src;
    REQUIRE(src.register_custom_node_type(make_custom_node_type(
        "pulp.test.versioned-node", 1, 1, 1, "Version 1")));
    auto custom = src.add_custom_node("pulp.test.versioned-node", 1, "Old Node");
    REQUIRE(custom != 0);

    const auto json = GraphSerializer::to_json(src);

    SignalGraph dst;
    REQUIRE(dst.register_custom_node_type(make_custom_node_type(
        "pulp.test.versioned-node", 1, 1, 1, "Version 1")));
    REQUIRE(dst.register_custom_node_type(make_custom_node_type(
        "pulp.test.versioned-node", 2, 2, 2, "Version 2")));
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(result.missing_custom_node_types.empty());
    REQUIRE(dst.nodes().size() == 1);
    REQUIRE(dst.nodes().front().type == NodeType::Custom);
    REQUIRE(dst.nodes().front().custom_type_version == 1);
    REQUIRE(dst.nodes().front().num_input_ports == 1);
    REQUIRE(dst.nodes().front().num_output_ports == 1);
}

TEST_CASE("GraphSerializer preserves unresolved custom nodes when registered ABI mismatches",
          "[host][serializer][node-abi]") {
    SignalGraph src;
    REQUIRE(src.register_custom_node_type(make_custom_node_type(
        "pulp.test.mismatched-node", 7, 2, 1, "Wide Node")));
    auto custom = src.add_custom_node("pulp.test.mismatched-node", 7, "Wide Node");
    REQUIRE(custom != 0);

    const auto json = GraphSerializer::to_json(src);

    SignalGraph dst;
    REQUIRE(dst.register_custom_node_type(make_custom_node_type(
        "pulp.test.mismatched-node", 7, 1, 1, "Narrow Node")));
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(missing_custom_types_contain(result, "pulp.test.mismatched-node@7"));
    REQUIRE(dst.nodes().size() == 1);

    const auto* node = find_node_named(dst, "Wide Node");
    REQUIRE(node != nullptr);
    REQUIRE(node->type == NodeType::Custom);
    REQUIRE(node->custom_type_id == "pulp.test.mismatched-node");
    REQUIRE(node->custom_type_version == 7);
    REQUIRE(node->num_input_ports == 2);
    REQUIRE(node->num_output_ports == 1);
}

TEST_CASE("GraphSerializer serializes plugin formats and state blobs",
          "[host][serializer][issue-643]") {
    struct ExpectedFormat {
        PluginFormat format;
        const char* wire;
        const char* uid;
    };
    const ExpectedFormat formats[] = {
        {PluginFormat::VST3, "vst3", "pulp.test.vst3"},
        {PluginFormat::AudioUnit, "au", "pulp.test.au"},
        {PluginFormat::AudioUnitV3, "auv3", "pulp.test.auv3"},
        {PluginFormat::CLAP, "clap", "pulp.test.clap"},
        {PluginFormat::LV2, "lv2", "pulp.test.lv2"},
    };

    for (const auto& expected : formats) {
        SignalGraph src;
        auto info = make_fake_plugin_info(expected.wire, expected.uid,
                                          expected.format, 1, 1);
        src.add_plugin_node(
            std::make_unique<SerializerSlot>(
                info, std::vector<uint8_t>{0x00, 0x01, 0x02, 0xff}),
            1, 1, expected.wire);

        const auto json = GraphSerializer::to_json(src);
        REQUIRE(json.find(std::string("\"format\": \"") + expected.wire + "\"") !=
                std::string::npos);
        REQUIRE(json.find(std::string("\"unique_id\": \"") + expected.uid + "\"") !=
                std::string::npos);
        REQUIRE(json.find("\"state_b64\": \"AAEC/w==\"") != std::string::npos);

        SignalGraph dst;
        auto result = GraphSerializer::from_json(dst, json);
        REQUIRE(result.ok);
        REQUIRE(missing_plugins_contain(
            result, std::string(expected.wire) + ":" + expected.uid));
    }
}

TEST_CASE("GraphSerializer resolves the canonical BuiltIn instrument round-trip",
          "[host][serializer][builtin]") {
    PluginInfo info;
    info.format = PluginFormat::BuiltIn;
    info.unique_id = "pulp.instrument.basic";
    info.num_inputs = 0;
    info.num_outputs = 2;

    auto slot = PluginSlot::load(info);
    REQUIRE(slot != nullptr);

    SignalGraph src;
    src.add_plugin_node(std::move(slot), 0, 2, "Basic Instrument");
    const auto json = GraphSerializer::to_json(src);
    REQUIRE(json.find("\"format\": \"builtin\"") != std::string::npos);
    REQUIRE(json.find("\"unique_id\": \"pulp.instrument.basic\"") !=
            std::string::npos);
    REQUIRE(json.find("\"last_path\": \"\"") != std::string::npos);

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.empty());
    REQUIRE(dst.nodes().size() == 1);

    const auto& restored = dst.nodes().front();
    REQUIRE(restored.type == NodeType::Plugin);
    REQUIRE(restored.plugin != nullptr);
    REQUIRE(restored.name == "Basic Instrument");
    REQUIRE(restored.num_input_ports == 0);
    REQUIRE(restored.num_output_ports == 2);
    REQUIRE(restored.plugin_info.format == PluginFormat::BuiltIn);
    REQUIRE(restored.plugin_info.unique_id == "pulp.instrument.basic");
    REQUIRE(restored.plugin_info.path.empty());
}

TEST_CASE("GraphSerializer serializes short plugin state blobs with stable padding",
          "[host][serializer]") {
    SignalGraph src;
    auto one_byte = make_fake_plugin_info("OneByteState", "pulp.test.state.one",
                                          PluginFormat::CLAP, 1, 1);
    auto two_bytes = make_fake_plugin_info("TwoByteState", "pulp.test.state.two",
                                           PluginFormat::CLAP, 1, 1);
    src.add_plugin_node(
        std::make_unique<SerializerSlot>(
            one_byte, std::vector<uint8_t>{0x00}),
        1, 1, "OneByteState");
    src.add_plugin_node(
        std::make_unique<SerializerSlot>(
            two_bytes, std::vector<uint8_t>{0x00, 0x01}),
        1, 1, "TwoByteState");

    const auto json = GraphSerializer::to_json(src);
    REQUIRE(json.find("\"unique_id\": \"pulp.test.state.one\"") !=
            std::string::npos);
    REQUIRE(json.find("\"state_b64\": \"AA==\"") != std::string::npos);
    REQUIRE(json.find("\"unique_id\": \"pulp.test.state.two\"") !=
            std::string::npos);
    REQUIRE(json.find("\"state_b64\": \"AAE=\"") != std::string::npos);
}

TEST_CASE("GraphSerializer preserves unresolved plugin identity when reserializing",
          "[host][serializer][issue-493]") {
    SignalGraph src;
    auto info = make_fake_plugin_info("MissingEcho", "pulp.test.missing.echo",
                                      PluginFormat::CLAP, 2, 2);
    src.add_plugin_node(info);

    const auto first_json = GraphSerializer::to_json(src);
    REQUIRE(first_json.find("\"unique_id\": \"pulp.test.missing.echo\"") !=
            std::string::npos);
    REQUIRE(first_json.find("\"state_b64\"") == std::string::npos);

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, first_json);
    REQUIRE(result.ok);
    REQUIRE(missing_plugins_contain(result, "clap:pulp.test.missing.echo"));
    REQUIRE(dst.nodes().size() == 1);
    REQUIRE(dst.nodes().front().type == NodeType::Plugin);
    REQUIRE(dst.nodes().front().plugin == nullptr);

    const auto second_json = GraphSerializer::to_json(dst);
    REQUIRE(second_json.find("\"format\": \"clap\"") != std::string::npos);
    REQUIRE(second_json.find("\"unique_id\": \"pulp.test.missing.echo\"") !=
            std::string::npos);
    REQUIRE(second_json.find("\"last_path\": \"/nonexistent/MissingEcho.clap\"") !=
            std::string::npos);
    REQUIRE(second_json.find("\"state_b64\"") == std::string::npos);
}

TEST_CASE("GraphSerializer preserves unresolved plugin display name",
          "[host][serializer][issue-493]") {
    const auto json = R"({
      "format_version": 1,
      "nodes": [
        {
          "id": 7,
          "type": "plugin",
          "name": "User Label",
          "num_input_ports": 1,
          "num_output_ports": 1,
          "plugin": {
            "format": "clap",
            "unique_id": "pulp.test.renamed.missing",
            "name": "Plugin Metadata Name",
            "manufacturer": "PulpTest",
            "version": "1.0.0",
            "last_path": "/nonexistent/Plugin Metadata Name.clap"
          }
        }
      ],
      "connections": []
    })";

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(missing_plugins_contain(result, "clap:pulp.test.renamed.missing"));
    REQUIRE(dst.nodes().size() == 1);

    const auto& node = dst.nodes().front();
    REQUIRE(node.type == NodeType::Plugin);
    REQUIRE(node.plugin == nullptr);
    REQUIRE(node.name == "User Label");
    REQUIRE(node.plugin_info.name == "Plugin Metadata Name");
    REQUIRE(node.plugin_info.unique_id == "pulp.test.renamed.missing");

    const auto second_json = GraphSerializer::to_json(dst);
    REQUIRE(second_json.find("\"name\": \"User Label\"") != std::string::npos);
    REQUIRE(second_json.find("\"name\": \"Plugin Metadata Name\"") !=
            std::string::npos);
    REQUIRE(second_json.find("\"unique_id\": \"pulp.test.renamed.missing\"") !=
            std::string::npos);
    REQUIRE(second_json.find("\"state_b64\"") == std::string::npos);
}

TEST_CASE("GraphSerializer clears partially loaded graphs after plugin field errors",
          "[host][serializer][issue-493]") {
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "audio_in",
      "name": "Input",
      "num_input_ports": 0,
      "num_output_ports": 1,
      "gain": 1
    },
    {
      "id": 2,
      "type": "plugin",
      "name": "Broken plugin",
      "num_input_ports": 1,
      "num_output_ports": 1,
      "gain": 1,
      "plugin": {
        "format": "clap",
        "unique_id": 123,
        "name": "Broken plugin",
        "manufacturer": "PulpTest",
        "version": "1.0.0",
        "last_path": "/missing/broken.clap"
      }
    }
  ],
  "connections": []
})");

    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("field deserialization failed") != std::string::npos);
    REQUIRE(dst.nodes().empty());
    REQUIRE(dst.connections().empty());
}

TEST_CASE("GraphSerializer reports plugin nodes missing plugin payload",
          "[host][serializer]") {
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 1,
      "type": "audio_in",
      "name": "Input",
      "num_input_ports": 0,
      "num_output_ports": 1,
      "gain": 1
    },
    {
      "id": 2,
      "type": "plugin",
      "name": "Missing Identity",
      "num_input_ports": 1,
      "num_output_ports": 1,
      "gain": 1
    },
    {
      "id": 3,
      "type": "audio_out",
      "name": "Output",
      "num_input_ports": 1,
      "num_output_ports": 0,
      "gain": 1
    }
  ],
  "connections": [
    {
      "source_node": 1,
      "source_port": 0,
      "dest_node": 2,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 1,
      "source_port": 0,
      "dest_node": 3,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    }
  ]
})");

    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(missing_plugins_contain(result, "Missing Identity (no plugin info)"));
    REQUIRE(dst.nodes().size() == 2);
    REQUIRE(dst.connections().size() == 1);

    for (const auto& node : dst.nodes()) {
        REQUIRE(node.name != "Missing Identity");
    }
    REQUIRE_FALSE(dst.connections().front().feedback);
    REQUIRE_FALSE(dst.connections().front().midi);
    REQUIRE_FALSE(dst.connections().front().automation);
}

TEST_CASE("GraphSerializer defaults missing gain and coerces non-numeric layout to zero",
          "[host][serializer]") {
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 2,
  "nodes": [
    {
      "id": 1,
      "type": "gain",
      "name": "Default Gain",
      "num_input_ports": 2,
      "num_output_ports": 2,
      "layout": {"x": "left", "y": false}
    },
    {
      "id": 2,
      "type": "audio_out",
      "name": "Output",
      "num_input_ports": 2,
      "num_output_ports": 0,
      "gain": "loud",
      "layout": {"x": 16, "y": "bottom"}
    }
  ],
  "connections": []
})");

    REQUIRE(result.ok);
    REQUIRE(dst.nodes().size() == 2);
    REQUIRE(result.editor_layout.size() == 2);

    const auto* gain = find_node_named(dst, "Default Gain");
    REQUIRE(gain != nullptr);
    REQUIRE(gain->gain == 1.0f);

    const auto* output = find_node_named(dst, "Output");
    REQUIRE(output != nullptr);
    REQUIRE(output->gain == 0.0f);

    bool saw_zero_layout = false;
    bool saw_mixed_layout = false;
    for (const auto& [id, pos] : result.editor_layout) {
        if (pos.first == 0.0f && pos.second == 0.0f) saw_zero_layout = true;
        if (pos.first == 16.0f && pos.second == 0.0f) saw_mixed_layout = true;
    }
    REQUIRE(saw_zero_layout);
    REQUIRE(saw_mixed_layout);
}

TEST_CASE("GraphSerializer round-trips MIDI routing", "[host][serializer]") {
    SignalGraph src;
    auto midi_in = src.add_midi_input_node();
    auto midi_out = src.add_midi_output_node();
    REQUIRE(src.connect_midi(midi_in, midi_out));
    REQUIRE(src.connections().size() == 1);

    const auto json = GraphSerializer::to_json(src);
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(dst.nodes().size() == 2);

    // Verify the MIDI edge itself survives, not just the MIDI nodes. A
    // regression that dropped MIDI edges on decode would still leave both
    // MIDI nodes present but the `midi` connection broken.
    int midi_edge_count = 0;
    for (const auto& c : dst.connections()) {
        if (c.midi) midi_edge_count++;
    }
    REQUIRE(midi_edge_count == 1);
}

TEST_CASE("GraphSerializer decodes feedback edges and integer layout coordinates",
          "[host][serializer][issue-643]") {
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, R"({
  "format_version": 1,
  "nodes": [
    {
      "id": 10,
      "type": "audio_in",
      "name": "Input",
      "num_input_ports": 0,
      "num_output_ports": 1,
      "gain": 1,
      "layout": {"x": 12, "y": 34}
    },
    {
      "id": 11,
      "type": "gain",
      "name": "Loop",
      "num_input_ports": 2,
      "num_output_ports": 2,
      "gain": 2,
      "layout": {"x": 56.5, "y": 78}
    },
    {
      "id": 12,
      "type": "audio_out",
      "name": "Output",
      "num_input_ports": 1,
      "num_output_ports": 0,
      "gain": 1
    }
  ],
  "connections": [
    {
      "source_node": 10,
      "source_port": 0,
      "dest_node": 11,
      "dest_port": 0,
      "feedback": false,
      "midi": false,
      "automation": false
    },
    {
      "source_node": 11,
      "source_port": 0,
      "dest_node": 11,
      "dest_port": 1,
      "feedback": true,
      "midi": false,
      "automation": false
    }
  ]
})");

    REQUIRE(result.ok);
    REQUIRE(result.error.empty());
    REQUIRE(dst.nodes().size() == 3);
    REQUIRE(result.editor_layout.size() == 2);

    bool saw_input_layout = false;
    bool saw_gain_layout = false;
    for (const auto& [id, pos] : result.editor_layout) {
        if (pos.first == 12.0f && pos.second == 34.0f) saw_input_layout = true;
        if (pos.first == 56.5f && pos.second == 78.0f) saw_gain_layout = true;
    }
    REQUIRE(saw_input_layout);
    REQUIRE(saw_gain_layout);

    int feedback_edges = 0;
    int audio_edges = 0;
    for (const auto& c : dst.connections()) {
        if (c.feedback) ++feedback_edges;
        if (!c.feedback && !c.midi && !c.automation) ++audio_edges;
    }
    REQUIRE(feedback_edges == 1);
    REQUIRE(audio_edges == 1);

    bool saw_gain = false;
    for (const auto& node : dst.nodes()) {
        if (node.name == "Loop") {
            saw_gain = true;
            REQUIRE(node.gain == 2.0f);
        }
    }
    REQUIRE(saw_gain);
}

TEST_CASE("GraphSerializer serializes and decodes automation connection fields",
          "[host][serializer][issue-643]") {
    SignalGraph src;
    auto input = src.add_input_node(1, "Input");
    auto info = make_fake_plugin_info("AutoTarget", "pulp.test.auto", PluginFormat::CLAP, 1, 1);
    auto plugin = src.add_plugin_node(std::make_unique<SerializerSlot>(info), 1, 1,
                                      "AutoTarget");
    REQUIRE(src.connect_automation(input, 0, plugin, SerializerSlot::kParamId,
                                   -1.0f, 1.0f, 12.5f, AutomationMix::Add));

    const auto json = GraphSerializer::to_json(src);
    REQUIRE(json.find("\"automation\": true") != std::string::npos);
    REQUIRE(json.find("\"auto_param_id\": 42") != std::string::npos);
    REQUIRE(json.find("\"auto_range_lo\": -1") != std::string::npos);
    REQUIRE(json.find("\"auto_range_hi\": 1") != std::string::npos);
    REQUIRE(json.find("\"auto_smoothing\": 12.5") != std::string::npos);
    REQUIRE(json.find("\"auto_mix\": 1") != std::string::npos);

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(missing_plugins_contain(result, "clap:pulp.test.auto"));
    REQUIRE(dst.nodes().size() == 2);
    REQUIRE(dst.connections().empty());
}

TEST_CASE("GraphSerializer serializes audio-rate modulation connection fields",
          "[host][serializer][audio-rate]") {
    SignalGraph src;
    auto input = src.add_input_node(1, "Input");
    auto info = make_fake_plugin_info("AudioRateTarget", "pulp.test.audio-rate",
                                      PluginFormat::CLAP, 1, 1);
    auto plugin = src.add_plugin_node(
        std::make_unique<SerializerSlot>(
            info, std::vector<uint8_t>{}, ParamRate::AudioRate),
        1, 1, "AudioRateTarget");
    REQUIRE(src.connect_audio_rate_modulation(
        input, 0, plugin, SerializerSlot::kParamId,
        -2.0f, 2.0f, 5.0f, AutomationMix::Add));

    const auto json = GraphSerializer::to_json(src);
    REQUIRE(json.find("\"automation\": false") != std::string::npos);
    REQUIRE(json.find("\"audio_rate_modulation\": true") != std::string::npos);
    REQUIRE(json.find("\"auto_param_id\": 42") != std::string::npos);
    REQUIRE(json.find("\"auto_range_lo\": -2") != std::string::npos);
    REQUIRE(json.find("\"auto_range_hi\": 2") != std::string::npos);
    REQUIRE(json.find("\"auto_smoothing\": 5") != std::string::npos);
    REQUIRE(json.find("\"auto_mix\": 1") != std::string::npos);

    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE(missing_plugins_contain(result, "clap:pulp.test.audio-rate"));
    REQUIRE(dst.nodes().size() == 2);
    REQUIRE(dst.connections().empty());
}

// When graph_serializer rehydrates a graph with an unresolved plugin, it creates
// a placeholder Plugin node with a null slot. SignalGraph::process()
// deterministically passes input through to output (or zero-fills when channel
// counts mismatch) so downstream AudioOutput never sees stale audio.
TEST_CASE("SignalGraph processes missing-plugin node as deterministic pass-through",
          "[host][serializer][issue-491]") {
    SignalGraph src;
    auto input = src.add_input_node(2, "In");
    auto plugin = src.add_plugin_node(
        make_fake_plugin_info("Ghost", "com.pulp.test.ghost"));
    auto output = src.add_output_node(2, "Out");
    // Wire both L and R channels so the missing-plugin node has
    // something meaningful to pass through.
    REQUIRE(src.connect(input, 0, plugin, 0));
    REQUIRE(src.connect(input, 1, plugin, 1));
    REQUIRE(src.connect(plugin, 0, output, 0));
    REQUIRE(src.connect(plugin, 1, output, 1));

    const auto json = GraphSerializer::to_json(src);
    SignalGraph dst;
    auto result = GraphSerializer::from_json(dst, json);
    REQUIRE(result.ok);
    REQUIRE_FALSE(result.missing_plugins.empty());

    REQUIRE(dst.prepare(48000.0, 64));
    // The placeholder Plugin node is now executor-eligible: it routes through the
    // canonical executor's pass-through-or-zero, matching the legacy walk.
    REQUIRE(pulp::host::signal_graph_executor_eligible(dst));

    const int num_samples = 64;
    std::vector<float> in_l(num_samples), in_r(num_samples);
    std::vector<float> out_l(num_samples, 123.0f), out_r(num_samples, -456.0f);
    for (int i = 0; i < num_samples; ++i) {
        in_l[i] = 0.25f;
        in_r[i] = -0.5f;
    }
    float* in_chs[2]  = { in_l.data(),  in_r.data()  };
    float* out_chs[2] = { out_l.data(), out_r.data() };
    pulp::audio::BufferView<const float> in_view(
        const_cast<const float**>(in_chs), 2,
        static_cast<std::size_t>(num_samples));
    pulp::audio::BufferView<float> out_view(
        out_chs, 2, static_cast<std::size_t>(num_samples));

    dst.process(out_view, in_view, num_samples);

    // Pass-through behavior: output == input (the missing-plugin node
    // forwarded audio verbatim, then AudioOutput accumulated it).
    for (int i = 0; i < num_samples; ++i) {
        REQUIRE(out_l[i] == 0.25f);
        REQUIRE(out_r[i] == -0.5f);
    }

    // Run a second block whose input is all zeros; stale data from the
    // first block must NOT leak through. Before the fix, the placeholder
    // node wrote nothing and the next AudioOutput accumulation carried
    // whatever lived in output_data scratch.
    for (int i = 0; i < num_samples; ++i) {
        in_l[i] = 0.0f;
        in_r[i] = 0.0f;
        out_l[i] = 77.0f;
        out_r[i] = 77.0f;
    }
    dst.process(out_view, in_view, num_samples);
    for (int i = 0; i < num_samples; ++i) {
        REQUIRE(out_l[i] == 0.0f);
        REQUIRE(out_r[i] == 0.0f);
    }
}

namespace {
class RuntimeOnlyProcessor final : public pulp::format::Processor {
  public:
    pulp::format::PluginDescriptor descriptor() const override {
        pulp::format::PluginDescriptor descriptor;
        descriptor.name = "RuntimeOnly";
        descriptor.manufacturer = "Pulp";
        descriptor.bundle_id = "dev.pulp.test.runtime-only";
        descriptor.version = "1.0.0";
        descriptor.category = pulp::format::PluginCategory::Effect;
        descriptor.input_buses = {{"Main In", 1, false}};
        descriptor.output_buses = {{"Main Out", 1, false}};
        return descriptor;
    }
    void define_parameters(pulp::state::StateStore&) override {}
    void prepare(const pulp::format::PrepareContext&) override {}
    void process(pulp::audio::BufferView<float>&, const pulp::audio::BufferView<const float>&,
                 pulp::midi::MidiBuffer&, pulp::midi::MidiBuffer&,
                 const pulp::format::ProcessContext&) override {}
};
} // namespace

TEST_CASE("GraphSerializer refuses runtime-owned Processor nodes",
          "[host][serializer][processor-node]") {
    SignalGraph graph;
    REQUIRE(graph.add_processor_node(std::make_unique<RuntimeOnlyProcessor>()) != 0);
    REQUIRE(GraphSerializer::to_json(graph).empty());

    constexpr auto encoded = R"json({
      "format_version": 2,
      "nodes": [{
        "id": 1,
        "type": "processor",
        "name": "runtime-only",
        "num_input_ports": 1,
        "num_output_ports": 1
      }],
      "connections": []
    })json";
    SignalGraph loaded;
    const auto result = GraphSerializer::from_json(loaded, encoded);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("unsupported by .pulpgraph") != std::string::npos);
    REQUIRE(loaded.nodes().empty());
}
