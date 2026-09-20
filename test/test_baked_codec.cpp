// The .pulpbake plan payload codec: serialize round-trips exactly, and the bounded
// parser rejects truncation, an unknown version, an over-cap count, and a port that
// exceeds its node's declared arity — all BEFORE building anything.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/baked_codec.hpp>
#include <pulp/runtime/crypto.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

using pulp::host::BakedPlan;
using pulp::host::NodeType;
using pulp::host::detail::parse_plan_bounded;
using pulp::host::detail::serialize_plan;

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

BakedPlan sample_region_plan() {
    BakedPlan p;
    p.format_version = pulp::host::kBakedMaxSupportedFormatVersion;
    p.input_channels = 1;
    p.output_channels = 1;
    p.nodes.push_back({1, NodeType::AudioInput, 0, 1, 1.0f, {}, 0, {}});
    p.nodes.push_back({2, NodeType::AudioOutput, 1, 0, 1.0f, {}, 0, {}});
    p.nodes.push_back({3, NodeType::Custom, 1, 1, 1.0f, "pulp.core.sample-region.input", 1, {}});
    p.nodes.push_back({4, NodeType::Custom, 1, 1, 1.0f, "pulp.core.sample-region.output", 1, {}});
    p.nodes.push_back(
        {5, NodeType::Custom, 0, 1, 1.0f, "pulp.core.sample-region.parameter", 1, {}});
    p.nodes.push_back({6, NodeType::Custom, 0, 1, 1.0f, "pulp.core.sample-region.constant", 1, {}});
    p.nodes.push_back({7, NodeType::Custom, 2, 1, 1.0f, "pulp.core.sample-region.add", 1, {}});
    p.nodes.push_back({8, NodeType::Custom, 1, 1, 1.0f, "pulp.core.unit-delay", 1, {}});
    p.nodes.push_back({9, NodeType::Custom, 2, 1, 1.0f, "pulp.core.sample-region.multiply", 1, {}});
    p.connections = {
        {1, 0, 3, 0, false}, {3, 0, 7, 0, false}, {8, 0, 7, 1, false}, {7, 0, 8, 0, false},
        {7, 0, 9, 0, false}, {5, 0, 9, 1, false}, {9, 0, 4, 0, false}, {4, 0, 2, 0, false},
    };

    pulp::host::SampleRegionDefinition region;
    region.region_id = 17;
    region.members = {
        {3,
         "pulp.core.sample-region.input",
         1,
         {pulp::host::SampleKernelConfigKind::BoundaryIndex, 0, 0.0f}},
        {4,
         "pulp.core.sample-region.output",
         1,
         {pulp::host::SampleKernelConfigKind::BoundaryIndex, 0, 0.0f}},
        {5,
         "pulp.core.sample-region.parameter",
         1,
         {pulp::host::SampleKernelConfigKind::PromotedParameterId, 42, 0.0f}},
        {6,
         "pulp.core.sample-region.constant",
         1,
         {pulp::host::SampleKernelConfigKind::FiniteConstant, 0, 0.5f}},
        {7, "pulp.core.sample-region.add", 1, {pulp::host::SampleKernelConfigKind::None, 0, 0.0f}},
        {8, "pulp.core.unit-delay", 1, {pulp::host::SampleKernelConfigKind::None, 0, 0.0f}},
        {9,
         "pulp.core.sample-region.multiply",
         1,
         {pulp::host::SampleKernelConfigKind::None, 0, 0.0f}},
    };
    region.input_boundaries = {3};
    region.output_boundaries = {4};
    pulp::host::SampleRegionPromotedParameter parameter;
    parameter.param_id = 42;
    parameter.key = "feedback";
    parameter.name = "Feedback";
    parameter.unit = "";
    parameter.range = pulp::state::ParamRange::linear(0.0f, 1.0f, 0.5f);
    parameter.rate = pulp::state::ParamRate::ControlRate;
    parameter.smoothing_ramp_seconds = 0.0f;
    parameter.bound_node_id = 5;
    parameter.bound_port = 0;
    region.promoted_parameters.push_back(std::move(parameter));
    p.sample_regions.push_back(std::move(region));
    return p;
}

