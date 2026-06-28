// ModulationMatrix data-model tests.

#include <catch2/catch_test_macros.hpp>
#include <pulp/state/modulation_lane.hpp>
#include <pulp/view/modulation_matrix.hpp>

using namespace pulp;
using namespace pulp::state;
using namespace pulp::view;

TEST_CASE("add/find/size", "[view][mod-matrix]") {
    ModulationMatrix m;
    REQUIRE(m.empty());
    ModRoute lfo_to_cutoff{.source = 1, .destination = 100, .depth = 0.5f};
    auto idx = m.add(lfo_to_cutoff);
    REQUIRE(idx == 0);
    REQUIRE(m.size() == 1);
    REQUIRE(m.find(1, 100).has_value());
    REQUIRE_FALSE(m.find(1, 999).has_value());
}

TEST_CASE("add with same source+destination replaces in place",
          "[view][mod-matrix]") {
    ModulationMatrix m;
    m.add({1, 100, 0.25f, false, ModCurve::Linear});
    auto idx = m.add({1, 100, 0.75f, true, ModCurve::Exponential});
    REQUIRE(idx == 0);
    REQUIRE(m.size() == 1);
    REQUIRE(m.routes()[0].depth == 0.75f);
    REQUIRE(m.routes()[0].bipolar);
    REQUIRE(m.routes()[0].curve == ModCurve::Exponential);
}

TEST_CASE("remove + remove_by_destination", "[view][mod-matrix]") {
    ModulationMatrix m;
    m.add({1, 100, 0.5f, false, ModCurve::Linear});
    m.add({2, 100, 0.25f, true, ModCurve::Quadratic});
    m.add({1, 200, 0.5f, false, ModCurve::Linear});
    REQUIRE(m.size() == 3);
    m.remove(0);
    REQUIRE(m.size() == 2);
    m.remove_by_destination(100);
    REQUIRE(m.size() == 1);
    REQUIRE(m.routes()[0].destination == 200);
}

TEST_CASE("remove out of range and clear keep matrix reusable",
          "[view][mod-matrix]") {
    ModulationMatrix m;
    m.add({1, 100, 0.5f, false, ModCurve::Linear});
    m.remove(99);
    REQUIRE(m.size() == 1);
    REQUIRE(m.routes()[0].source == 1);

    m.clear();
    REQUIRE(m.empty());

    m.add({2, 200, -0.25f, true, ModCurve::SCurve});
    REQUIRE(m.size() == 1);
    REQUIRE(m.find(2, 200).has_value());
}

TEST_CASE("serialize + deserialize round-trip", "[view][mod-matrix]") {
    ModulationMatrix a;
    a.add({1, 100, 0.5f, false, ModCurve::Linear});
    a.add({2, 200, -0.25f, true, ModCurve::SCurve});
    a.add({3, 300, 1.0f, false, ModCurve::Exponential});

    auto blob = a.serialize();
    ModulationMatrix b;
    REQUIRE(b.deserialize(blob.data(), blob.size()));
    REQUIRE(b.size() == a.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        REQUIRE(a.routes()[i] == b.routes()[i]);
    }
}

TEST_CASE("deserialize rejects malformed blobs", "[view][mod-matrix]") {
    ModulationMatrix b;
    // Empty
    REQUIRE_FALSE(b.deserialize(nullptr, 0));
    // Wrong magic
    uint8_t bad[] = {0, 0, 0, 0, 0, 0, 0, 0};
    REQUIRE_FALSE(b.deserialize(bad, sizeof(bad)));
    // Valid magic, but claims 1000 routes in 8 bytes
    uint8_t short_hdr[] = {0x31, 0x4D, 0x4D, 0x50, 0xE8, 0x03, 0, 0};
    REQUIRE_FALSE(b.deserialize(short_hdr, sizeof(short_hdr)));
}

