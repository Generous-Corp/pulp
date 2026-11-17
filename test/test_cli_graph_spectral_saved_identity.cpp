// `pulp graph save --saved-identity` on a spectral graph, through the built
// CLI: a format 6 spectral document gains the saved-identity v7 envelope, the
// v7 document rewrites byte for byte, and a document whose spectral edge would
// not restore is refused without writing the output.

#include "test_cli_shellout_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <pulp/signal/spectral_frame_codec.hpp>

#include <filesystem>
#include <fstream>
#include <string>

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
