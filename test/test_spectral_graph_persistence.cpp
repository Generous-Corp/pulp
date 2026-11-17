#include "../examples/g10-routed-consumer/asset_morph_oracle.hpp"
#include "support/unique_temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/crypto.hpp>

#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <string>

namespace {
using namespace pulp::host;
struct Inputs {
    std::vector<asset_oracle::Frame> a = asset_oracle::analyze(asset_oracle::stimulus(0));
    std::vector<asset_oracle::Frame> b = asset_oracle::analyze(asset_oracle::stimulus(1));
    pulp::signal::EncodedSpectralStream aa = asset_oracle::encode(a), bb = asset_oracle::encode(b);
};
const Inputs& inputs() {
    static Inputs value;
    return value;
}
std::unique_ptr<SignalGraph> fixture(bool stages = false, const char* identity_name = "identity") {
    auto graph = std::make_unique<SignalGraph>();
    const auto input = graph->add_input_node(1, "control");
    const auto source =
        graph->add_spectral_asset_morph_v1(inputs().aa.view(), inputs().bb.view(), 769, "assets");
    REQUIRE(source);
    const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
    auto frames = source.value->frames;
    if (stages) {
        const auto identity = graph->add_spectral_stage(
            shape, *QualifiedSpectralStage::identity_v1(shape).value, identity_name);
        REQUIRE(identity);
        REQUIRE(graph->connect(frames, identity.value->frames_in));
        frames = identity.value->frames_out;
        std::vector<float> gains(129, 1.0f);
        const auto mask = graph->add_spectral_stage(
            shape, *QualifiedSpectralStage::real_mask_v1(shape, gains).value, "mask");
        REQUIRE(mask);
        REQUIRE(graph->connect(frames, mask.value->frames_in));
        frames = mask.value->frames_out;
    }
    const auto synthesis = graph->add_spectral_synthesis(shape, "synthesis");
    REQUIRE(synthesis);
    const auto output = graph->add_output_node(1, "audio");
    REQUIRE(graph->connect(graph->node(input)->out("audio"), source.value->morph));
    REQUIRE(graph->connect(frames, synthesis.value->frames));
    REQUIRE(graph->connect(synthesis.value->audio[0], graph->node(output)->in("audio")));
    return graph;
}
std::vector<float> timeline(float constant = -1) {
    std::vector<float> result(inputs().a.size() * 64 + 1024);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = constant < 0 ? float((i * 13 % 257) / 256.0) : constant;
    return result;
}
std::vector<float> render(SignalGraph& graph, const std::vector<float>& control, int partition,
                          bool reset = false) {
    std::vector<float> audio(control.size());
    for (std::size_t t = 0; t < control.size();) {
        const int count = static_cast<int>(
            std::min(control.size() - t, std::size_t(partition ? partition : 1 + t * 17 % 511)));
        const float* in[] = {control.data() + t};
        float* out[] = {audio.data() + t};
        pulp::audio::BufferView<float> ov(out, 1, count);
        pulp::audio::BufferView<const float> iv(in, 1, count);
        pulp::format::ProcessContext transport;
        transport.reset_requested = reset && t == 0;
        transport.position_samples = 99999;
        graph.process(ov, iv, count, transport);
        t += count;
    }
    return audio;
}
std::string hex(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    for (auto b : bytes) {
        out += "0123456789abcdef"[b >> 4];
        out += "0123456789abcdef"[b & 15];
    }
    return out;
}
// Re-extend both saved assets to `length` with self-consistent content IDs and
// references, so only the stored frame count can disagree with the stream rule.
std::string with_extent(const std::string& json, std::uint64_t length, std::uint64_t frames,
                        const std::string& contract_override = {}) {
    auto root = choc::json::parse(json);
    auto assets = choc::value::createEmptyArray();
    std::map<std::string, std::string> renamed;
    for (const auto row : root["spectral_assets"]) {
        auto record = choc::value::Value(row);
        const auto contract = contract_override.empty()
                                  ? std::string(row["contract_hex"].getString())
                                  : contract_override;
        const auto material = std::string("pulp.analyzed-spectral-asset.v1\n") +
                              std::to_string(length) + "\n" + contract + "\n" +
                              std::string(row["bytes_sha256"].getString());
        const auto id = hex(pulp::runtime::sha256(material));
        renamed.emplace(std::string(row["id"].getString()), id);
        record.setMember("id", id);
        record.setMember("source_samples", static_cast<int64_t>(length));
        record.setMember("frame_count", static_cast<int64_t>(frames));
        record.setMember("contract_hex", contract);
        assets.addArrayElement(record);
    }
    root.setMember("spectral_assets", assets);
    auto nodes = choc::value::createEmptyArray();
    for (const auto row : root["nodes"]) {
        auto node = choc::value::Value(row);
        if (row.hasObjectMember("spectral") && row["spectral"].hasObjectMember("asset_a")) {
            auto spectral = choc::value::Value(row["spectral"]);
            spectral.setMember("asset_a",
                               renamed.at(std::string(row["spectral"]["asset_a"].getString())));
            spectral.setMember("asset_b",
                               renamed.at(std::string(row["spectral"]["asset_b"].getString())));
            if (!contract_override.empty())
                spectral.setMember("contract_hex", contract_override);
            node.setMember("spectral", spectral);
        }
        nodes.addArrayElement(node);
    }
    root.setMember("nodes", nodes);
    return choc::json::toString(root, true);
}
void replace(std::string& json, const std::string& from, const std::string& to) {
    const auto pos = json.find(from);
    REQUIRE(pos != std::string::npos);
    json.replace(pos, from.size(), to);
}
} // namespace

