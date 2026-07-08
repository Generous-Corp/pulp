// The .pulpbake plan payload codec: serialize round-trips exactly, and the bounded
// parser rejects truncation, an unknown version, an over-cap count, and a port that
// exceeds its node's declared arity — all BEFORE building anything.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/baked_codec.hpp>

#include <cstdint>
#include <vector>

using pulp::host::BakedPlan;
using pulp::host::NodeType;
using pulp::host::parse_plan_bounded;
using pulp::host::serialize_plan;

namespace {

BakedPlan sample_plan() {
    BakedPlan p;
    p.format_version = 1;
    p.input_channels = 1;
    p.output_channels = 1;
    p.nodes.push_back({/*id=*/1, NodeType::AudioInput, 0, 1, 1.0f, {}, 0, {}});
    p.nodes.push_back({/*id=*/2, NodeType::Gain, 1, 1, 0.5f, {}, 0, {}});
    p.nodes.push_back(
        {/*id=*/3, NodeType::Custom, 1, 1, 1.0f, "bakegain", 1, {0xDE, 0xAD, 0xBE, 0xEF}});
    p.nodes.push_back({/*id=*/4, NodeType::AudioOutput, 1, 0, 1.0f, {}, 0, {}});
    p.connections.push_back({1, 0, 2, 0, false});
    p.connections.push_back({2, 0, 3, 0, false});
    p.connections.push_back({3, 0, 4, 0, false});
    return p;
}

}  // namespace

TEST_CASE("BakedPlan serialize/parse round-trips exactly", "[host][bake][codec]") {
    const BakedPlan plan = sample_plan();
    const auto bytes = serialize_plan(plan);
    const auto parsed = parse_plan_bounded(bytes);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == plan);
}

TEST_CASE("parse_plan_bounded rejects malformed / over-cap plan bytes",
          "[host][bake][codec][security]") {
    const auto good = serialize_plan(sample_plan());

    SECTION("truncated bytes -> nullopt") {
        std::vector<std::uint8_t> trunc(good.begin(), good.begin() + good.size() / 2);
        CHECK_FALSE(parse_plan_bounded(trunc).has_value());
    }
    SECTION("trailing garbage -> nullopt") {
        auto extra = good;
        extra.push_back(0x00);
        CHECK_FALSE(parse_plan_bounded(extra).has_value());
    }
    SECTION("unknown/future format_version -> nullopt") {
        auto bad = good;
        bad[0] = 0xFF;  // huge version in the first LE u32
        CHECK_FALSE(parse_plan_bounded(bad).has_value());
    }
    SECTION("node_count over cap -> nullopt (no allocation)") {
        // version(1) + in(0) + out(0) + node_count(0x00FFFFFF, well over 512).
        std::vector<std::uint8_t> bytes = {
            1, 0, 0, 0,   0, 0, 0, 0,   0, 0, 0, 0,   0xFF, 0xFF, 0xFF, 0x00};
        CHECK_FALSE(parse_plan_bounded(bytes).has_value());
    }
    SECTION("connection port beyond node arity -> nullopt") {
        BakedPlan p = sample_plan();
        p.connections.push_back({2, 0, 3, /*dst_port=*/9, false});  // node 3 has 1 input
        CHECK_FALSE(parse_plan_bounded(serialize_plan(p)).has_value());
    }
    SECTION("connection referencing an unknown node -> nullopt") {
        BakedPlan p = sample_plan();
        p.connections.push_back({999, 0, 4, 0, false});
        CHECK_FALSE(parse_plan_bounded(serialize_plan(p)).has_value());
    }
}
