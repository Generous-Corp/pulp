#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>

#include <pulp/format/graph_runtime_executor.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/graph_identity.hpp>
#include <pulp/runtime/crypto.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>
#include <set>
#include <string>
#include <unordered_map>

using namespace pulp::host;

static std::span<const std::byte> receipt_input(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

static std::string stereo_payload() {
    return R"G3({"assets":{},"edges":{"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"gain_a\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"gain_b\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"gain_b","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"gain_a","port":"audio"}},"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"gain_b\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"output\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"output","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"gain_b","port":"audio"}},"[\"audio\",{\"direction\":\"out\",\"kind\":\"audio\",\"node\":\"input\",\"port\":\"audio\"},{\"direction\":\"in\",\"kind\":\"audio\",\"node\":\"gain_a\",\"port\":\"audio\"},null]":{"channel_map":[["L","L"],["R","R"]],"destination":{"direction":"in","kind":"audio","node":"gain_a","port":"audio"},"feedback":false,"lane":"audio","semantic_order":null,"source":{"direction":"out","kind":"audio","node":"input","port":"audio"}}},"interfaces":[{"channels":["L","R"],"endpoint":{"direction":"out","kind":"audio","node":"input","port":"audio"},"key":"main-in","kind":"audio","role":"input"},{"channels":["L","R"],"endpoint":{"direction":"in","kind":"audio","node":"output","port":"audio"},"key":"main-out","kind":"audio","role":"output"}],"nodes":{"gain_a":{"config":{"gain":"f32:3f800000"},"implementation":"pulp.builtin.gain.v1","initial_state":{"mode":"default","revision":"gain.v1"},"kind":"gain","kind_contract":"gain.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"gain_b":{"config":{"gain":"f32:3f800000"},"implementation":"pulp.builtin.gain.v1","initial_state":{"mode":"default","revision":"gain.v1"},"kind":"gain","kind_contract":"gain.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"input":{"config":{},"implementation":"pulp.builtin.audio-input.v1","initial_state":{"mode":"default","revision":"io.v1"},"kind":"audio_input","kind_contract":"io.v1","parameter_bindings":{},"policies":{},"ports":{"in":{},"out":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}}}},"output":{"config":{},"implementation":"pulp.builtin.audio-output.v1","initial_state":{"mode":"default","revision":"io.v1"},"kind":"audio_output","kind_contract":"io.v1","parameter_bindings":{},"policies":{},"ports":{"in":{"audio":{"channels":["L","R"],"contract":"audio-port.v1","kind":"audio"}},"out":{}}}},"policies":{"event_order":"none","fault":"fail","feedback":"forbid","rate_conversion":"none","seed":null,"state_retention":"none"},"profile":"pulp.graph.identity.v1"})G3";
}

TEST_CASE("D4 preserves D1/D2 semantic validity for a non-stereo payload") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->add_input_node(2) != 0);

    auto mono = stereo_payload();
    auto replace_all = [&](std::string_view from, std::string_view to) {
        std::size_t offset = 0;
        while ((offset = mono.find(from, offset)) != std::string::npos) {
            mono.replace(offset, from.size(), to);
            offset += to.size();
        }
    };
    replace_all("[\"L\",\"R\"]", "[\"C\"]");
    replace_all("[[\"L\",\"L\"],[\"R\",\"R\"]]", "[[\"C\",\"C\"]]");
    REQUIRE(edit->bind_authored_identity(mono, {}).refusal ==
            PreparedDescriptionRefusal::UnsupportedPreparedProfile);
}

TEST_CASE("D4 empty prepared export is detached and canonical") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);

    const auto bound = edit->bind_authored_identity(
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\"}",
        {});
    REQUIRE(bound);
    REQUIRE(edit->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);

    auto snapshot = edit->committed_execution_snapshot();
    REQUIRE(snapshot);
    const auto exported = snapshot.export_prepared_description();
    REQUIRE(exported);
    REQUIRE(exported.value->schema == "pulp.graph.prepared-io-gain.v1");
    REQUIRE(exported.value->semantic_profile == "pulp.graph.identity.v1");
    REQUIRE(exported.value->max_block_frames == 128);
    REQUIRE(exported.value->channels.empty());
    REQUIRE(exported.value->owner_graph_identity == graph.authoring_receipt().graph_identity);
    REQUIRE(exported.value->owner_graph_identity != 0);
    REQUIRE(exported.value->execution_generation == graph.authoring_receipt().generation);
    REQUIRE(exported.value->execution_generation != 0);
    REQUIRE(exported.value->semantic_digest.rfind("sha256:", 0) == 0);
    REQUIRE_FALSE(exported.value->canonical_bytes().empty());
    REQUIRE(exported.value->content_hash().size() == 64);
}

