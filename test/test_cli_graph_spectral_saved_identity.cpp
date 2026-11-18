// `pulp graph save --saved-identity` on a spectral graph, through the built
// CLI: a format 6 spectral document gains the saved-identity v7 envelope, the
// v7 document rewrites byte for byte, and a document whose spectral edge would
// not restore is refused without writing the output. `pulp graph diff` reports
// spectral stage, asset and edge changes in JSON and text, and the MCP diff
// operation returns the CLI's JSON byte for byte.

#include "spectral_stream_oracle.hpp"
#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/signal/spectral_frame_codec.hpp>

#if defined(PULP_TEST_GRAPH_DIFF_MCP)
#include "mcp_tools.hpp"
#endif

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using namespace pulp_test_cli;

namespace {
// A format 6 spectral document: control input -> asset morph -> synthesis ->
// output, over two silent canonical streams (FFT 256, hop 64, 769 samples).
std::string spectral_format6_document() {
    const pulp::signal::SpectralStreamGeometry geometry{256, 64, 1, 48000};
    const std::size_t length = 769, frames = (length + 256 - 64 + 63) / 64;
    auto stream = pulp::signal::make_spectral_stream(geometry, frames);
    REQUIRE(stream);
    auto bytes = pulp::signal::encode_spectral_stream(*stream.value);
    REQUIRE(bytes);
    pulp::host::SignalGraph graph;
    const auto input = graph.add_input_node(1, "control");
    const auto morph = graph.add_spectral_asset_morph_v1(bytes.value->view(), bytes.value->view(),
                                                         length, "morph");
    REQUIRE(morph);
    const auto shape = *pulp::host::SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto synthesis = graph.add_spectral_synthesis(shape, "synthesis");
    REQUIRE(synthesis);
    const auto output = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(graph.node(input)->out("audio"), morph.value->morph));
    REQUIRE(graph.connect(morph.value->frames, synthesis.value->frames));
    REQUIRE(graph.connect(synthesis.value->audio[0], graph.node(output)->in("audio")));
    auto document = pulp::host::GraphSerializer::to_json(graph);
    REQUIRE_FALSE(document.empty());
    return document;
}
} // namespace

TEST_CASE("graph save --saved-identity writes the spectral v7 document",
          "[cli][shellout][graph][spectral]") {
    if (!fs::exists(pulp_binary()))
        SKIP("pulp not built");
    const auto dir = unique_temp_dir("pulp-graph-spectral-v7");
    fs::create_directories(dir);
    const auto document = spectral_format6_document();
    const auto source = dir / "spectral.pulpgraph";
    std::ofstream(source, std::ios::binary) << document;

    const auto stamped = dir / "stamped.pulpgraph";
    const auto saved = run_pulp({"graph", "save", source.string(), "--output", stamped.string(),
                                 "--saved-identity", "--json"});
    INFO(saved.stdout_output << saved.stderr_output);
    REQUIRE(saved.exit_code == 0);
    const auto v7 = read_file(stamped);
    const auto root = choc::json::parse(v7);
    REQUIRE(root["format_version"].getInt64() == 7);
    REQUIRE(root["identity_schema"].getString() == "pulp.graph.saved-identity.v7");
    REQUIRE(root["spectral_assets"].size() == 1);
    pulp::host::SignalGraph loaded;
    const auto restored = pulp::host::GraphSerializer::from_saved_document(loaded, v7);
    INFO(restored.error);
    REQUIRE(restored.ok);

    // The v7 document rewrites to the same bytes.
    const auto rewritten = dir / "rewritten.pulpgraph";
    const auto again =
        run_pulp({"graph", "save", stamped.string(), "--output", rewritten.string(), "--json"});
    INFO(again.stdout_output << again.stderr_output);
    REQUIRE(again.exit_code == 0);
    REQUIRE(read_file(rewritten) == v7);

    // A spectral edge marked as plain audio cannot restore; nothing is written.
    auto broken = document;
    const auto edge = broken.find("\"spectral\": true");
    REQUIRE(edge != std::string::npos);
    broken.replace(edge, 16, "\"spectral\": false");
    const auto lossy = dir / "lossy.pulpgraph";
    std::ofstream(lossy, std::ios::binary) << broken;
    const auto refused_out = dir / "refused.pulpgraph";
    const auto refused = run_pulp({"graph", "save", lossy.string(), "--output",
                                   refused_out.string(), "--saved-identity", "--json"});
    INFO(refused.stdout_output << refused.stderr_output);
    REQUIRE(refused.exit_code == 1);
    CHECK_FALSE(fs::exists(refused_out));
    fs::remove_all(dir);
}

