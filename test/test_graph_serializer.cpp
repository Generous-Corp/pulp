// GraphSerializer round-trip tests.
//
// Verifies that a populated SignalGraph survives to_json / from_json with
// topology, connections, editor layout, and plugin identity preserved.
// Plugin state (save_state blobs) is covered only in shape here — a full
// state round-trip requires a real loaded plugin and lives in the
// integration lane gated on PULP_TEST_CLAP_PATH.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>

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
    int type_counts[6] = {0};
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
