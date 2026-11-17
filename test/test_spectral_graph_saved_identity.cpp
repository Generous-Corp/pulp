// Spectral graphs in the saved-identity v7 document, through the public
// GraphSerializer path: authored node IDs, readable identities with exact
// sample rate bits, live analysis declarations, the single-stream asset
// source with its finite-source policy, format 6 migration, the common
// inspect and diff path, and refusal of malformed, corrupted and oversized
// documents before the caller's graph is touched. Audio is checked against the
// independent double-precision analysis grid and inverse DFT/OLA oracle.

#include "../core/host/src/graph_serializer_test_access.hpp"
#include "../examples/g10-routed-consumer/asset_morph_oracle.hpp"
#include "harness/rt_allocation_probe.hpp"
#include "spectral_stream_oracle.hpp"
#include "support/unique_temp_dir.hpp"

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/graph_history.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/subgraph_persistence.hpp>
#include <pulp/runtime/base64.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pulp::host;
namespace oracle = pulp::test::spectral_oracle;
using oracle::Geometry;

constexpr std::string_view kV7 = "pulp.graph.saved-identity.v7";

// A sample rate whose bits no decimal round trip at default precision keeps:
// the next double above 44100.
const double odd_rate = std::nextafter(44100.0, 1e9);

// Renders `total` samples of `input` (channels x samples, may be empty) through
// `graph` at fixed or ragged partitions, asserting zero callback allocations.
oracle::Audio render(SignalGraph& graph, const oracle::Audio& input, int out_channels,
                     std::size_t total, int partition) {
    oracle::Audio audio(out_channels, std::vector<float>(total));
    std::vector<std::vector<float>> padded = input;
    for (auto& channel : padded)
        channel.resize(total);
    std::vector<const float*> ins(padded.size());
    std::vector<float*> outs(out_channels);
    for (std::size_t pos = 0; pos < total;) {
        const auto count = std::min<std::size_t>(total - pos, partition ? std::size_t(partition)
                                                                        : 1 + pos * 17 % 511);
        for (std::size_t c = 0; c < padded.size(); ++c)
            ins[c] = padded[c].data() + pos;
        for (int c = 0; c < out_channels; ++c)
            outs[c] = audio[c].data() + pos;
        pulp::audio::BufferView<const float> iv(ins.empty() ? nullptr : ins.data(), int(ins.size()),
                                                int(count));
        pulp::audio::BufferView<float> ov(outs.data(), out_channels, int(count));
        pulp::format::ProcessContext transport;
        std::size_t allocations = 0;
        {
            pulp::test::RtAllocationProbe probe;
            graph.process(ov, iv, int(count), transport);
            allocations = probe.allocation_count();
        }
        REQUIRE(allocations == 0);
        pos += count;
    }
    return audio;
}

void clean_diagnostics(SignalGraph& graph) {
    REQUIRE(graph.spectral_error() == SpectralError::None);
    for (const auto& diagnostic : graph.poll_live_dsp_telemetry().spectral)
        REQUIRE(diagnostic.first_error == 0);
    REQUIRE(graph.routed_walk_fallbacks() == 0);
}

std::vector<float> mask_gains(const Geometry& g) {
    std::vector<float> gains(std::size_t(g.channels) * (g.fft / 2 + 1));
    for (std::size_t i = 0; i < gains.size(); ++i)
        gains[i] = float((i * 7 + i / 3) % 5) / 4.0f; // 0, 0.25, ... 1: exact in binary32
    return gains;
}

// audio_in -> analysis -> real mask -> synthesis -> audio_out at `rate`.
struct AnalysisGraph {
    Geometry g;
    std::unique_ptr<SignalGraph> graph = std::make_unique<SignalGraph>();
    NodeId analysis = 0, stage = 0, synthesis = 0;

    AnalysisGraph(Geometry geometry, double rate, std::vector<float> gains) : g(geometry) {
        const auto shape = *SpectralIdentity::make(g.fft, g.hop, g.channels, rate).value;
        const auto input = graph->add_input_node(g.channels, "input");
        const auto a = graph->add_spectral_analysis(shape, "analysis");
        REQUIRE(a);
        analysis = a.value->node;
        const auto s = graph->add_spectral_stage(
            shape, *QualifiedSpectralStage::real_mask_v1(shape, gains).value, "mask");
        REQUIRE(s);
        stage = s.value->node;
        const auto y = graph->add_spectral_synthesis(shape, "synthesis");
        REQUIRE(y);
        synthesis = y.value->node;
        const auto output = graph->add_output_node(g.channels, "output");
        for (int c = 0; c < g.channels; ++c) {
            REQUIRE(graph->connect(graph->node(input)->output_ports[c], a.value->audio[c]));
            REQUIRE(graph->connect(y.value->audio[c], graph->node(output)->input_ports[c]));
        }
        REQUIRE(graph->connect(a.value->frames, s.value->frames_in));
        REQUIRE(graph->connect(s.value->frames_out, y.value->frames));
    }
};

