#include <catch2/catch_test_macros.hpp>

#include "pulp/audio/live_dsp_telemetry.hpp"
#include "pulp/audio/live_dsp_telemetry_json.hpp"

#include <choc/text/choc_JSON.h>

#include <string>
#include <vector>

using namespace pulp::audio;

namespace {

// Build a store, run `blocks` blocks that time every node with a little real
// work, drain, and return the published snapshot (owned by the store, so the
// store must outlive the returned reference — callers keep both in scope).
const LiveDspTelemetrySnapshot& drive(LiveDspTelemetryStore& store,
                                      const std::vector<LiveDspNodeInfo>& nodes,
                                      int blocks) {
    REQUIRE(store.prepare({}, nodes));
    store.set_enabled(true);
    for (int b = 0; b < blocks; ++b) {
        auto writer = store.begin_block(128, 48000.0);
        for (std::uint32_t n = 0; n < static_cast<std::uint32_t>(nodes.size()); ++n) {
            auto scope = writer.node(n);
            volatile double acc = 0.0;
            for (int i = 0; i < 1024; ++i) acc += static_cast<double>(i) * 0.25;
            (void)acc;
        }
        writer.finish();
    }
    store.drain();
    return store.latest();
}

LiveDspNodeInfo node(std::uint64_t id, LiveDspNodeKind kind, const char* name) {
    LiveDspNodeInfo info;
    info.node_id = id;
    info.kind = kind;
    info.set_name(name);
    return info;
}

}  // namespace

TEST_CASE("live-dsp telemetry JSON carries the graph-level schema", "[live-dsp-telemetry][json]") {
    LiveDspTelemetryStore store;
    const std::vector<LiveDspNodeInfo> nodes{
        node(1, LiveDspNodeKind::AudioInput, "In"),
        node(2, LiveDspNodeKind::Gain, "Gain"),
        node(3, LiveDspNodeKind::AudioOutput, "Out"),
    };
    const auto& snap = drive(store, nodes, 16);

    const auto json = live_dsp_telemetry_snapshot_to_json(snap);
    const auto v = choc::json::parse(json);

    REQUIRE(v.isObject());
    REQUIRE(v["available"].getWithDefault(false));
    REQUIRE(v["enabled"].getWithDefault(false));
    REQUIRE(v["node_count"].getWithDefault(std::int64_t{0}) == 3);
    REQUIRE(v["blocks_written"].getWithDefault(std::int64_t{0}) == 16);
    REQUIRE(v["blocks_drained"].getWithDefault(std::int64_t{0}) == 16);
    REQUIRE(v["sample_rate"].getWithDefault(0.0) == 48000.0);
    REQUIRE(v["last_frame_count"].getWithDefault(std::int64_t{0}) == 128);
    // last_graph_load is a fraction; sane and non-negative for a light graph.
    REQUIRE(v["last_graph_load"].getWithDefault(-1.0) >= 0.0);
    REQUIRE(v["nodes"].isArray());
    REQUIRE(v["nodes"].size() == 3);
}

TEST_CASE("live-dsp telemetry JSON node objects carry kind names and percentiles",
          "[live-dsp-telemetry][json]") {
    LiveDspTelemetryStore store;
    const std::vector<LiveDspNodeInfo> nodes{
        node(7, LiveDspNodeKind::Plugin, "Reverb"),
        node(9, LiveDspNodeKind::Custom, "MyNode"),
    };
    const auto& snap = drive(store, nodes, 24);

    const auto v = choc::json::parse(live_dsp_telemetry_snapshot_to_json(snap));
    const auto& arr = v["nodes"];
    REQUIRE(arr.size() == 2);

    const auto& n0 = arr[0];
    REQUIRE(n0["node_id"].getWithDefault(std::int64_t{0}) == 7);
    REQUIRE(n0["kind"].getWithDefault(std::string{}) == "plugin");
    REQUIRE(n0["name"].getWithDefault(std::string{}) == "Reverb");
    REQUIRE(n0["sample_count"].getWithDefault(std::int64_t{0}) == 24);
    // Percentile ordering and jitter identity are preserved through JSON.
    const auto p50 = n0["p50_elapsed_ns"].getWithDefault(std::int64_t{-1});
    const auto p95 = n0["p95_elapsed_ns"].getWithDefault(std::int64_t{-1});
    const auto jitter = n0["jitter_ns"].getWithDefault(std::int64_t{-999});
    REQUIRE(p50 >= 0);
    REQUIRE(p95 >= p50);
    REQUIRE(jitter == p95 - p50);

    REQUIRE(arr[1]["kind"].getWithDefault(std::string{}) == "custom");
    REQUIRE(arr[1]["name"].getWithDefault(std::string{}) == "MyNode");
}

TEST_CASE("live-dsp telemetry JSON serializes an empty/disabled snapshot", "[live-dsp-telemetry][json]") {
    // A default snapshot (never enabled, never drained): available=false and an
    // empty nodes array — still valid JSON a reader can parse.
    LiveDspTelemetrySnapshot snap;
    const auto v = choc::json::parse(live_dsp_telemetry_snapshot_to_json(snap));

    REQUIRE(v.isObject());
    REQUIRE_FALSE(v["available"].getWithDefault(true));
    REQUIRE_FALSE(v["enabled"].getWithDefault(true));
    REQUIRE(v["node_count"].getWithDefault(std::int64_t{-1}) == 0);
    REQUIRE(v["blocks_written"].getWithDefault(std::int64_t{-1}) == 0);
    REQUIRE(v["nodes"].isArray());
    REQUIRE(v["nodes"].size() == 0);
}

TEST_CASE("live-dsp telemetry JSON compact vs pretty parse to the same tree", "[live-dsp-telemetry][json]") {
    LiveDspTelemetryStore store;
    const std::vector<LiveDspNodeInfo> nodes{node(1, LiveDspNodeKind::Utility, "U")};
    const auto& snap = drive(store, nodes, 4);

    const auto pretty = live_dsp_telemetry_snapshot_to_json(snap, /*pretty=*/true);
    const auto compact = live_dsp_telemetry_snapshot_to_json(snap, /*pretty=*/false);
    REQUIRE(pretty != compact);  // formatting differs
    REQUIRE(choc::json::parse(pretty)["nodes"].size()
            == choc::json::parse(compact)["nodes"].size());
    REQUIRE(choc::json::parse(compact)["nodes"][0]["kind"].getWithDefault(std::string{}) == "utility");
}