namespace {
// source -> mask -> synthesis -> output over the asset of `seed`'s stimulus at
// FFT 256, hop 96, two channels.
std::string spectral_source_document(int seed, pulp::host::SignalGraph& graph) {
    namespace oracle = pulp::test::spectral_oracle;
    const oracle::Geometry g{256, 96, 2};
    const std::uint64_t length = 600;
    const auto bytes = oracle::encode(g, oracle::analyze(g, oracle::stimulus(g, length, seed)));
    const auto output = graph.add_output_node(g.channels, "out");
    const auto source = graph.add_spectral_asset_source_v1(bytes.view(), length, "source");
    REQUIRE(source);
    const auto shape = *pulp::host::SpectralIdentity::make(g.fft, g.hop, g.channels, 48000).value;
    const auto mask = graph.add_spectral_stage(
        shape,
        *pulp::host::QualifiedSpectralStage::real_mask_v1(shape, std::vector<float>(258, 1.0f))
             .value,
        "mask");
    REQUIRE(mask);
    const auto synthesis = graph.add_spectral_synthesis(shape, "synthesis");
    REQUIRE(synthesis);
    REQUIRE(graph.connect(source.value->frames.handle, mask.value->frames_in.handle));
    REQUIRE(graph.connect(mask.value->frames_out.handle, synthesis.value->frames.handle));
    for (int c = 0; c < g.channels; ++c)
        REQUIRE(graph.connect(synthesis.value->audio[c], graph.node(output)->input_ports[c]));
    auto document = pulp::host::GraphSerializer::to_saved_document(graph);
    REQUIRE_FALSE(document.empty());
    return document;
}
} // namespace