// asset source -> synthesis -> audio_out, from oracle frames of a stimulus.
struct SourceGraph {
    Geometry g;
    std::uint64_t length;
    oracle::Audio input;
    oracle::Stream exact;
    pulp::signal::EncodedSpectralStream bytes;
    std::unique_ptr<SignalGraph> graph = std::make_unique<SignalGraph>();
    SpectralAssetSourcePorts source;

    SourceGraph(Geometry geometry, std::uint64_t samples)
        : g(geometry), length(samples), input(oracle::stimulus(g, samples)),
          exact(oracle::analyze(g, input)), bytes(oracle::encode(g, exact)) {
        const auto output = graph->add_output_node(g.channels, "output");
        const auto made = graph->add_spectral_asset_source_v1(bytes.view(), length, "source");
        REQUIRE(made);
        source = *made.value;
        const auto shape = *SpectralIdentity::make(g.fft, g.hop, g.channels, 48000).value;
        const auto y = graph->add_spectral_synthesis(shape, "synthesis");
        REQUIRE(y);
        REQUIRE(graph->connect(source.frames, y.value->frames));
        for (int c = 0; c < g.channels; ++c)
            REQUIRE(graph->connect(y.value->audio[c], graph->node(output)->input_ports[c]));
    }
};

// The asset morph fixture the format 6 tests use: input -> morph -> identity ->
// mask -> synthesis -> output.
const auto& morph_inputs() {
    struct Inputs {
        std::vector<asset_oracle::Frame> a = asset_oracle::analyze(asset_oracle::stimulus(0));
        std::vector<asset_oracle::Frame> b = asset_oracle::analyze(asset_oracle::stimulus(1));
        pulp::signal::EncodedSpectralStream aa = asset_oracle::encode(a),
                                            bb = asset_oracle::encode(b);
    };
    static Inputs value;
    return value;
}
std::unique_ptr<SignalGraph> morph_fixture() {
    auto graph = std::make_unique<SignalGraph>();
    const auto input = graph->add_input_node(1, "control");
    const auto source = graph->add_spectral_asset_morph_v1(morph_inputs().aa.view(),
                                                           morph_inputs().bb.view(), 769, "assets");
    REQUIRE(source);
    const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto identity = graph->add_spectral_stage(
        shape, *QualifiedSpectralStage::identity_v1(shape).value, "identity");
    REQUIRE(identity);
    const auto mask = graph->add_spectral_stage(
        shape, *QualifiedSpectralStage::real_mask_v1(shape, std::vector<float>(129, 1.0f)).value,
        "mask");
    REQUIRE(mask);
    const auto synthesis = graph->add_spectral_synthesis(shape, "synthesis");
    REQUIRE(synthesis);
    const auto output = graph->add_output_node(1, "audio");
    REQUIRE(graph->connect(graph->node(input)->out("audio"), source.value->morph));
    REQUIRE(graph->connect(source.value->frames, identity.value->frames_in));
    REQUIRE(graph->connect(identity.value->frames_out, mask.value->frames_in));
    REQUIRE(graph->connect(mask.value->frames_out, synthesis.value->frames));
    REQUIRE(graph->connect(synthesis.value->audio[0], graph->node(output)->in("audio")));
    return graph;
}
std::vector<float> morph_control() {
    std::vector<float> result(morph_inputs().a.size() * 64 + 1024);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = float((i * 13 % 257) / 256.0);
    return result;
}

void replace(std::string& json, const std::string& from, const std::string& to) {
    const auto pos = json.find(from);
    INFO(from);
    REQUIRE(pos != std::string::npos);
    json.replace(pos, from.size(), to);
}