TEST_CASE("Spectral saved graph reproduces independent audio and absolute live hop chronology",
          "[host][spectral][replay]") {
    const auto original = fixture();
    const auto json = GraphSerializer::to_json(*original);
    REQUIRE_FALSE(json.empty());
    REQUIRE(json.find("\"format_version\": 6") != std::string::npos);
    for (const auto& control : {timeline(), timeline(0), timeline(1), timeline(0.37f)}) {
        const auto expected = asset_oracle::expected(inputs().a, inputs().b, control);
        std::vector<float> reference;
        for (int partition : {32, 64, 128, 0}) {
            SignalGraph loaded;
            const auto result = GraphSerializer::from_json(loaded, json);
            INFO(result.error);
            REQUIRE(result.ok);
            REQUIRE(GraphSerializer::to_json(loaded) == json);
            REQUIRE(loaded.prepare(48000, 4096));
            REQUIRE(loaded.latency_samples() == 320);
            const auto audio = render(loaded, control, partition);
            REQUIRE_NOTHROW(asset_oracle::compare(audio, expected));
            REQUIRE((reference.empty() || reference == audio));
            reference = audio;
            for (std::size_t i = 0; i < audio.size(); ++i)
                if (i < 320 || i >= 320 + inputs().a.size() * 64)
                    REQUIRE(audio[i] == 0);
            REQUIRE(loaded.routed_walk_fallbacks() == 0);
            for (const auto& d : loaded.poll_live_dsp_telemetry().spectral)
                REQUIRE(d.first_error == 0);
            REQUIRE(render(loaded, control, partition, true) == audio);
        }
    }
}

TEST_CASE("Spectral identity and real-mask stages persist through existing typed factories",
          "[host][spectral][replay]") {
    auto graph = fixture(true);
    const auto json = GraphSerializer::to_json(*graph);
    REQUIRE_FALSE(json.empty());
    SignalGraph loaded;
    REQUIRE(GraphSerializer::from_json(loaded, json).ok);
    REQUIRE(GraphSerializer::to_json(loaded) == json);
    REQUIRE(loaded.prepare(48000, 4096));
    const auto control = timeline();
    REQUIRE_NOTHROW(asset_oracle::compare(render(loaded, control, 0),
                                          asset_oracle::expected(inputs().a, inputs().b, control)));
}

TEST_CASE("Spectral replay matches independent numeric audio without a serialization proxy",
          "[host][spectral][a3-replay-audio-oracle]") {
    const auto original = fixture();
    const auto saved = GraphSerializer::to_json(*original);
    REQUIRE_FALSE(saved.empty());
    SignalGraph loaded;
    REQUIRE(GraphSerializer::from_json(loaded, saved).ok);
    REQUIRE(loaded.prepare(48000, 4096));
    const auto control = timeline();
    REQUIRE_NOTHROW(asset_oracle::compare(render(loaded, control, 0),
                                          asset_oracle::expected(inputs().a, inputs().b, control)));
}

TEST_CASE("Saved-identity envelopes remain unsupported for spectral graphs",
          "[host][spectral][replay][saved-identity]") {
    const auto graph = fixture();
    CHECK(GraphSerializer::to_saved_document(*graph).empty());
}