TEST_CASE("D4 export rejects a retired owner generation") {
    SignalGraph graph;
    auto first = graph.begin_prepared_topology_edit();
    REQUIRE(first);
    REQUIRE(first->bind_authored_identity(
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\"}",
        {}));
    REQUIRE(first->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(first->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    auto retired = first->committed_execution_snapshot();

    auto failed = graph.begin_prepared_topology_edit();
    REQUIRE(failed);
    REQUIRE(failed->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE_FALSE(failed->disconnect(0, 0, 0, 0));
    REQUIRE(failed->commit() == SignalGraph::PreparedTopologyEdit::Result::InvalidMutation);
    const auto preserved = retired.export_prepared_description();
    REQUIRE(preserved);
    REQUIRE(preserved.value->owner_generation == graph.authoring_receipt().generation);

    auto second = graph.begin_prepared_topology_edit();
    REQUIRE(second);
    REQUIRE(second->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(second->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    REQUIRE(retired.export_prepared_description().refusal ==
            PreparedDescriptionRefusal::SnapshotChanged);
}

static PreparedDescriptionData unsigned_limit_capture() {
    SignalGraph graph;
    graph.set_limits({.max_nodes = std::numeric_limits<std::size_t>::max(),
                      .max_connections = std::numeric_limits<std::size_t>::max(),
                      .max_ports = std::numeric_limits<std::size_t>::max(),
                      .max_block_size = 16384,
                      .max_estimated_work_units = std::numeric_limits<std::size_t>::max()});
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto input = edit->add_input_node(2, "Input");
    const auto first = edit->add_gain_node("Gain A");
    const auto second = edit->add_gain_node("Gain B");
    const auto output = edit->add_output_node(2, "Output");
    AuthoredRuntimeBinding binding;
    binding.nodes = {{input, "input"}, {first, "gain_a"}, {second, "gain_b"}, {output, "output"}};
    for (std::uint32_t channel = 0; channel < 2; ++channel) {
        REQUIRE(edit->connect(input, channel, first, channel));
        REQUIRE(edit->connect(first, channel, second, channel));
        REQUIRE(edit->connect(second, channel, output, channel));
        const auto key = channel == 0 ? "L" : "R";
        for (const auto node : {input, first, second})
            binding.channels.push_back({node, "audio", true, key, channel, channel});
        for (const auto node : {first, second, output})
            binding.channels.push_back({node, "audio", false, key, channel, channel});
    }
    binding.interfaces = {{"main-in", false, input, "audio", "L", 0, 0, 0},
                          {"main-in", false, input, "audio", "R", 1, 0, 1},
                          {"main-out", true, output, "audio", "L", 0, 0, 0},
                          {"main-out", true, output, "audio", "R", 1, 0, 1}};
    REQUIRE(edit->bind_authored_identity(stereo_payload(), binding));
    REQUIRE(edit->prepare(48000, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto capture = edit->committed_execution_snapshot().export_prepared_description();
    REQUIRE(capture);
    REQUIRE(capture.value->host_max_nodes == std::numeric_limits<std::size_t>::max());
    REQUIRE(capture.value->host_max_connections == std::numeric_limits<std::size_t>::max());
    REQUIRE(capture.value->host_max_ports == std::numeric_limits<std::size_t>::max());
    REQUIRE(capture.value->host_max_estimated_work_units ==
            std::numeric_limits<std::size_t>::max());
    REQUIRE(capture.value->effective_max_nodes <= 512);
    return *capture.value;
}

static std::string replace_once(std::string source, std::string_view from, std::string_view to) {
    const auto at = source.find(from);
    REQUIRE(at != std::string::npos);
    REQUIRE(source.find(from, at + from.size()) == std::string::npos);
    source.replace(at, from.size(), to);
    return source;
}

TEST_CASE("Resolved identity round trips exact unsigned limits", "[resolved-unsigned]") {
    auto capture = unsigned_limit_capture();
    const auto actual = capture_graph_identity_receipt(capture);
    REQUIRE(actual);
    REQUIRE(decode_graph_identity_receipt(receipt_input(actual.value().canonical_bytes())));
    capture.host_max_estimated_work_units = 0;
    const auto baseline = resolved_fingerprint(capture);
    REQUIRE(baseline);
    const std::array<std::pair<std::uint64_t, std::string_view>, 8> boundaries{{
        {9007199254740991ull, "9007199254740991"},
        {9007199254740992ull, "9007199254740992"},
        {9007199254740993ull, "9007199254740993"},
        {9223372036854775806ull, "9223372036854775806"},
        {9223372036854775807ull, "9223372036854775807"},
        {9223372036854775808ull, "9223372036854775808"},
        {9223372036854775809ull, "9223372036854775809"},
        {18446744073709551615ull, "18446744073709551615"},
    }};
    std::set<std::string> digests;
    for (const auto& [value, decimal] : boundaries) {
        INFO(decimal);
        capture.host_max_estimated_work_units = value;
        const auto receipt = capture_graph_identity_receipt(capture);
        REQUIRE(receipt);
        const auto& resolved = receipt.value().resolved;
        const auto expected = replace_once(std::string(baseline.value().canonical_bytes()),
                                           "\"max_estimated_work_units\":0",
                                           "\"max_estimated_work_units\":" + std::string(decimal));
        REQUIRE(resolved.canonical_bytes() == expected);
        REQUIRE(resolved.digest() ==
                "sha256:" + pulp::runtime::sha256_hex(std::string("pulp.graph.resolved-plan.v1") +
                                                      std::string(1, '\0') + expected));
        REQUIRE(digests.emplace(resolved.digest()).second);
        const auto bytes = receipt.value().canonical_bytes();
        const auto decoded = decode_graph_identity_receipt(receipt_input(bytes));
        REQUIRE(decoded);
        REQUIRE(decoded.value().resolved.canonical_bytes() == expected);
        REQUIRE(decoded.value().canonical_bytes() == bytes);
    }
}

TEST_CASE("Resolved numeric fields reject wire substitutes and overflow", "[resolved-unsigned]") {
    auto capture = unsigned_limit_capture();
    capture.host_max_estimated_work_units = 0;
    const auto receipt = capture_graph_identity_receipt(capture);
    REQUIRE(receipt);
    const auto payload = std::string(receipt.value().resolved.canonical_bytes());
    const auto bytes = receipt.value().canonical_bytes();
    const auto rewrite = [&](std::string_view from, std::string_view to) {
        const auto replacement = replace_once(payload, from, to);
        auto modified = replace_once(bytes, payload, replacement);
        const auto digest =
            "sha256:" + pulp::runtime::sha256_hex(std::string("pulp.graph.resolved-plan.v1") +
                                                  std::string(1, '\0') + replacement);
        return replace_once(modified, receipt.value().resolved.digest(), digest);
    };
    for (std::string_view invalid :
         {"18446744073709551616", "-1", "-0", "00", "01", "+1", "1.0", "1e3",
          "\"9223372036854775808\"", "true", "{\"high\":2147483648,\"low\":0}",
          "{\"$class\":\"pulp.identity.unsigned64\",\"high\":2147483648,\"low\":0}"}) {
        INFO(invalid);
        const auto modified = rewrite("\"max_estimated_work_units\":0",
                                      "\"max_estimated_work_units\":" + std::string(invalid));
        REQUIRE_FALSE(decode_graph_identity_receipt(receipt_input(modified)));
    }
    const auto narrow = rewrite("\"input_ports\":0", "\"input_ports\":4294967296");
    REQUIRE_FALSE(decode_graph_identity_receipt(receipt_input(narrow)));
    const auto wrong_minimum = rewrite("\"effective\":{\"block_frames\":128",
                                       "\"effective\":{\"block_frames\":18446744073709551615");
    REQUIRE_FALSE(decode_graph_identity_receipt(receipt_input(wrong_minimum)));
}

TEST_CASE("Exact numeric token opt in preserves the default JSON parser", "[resolved-unsigned]") {
    const auto defaults = choc::json::parse("[0,-42,1.25,1e+3,9223372036854775807]");
    REQUIRE(defaults[0].isInt());
    REQUIRE(defaults[1].getInt64() == -42);
    REQUIRE(defaults[2].getFloat64() == 1.25);
    REQUIRE(defaults[3].getFloat64() == 1000);
    REQUIRE(defaults[4].isFloat64());
    const auto token = +[](std::string_view number, bool negative) {
        return choc::value::Value(std::string(negative ? "-" : "") + std::string(number));
    };
    const std::string surrounded = "[18446744073709551615,-42,1e+3]trailing";
    const auto exact = choc::json::parseWithNumberTokenParser(
        std::string_view(surrounded.data(), surrounded.find(']') + 1), token);
    REQUIRE(exact[0].getString() == "18446744073709551615");
    REQUIRE(exact[1].getString() == "-42");
    REQUIRE(exact[2].getString() == "1e+3");
}

TEST_CASE("D4 refuses an unbound prepared snapshot") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto snapshot = edit->committed_execution_snapshot();
    const auto exported = snapshot.export_prepared_description();
    REQUIRE(exported.refusal == PreparedDescriptionRefusal::LogicalIdentityMissing);
}

TEST_CASE("D4 binds and exports a closed stereo Gain chain") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto input = edit->add_input_node(2, "Input");
    const auto gain_a = edit->add_gain_node("Gain A");
    const auto gain_b = edit->add_gain_node("Gain B");
    const auto output = edit->add_output_node(2, "Output");
    REQUIRE(input != 0);
    REQUIRE(gain_a != 0);
    REQUIRE(gain_b != 0);
    REQUIRE(output != 0);
    for (std::uint32_t channel = 0; channel < 2; ++channel) {
        REQUIRE(edit->connect(input, channel, gain_a, channel));
        REQUIRE(edit->connect(gain_a, channel, gain_b, channel));
        REQUIRE(edit->connect(gain_b, channel, output, channel));
    }

    AuthoredRuntimeBinding binding;
    binding.nodes = {{input, "input"}, {gain_a, "gain_a"}, {gain_b, "gain_b"}, {output, "output"}};
    const auto add_channels = [&](std::uint64_t node, bool output_direction, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i)
            binding.channels.push_back({node, "audio", output_direction, i == 0 ? "L" : "R", i, i});
    };
    add_channels(input, true, 2);
    add_channels(gain_a, false, 2);
    add_channels(gain_a, true, 2);
    add_channels(gain_b, false, 2);
    add_channels(gain_b, true, 2);
    add_channels(output, false, 2);
    binding.interfaces = {
        {"main-in", false, input, "audio", "L", 0, 0, 0},
        {"main-in", false, input, "audio", "R", 1, 0, 1},
        {"main-out", true, output, "audio", "L", 0, 0, 0},
        {"main-out", true, output, "audio", "R", 1, 0, 1},
    };
    const auto bound = edit->bind_authored_identity(stereo_payload(), binding);
    INFO(prepared_description_refusal_name(bound.refusal));
    REQUIRE(bound);
    REQUIRE(edit->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto snapshot = edit->committed_execution_snapshot();
    const auto exported = snapshot.export_prepared_description();
    REQUIRE(exported);
    REQUIRE(exported.value->nodes.size() == 4);
    const auto resolved = resolved_fingerprint(*exported.value);
    REQUIRE(resolved);
    REQUIRE(resolved.value().domain() == "pulp.graph.resolved-plan.v1");
    REQUIRE(resolved.value().profile() == "pulp.graph.resolved.io-gain.v1");
    REQUIRE(resolved.value().canonical_bytes().find("\"buffers\"") != std::string_view::npos);
    const auto receipt = capture_graph_identity_receipt(*exported.value);
    REQUIRE(receipt);
    REQUIRE(receipt.value().canonical_bytes().find("pulp.graph.identity.receipt.v1") !=
            std::string::npos);
    const auto resolved_bytes = std::string(resolved.value().canonical_bytes());
    REQUIRE(resolved_bytes.size() == 13606);
    REQUIRE(resolved.value().digest() ==
            "sha256:83f5e4e38d2b9610962fcf8c04a11cc11b511ec52d88bdbbb8db61110d7da48e");
    REQUIRE(resolved.value().digest() ==
            "sha256:" + pulp::runtime::sha256_hex(std::string("pulp.graph.resolved-plan.v1") +
                                                  std::string(1, '\0') + resolved_bytes));
    const auto receipt_bytes = receipt.value().canonical_bytes();
    REQUIRE(receipt_bytes.find("\"state_blobs\":{}") != std::string::npos);
    std::vector<std::byte> receipt_input(receipt_bytes.size());
    std::transform(receipt_bytes.begin(), receipt_bytes.end(), receipt_input.begin(),
                   [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });
    REQUIRE(decode_graph_identity_receipt(receipt_input));
    auto malformed_state = receipt_bytes;
    const auto state_marker = malformed_state.find("\"state_blobs\":{}");
    REQUIRE(state_marker != std::string::npos);
    malformed_state.replace(state_marker, std::string("\"state_blobs\":{}").size(),
                            "\"state_blobs\":[]");
    std::vector<std::byte> malformed_state_input(malformed_state.size());
    std::transform(malformed_state.begin(), malformed_state.end(), malformed_state_input.begin(),
                   [](char c) { return static_cast<std::byte>(static_cast<unsigned char>(c)); });
    REQUIRE_FALSE(decode_graph_identity_receipt(malformed_state_input));
    receipt_input[receipt_input.size() / 2] ^= std::byte{1};
    REQUIRE_FALSE(decode_graph_identity_receipt(receipt_input));
    auto provenance_variant = *exported.value;
    provenance_variant.snapshot_id += 101;
    provenance_variant.pool_snapshot_id += 101;
    provenance_variant.pool_binding_identity ^= 0x55;
    provenance_variant.execution_generation += 3;
    provenance_variant.owner_generation += 7;
    provenance_variant.execution_selection_generation += 11;
    REQUIRE(resolved_fingerprint(provenance_variant));
    REQUIRE(resolved_fingerprint(provenance_variant).value().canonical_bytes() == resolved_bytes);
    REQUIRE(resolved_fingerprint(provenance_variant).value().digest() == resolved.value().digest());
    auto zero_host_limit = *exported.value;
    zero_host_limit.host_max_nodes = 0;
    REQUIRE_FALSE(resolved_fingerprint(zero_host_limit));
    auto forged_alias = *exported.value;
    forged_alias.resolved_alias_endpoint->node = "forged";
    REQUIRE_FALSE(resolved_fingerprint(forged_alias));
    auto overlapping_storage = *exported.value;
    REQUIRE(overlapping_storage.resolved_buffer_values.size() > 1);
    bool overlapped = false;
    for (std::size_t i = 0; i < overlapping_storage.resolved_buffer_values.size() && !overlapped;
         ++i)
        for (std::size_t j = i + 1; j < overlapping_storage.resolved_buffer_values.size(); ++j)
            if (overlapping_storage.resolved_buffer_values[i].class_id ==
                overlapping_storage.resolved_buffer_values[j].class_id) {
                overlapping_storage.resolved_buffer_values[j].lifetime_begin =
                    overlapping_storage.resolved_buffer_values[i].lifetime_begin;
                overlapping_storage.resolved_buffer_values[j].lifetime_end =
                    overlapping_storage.resolved_buffer_values[i].lifetime_end;
                overlapped = true;
                break;
            }
    REQUIRE(overlapped);
    REQUIRE_FALSE(resolved_fingerprint(overlapping_storage));
    REQUIRE(exported.value->edges.size() == 6);
    REQUIRE(exported.value->channels == std::vector<std::string>{"L", "R"});
    REQUIRE(exported.value->interfaces.size() == 2);
    for (const auto& interface : exported.value->interfaces) {
        REQUIRE(interface.runtime_bus_index == 0);
        REQUIRE(interface.runtime_channel_indices == std::vector<std::uint32_t>{0, 1});
    }
    REQUIRE(exported.value->prepared_plan_node_count == 4);
    REQUIRE(exported.value->prepared_plan_connection_count == 6);
    REQUIRE(exported.value->serial_pool_bound);
    REQUIRE(exported.value->input_modes.size() == 6);
    REQUIRE(exported.value->connection_delay_samples.size() == 6);
    REQUIRE(exported.value->reads_in_place.size() == 6);
    REQUIRE(exported.value->pool_slot_count == exported.value->buffer_slot_count);
    REQUIRE(exported.value->snapshot_id != 0);
    REQUIRE(exported.value->pool_snapshot_id == exported.value->snapshot_id);
    REQUIRE(exported.value->pool_binding_identity != 0);
    REQUIRE(exported.value->executor_contract == "pulp.graph.canonical-executor.f32.v1");
    REQUIRE(exported.value->execution_mode == "serial");
    REQUIRE(exported.value->tie_break_contract == "ready-node-id-ascending.v1");
    REQUIRE(exported.value->effective_max_nodes == 512);
    REQUIRE(exported.value->effective_max_connections == 2048);
    REQUIRE(exported.value->effective_max_ports_per_node == 64);
    REQUIRE(exported.value->effective_max_ports == 4096);
    REQUIRE(exported.value->effective_max_block_frames >= 128);
    REQUIRE(exported.value->host_input_alias_eligible);
    REQUIRE(exported.value->host_input_alias_fallback == "internal_copy");
    REQUIRE(exported.value->host_input_alias_source == "input:audio:out");
    REQUIRE(exported.value->host_input_alias_view == "input:audio:out");
    REQUIRE(exported.value->host_input_alias_mappings ==
            std::vector<std::string>{"input:audio:out:channel=L->input:audio:out:channel=L",
                                     "input:audio:out:channel=R->input:audio:out:channel=R"});
    REQUIRE(exported.value->schedule ==
            std::vector<std::string>{"input", "gain_a", "gain_b", "output"});
    // The detached vector retains plan order for diagnostics, while the
    // portable encoder treats nodes as a logical-key map.
    const auto canonical = exported.value->canonical_bytes();
    REQUIRE(canonical.find("\"key\":\"gain_a\"") < canonical.find("\"key\":\"gain_b\""));
    REQUIRE(canonical.find("\"key\":\"gain_b\"") < canonical.find("\"key\":\"input\""));
    REQUIRE(canonical.find("\"key\":\"input\"") < canonical.find("\"key\":\"output\""));
    // Independent topological oracle: derive the ready-node-id order from the
    // published graph edges rather than copying the exporter schedule.
    std::unordered_map<NodeId, std::size_t> indegree;
    for (const auto& node : graph.nodes())
        indegree[node.id] = 0;
    for (const auto& connection : graph.connections())
        ++indegree.at(connection.dest_node);
    std::set<NodeId> ready;
    for (const auto& [id, degree] : indegree)
        if (degree == 0)
            ready.insert(id);
    std::vector<std::string> independent_schedule;
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        const auto binding_node =
            std::find_if(binding.nodes.begin(), binding.nodes.end(),
                         [&](const AuthoredNodeBinding& node) { return node.node_id == id; });
        REQUIRE(binding_node != binding.nodes.end());
        independent_schedule.push_back(binding_node->authored_key);
        for (const auto& connection : graph.connections()) {
            if (connection.source_node != id)
                continue;
            auto& degree = indegree.at(connection.dest_node);
            REQUIRE(degree > 0);
            if (--degree == 0)
                ready.insert(connection.dest_node);
        }
    }
    REQUIRE(independent_schedule == exported.value->schedule);
    REQUIRE(independent_schedule.size() == graph.nodes().size());
    for (const auto& value : exported.value->buffer_values) {
        const auto class_marker = value.find("class=");
        const auto lifetime_marker = value.find(",lifetime=[");
        REQUIRE(class_marker != std::string::npos);
        REQUIRE(lifetime_marker != std::string::npos);
        const auto class_begin = class_marker + 6;
        const auto class_end = value.find(',', class_begin);
        const auto class_number = std::stoul(value.substr(class_begin, class_end - class_begin));
        REQUIRE(class_number < exported.value->buffer_classes.size());
        const auto lifetime_begin = lifetime_marker + 11;
        const auto comma = value.find(',', lifetime_begin);
        const auto close = value.find(')', comma);
        REQUIRE(comma != std::string::npos);
        REQUIRE(close != std::string::npos);
        REQUIRE(std::stoul(value.substr(lifetime_begin, comma - lifetime_begin)) <
                std::stoul(value.substr(comma + 1, close - comma - 1)));
    }
    REQUIRE_FALSE(exported.value->buffer_classes.empty());
    REQUIRE_FALSE(exported.value->buffer_values.empty());
    const auto has_buffer_record = [&](std::string_view text) {
        return std::any_of(
            exported.value->buffer_values.begin(), exported.value->buffer_values.end(),
            [&](const std::string& value) { return value.find(text) != std::string::npos; });
    };
    REQUIRE(has_buffer_record("role=external_input"));
    REQUIRE(has_buffer_record("role=external_output"));
    REQUIRE(has_buffer_record("phase=output_copy"));
    REQUIRE(has_buffer_record("channel=L"));
    REQUIRE(has_buffer_record("channel=R"));
    const auto external_input_lifetime =
        std::find_if(exported.value->buffer_values.begin(), exported.value->buffer_values.end(),
                     [](const std::string& value) {
                         return value.find("input:port=audio:direction=out,role=external_input") !=
                                std::string::npos;
                     });
    REQUIRE(external_input_lifetime != exported.value->buffer_values.end());
    // The AudioInput value remains live through its last reader (gain_a at
    // processing position 1), rather than ending at the producer position.
    REQUIRE(external_input_lifetime->find(",lifetime=[0,2)") != std::string::npos);
    REQUIRE(exported.value->nodes[0].kind == "AudioInput");
    REQUIRE(exported.value->nodes[1].kind == "Gain");
    REQUIRE(exported.value->nodes[1].gain_bits == 0x3f800000u);
    REQUIRE(exported.value->nodes[2].kind == "Gain");
    REQUIRE(exported.value->nodes[3].kind == "AudioOutput");
    for (const auto& node : exported.value->nodes) {
        REQUIRE(node.latency_samples == 0);
        REQUIRE(node.tail_samples == 0);
        REQUIRE_FALSE(node.transport_sensitive);
    }
    std::vector<std::string> actual_edges;
    for (const auto& edge : exported.value->edges)
        actual_edges.push_back(edge.source + "->" + edge.destination + ":" + edge.final_input_mode);
    std::sort(actual_edges.begin(), actual_edges.end());
    REQUIRE(actual_edges ==
            std::vector<std::string>{
                "gain_a:port=audio:channel=L->gain_b:port=audio:channel=L:in_place",
                "gain_a:port=audio:channel=R->gain_b:port=audio:channel=R:in_place",
                "gain_b:port=audio:channel=L->output:port=audio:channel=L:in_place",
                "gain_b:port=audio:channel=R->output:port=audio:channel=R:in_place",
                "input:port=audio:channel=L->gain_a:port=audio:channel=L:in_place",
                "input:port=audio:channel=R->gain_a:port=audio:channel=R:in_place"});
}

TEST_CASE("D4 independent alias oracle covers width overlap and restoration") {
    using namespace pulp::format;
    pulp::graph::GraphRuntimePlan plan;
    pulp::graph::GraphRuntimeNodePlan input_node{};
    input_node.id = 1;
    input_node.kind = pulp::graph::GraphRuntimeNodeKind::AudioInput;
    input_node.output_ports = 2;
    input_node.first_outbound_connection = 0;
    input_node.outbound_connection_count = 2;
    pulp::graph::GraphRuntimeNodePlan output_node{};
    output_node.id = 2;
    output_node.kind = pulp::graph::GraphRuntimeNodeKind::AudioOutput;
    output_node.input_ports = 2;
    output_node.first_inbound_connection = 0;
    output_node.inbound_connection_count = 2;
    plan.nodes = {input_node, output_node};
    for (std::uint32_t channel = 0; channel < 2; ++channel) {
        pulp::graph::GraphRuntimeConnectionPlan edge{};
        edge.source_index = 0;
        edge.source_port = channel;
        edge.dest_index = 1;
        edge.dest_port = channel;
        plan.connections.push_back(edge);
    }
    plan.inbound_connection_indices = {0, 1};
    plan.outbound_connection_indices = {0, 1};
    plan.processing_order_indices = {0, 1};
    std::array<GraphRuntimeNodeBinding, 2> bindings{{
        GraphRuntimeNodeBinding{.node_id = 1, .required = false},
        GraphRuntimeNodeBinding{.node_id = 2, .required = false},
    }};
    GraphRuntimeSnapshot snapshot;
    REQUIRE(snapshot.reset(std::move(plan), bindings));
    GraphRuntimeBufferPool pool;
    REQUIRE(pool.reset(snapshot.buffer_slot_count(), 64));
    REQUIRE(pool.bind(snapshot));

    pulp::audio::Buffer<float> input_storage(2, 64);
    pulp::audio::Buffer<float> output_storage(2, 64);
    const std::array<const float*, 2> input_channels{input_storage.view().channel_ptr(0),
                                                     input_storage.view().channel_ptr(1)};
    const pulp::audio::BufferView<const float> input_view(input_channels.data(), 2, 64);
    const auto output_view = output_storage.view();
    BusBuffer input_bus{"main", BusDirection::Input, BusRole::Main, true, input_view, {}};
    BusBuffer output_bus{"main", BusDirection::Output, BusRole::Main, true, {}, output_view};
    const auto* restored_left = pool.bound_node(1).inputs[0];
    const auto* restored_right = pool.bound_node(1).inputs[1];
    REQUIRE(pool.alias_host_input(&input_bus, &output_bus, 64));
    REQUIRE(pool.bound_node(1).inputs[0] == input_channels[0]);
    REQUIRE(pool.bound_node(1).inputs[1] == input_channels[1]);
    pool.release_host_input();
    REQUIRE(pool.bound_node(1).inputs[0] == restored_left);
    REQUIRE(pool.bound_node(1).inputs[1] == restored_right);

    pulp::audio::Buffer<float> narrow_storage(1, 64);
    const std::array<const float*, 1> narrow_channels{narrow_storage.view().channel_ptr(0)};
    const pulp::audio::BufferView<const float> narrow_view(narrow_channels.data(), 1, 64);
    BusBuffer narrow_bus{"main", BusDirection::Input, BusRole::Main, true, narrow_view, {}};
    REQUIRE_FALSE(pool.alias_host_input(&narrow_bus, &output_bus, 64));

    pulp::audio::Buffer<float> overlap_storage(2, 64);
    const std::array<const float*, 2> overlap_channels{overlap_storage.view().channel_ptr(0),
                                                       overlap_storage.view().channel_ptr(1)};
    const pulp::audio::BufferView<const float> overlap_input(overlap_channels.data(), 2, 64);
    const auto overlap_output = overlap_storage.view();
    BusBuffer overlap_in{"main", BusDirection::Input, BusRole::Main, true, overlap_input, {}};
    BusBuffer overlap_out{"main", BusDirection::Output, BusRole::Main, true, {}, overlap_output};
    REQUIRE_FALSE(pool.alias_host_input(&overlap_in, &overlap_out, 64));
}

TEST_CASE("D4 binding mutation requires an explicit rebind") {
    // This exercises the transactional stale-binding latch using a valid empty
    // binding before a candidate mutation.
    SignalGraph empty_graph;
    auto empty_edit = empty_graph.begin_prepared_topology_edit();
    REQUIRE(empty_edit);
    REQUIRE(empty_edit->bind_authored_identity(
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\"}",
        {}));
    REQUIRE(empty_edit->add_gain_node() != 0);
    REQUIRE(empty_edit->prepare(48000.0, 128) ==
            SignalGraph::PreparedTopologyEdit::Result::PreflightFailed);
}

TEST_CASE("D4 execution selection ABA retires an exported snapshot") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->bind_authored_identity(
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\"}",
        {}));
    REQUIRE(edit->prepare(48000.0, 128) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    const auto snapshot = edit->committed_execution_snapshot();
    REQUIRE(snapshot);
    graph.set_canonical_executor_routing_enabled(false);
    graph.set_canonical_executor_routing_enabled(true);
    REQUIRE(snapshot.export_prepared_description().refusal ==
            PreparedDescriptionRefusal::SnapshotChanged);
}

TEST_CASE("D4 execution selection generation saturates") {
    std::atomic<std::uint64_t> generation{std::numeric_limits<std::uint64_t>::max() - 1};
    pulp::host::detail::advance_execution_selection_generation(generation);
    REQUIRE(generation.load(std::memory_order_relaxed) ==
            std::numeric_limits<std::uint64_t>::max());
    pulp::host::detail::advance_execution_selection_generation(generation);
    REQUIRE(generation.load(std::memory_order_relaxed) ==
            std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("D4 selection mutation after binding requires a rebind") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->bind_authored_identity(
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\"}",
        {}));
    edit->set_canonical_executor_routing_enabled(false);
    REQUIRE(edit->prepare(48000.0, 128) ==
            SignalGraph::PreparedTopologyEdit::Result::PreflightFailed);
}

TEST_CASE("D4 actual host limits reject an over-limit preparation transaction") {
    SignalGraph graph;
    graph.set_limits({.max_nodes = 4096,
                      .max_connections = 16384,
                      .max_ports = 32768,
                      .max_block_size = 64,
                      .max_estimated_work_units = 0});
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    REQUIRE(edit->bind_authored_identity(
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\"}",
        {}));
    REQUIRE(edit->prepare(48000.0, 128) ==
            SignalGraph::PreparedTopologyEdit::Result::PreflightFailed);
    REQUIRE_FALSE(edit->committed_execution_snapshot());
}

TEST_CASE("D4 rejects a swapped authored channel") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto input = edit->add_input_node(2, "Input");
    const auto gain_a = edit->add_gain_node("Gain A");
    const auto gain_b = edit->add_gain_node("Gain B");
    const auto output = edit->add_output_node(2, "Output");
    REQUIRE(input != 0);
    REQUIRE(gain_a != 0);
    REQUIRE(gain_b != 0);
    REQUIRE(output != 0);
    for (std::uint32_t channel = 0; channel < 2; ++channel) {
        REQUIRE(edit->connect(input, channel, gain_a, channel));
        REQUIRE(edit->connect(gain_a, channel, gain_b, channel));
        REQUIRE(edit->connect(gain_b, channel, output, channel));
    }
    AuthoredRuntimeBinding binding;
    binding.nodes = {{input, "input"}, {gain_a, "gain_a"}, {gain_b, "gain_b"}, {output, "output"}};
    const auto add_channels = [&](std::uint64_t node, bool output_direction) {
        binding.channels.push_back({node, "audio", output_direction, "L", 0, 0});
        binding.channels.push_back({node, "audio", output_direction, "R", 1, 1});
    };
    add_channels(input, true);
    add_channels(gain_a, false);
    add_channels(gain_a, true);
    add_channels(gain_b, false);
    add_channels(gain_b, true);
    add_channels(output, false);
    binding.interfaces = {
        {"main-in", false, input, "audio", "L", 0, 0, 0},
        {"main-in", false, input, "audio", "R", 1, 0, 1},
        {"main-out", true, output, "audio", "L", 0, 0, 0},
        {"main-out", true, output, "audio", "R", 1, 0, 1},
    };
    std::swap(binding.channels[0].authored_channel, binding.channels[1].authored_channel);
    REQUIRE(edit->bind_authored_identity(stereo_payload(), binding).refusal ==
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
}

TEST_CASE("D4 rejects an event lane in the closed audio profile") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto input = edit->add_input_node(2, "Input");
    const auto gain_a = edit->add_gain_node("Gain A");
    const auto gain_b = edit->add_gain_node("Gain B");
    const auto output = edit->add_output_node(2, "Output");
    REQUIRE(edit->connect(input, 1, gain_a, 1));
    REQUIRE(edit->connect_midi(input, gain_a));
    for (std::uint32_t channel = 0; channel < 2; ++channel) {
        REQUIRE(edit->connect(gain_a, channel, gain_b, channel));
        REQUIRE(edit->connect(gain_b, channel, output, channel));
    }

    AuthoredRuntimeBinding binding;
    binding.nodes = {{input, "input"}, {gain_a, "gain_a"}, {gain_b, "gain_b"}, {output, "output"}};
    const auto add_channels = [&](std::uint64_t node, bool output_direction, std::uint32_t count) {
        for (std::uint32_t i = 0; i < count; ++i)
            binding.channels.push_back({node, "audio", output_direction, i == 0 ? "L" : "R", i, i});
    };
    add_channels(input, true, 2);
    add_channels(gain_a, false, 2);
    add_channels(gain_a, true, 2);
    add_channels(gain_b, false, 2);
    add_channels(gain_b, true, 2);
    add_channels(output, false, 2);
    binding.interfaces = {
        {"main-in", false, input, "audio", "L", 0, 0, 0},
        {"main-in", false, input, "audio", "R", 1, 0, 1},
        {"main-out", true, output, "audio", "L", 0, 0, 0},
        {"main-out", true, output, "audio", "R", 1, 0, 1},
    };
    REQUIRE(edit->bind_authored_identity(stereo_payload(), binding).refusal ==
            PreparedDescriptionRefusal::UnsupportedPreparedProfile);
}

TEST_CASE("D4 rejects unknown and duplicate semantic fields") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const std::string unknown =
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\",\"unknown\":0}";
    REQUIRE(edit->bind_authored_identity(unknown, {}).refusal ==
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    const std::string duplicate =
        "{\"assets\":{},\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\"}";
    REQUIRE(edit->bind_authored_identity(duplicate, {}).refusal ==
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    const std::string escaped_duplicate =
        "{\"assets\":{},\"asset\\u0073\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\","
        "\"rate_conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},"
        "\"profile\":\"pulp.graph.identity.v1\"}";
    REQUIRE(edit->bind_authored_identity(escaped_duplicate, {}).refusal ==
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    std::string malformed_utf8 =
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},\"policies\":{},"
        "\"profile\":\"pulp.graph.identity.v1\",\"x\":\"";
    malformed_utf8.push_back(static_cast<char>(0xc0));
    malformed_utf8.push_back(static_cast<char>(0x80));
    malformed_utf8 += "\"}";
    REQUIRE(edit->bind_authored_identity(malformed_utf8, {}).refusal ==
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    const std::string unpaired_surrogate =
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},\"policies\":{},"
        "\"profile\":\"pulp.graph.identity.v1\",\"x\":\"\\uD800\"}";
    REQUIRE(edit->bind_authored_identity(unpaired_surrogate, {}).refusal ==
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    const std::string trailing_comma =
        "{\"assets\":{},\"edges\":{},\"interfaces\":[,],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":\"pulp."
        "graph.identity.v1\"}";
    REQUIRE(edit->bind_authored_identity(trailing_comma, {}).refusal ==
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
    const std::string missing_value =
        "{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
        "\"policies\":{\"event_order\":\"none\",\"fault\":\"fail\",\"feedback\":\"forbid\",\"rate_"
        "conversion\":\"none\",\"seed\":null,\"state_retention\":\"none\"},\"profile\":}";
    REQUIRE(edit->bind_authored_identity(missing_value, {}).refusal ==
            PreparedDescriptionRefusal::AuthoredBindingMismatch);
}