// A prepared one-channel passthrough the refusal tests must leave intact.
struct Bystander {
    SignalGraph graph;
    std::string before;
    Bystander() {
        const auto in = graph.add_input_node(1, "kept_in");
        const auto out = graph.add_output_node(1, "kept_out");
        REQUIRE(graph.connect(graph.node(in)->out("audio"), graph.node(out)->in("audio")));
        REQUIRE(graph.prepare(48000, 4096));
        before = GraphSerializer::to_saved_document(graph);
        REQUIRE_FALSE(before.empty());
    }
    void untouched() {
        REQUIRE(GraphSerializer::to_saved_document(graph) == before);
        std::vector<float> ramp(300);
        for (std::size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = float(i) / 300.0f;
        const auto audio = render(graph, {ramp}, 1, ramp.size(), 64);
        REQUIRE(audio[0] == ramp);
    }
};

void refused_untouched(Bystander& old, const std::string& document) {
    for (const bool saved : {false, true}) {
        CAPTURE(saved);
        const auto result = saved ? GraphSerializer::from_saved_document(old.graph, document)
                                  : GraphSerializer::from_json(old.graph, document);
        INFO(result.error);
        REQUIRE_FALSE(result.ok);
        REQUIRE_FALSE(result.error.empty());
        REQUIRE(result.persisted_node_ids.empty());
        REQUIRE(result.editor_layout.empty());
        REQUIRE(result.identity_node_ids.empty());
        old.untouched();
    }
}

std::string node_authored_id(const SignalGraph& graph, const std::string& name) {
    for (const auto& n : graph.nodes())
        if (n.name == name)
            return n.authored_id.str();
    FAIL("no node named " << name);
    return {};
}
} // namespace

TEST_CASE("Spectral graph saves and restores the saved-identity v7 document",
          "[host][spectral][saved-identity]") {
    auto graph = morph_fixture();
    const auto layout_id = graph->nodes().front().id;
    const auto saved = GraphSerializer::to_saved_document(*graph, {{layout_id, {3.5f, -1.25f}}});
    REQUIRE_FALSE(saved.empty());
    const auto root = choc::json::parse(saved);
    REQUIRE(root["format_version"].getInt64() == 7);
    REQUIRE(root["identity_schema"].getString() == kV7);
    REQUIRE(root["spectral_assets"].size() == 2);

    // The common inspection admits it as a v7 document.
    const auto inspection = GraphSerializer::inspect_document(saved);
    INFO(inspection.detail);
    REQUIRE(inspection.ok());
    REQUIRE(inspection.document.profile() == kV7);

    SignalGraph loaded;
    const auto result = GraphSerializer::from_saved_document(loaded, saved);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(result.identity_node_ids.size() == graph->nodes().size());
    REQUIRE(result.persisted_node_ids.size() == graph->nodes().size());
    REQUIRE(result.editor_layout.at(result.persisted_node_ids.at(layout_id)) ==
            std::pair{3.5f, -1.25f});
    // Every node keeps the authored ID it was saved with, and the document
    // the restored graph writes is byte-identical.
    for (const auto& n : graph->nodes())
        REQUIRE(node_authored_id(loaded, n.name) == n.authored_id.str());
    REQUIRE(GraphSerializer::to_saved_document(loaded, result.editor_layout) == saved);
    REQUIRE(diff_graph_documents(saved, GraphSerializer::to_saved_document(loaded)).equal());

    // The restored graph plays the independent morph oracle's audio.
    REQUIRE(loaded.prepare(48000, 4096));
    const auto control = morph_control();
    const auto audio = render(loaded, {control}, 1, control.size(), 0);
    REQUIRE_NOTHROW(asset_oracle::compare(
        audio[0], asset_oracle::expected(morph_inputs().a, morph_inputs().b, control)));
    clean_diagnostics(loaded);

    // Format 6 stays the plain writer's spectral document.
    const auto plain = GraphSerializer::to_json(*graph);
    REQUIRE(choc::json::parse(plain)["format_version"].getInt64() == 6);
    REQUIRE(plain.find("authored_id") == std::string::npos);
}

TEST_CASE("Format 6 spectral documents migrate to v7 with deterministic authored IDs",
          "[host][spectral][saved-identity][migration]") {
    const auto v6 = GraphSerializer::to_json(*morph_fixture());
    REQUIRE_FALSE(v6.empty());
    SignalGraph first, second;
    REQUIRE(GraphSerializer::from_json(first, v6).ok);
    REQUIRE(GraphSerializer::from_json(second, v6).ok);
    // The same format 6 document always restores the same IDs, and the v7
    // documents written from either load are byte-identical.
    const auto migrated = GraphSerializer::to_saved_document(first);
    REQUIRE_FALSE(migrated.empty());
    REQUIRE(migrated == GraphSerializer::to_saved_document(second));
    // The migrated v7 document restores those IDs exactly.
    SignalGraph third;
    const auto restored = GraphSerializer::from_saved_document(third, migrated);
    INFO(restored.error);
    REQUIRE(restored.ok);
    for (const auto& n : first.nodes())
        REQUIRE(node_authored_id(third, n.name) == n.authored_id.str());
    // Continuity is never guessed across documents: any difference in the
    // format 6 source changes every migrated ID.
    auto edited = v6;
    replace(edited, "\"name\": \"synthesis\"", "\"name\": \"synthesis_renamed\"");
    SignalGraph other;
    REQUIRE(GraphSerializer::from_json(other, edited).ok);
    REQUIRE(node_authored_id(other, "assets") != node_authored_id(first, "assets"));
    // A format 6 record carrying an authored_id is refused; only v7 has them.
    auto forged = v6;
    replace(forged, "\"type\": \"spectral_synthesis\"",
            "\"authored_id\": \"nid-forged\", \"type\": \"spectral_synthesis\"");
    Bystander old;
    refused_untouched(old, forged);
    // The migrated document plays the same audio as the format 6 load.
    REQUIRE(first.prepare(48000, 4096));
    REQUIRE(third.prepare(48000, 4096));
    const auto control = morph_control();
    REQUIRE(render(first, {control}, 1, control.size(), 64) ==
            render(third, {control}, 1, control.size(), 64));
}

