// The .pulpbake plan payload codec: serialize round-trips exactly, and the bounded
// parser rejects truncation, an unknown version, an over-cap count, and a port that
// exceeds its node's declared arity — all BEFORE building anything.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/baked_codec.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <cstdint>
#include <string_view>
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

TEST_CASE("signed .pulpbake round-trips under trust and rejects every tamper",
          "[host][bake][codec][security]") {
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<std::uint8_t>(i + 1);
    const auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());

    const BakedPlan plan = sample_plan();
    const auto bytes = pulp::host::write_baked_signed(plan, kp->private_key);
    REQUIRE_FALSE(bytes.empty());

    pulp::host::BakedTrust trust;
    trust.trusted_public_keys.push_back(kp->public_key);

    SECTION("trusted round-trip is bit-identical") {
        const auto got = pulp::host::verify_and_extract_plan(bytes, trust);
        REQUIRE(got.has_value());
        CHECK(*got == plan);
    }
    SECTION("untrusted signer (empty trust) -> nullopt") {
        CHECK_FALSE(pulp::host::verify_and_extract_plan(bytes, pulp::host::BakedTrust{}).has_value());
    }
    SECTION("tampered plan byte -> nullopt (hash mismatch, before parse)") {
        auto b = bytes;
        b.back() ^= 0xFF;  // last byte is in the plan region
        CHECK_FALSE(pulp::host::verify_and_extract_plan(b, trust).has_value());
    }
    SECTION("tampered signature -> nullopt") {
        auto b = bytes;
        // Prelude is 16 bytes (magic8 + manifest_len4 + plan_len4); the v1 manifest is
        // 144 bytes and ends with the 64-byte signature.
        b[16 + 144 - 1] ^= 0xFF;
        CHECK_FALSE(pulp::host::verify_and_extract_plan(b, trust).has_value());
    }
    SECTION("truncated envelope -> nullopt") {
        std::vector<std::uint8_t> t(bytes.begin(), bytes.begin() + bytes.size() / 2);
        CHECK_FALSE(pulp::host::verify_and_extract_plan(t, trust).has_value());
    }
    SECTION("bad magic -> nullopt") {
        auto b = bytes;
        b[0] ^= 0xFF;
        CHECK_FALSE(pulp::host::verify_and_extract_plan(b, trust).has_value());
    }
}

TEST_CASE("verify_and_extract_plan gates malicious envelopes even when otherwise well-formed",
          "[host][bake][codec][security]") {
    SECTION("prelude declares an over-cap manifest length -> nullopt before any parse") {
        std::vector<std::uint8_t> bytes;
        for (char c : std::string_view("PULPBAKE")) bytes.push_back(static_cast<std::uint8_t>(c));
        // manifest_len = 0x00FFFFFF (>> the 4 KiB cap); plan_len = 0.
        bytes.insert(bytes.end(), {0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00});
        CHECK_FALSE(pulp::host::verify_and_extract_plan(bytes, pulp::host::BakedTrust{}).has_value());
    }
    SECTION("a validly-SIGNED plan whose custom state exceeds the cap is still rejected at parse") {
        // The signature is authentic and the hash matches, so verification passes — but
        // the bounded plan parse must STILL reject the over-cap state blob. Proves the
        // caps gate independently of the signature (verify does not imply safe-to-parse).
        std::array<std::uint8_t, 32> seed{};
        for (std::size_t i = 0; i < seed.size(); ++i) seed[i] = static_cast<std::uint8_t>(i + 3);
        const auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
        REQUIRE(kp.has_value());

        BakedPlan p;
        p.input_channels = 1;
        p.output_channels = 1;
        BakedPlan::Node cn;
        cn.id = 1;
        cn.type = NodeType::Custom;
        cn.num_input_ports = 1;
        cn.num_output_ports = 1;
        cn.custom_type_id = "x";
        cn.custom_version = 1;
        cn.custom_state.assign(pulp::host::kBakedMaxCustomState + 1, 0xAB);  // just over cap
        p.nodes.push_back(std::move(cn));

        const auto bytes = pulp::host::write_baked_signed(p, kp->private_key);
        REQUIRE_FALSE(bytes.empty());  // serialize+sign does not cap; the LOAD path does
        pulp::host::BakedTrust trust;
        trust.trusted_public_keys.push_back(kp->public_key);
        CHECK_FALSE(pulp::host::verify_and_extract_plan(bytes, trust).has_value());
    }
}