BakedPlan legacy_v1_golden_plan() {
    BakedPlan p;
    p.input_channels = 1;
    p.output_channels = 1;
    p.nodes.push_back({1, NodeType::AudioInput, 0, 1, 1.0f, {}, 0, {}});
    p.nodes.push_back({2, NodeType::Gain, 2, 2, 0.25f, {}, 0, {}});
    p.nodes.push_back({3, NodeType::AudioOutput, 1, 0, 1.0f, {}, 0, {}});
    p.connections = {{1, 0, 2, 0, false}, {2, 0, 3, 0, false}};
    return p;
}

std::vector<std::uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    const std::string bytes{std::istreambuf_iterator<char>(input), {}};
    return {bytes.begin(), bytes.end()};
}

std::array<std::uint8_t, 32> test_seed(std::uint8_t offset = 1) {
    std::array<std::uint8_t, 32> seed{};
    for (std::size_t i = 0; i < seed.size(); ++i)
        seed[i] = static_cast<std::uint8_t>(i + offset);
    return seed;
}

}  // namespace

TEST_CASE("BakedPlan serialize/parse round-trips exactly", "[host][bake][codec]") {
    const BakedPlan plan = sample_plan();
    const auto bytes = serialize_plan(plan);
    const auto parsed = parse_plan_bounded(bytes);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == plan);
}

TEST_CASE("legacy v1 signed fixture remains byte-identical", "[host][bake][codec][compatibility]") {
    const auto seed = test_seed();
    const auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());

    const auto plan = legacy_v1_golden_plan();
    REQUIRE(plan.format_version == pulp::host::kBakedPlanV1FormatVersion);
    const auto generated = pulp::host::write_baked_signed(plan, kp->private_key);
    const auto fixture = read_bytes(fs::path(__FILE__).parent_path().parent_path() /
                                    "test/fixtures/sample-region-compat/bake/legacy-v1.pulpbake");
    REQUIRE_FALSE(fixture.empty());

    // This covers the legacy payload, manifest, v1 signature domain, signature, and
    // final envelope framing in one immutable comparison.
    REQUIRE(generated == fixture);

    pulp::host::BakedTrust trust;
    trust.trusted_public_keys.push_back(kp->public_key);
    const auto decoded = pulp::host::verify_and_extract_plan(fixture, trust);
    REQUIRE(decoded.has_value());
    CHECK(*decoded == plan);
}

TEST_CASE("BakedPlan v2 region tail round-trips canonically",
          "[host][bake][codec][sample-region]") {
    const BakedPlan plan = sample_region_plan();
    const auto bytes = serialize_plan(plan);
    const auto parsed = parse_plan_bounded(bytes);
    REQUIRE(parsed.has_value());
    CHECK(*parsed == plan);
    REQUIRE(parsed->sample_regions.size() == 1);
    CHECK(parsed->sample_regions.front().region_id == 17);
    CHECK(parsed->sample_regions.front().members.size() == 7);
    CHECK(parsed->sample_regions.front().promoted_parameters.front().param_id == 42);
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
    SECTION("duplicate node identity -> nullopt") {
        BakedPlan p = sample_plan();
        auto duplicate = p.nodes.back();
        duplicate.id = p.nodes.front().id;
        p.nodes.push_back(std::move(duplicate));
        CHECK_FALSE(parse_plan_bounded(serialize_plan(p)).has_value());
    }
}