TEST_CASE("Spectral asset corruption and unsupported contracts preserve the old prepared graph",
          "[host][spectral][replay][negative]") {
    const auto original = fixture();
    const auto good = GraphSerializer::to_json(*original);
    REQUIRE_FALSE(good.empty());
    SignalGraph positive;
    REQUIRE(GraphSerializer::from_json(positive, good).ok);
    const std::vector<std::pair<std::string, std::string>> mutations{
        {"\"format_version\": 6", "\"format_version\": 4"},
        {"\"format_version\": 6", "\"format_version\": 5"},
        {"\"format_version\": 6", "\"format_version\": 7"},
        {"pulp.analyzed-spectral-asset", "pulp.unsupported-spectral-asset"},
        {"\"version\": 1", "\"version\": 2"},
        {"\"source_samples\": 769", "\"source_samples\": 770"},
        {"\"frame_count\": 16", "\"frame_count\": 17"},
        {"\"frame_count\": 16", "\"frame_count\": 0"},
        {"\"sidechain\": false", "\"sidechain\": false, \"control_signal\": true"},
        {"\"bytes_b64\": \"", "\"bytes_b64\": \"AAAA"},
        {"\"bytes_sha256\": \"", "\"bytes_sha256\": \"0"},
        {"\"source_samples\": 769", "\"unsupported_contract\": true, \"source_samples\": 769"},
        {"\"asset_a\": \"", "\"asset_a\": \"missing-"},
        {"\"contract_hex\": \"", "\"contract_hex\": \"ff"},
        {"\"spectral\": true", "\"spectral\": false"},
        {"\"feedback\": false", "\"feedback\": true"},
        {"\"feedback\": false", "\"unsupported_contract\": true, \"feedback\": false"},
        {"\"num_input_ports\": 0", "\"num_input_ports\": 1"},
        {"\"type\": \"spectral_synthesis\"", "\"type\": \"plugin\""},
    };
    SignalGraph old;
    const auto in = old.add_input_node(1), out = old.add_output_node(1);
    REQUIRE(old.connect(old.node(in)->out("audio"), old.node(out)->in("audio")));
    REQUIRE(old.prepare(48000, 4096));
    const auto before = GraphSerializer::to_json(old);
    for (const auto& [from, to] : mutations) {
        auto broken = good;
        replace(broken, from, to);
        const auto result = GraphSerializer::from_json(old, broken);
        INFO(from);
        INFO(result.error);
        REQUIRE_FALSE(result.ok);
        REQUIRE_FALSE(result.error.empty());
        REQUIRE(GraphSerializer::to_json(old) == before);
        const auto control = timeline(0.25f);
        REQUIRE(render(old, control, 64) == control);
    }
    // Use valid alternative canonical bytes, rather than malformed bytes that
    // the codec would reject even if production digest validation disappeared.
    auto substituted = good;
    replace(substituted, pulp::runtime::base64_encode(inputs().aa.bytes.get(), inputs().aa.size),
            pulp::runtime::base64_encode(inputs().bb.bytes.get(), inputs().bb.size));
    REQUIRE_FALSE(GraphSerializer::from_json(old, substituted).ok);
    REQUIRE(GraphSerializer::to_json(old) == before);
    // Both extents still fit the same canonical frame count. The asset ID,
    // rather than coincidental shape mismatch, must bind the original length.
    auto changed_extent = good;
    replace(changed_extent, "\"source_samples\": 769", "\"source_samples\": 770");
    replace(changed_extent, "\"source_samples\": 769", "\"source_samples\": 770");
    REQUIRE_FALSE(GraphSerializer::from_json(old, changed_extent).ok);
    REQUIRE(GraphSerializer::to_json(old) == before);
    // Insert a complete valid duplicate record; a malformed fixture would not
    // isolate the table's uniqueness and reference checks.
    const auto table = good.find("\"spectral_assets\": [");
    REQUIRE(table != std::string::npos);
    const auto first = good.find('{', table), end = good.find('}', first);
    REQUIRE(first != std::string::npos);
    REQUIRE(end != std::string::npos);
    const auto record = good.substr(first, end - first + 1);
    auto duplicate = good;
    duplicate.insert(end + 1, "," + record);
    REQUIRE_FALSE(GraphSerializer::from_json(old, duplicate).ok);
    REQUIRE(GraphSerializer::to_json(old) == before);
    auto missing = good;
    missing.erase(first, good.find(',', end) - first + 1);
    REQUIRE_FALSE(GraphSerializer::from_json(old, missing).ok);
    REQUIRE(GraphSerializer::to_json(old) == before);
    auto malformed = good.substr(0, good.size() / 2);
    REQUIRE_FALSE(GraphSerializer::from_json(old, malformed).ok);
    REQUIRE(GraphSerializer::to_json(old) == before);
}