TEST_CASE("Analysis to stage to synthesis audio survives a saved and restored graph",
          "[host][spectral][saved-identity][analysis]") {
    // A hop that does not divide the FFT, two channels, and a sample rate
    // whose exact bits a lossy decimal writer would round away.
    const Geometry g{256, 100, 2};
    const auto gains = mask_gains(g);
    AnalysisGraph fixture(g, odd_rate, gains);
    const auto saved = GraphSerializer::to_saved_document(*fixture.graph);
    REQUIRE_FALSE(saved.empty());
    // The analysis node saves its declaration only.
    const auto root = choc::json::parse(saved);
    REQUIRE(root["spectral_assets"].size() == 0);
    for (const auto n : root["nodes"]) {
        if (n["type"].getString() != "spectral_analysis")
            continue;
        const auto spectral = n["spectral"];
        REQUIRE(spectral.size() == 2);
        REQUIRE(spectral["identity"]["fft_size"].getInt64() == 256);
        REQUIRE(spectral["identity"]["hop"].getInt64() == 100);
        REQUIRE(spectral["identity"]["channels"].getInt64() == 2);
        char bits[17];
        std::snprintf(bits, sizeof bits, "%016llx",
                      static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(odd_rate)));
        REQUIRE(spectral["identity"]["sample_rate_bits"].getString() == std::string(bits));
    }

    SignalGraph loaded;
    const auto result = GraphSerializer::from_saved_document(loaded, saved);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(GraphSerializer::to_saved_document(loaded) == saved);
    // Geometry and the exact rate bits survive; preparation at the rounded
    // rate is refused, at the recorded rate admitted.
    for (const auto& n : loaded.nodes())
        if (n.spectral_identity) {
            REQUIRE(n.spectral_identity->fft_size() == 256);
            REQUIRE(n.spectral_identity->hop() == 100);
            REQUIRE(n.spectral_identity->channels() == 2);
            REQUIRE(n.spectral_identity->sample_rate_bits() ==
                    std::bit_cast<std::uint64_t>(odd_rate));
        }
    REQUIRE_FALSE(loaded.prepare(44100.0, 4096));
    REQUIRE(loaded.prepare(odd_rate, 4096));

    const std::size_t length = 1500;
    const auto input = oracle::stimulus(g, length);
    const std::size_t delay = g.fft + g.hop;
    const std::size_t total = length + delay + 3 * g.fft;
    // Independent oracle: the analysis grid DFT of the input, masked per bin,
    // then inverse DFT and squared-window OLA after the N + H delay.
    auto frames = oracle::analyze(g, input);
    for (auto& frame : frames)
        for (std::size_t i = 0; i < frame.size(); ++i)
            frame[i] *= double(gains[i]);
    const auto wanted = oracle::resynthesize(g, frames, total);
    // Negative control: the unmasked reconstruction is far from the wanted
    // signal, so matching it proves the restored mask gains took effect.
    const auto unmasked = oracle::resynthesize(g, oracle::analyze(g, input), total);
    double separation = 0;
    for (int c = 0; c < g.channels; ++c)
        for (std::size_t t = 0; t < total; ++t)
            separation = std::max(separation, std::abs(unmasked[c][t] - wanted[c][t]));
    REQUIRE(separation > 0.05);

    // Each partition renders a freshly restored graph and a freshly built
    // original from time zero.
    oracle::Audio reference;
    for (int partition : {64, 100, 0}) {
        CAPTURE(partition);
        SignalGraph restored_graph;
        REQUIRE(GraphSerializer::from_saved_document(restored_graph, saved).ok);
        REQUIRE(restored_graph.prepare(odd_rate, 4096));
        const auto restored = render(restored_graph, input, g.channels, total, partition);
        oracle::compare(restored, wanted, 1e-4, 1e-5);
        REQUIRE((reference.empty() || restored == reference));
        reference = restored;
        AnalysisGraph original(g, odd_rate, gains);
        REQUIRE(original.graph->prepare(odd_rate, 4096));
        REQUIRE(restored == render(*original.graph, input, g.channels, total, partition));
        clean_diagnostics(restored_graph);
    }
}