TEST_CASE("empty matrix round-trips", "[view][mod-matrix]") {
    ModulationMatrix a;
    auto blob = a.serialize();
    ModulationMatrix b;
    b.add({99, 999, 0.1f});          // must be cleared by deserialize
    REQUIRE(b.deserialize(blob.data(), blob.size()));
    REQUIRE(b.empty());
}

TEST_CASE("typed modulation lanes accept compatible scoped routes",
          "[state][modulation][lane]") {
    ModulationLane lane{
        .source = {
            .id = 1,
            .scope = ModulationScope::Voice,
            .rate = ModulationRate::Control,
            .units = "env",
        },
        .target = {
            .param_id = 100,
            .scope = ModulationScope::Voice,
            .param_rate = ParamRate::ControlRate,
            .units = "Hz",
        },
        .mix = ModulationMixMode::Add,
        .depth = 0.5f,
    };

    const auto result = validate_modulation_lane(lane);
    REQUIRE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::None);
}

TEST_CASE("typed modulation lanes reject invalid source and target metadata",
          "[state][modulation][lane]") {
    ModulationLane lane{
        .source = {.id = 1},
        .target = {.param_id = 100},
    };

    lane.source.id = 0;
    auto result = validate_modulation_lane(lane);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::InvalidSource);

    lane.source.id = 1;
    lane.target.param_id = 0;
    result = validate_modulation_lane(lane);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::InvalidTarget);

    lane.target.param_id = 100;
    lane.target.writable = false;
    result = validate_modulation_lane(lane);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::TargetNotWritable);

    lane.target.writable = true;
    lane.target.modulatable = false;
    result = validate_modulation_lane(lane);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::TargetNotModulatable);
}

TEST_CASE("typed modulation lanes validate source and target scope",
          "[state][modulation][lane][scope]") {
    ModulationLane lane{
        .source = {
            .id = 10,
            .scope = ModulationScope::Global,
        },
        .target = {
            .param_id = 20,
            .scope = ModulationScope::Note,
        },
    };
    REQUIRE(validate_modulation_lane(lane).accepted);

    lane.source.scope = ModulationScope::Voice;
    auto result = validate_modulation_lane(lane);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::ScopeMismatch);

    lane.source.scope = ModulationScope::GraphNode;
    lane.target.scope = ModulationScope::GraphNode;
    REQUIRE(validate_modulation_lane(lane).accepted);
}

TEST_CASE("typed modulation lanes reject audio-rate sources for control-rate targets",
          "[state][modulation][lane][rate]") {
    ModulationLane lane{
        .source = {
            .id = 77,
            .rate = ModulationRate::Audio,
        },
        .target = {
            .param_id = 88,
            .param_rate = ParamRate::ControlRate,
        },
    };

    auto result = validate_modulation_lane(lane);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::AudioSourceRequiresAudioTarget);

    lane.target.param_rate = ParamRate::AudioRate;
    result = validate_modulation_lane(lane);
    REQUIRE(result.accepted);
    REQUIRE(modulation_rate_for(lane.target.param_rate) == ModulationRate::Audio);
}

TEST_CASE("typed modulation lanes accept per-voice expression routes",
          "[state][modulation][lane][voice]") {
    ModulationLane lane{
        .source = {
            .id = 11,
            .scope = ModulationScope::Voice,
            .rate = ModulationRate::Control,
            .units = "pressure",
        },
        .target = {
            .param_id = 900,
            .scope = ModulationScope::Voice,
            .param_rate = ParamRate::ControlRate,
            .units = "pressure",
        },
        .mix = ModulationMixMode::Replace,
        .depth = 0.8f,
    };

    auto result = validate_modulation_lane(lane);
    REQUIRE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::None);
    REQUIRE(lane.mix == ModulationMixMode::Replace);
    REQUIRE(lane.depth == 0.8f);

    lane.target.scope = ModulationScope::Global;
    result = validate_modulation_lane(lane);
    REQUIRE_FALSE(result.accepted);
    REQUIRE(result.reason == ModulationLaneRejectReason::ScopeMismatch);
}