TEST_CASE("Saved spectral asset geometry is checked against the finite stream rule",
          "[host][spectral][replay][negative][finite-stream-rule]") {
    const auto good = GraphSerializer::to_json(*fixture());
    REQUIRE_FALSE(good.empty());
    // 769 samples at FFT 256 / hop 64 is ceil((769 + 192) / 64) = 16 frames.
    REQUIRE(good.find("\"frame_count\": 16") != std::string::npos);
    // Positive control: the self-consistent rewrite at the original extent
    // loads, so the helper itself is not what refuses below.
    {
        SignalGraph loaded;
        const auto result = GraphSerializer::from_json(loaded, with_extent(good, 769, 16));
        INFO(result.error);
        REQUIRE(result.ok);
    }
    SignalGraph old;
    old.add_input_node(1, "retained");
    const auto before = GraphSerializer::to_json(old);
    // 833 samples derive 17 frames; the stored 16 must be refused by the rule
    // before any asset bytes reach the decoder or factory.
    auto result = GraphSerializer::from_json(old, with_extent(good, 833, 16));
    INFO(result.error);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("finite stream rule") != std::string::npos);
    REQUIRE(GraphSerializer::to_json(old) == before);
    // A frame contract the identity admits but the rule refuses outright: a
    // hop of 96 does not divide FFT 256, so no frame count is derivable.
    const auto encoded = SpectralIdentity::make(256, 96, 1, 48000).value->frame_contract().encode();
    result = GraphSerializer::from_json(
        old,
        with_extent(good, 769, 16, hex(std::vector<std::uint8_t>(encoded.begin(), encoded.end()))));
    INFO(result.error);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("finite stream rule") != std::string::npos);
    REQUIRE(GraphSerializer::to_json(old) == before);
    // A refused rule derives no frame count, so a stored zero must not match it.
    result = GraphSerializer::from_json(
        old,
        with_extent(good, 769, 0, hex(std::vector<std::uint8_t>(encoded.begin(), encoded.end()))));
    INFO(result.error);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("finite stream rule") != std::string::npos);
    REQUIRE(GraphSerializer::to_json(old) == before);
    // With the stored count matching the rule, the rule check passes and the
    // factory's frame-count binding refuses the 16-frame bytes instead.
    result = GraphSerializer::from_json(old, with_extent(good, 833, 17));
    INFO(result.error);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("finite stream rule") == std::string::npos);
    REQUIRE(GraphSerializer::to_json(old) == before);
}

TEST_CASE("Spectral candidate respects destination limits before authoring replacement",
          "[host][spectral][replay][limits]") {
    auto graph = fixture();
    const auto json = GraphSerializer::to_json(*graph);
    REQUIRE_FALSE(json.empty());
    SignalGraph old;
    old.add_input_node(1, "retained");
    const auto before = GraphSerializer::to_json(old);
    auto limits = old.limits();
    limits.max_nodes = 2;
    old.set_limits(limits);
    const auto result = GraphSerializer::from_json(old, json);
    REQUIRE_FALSE(result.ok);
    REQUIRE(result.error.find("destination graph limits") != std::string::npos);
    REQUIRE(GraphSerializer::to_json(old) == before);
}

TEST_CASE("Spectral saved graph retains editor layout and refuses mixed opaque state",
          "[host][spectral][replay]") {
    auto graph = fixture();
    const auto id = graph->nodes().front().id;
    const auto json = GraphSerializer::to_json(*graph, {{id, {12.5f, -9.25f}}});
    REQUIRE_FALSE(json.empty());
    SignalGraph loaded;
    const auto result = GraphSerializer::from_json(loaded, json);
    REQUIRE(result.ok);
    REQUIRE(result.editor_layout.at(id) == std::pair{12.5f, -9.25f});
    graph->add_gain_node("unsupported_mix");
    REQUIRE(GraphSerializer::to_json(*graph).empty());
}

namespace {
// The admission rule of the last pre-spectral reader (format 5): any integer
// format_version up to 5 is migrated forward and loaded, and anything newer is
// refused as "unsupported graph format_version". That reader resolves unknown
// spectral node types as unresolved Custom nodes and ignores spectral_assets,
// so a spectral save it admits would lose the assets on re-save.
bool pre_spectral_reader_admits(const std::string& json) {
    const auto root = choc::json::parse(json);
    const auto& version = root["format_version"];
    if (!version.isInt32() && !version.isInt64())
        return false;
    constexpr std::int64_t kLastPreSpectralFormatVersion = 5;
    return version.getInt64() <= kLastPreSpectralFormatVersion;
}
} // namespace