TEST_CASE("Single-stream asset source persists with its finite-source policy",
          "[host][spectral][saved-identity][asset-source]") {
    const Geometry g{256, 96, 2};
    const std::uint64_t length = 1000;
    SourceGraph fixture(g, length);
    // The source factory mints an authored ID like every other node factory,
    // and a second source gets a different one.
    const auto& minted = fixture.graph->node(fixture.source.node)->authored_id;
    REQUIRE_FALSE(minted.empty());
    const auto second =
        fixture.graph->add_spectral_asset_source_v1(fixture.bytes.view(), length, "second");
    REQUIRE(second);
    REQUIRE_FALSE(fixture.graph->node(second.value->node)->authored_id.empty());
    REQUIRE(fixture.graph->node(second.value->node)->authored_id != minted);
    REQUIRE(fixture.graph->remove_node(second.value->node));
    // The format 6 document cannot carry the source and says what to use.
    const pulp::test::ScopedTempDir dir("pulp-spectral-source-save");
    const auto plain = GraphSerializer::to_file(*fixture.graph, dir.path / "g.pulpgraph");
    REQUIRE_FALSE(plain.ok);
    REQUIRE(plain.error.find("single-stream spectral asset source") != std::string::npos);
    REQUIRE(plain.error.find("to_saved_document") != std::string::npos);

    const auto saved = GraphSerializer::to_saved_document(*fixture.graph);
    REQUIRE_FALSE(saved.empty());
    const auto root = choc::json::parse(saved);
    REQUIRE(root["spectral_assets"].size() == 1);
    bool recorded = false;
    for (const auto n : root["nodes"])
        if (n["type"].getString() == "spectral_asset_source") {
            REQUIRE(n["spectral"]["finite_policy"].getInt64() == 2);
            recorded = true;
        }
    REQUIRE(recorded);

    SignalGraph loaded;
    const auto result = GraphSerializer::from_saved_document(loaded, saved);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(GraphSerializer::to_saved_document(loaded) == saved);
    const std::size_t delay = g.fft + g.hop;
    const auto cutoff = delay + std::size_t(oracle::last_window_end(g, length));
    const std::size_t total = cutoff + 3 * g.fft + 101;
    const auto wanted = oracle::resynthesize(g, oracle::rounded(fixture.exact), total);
    for (int partition : {64, 0}) {
        CAPTURE(partition);
        SignalGraph restored_graph;
        REQUIRE(GraphSerializer::from_saved_document(restored_graph, saved).ok);
        REQUIRE(restored_graph.prepare(48000, 4096));
        const auto restored = render(restored_graph, {}, g.channels, total, partition);
        oracle::compare(restored, wanted, 2e-5, 2e-6);
        SourceGraph original(g, length);
        REQUIRE(original.graph->prepare(48000, 4096));
        REQUIRE(restored == render(*original.graph, {}, g.channels, total, partition));
        for (int c = 0; c < g.channels; ++c)
            for (std::size_t t = cutoff; t < total; ++t)
                REQUIRE(restored[c][t] == 0);
        clean_diagnostics(restored_graph);
    }

    // The recorded policy must be the one the source factory applies.
    Bystander old;
    for (const auto& policy : {"1", "3", "\"2\""}) {
        auto broken = saved;
        replace(broken, "\"finite_policy\": 2", std::string("\"finite_policy\": ") + policy);
        refused_untouched(old, broken);
    }
    auto missing = choc::json::parse(saved);
    auto nodes = choc::value::createEmptyArray();
    for (const auto n : missing["nodes"]) {
        auto record = choc::value::Value(n);
        if (n["type"].getString() == "spectral_asset_source") {
            auto spectral = choc::value::createObject("SpectralNode");
            for (const auto* key : {"contract_hex", "asset"})
                spectral.addMember(key, std::string(n["spectral"][key].getString()));
            spectral.addMember("identity", choc::value::Value(n["spectral"]["identity"]));
            record.setMember("spectral", spectral);
        }
        nodes.addArrayElement(record);
    }
    missing.setMember("nodes", nodes);
    refused_untouched(old, choc::json::toString(missing, true));
}

