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
#include <pulp/host/signal_graph.hpp>

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
  "format_version": 3,
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
    REQUIRE(future_result.error.find("unsupported graph format_version 3")
            != std::string::npos);
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
                                  "\"format_version\": 2");
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
    REQUIRE(result.error.find("field deserialization failed") != std::string::npos);
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
          "[host][serializer][coverage][phase3]") {
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

// Issue #491 P2 bonus: when graph_serializer rehydrates a graph with a
// plugin that can't be resolved, it creates a "placeholder" Plugin node
// with a null slot. Without an explicit branch in SignalGraph::process()
// the node's output scratch kept stale data across blocks. The fix
// deterministically passes input through to output (or zero-fills when
// channel counts mismatch) so downstream AudioOutput never sees stale
// audio.
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

    // Pass-through behaviour: output == input (the missing-plugin node
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