TEST_CASE("graph diff reports spectral stage, asset and edge changes with MCP parity",
          "[cli][shellout][graph][spectral][diff]") {
    if (!fs::exists(pulp_binary()))
        SKIP("pulp not built");
    const auto dir = unique_temp_dir("pulp-graph-spectral-diff");
    fs::create_directories(dir);
    pulp::host::SignalGraph graph, other;
    const auto before = spectral_source_document(0, graph);
    const auto replacement = choc::json::parse(spectral_source_document(1, other));

    // Same identities: halve the mask and swap the source's asset bytes.
    auto edited = choc::json::parse(before);
    const auto old_asset = std::string(edited["spectral_assets"][0]["id"].getString());
    const auto new_asset = std::string(replacement["spectral_assets"][0]["id"].getString());
    REQUIRE(old_asset != new_asset);
    edited.setMember("spectral_assets", choc::value::Value(replacement["spectral_assets"]));
    std::string mask_id, source_id;
    auto nodes = choc::value::createEmptyArray();
    for (const auto n : edited["nodes"]) {
        auto record = choc::value::Value(n);
        auto spectral = n.hasObjectMember("spectral") ? choc::value::Value(n["spectral"])
                                                      : choc::value::Value();
        if (n["type"].getString() == "spectral_stage") {
            mask_id = std::string(n["authored_id"].getString());
            auto gains = choc::value::createEmptyArray();
            for (std::uint32_t i = 0; i < n["spectral"]["gains"].size(); ++i)
                gains.addArrayElement(0.5);
            spectral.setMember("gains", gains);
            record.setMember("spectral", spectral);
        } else if (n["type"].getString() == "spectral_asset_source") {
            source_id = std::string(n["authored_id"].getString());
            spectral.setMember("asset", new_asset);
            record.setMember("spectral", spectral);
        }
        nodes.addArrayElement(record);
    }
    edited.setMember("nodes", nodes);
    const auto after = choc::json::toString(edited, true);
    {
        pulp::host::SignalGraph check;
        const auto loaded = pulp::host::GraphSerializer::from_saved_document(check, after);
        INFO(loaded.error);
        REQUIRE(loaded.ok);
    }
    const auto before_path = dir / "before.pulpgraph";
    const auto after_path = dir / "after.pulpgraph";
    std::ofstream(before_path, std::ios::binary) << before;
    std::ofstream(after_path, std::ios::binary) << after;

    const auto json =
        run_pulp({"graph", "diff", before_path.string(), after_path.string(), "--json"});
    INFO(json.stdout_output << json.stderr_output);
    REQUIRE(json.exit_code == 0);
    const auto report = choc::json::parse(json.stdout_output);
    REQUIRE_FALSE(report["equal"].getBool());
    REQUIRE(report["spectral_assets"]["added"].size() == 1);
    REQUIRE(std::string(report["spectral_assets"]["added"][0].getString()) == new_asset);
    REQUIRE(report["spectral_assets"]["removed"].size() == 1);
    REQUIRE(std::string(report["spectral_assets"]["removed"][0].getString()) == old_asset);
    REQUIRE(report["spectral_assets"]["changed"].size() == 0);
    REQUIRE(report["nodes"]["changed"].size() == 2);
    std::map<std::string, std::string> changed;
    for (const auto change : report["nodes"]["changed"]) {
        REQUIRE(change["fields"].size() == 1);
        changed[std::string(change["identity"].getString())] =
            std::string(change["fields"][0].getString());
    }
    REQUIRE(changed.at(mask_id) == "spectral.gains");
    REQUIRE(changed.at(source_id) == "spectral.asset");
    REQUIRE(report["connections"]["added"].size() == 0);
    REQUIRE(report["sections_changed"].size() == 0);

    const auto text = run_pulp({"graph", "diff", before_path.string(), after_path.string()});
    INFO(text.stdout_output);
    REQUIRE(text.exit_code == 0);
    REQUIRE(text.stdout_output.find("~ node " + mask_id + " (spectral.gains)") !=
            std::string::npos);
    REQUIRE(text.stdout_output.find("~ node " + source_id + " (spectral.asset)") !=
            std::string::npos);
    REQUIRE(text.stdout_output.find("+ spectral asset " + new_asset) != std::string::npos);
    REQUIRE(text.stdout_output.find("- spectral asset " + old_asset) != std::string::npos);
    REQUIRE(text.stdout_output.find("no semantic differences") == std::string::npos);

    // Control: a spectral document compared with itself has no differences.
    const auto same = run_pulp({"graph", "diff", before_path.string(), before_path.string()});
    REQUIRE(same.exit_code == 0);
    REQUIRE(same.stdout_output == "no semantic differences\n");

#if defined(PULP_TEST_GRAPH_DIFF_MCP)
    // The MCP diff runs the matched sibling pulp-cpp and returns its JSON.
    pulp_mcp::configure_graph_executable((pulp_binary().parent_path() / "pulp-mcp").string());
    auto arguments = choc::value::createObject("Arguments");
    arguments.addMember("operation", "diff");
    arguments.addMember("before", before_path.string());
    arguments.addMember("after", after_path.string());
    const auto response =
        choc::json::parse(pulp_mcp::handle_graph(choc::json::toString(arguments)));
    pulp_mcp::configure_graph_executable("");
    REQUIRE_FALSE(response["isError"].getBool());
    REQUIRE(std::string(response["content"][0]["text"].getString()) == json.stdout_output);
#endif
    fs::remove_all(dir);
}