TEST_CASE("Spectral saves use a graph format the pre-spectral reader refuses",
          "[host][spectral][replay][compat]") {
    // Spectral documents stay at format 6; format 7 is the saved-identity v7
    // document, which a spectral graph does not have.
    REQUIRE(GraphSerializer::current_format_version() == 7);
    for (bool stages : {false, true}) {
        const auto json = GraphSerializer::to_json(*fixture(stages));
        REQUIRE_FALSE(json.empty());
        CHECK_FALSE(pre_spectral_reader_admits(json));
        SignalGraph loaded;
        const auto result = GraphSerializer::from_json(loaded, json);
        INFO(result.error);
        CHECK(result.ok);
    }

    // Ordinary graphs keep their existing byte-stable formats, so the version
    // bump does not make pre-spectral readers refuse graphs they understand.
    SignalGraph ordinary;
    const auto input = ordinary.add_input_node(2, "in");
    const auto output = ordinary.add_output_node(2, "out");
    REQUIRE(ordinary.connect(input, 0, output, 0));
    const auto plain = GraphSerializer::to_json(ordinary);
    REQUIRE_FALSE(plain.empty());
    CHECK(pre_spectral_reader_admits(plain));
}

TEST_CASE("Malformed ordinary graphs keep the ordinary loader's error",
          "[host][spectral][replay][negative]") {
    const std::vector<std::string> plain{
        R"({"format_version":5,"nodes":[{"id":1,"type":"gain")",
        "[1,2,3]",
        R"({"format_version":5,"nodes":[5],"connections":[]})",
    };
    for (const auto& document : plain) {
        INFO(document);
        SignalGraph graph;
        graph.add_gain_node("Kept");
        const auto result = GraphSerializer::from_json(graph, document);
        INFO(result.error);
        REQUIRE_FALSE(result.ok);
        CHECK(result.error.find("spectral") == std::string::npos);
    }
    SignalGraph truncated;
    const auto parse = GraphSerializer::from_json(truncated, plain[0]);
    CHECK(parse.error.starts_with("JSON parse failed"));

    // Control: a document that names spectral content still gets the
    // spectral reader's error.
    const auto good = GraphSerializer::to_json(*fixture());
    REQUIRE_FALSE(good.empty());
    auto wrong_version = good;
    replace(wrong_version, "\"format_version\": 6", "\"format_version\": 5");
    SignalGraph graph;
    const auto spectral = GraphSerializer::from_json(graph, wrong_version);
    REQUIRE_FALSE(spectral.ok);
    CHECK(spectral.error.find("spectral") != std::string::npos);
}

TEST_CASE("Spectral graph with repeated node names saves and reloads", "[host][spectral][replay]") {
    // Node ids identify nodes in the saved document; a name is a label, and
    // the authoring factories accept repeated ones.
    auto graph = fixture(true, "mask");
    const auto json = GraphSerializer::to_json(*graph);
    REQUIRE_FALSE(json.empty());
    SignalGraph loaded;
    const auto result = GraphSerializer::from_json(loaded, json);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(GraphSerializer::to_json(loaded) == json);
    REQUIRE(loaded.prepare(48000, 4096));
    const auto control = timeline();
    REQUIRE_NOTHROW(asset_oracle::compare(render(loaded, control, 0),
                                          asset_oracle::expected(inputs().a, inputs().b, control)));
}

namespace {
// Silent canonical asset bytes for an arbitrary dividing geometry.
pulp::signal::EncodedSpectralStream silent_asset(pulp::signal::SpectralStreamGeometry geometry,
                                                 std::size_t source_samples) {
    const auto frames =
        (source_samples + geometry.fft_size - geometry.hop + geometry.hop - 1) / geometry.hop;
    auto stream = pulp::signal::make_spectral_stream(geometry, frames);
    REQUIRE(stream);
    auto bytes = pulp::signal::encode_spectral_stream(*stream.value);
    REQUIRE(bytes);
    return std::move(*bytes.value);
}
} // namespace