TEST_CASE("Malformed v7 spectral documents are refused before the caller's graph is touched",
          "[host][spectral][saved-identity][negative]") {
    const Geometry g{256, 100, 2};
    AnalysisGraph analysis(g, odd_rate, mask_gains(g));
    const auto with_analysis = GraphSerializer::to_saved_document(*analysis.graph);
    SourceGraph source(Geometry{256, 96, 1}, 700);
    const auto with_source = GraphSerializer::to_saved_document(*source.graph);
    REQUIRE_FALSE(with_analysis.empty());
    REQUIRE_FALSE(with_source.empty());
    Bystander old;
    // Positive controls: both documents load into a fresh graph.
    for (const auto* document : {&with_analysis, &with_source}) {
        SignalGraph fresh;
        const auto loaded = GraphSerializer::from_saved_document(fresh, *document);
        INFO(loaded.error);
        REQUIRE(loaded.ok);
    }
    const auto analysis_id = node_authored_id(*analysis.graph, "analysis");
    const auto stage_id = node_authored_id(*analysis.graph, "mask");
    char bits[17];
    std::snprintf(bits, sizeof bits, "%016llx",
                  static_cast<unsigned long long>(std::bit_cast<std::uint64_t>(odd_rate)));
    std::string other_bits = bits;
    other_bits.back() = other_bits.back() == '0' ? '1' : '0';

    const std::vector<std::pair<std::string, std::string>> analysis_mutations{
        // Unknown and legacy versions.
        {"\"format_version\": 7", "\"format_version\": 8"},
        {"\"format_version\": 7", "\"format_version\": 6"},
        {"\"identity_schema\": \"pulp.graph.saved-identity.v7\"",
         "\"identity_schema\": \"pulp.graph.saved-identity.v9\""},
        // Malformed or inconsistent authored identity.
        {"\"authored_id\": \"" + analysis_id + "\"", "\"authored_id\": \"nid-!\""},
        {"\"authored_id\": \"" + analysis_id + "\"", "\"authored_id\": \"" + stage_id + "\""},
        // The readable identity must match the frame contract exactly.
        {std::string("\"sample_rate_bits\": \"") + bits + "\"",
         "\"sample_rate_bits\": \"" + other_bits + "\""},
        {"\"hop\": 100", "\"hop\": 64"},
        {"\"contract_hex\": \"", "\"contract_hex\": \"ff"},
        // Invalid typed edges.
        {"\"spectral\": true", "\"spectral\": false"},
        {"\"control_signal\": false", "\"control_signal\": true"},
        {"\"control_signal\": false,", ""},
        {"\"dest_port\": 1", "\"dest_port\": 7"},
        // The analysis record holds its declaration and nothing else.
        {"\"type\": \"spectral_analysis\"",
         "\"type\": \"spectral_analysis\", \"captured_audio\": []"},
        {"\"type\": \"spectral_analysis\"", "\"type\": \"spectral_synthesis\""},
        // Mixed graph families.
        {"\"spectral_assets\": [", "\"subgraphs\": [], \"spectral_assets\": ["},
        {"\"type\": \"audio_in\"", "\"type\": \"gain\""},
        {"\"spectral_assets\": [", "\"processors\": [{\"node_id\": 1}], \"spectral_assets\": ["},
    };
    for (const auto& [from, to] : analysis_mutations) {
        CAPTURE(from, to);
        auto broken = with_analysis;
        replace(broken, from, to);
        refused_untouched(old, broken);
    }
    // The readable identity needs the analysis record's spectral member; a
    // record without it is refused too.
    {
        auto broken = choc::json::parse(with_analysis);
        auto nodes = choc::value::createEmptyArray();
        for (const auto n : broken["nodes"]) {
            auto record = choc::value::Value(n);
            if (n["type"].getString() == "spectral_analysis") {
                auto spectral = choc::value::createObject("SpectralNode");
                spectral.addMember("contract_hex",
                                   std::string(n["spectral"]["contract_hex"].getString()));
                record.setMember("spectral", spectral);
            }
            nodes.addArrayElement(record);
        }
        broken.setMember("nodes", nodes);
        refused_untouched(old, choc::json::toString(broken, true));
    }

    const std::vector<std::pair<std::string, std::string>> source_mutations{
        // Hash and asset corruption.
        {"\"bytes_sha256\": \"", "\"bytes_sha256\": \"0"},
        {"\"bytes_b64\": \"", "\"bytes_b64\": \"AAAA"},
        {"\"asset\": \"", "\"asset\": \"missing-"},
        {"\"frame_count\": ", "\"frame_count\": 1"},
        {"\"source_samples\": 700", "\"source_samples\": 701"},
        {"\"version\": 1", "\"version\": 2"},
    };
    for (const auto& [from, to] : source_mutations) {
        CAPTURE(from, to);
        auto broken = with_source;
        replace(broken, from, to);
        refused_untouched(old, broken);
    }
    // Valid alternative canonical bytes under the original hash: the digest
    // check, not codec validation, refuses them.
    {
        SourceGraph other(Geometry{256, 96, 1}, 700);
        other.input[0][5] += 0.5f;
        const auto bytes = oracle::encode(other.g, oracle::analyze(other.g, other.input));
        auto substituted = with_source;
        replace(substituted,
                pulp::runtime::base64_encode(source.bytes.bytes.get(), source.bytes.size),
                pulp::runtime::base64_encode(bytes.bytes.get(), bytes.size));
        refused_untouched(old, substituted);
    }
    // Resource exhaustion: an asset table past its bound is refused before any
    // row is decoded.
    {
        auto broken = choc::json::parse(with_source);
        auto rows = choc::value::createEmptyArray();
        for (int i = 0; i < 257; ++i)
            rows.addArrayElement(choc::value::Value(broken["spectral_assets"][0]));
        broken.setMember("spectral_assets", rows);
        const auto document = choc::json::toString(broken, true);
        const auto result = GraphSerializer::from_json(old.graph, document);
        REQUIRE_FALSE(result.ok);
        REQUIRE(result.error.find("oversized") != std::string::npos);
        old.untouched();
    }
}

