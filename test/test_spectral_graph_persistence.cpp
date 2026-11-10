#include <catch2/catch_test_macros.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/runtime/base64.hpp>
#include "../examples/g10-routed-consumer/asset_morph_oracle.hpp"

#include <functional>

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
std::unique_ptr<SignalGraph> fixture(bool stages = false) {
    auto graph = std::make_unique<SignalGraph>();
    const auto input = graph->add_input_node(1, "control");
    const auto source = graph->add_spectral_asset_morph_v1(inputs().aa.view(), inputs().bb.view(), 769, "assets");
    REQUIRE(source);
    const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
    auto frames = source.value->frames;
    if (stages) {
        const auto identity = graph->add_spectral_stage(shape, *QualifiedSpectralStage::identity_v1(shape).value,
                                                       "identity");
        REQUIRE(identity);
        REQUIRE(graph->connect(frames, identity.value->frames_in));
        frames = identity.value->frames_out;
        std::vector<float> gains(129, 1.0f);
        const auto mask = graph->add_spectral_stage(shape, *QualifiedSpectralStage::real_mask_v1(shape, gains).value,
                                                   "mask");
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
        const int count = static_cast<int>(std::min(control.size() - t,
                                      std::size_t(partition ? partition : 1 + t * 17 % 511)));
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
    REQUIRE(json.find("\"format_version\": 5") != std::string::npos);
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

TEST_CASE("Spectral asset corruption and unsupported contracts preserve the old prepared graph",
          "[host][spectral][replay][negative]") {
    const auto original = fixture();
    const auto good = GraphSerializer::to_json(*original);
    REQUIRE_FALSE(good.empty());
    SignalGraph positive;
    REQUIRE(GraphSerializer::from_json(positive, good).ok);
    const std::vector<std::pair<std::string, std::string>> mutations{
        {"\"format_version\": 5", "\"format_version\": 4"},
        {"\"format_version\": 5", "\"format_version\": 6"},
        {"pulp.analyzed-spectral-asset", "pulp.unsupported-spectral-asset"},
        {"\"version\": 1", "\"version\": 2"},
        {"\"source_samples\": 769", "\"source_samples\": 770"},
        {"\"bytes_b64\": \"", "\"bytes_b64\": \"AAAA"},
        {"\"bytes_sha256\": \"", "\"bytes_sha256\": \"0"},
        {"\"asset_a\": \"", "\"asset_a\": \"missing-"},
        {"\"contract_hex\": \"", "\"contract_hex\": \"ff"},
        {"\"spectral\": true", "\"spectral\": false"},
        {"\"feedback\": false", "\"feedback\": true"},
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
    const auto bytes_marker = substituted.find("\"bytes_b64\": \"");
    REQUIRE(bytes_marker != std::string::npos);
    const auto changed_byte = bytes_marker + std::string("\"bytes_b64\": \"").size();
    REQUIRE(changed_byte < substituted.size());
    substituted[changed_byte] = substituted[changed_byte] == 'A' ? 'B' : 'A';
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