TEST_CASE("Spectral graph saves at the block it is prepared for, not the largest block",
          "[host][spectral][replay][limits]") {
    // FFT 4096, hop 16, four channels: two retained assets and two frame edges
    // fit the 64 MiB bank at 512-sample blocks, but the edges alone grow past
    // what remains at 4096-sample blocks.
    constexpr std::size_t length = 256;
    const pulp::signal::SpectralStreamGeometry geometry{4096, 16, 4, 48000};
    const auto a = silent_asset(geometry, length);
    const auto b = silent_asset(geometry, length);
    SignalGraph graph;
    const auto input = graph.add_input_node(1, "control");
    const auto source = graph.add_spectral_asset_morph_v1(a.view(), b.view(), length, "assets");
    INFO(int(source.error));
    REQUIRE(source);
    const auto shape = *SpectralIdentity::make(4096, 16, 4, 48000).value;
    const auto stage = graph.add_spectral_stage(
        shape, *QualifiedSpectralStage::identity_v1(shape).value, "identity");
    REQUIRE(stage);
    const auto synthesis = graph.add_spectral_synthesis(shape, "synthesis");
    REQUIRE(synthesis);
    const auto output = graph.add_output_node(4, "audio");
    REQUIRE(graph.connect(graph.node(input)->out("audio"), source.value->morph));
    REQUIRE(graph.connect(source.value->frames, stage.value->frames_in));
    REQUIRE(graph.connect(stage.value->frames_out, synthesis.value->frames));
    for (int channel = 0; channel < 4; ++channel)
        REQUIRE(graph.connect(synthesis.value->audio[channel],
                              graph.node(output)->input_ports[channel]));

    REQUIRE_FALSE(graph.prepare(48000, 4096));
    REQUIRE(graph.prepare(48000, 512));
    REQUIRE(graph.stop_spectral_controller() == SpectralError::None);
    const auto json = GraphSerializer::to_json(graph);
    REQUIRE_FALSE(json.empty());

    // A destination that declares the block it will be prepared for loads the
    // graph; one that may be prepared at 4096 samples refuses it up front.
    SignalGraph loaded;
    auto limits = loaded.limits();
    limits.max_block_size = 512;
    loaded.set_limits(limits);
    const auto result = GraphSerializer::from_json(loaded, json);
    INFO(result.error);
    REQUIRE(result.ok);
    REQUIRE(loaded.prepare(48000, 512));
    SignalGraph unbounded;
    REQUIRE_FALSE(GraphSerializer::from_json(unbounded, json).ok);
}

TEST_CASE("Spectral save refusals name the rule the graph breaks",
          "[host][spectral][replay][negative]") {
    // A directory of its own, so concurrent suites in other checkouts on one
    // host never remove each other's file.
    const pulp::test::ScopedTempDir dir("pulp-spectral-save-reason");
    const auto path = dir.path / "graph.pulpgraph";

    // Control: the same graph saves when it breaks no rule.
    auto graph = fixture();
    const auto saved = GraphSerializer::to_file(*graph, path);
    INFO(saved.error);
    REQUIRE(saved.ok);

    const auto id = graph->nodes().front().id;
    const auto off_canvas = GraphSerializer::to_file(
        *graph, path, {{id, {std::numeric_limits<float>::quiet_NaN(), 0.0f}}});
    CHECK_FALSE(off_canvas.ok);
    CHECK(off_canvas.code == GraphSerializer::SaveResult::Code::serialization_refused);
    CHECK(off_canvas.error.find("non-finite editor position") != std::string::npos);

    const auto envelope = GraphSerializer::to_saved_file(*graph, path);
    CHECK_FALSE(envelope.ok);
    CHECK(envelope.error.find("no saved-identity envelope") != std::string::npos);

    graph->add_gain_node("unsupported_mix");
    const auto mixed = GraphSerializer::to_file(*graph, path);
    CHECK_FALSE(mixed.ok);
    CHECK(mixed.code == GraphSerializer::SaveResult::Code::serialization_refused);
    CHECK(mixed.error.find("unsupported_mix") != std::string::npos);
    CHECK(mixed.error.find("not an audio input, audio output or spectral node") !=
          std::string::npos);

    // A live analysis node has its own rule, not the generic node-type one.
    auto analysed = fixture();
    const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
    REQUIRE(analysed->add_spectral_analysis(shape, "live_analysis"));
    const auto analysis = GraphSerializer::to_file(*analysed, path);
    CHECK_FALSE(analysis.ok);
    CHECK(analysis.code == GraphSerializer::SaveResult::Code::serialization_refused);
    CHECK(analysis.error.find("live_analysis is a SpectralAnalysis node") != std::string::npos);
    CHECK(analysis.error.find("live analysis is not persisted") != std::string::npos);
}