TEST_CASE("Spectral v7 loads refuse resource exhaustion and injected allocation failures",
          "[host][spectral][saved-identity][negative][allocation]") {
    const Geometry g{256, 100, 2};
    AnalysisGraph analysis(g, odd_rate, mask_gains(g));
    const auto saved = GraphSerializer::to_saved_document(*analysis.graph);
    REQUIRE_FALSE(saved.empty());
    Bystander old;
    // An allocation failure while building the isolated candidate.
    GraphSerializerTestAccess::fail_next_spectral_candidate_build();
    auto result = GraphSerializer::from_json(old.graph, saved);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("allocation failure") != std::string::npos);
    REQUIRE(result.persisted_node_ids.empty());
    old.untouched();
    // An allocation failure while planning the restored authored IDs.
    GraphSerializerTestAccess::fail_next_authored_id_plan();
    result = GraphSerializer::from_saved_document(old.graph, saved);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("authored_id planning failed") != std::string::npos);
    REQUIRE(result.identity_node_ids.empty());
    old.untouched();
    // Control: each hook fires once, so the same document then loads.
    result = GraphSerializer::from_saved_document(old.graph, saved);
    INFO(result.error);
    REQUIRE(result.ok);

    // Destination limits refuse before replacement.
    Bystander limited;
    auto limits = limited.graph.limits();
    limits.max_nodes = 2;
    limited.graph.set_limits(limits);
    // New limits retire the prepared plan; prepare again under them.
    REQUIRE(limited.graph.prepare(48000, 4096));
    result = GraphSerializer::from_json(limited.graph, saved);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("destination graph limits") != std::string::npos);
    limited.untouched();

    // The spectral bank bound at the destination's admission block: FFT 1024,
    // hop 8, sixteen channels fits 64 MiB at 512-sample blocks but not at 4096.
    AnalysisGraph big(Geometry{1024, 8, 16}, 48000, mask_gains(Geometry{1024, 8, 16}));
    REQUIRE_FALSE(big.graph->prepare(48000, 4096));
    REQUIRE(big.graph->prepare(48000, 512));
    REQUIRE(big.graph->stop_spectral_controller() == SpectralError::None);
    const auto large = GraphSerializer::to_saved_document(*big.graph);
    REQUIRE_FALSE(large.empty());
    SignalGraph bounded;
    limits = bounded.limits();
    limits.max_block_size = 512;
    bounded.set_limits(limits);
    result = GraphSerializer::from_saved_document(bounded, large);
    INFO(result.error);
    REQUIRE(result.ok);
    Bystander unbounded;
    refused_untouched(unbounded, large);
}