TEST_CASE("v2 parser refuses noncanonical and hostile region records",
          "[host][bake][codec][sample-region][security]") {
    const auto good = serialize_plan(sample_region_plan());

    SECTION("future version -> nullopt") {
        auto bad = good;
        bad[0] = 3;
        CHECK_FALSE(parse_plan_bounded(bad).has_value());
    }
    SECTION("region member order -> nullopt") {
        auto bad_plan = sample_region_plan();
        std::swap(bad_plan.sample_regions.front().members[0],
                  bad_plan.sample_regions.front().members[1]);
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_plan)).has_value());
    }
    SECTION("promoted parameter order -> nullopt") {
        auto bad_plan = sample_region_plan();
        auto second = bad_plan.sample_regions.front().promoted_parameters.front();
        second.param_id = 43;
        second.key = "other";
        second.name = "Other";
        bad_plan.sample_regions.front().promoted_parameters.push_back(std::move(second));
        std::swap(bad_plan.sample_regions.front().promoted_parameters[0],
                  bad_plan.sample_regions.front().promoted_parameters[1]);
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_plan)).has_value());
    }
    SECTION("invalid UTF-8 in a region type identity -> nullopt") {
        auto bad_plan = sample_region_plan();
        bad_plan.nodes[2].custom_type_id = std::string("\xC0\x80", 2);
        bad_plan.sample_regions.front().members[0].type_id = bad_plan.nodes[2].custom_type_id;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_plan)).has_value());
    }
    SECTION("nonfinite region floats -> nullopt") {
        auto bad_config = sample_region_plan();
        bad_config.sample_regions.front().members[3].config.constant =
            std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_config)).has_value());

        auto bad_parameter = sample_region_plan();
        bad_parameter.sample_regions.front().promoted_parameters.front().range.min =
            std::numeric_limits<float>::infinity();
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_parameter)).has_value());
    }
    SECTION("noncanonical node identities, floats, and inactive fields -> nullopt") {
        auto zero_id = sample_region_plan();
        zero_id.nodes.front().id = 0;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(zero_id)).has_value());

        auto nonfinite_gain = sample_region_plan();
        nonfinite_gain.nodes.front().gain = std::numeric_limits<float>::quiet_NaN();
        CHECK_FALSE(parse_plan_bounded(serialize_plan(nonfinite_gain)).has_value());

        auto inactive_custom = sample_region_plan();
        inactive_custom.nodes.front().custom_type_id = "inactive";
        CHECK_FALSE(parse_plan_bounded(serialize_plan(inactive_custom)).has_value());

        auto inactive_gain = sample_region_plan();
        inactive_gain.nodes.front().gain = 0.5f;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(inactive_gain)).has_value());

        auto custom_gain = sample_region_plan();
        custom_gain.nodes[2].gain = 0.5f;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(custom_gain)).has_value());
    }
    SECTION("noncanonical connection boolean -> nullopt") {
        const auto plan = sample_region_plan();
        auto bad = serialize_plan(plan);
        std::size_t first_connection_offset = 16;
        for (const auto& node : plan.nodes)
            first_connection_offset += 25 + node.custom_type_id.size() + node.custom_state.size();
        first_connection_offset += 4;
        constexpr std::size_t feedback_offset = 4 + 2 + 4 + 2;
        REQUIRE(first_connection_offset + feedback_offset < bad.size());
        bad[first_connection_offset + feedback_offset] = 2;
        CHECK_FALSE(parse_plan_bounded(bad).has_value());
    }
    SECTION("inactive sample-kernel union fields -> nullopt") {
        auto bad_none = sample_region_plan();
        bad_none.sample_regions.front().members[4].config.boundary_index_or_parameter_id = 1;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_none)).has_value());

        auto bad_boundary = sample_region_plan();
        bad_boundary.sample_regions.front().members[0].config.constant = 1.0f;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_boundary)).has_value());

        auto bad_constant = sample_region_plan();
        bad_constant.sample_regions.front().members[3].config.boundary_index_or_parameter_id = 1;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_constant)).has_value());

        auto bad_parameter = sample_region_plan();
        bad_parameter.sample_regions.front().members[2].config.constant = 1.0f;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_parameter)).has_value());
    }
    SECTION("region count over cap -> nullopt") {
        const auto plan = sample_region_plan();
        auto bad = serialize_plan(plan);
        std::size_t region_count_offset = 16;
        for (const auto& node : plan.nodes)
            region_count_offset += 25 + node.custom_type_id.size() + node.custom_state.size();
        region_count_offset += 4 + plan.connections.size() * 13;
        REQUIRE(region_count_offset + 4 <= bad.size());
        bad[region_count_offset] = static_cast<std::uint8_t>(pulp::host::kBakedMaxRegions + 1);
        CHECK_FALSE(parse_plan_bounded(bad).has_value());
    }
    SECTION("region tail truncation and trailing bytes -> nullopt") {
        auto trunc = good;
        trunc.pop_back();
        CHECK_FALSE(parse_plan_bounded(trunc).has_value());

        auto trailing = good;
        trailing.push_back(0);
        CHECK_FALSE(parse_plan_bounded(trailing).has_value());
    }
    SECTION("member count over declared limit -> nullopt") {
        auto bad_plan = sample_region_plan();
        bad_plan.sample_regions.front().limits.max_member_nodes = 1;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_plan)).has_value());
    }
    SECTION("boundary and parameter counts over declared limits -> nullopt") {
        auto bad_boundaries = sample_region_plan();
        bad_boundaries.sample_regions.front().limits.max_input_boundaries = 0;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_boundaries)).has_value());

        auto bad_parameters = sample_region_plan();
        bad_parameters.sample_regions.front().limits.max_promoted_parameters = 0;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_parameters)).has_value());
    }
    SECTION("oversized region string -> nullopt") {
        auto bad_plan = sample_region_plan();
        bad_plan.nodes[2].custom_type_id.assign(pulp::host::kBakedMaxRegionString + 1, 'x');
        bad_plan.sample_regions.front().members[0].type_id = bad_plan.nodes[2].custom_type_id;
        CHECK_FALSE(parse_plan_bounded(serialize_plan(bad_plan)).has_value());
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

TEST_CASE("signed v2 region bake uses the v2 domain and round-trips",
          "[host][bake][codec][sample-region][security]") {
    const auto seed = test_seed(9);
    const auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());

    const auto bytes = pulp::host::write_baked_signed(sample_region_plan(), kp->private_key);
    REQUIRE_FALSE(bytes.empty());
    REQUIRE(bytes.size() > 16 + 144);

    pulp::host::BakedTrust trust;
    trust.trusted_public_keys.push_back(kp->public_key);
    const auto decoded = pulp::host::verify_and_extract_plan(bytes, trust);
    REQUIRE(decoded.has_value());
    CHECK(decoded->format_version == pulp::host::kBakedMaxSupportedFormatVersion);
    CHECK(decoded->sample_regions.size() == 1);

    SECTION("changing the manifest to v1 without re-signing is rejected") {
        auto tampered = bytes;
        // The manifest starts after the 16-byte envelope prelude. Its format is
        // the first little-endian u32.
        tampered[16] = static_cast<std::uint8_t>(pulp::host::kBakedPlanV1FormatVersion);
        CHECK_FALSE(pulp::host::verify_and_extract_plan(tampered, trust).has_value());
    }
}

TEST_CASE("signed v2 parser rejects too many region-touching connections",
          "[host][bake][codec][sample-region][security]") {
    auto plan = sample_region_plan();
    while (plan.connections.size() <= 128)
        plan.connections.push_back({3, 0, 7, 0, false});

    const auto seed = test_seed(37);
    const auto kp = pulp::runtime::ed25519_keypair_from_seed(seed.data(), seed.size());
    REQUIRE(kp.has_value());
    const auto bytes = pulp::host::write_baked_signed(plan, kp->private_key);
    REQUIRE_FALSE(bytes.empty());

    pulp::host::BakedTrust trust;
    trust.trusted_public_keys.push_back(kp->public_key);
    CHECK_FALSE(pulp::host::verify_and_extract_plan(bytes, trust).has_value());
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