TEST_CASE("Spectral v7 documents inspect and diff through the common path",
          "[host][spectral][saved-identity][diff]") {
    const Geometry g{256, 100, 2};
    AnalysisGraph fixture(g, odd_rate, mask_gains(g));
    auto& graph = *fixture.graph;
    const auto before = GraphSerializer::to_saved_document(graph, {{fixture.stage, {0, 0}}});
    REQUIRE(GraphSerializer::inspect_document(before).ok());
    REQUIRE(diff_graph_documents(before, before).equal());

    // Layout-only: equal, with a layout change under the stage's authored ID.
    const auto moved = GraphSerializer::to_saved_document(graph, {{fixture.stage, {9, 9}}});
    auto diff = diff_graph_documents(before, moved);
    REQUIRE(diff.ok());
    REQUIRE(diff.equal());
    REQUIRE(diff.layout_changed == std::vector<std::string>{node_authored_id(graph, "mask")});

    // A rename changes the name field under the same identity.
    REQUIRE(graph.rename_node(fixture.analysis, "renamed"));
    const auto renamed = GraphSerializer::to_saved_document(graph, {{fixture.stage, {0, 0}}});
    diff = diff_graph_documents(before, renamed);
    REQUIRE(diff.ok());
    REQUIRE(diff.nodes_added.empty());
    REQUIRE(diff.nodes_removed.empty());
    REQUIRE(diff.nodes_changed.size() == 1);
    REQUIRE(diff.nodes_changed[0].identity == node_authored_id(graph, "renamed"));
    REQUIRE(diff.nodes_changed[0].fields == std::vector<std::string>{"name"});

    // A different mask is a change to the stage's spectral field.
    AnalysisGraph other(g, odd_rate, std::vector<float>(mask_gains(g).size(), 1.0f));
    const auto replaced = GraphSerializer::to_saved_document(*other.graph);
    diff = diff_graph_documents(before, replaced);
    REQUIRE(diff.ok());
    // An independently built graph mints disjoint authored IDs.
    REQUIRE(diff.nodes_added.size() == 5);
    REQUIRE(diff.nodes_removed.size() == 5);

    // Same identities, edited mask: restore, swap the stage's gains in the
    // document and compare.
    auto edited = choc::json::parse(before);
    auto nodes = choc::value::createEmptyArray();
    for (const auto n : edited["nodes"]) {
        auto record = choc::value::Value(n);
        if (n["type"].getString() == "spectral_stage") {
            auto spectral = choc::value::Value(n["spectral"]);
            auto gains = choc::value::createEmptyArray();
            for (std::uint32_t i = 0; i < n["spectral"]["gains"].size(); ++i)
                gains.addArrayElement(1.0);
            spectral.setMember("gains", gains);
            record.setMember("spectral", spectral);
        }
        nodes.addArrayElement(record);
    }
    edited.setMember("nodes", nodes);
    const auto edited_json = choc::json::toString(edited, true);
    SignalGraph check;
    const auto loaded = GraphSerializer::from_saved_document(check, edited_json);
    INFO(loaded.error);
    REQUIRE(loaded.ok);
    diff = diff_graph_documents(before, edited_json);
    REQUIRE(diff.ok());
    REQUIRE(diff.nodes_changed.size() == 1);
    REQUIRE(diff.nodes_changed[0].fields == std::vector<std::string>{"spectral"});

    // A format 6 spectral document has no identity envelope, as for any
    // plain document; it must be migrated by loading and saving first.
    const auto plain = GraphSerializer::to_json(*morph_fixture());
    REQUIRE(diff_graph_documents(plain, plain).refusal ==
            GraphDocumentRefusal::LogicalIdentityMissing);
}

TEST_CASE("Spectral graph families stay closed to history and mixed nodes",
          "[host][spectral][saved-identity][boundary]") {
    const Geometry g{256, 100, 2};
    AnalysisGraph fixture(g, odd_rate, mask_gains(g));
    // History needs a retained-byte bound for embedded assets, so a spectral
    // graph is not captured even though it has a saved document.
    REQUIRE_FALSE(GraphSerializer::to_saved_document(*fixture.graph).empty());
    REQUIRE_FALSE(GraphEditHistory::checkpoint(*fixture.graph).valid());
    // Control: an ordinary graph is captured.
    SignalGraph plain;
    plain.add_input_node(1, "in");
    REQUIRE(GraphEditHistory::checkpoint(plain).valid());
    // Subgraph records do not mix with a spectral graph. Control: the same
    // record shape is written for an ordinary graph, and the spectral graph
    // writes without records.
    SubgraphInstance instance;
    instance.template_identity = "template";
    instance.instance_identity = "instance";
    instance.nodes = {plain.nodes().front().id};
    REQUIRE_FALSE(to_saved_subgraph_document(plain, {&instance, 1}).empty());
    REQUIRE_FALSE(to_saved_subgraph_document(*fixture.graph, {}).empty());
    instance.nodes = {fixture.graph->nodes().front().id};
    REQUIRE(to_saved_subgraph_document(*fixture.graph, {&instance, 1}).empty());
    // A non-spectral node makes the graph a mixed family, which no spectral
    // document carries.
    fixture.graph->add_gain_node("mixed");
    REQUIRE(GraphSerializer::to_saved_document(*fixture.graph).empty());
    REQUIRE(GraphSerializer::to_json(*fixture.graph).empty());
}
