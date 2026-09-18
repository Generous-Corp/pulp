#include <catch2/catch_test_macros.hpp>

#include <pulp/host/baked_graph_processor.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <bit>
#include <type_traits>

using namespace pulp;
using namespace pulp::host;

namespace {
SampleRegionPromotedParameter promoted(state::ParamID id, std::string key = "coefficient") {
    SampleRegionPromotedParameter value;
    value.param_id = id;
    value.key = std::move(key);
    value.name = "Coefficient";
    value.range = {-0.999f, 0.999f, 0.0f, 0.0f};
    value.bound_node_id = id + 100;
    return value;
}
SampleRegionDefinition region(SampleRegionId id,
                              std::vector<SampleRegionPromotedParameter> parameters) {
    SampleRegionDefinition value;
    value.region_id = id;
    value.promoted_parameters = std::move(parameters);
    return value;
}
SampleRegionParameterContract contract(std::vector<SampleRegionDefinition> regions) {
    return SampleRegionParameterContract::from_regions(regions);
}
state::ParamInfo ordinary(state::ParamID id) {
    return {.id = id, .name = "Gain", .unit = "dB", .range = {-60.0f, 12.0f, 0.0f}};
}
std::unique_ptr<state::StateStore> store_from(std::span<const state::ParamInfo> parameters) {
    auto store = std::make_unique<state::StateStore>();
    for (const auto& parameter : parameters)
        store->add_parameter(parameter);
    return store;
}
std::string formatter_a(float) {
    return "a";
}
std::string formatter_b(float) {
    return "b";
}
} // namespace

static_assert(!std::is_copy_constructible_v<SampleRegionParameterBinding>);
static_assert(!std::is_copy_assignable_v<SampleRegionParameterBinding>);
static_assert(!std::is_move_constructible_v<SampleRegionParameterBinding>);
static_assert(!std::is_move_assignable_v<SampleRegionParameterBinding>);

TEST_CASE("Sample region parameter owner freezes before private registration",
          "[host][sample-region][parameters]") {
    auto nonzero_default = promoted(90);
    nonzero_default.range.default_value = 0.25f;
    const auto candidate = contract({region(9, {nonzero_default, promoted(40, "feedback")})});
    REQUIRE(candidate.valid());
    CHECK_FALSE(candidate.frozen());
    REQUIRE(candidate.entries().size() == 2);
    CHECK(candidate.entries()[0].region_id == 9);
    CHECK(candidate.entries()[0].key == "feedback");
    CHECK(candidate.entries()[0].bound_node_id == 140);
    auto owner =
        SampleRegionParameterOwner::create(std::vector{ordinary(7), ordinary(3)}, candidate);
    REQUIRE(owner);
    REQUIRE(owner->contract().frozen());
    REQUIRE(owner->store().param_count() == 4);
    CHECK(owner->store().state_generation() == 0);
    const auto manifest = owner->store().all_params();
    CHECK(manifest[0].id == 7);
    CHECK(manifest[1].id == 3);
    CHECK(manifest[2].id == 40);
    CHECK(manifest[3].id == 90);
    CHECK(owner->store().get_value(7) == manifest[0].range.default_value);
    CHECK(owner->store().get_value(90) == 0.25f);
    CHECK(owner->binding().value(90) == manifest[3].range.default_value);
}

TEST_CASE("Sample region contract enforces global IDs and region-local keys",
          "[host][sample-region][parameters][negative]") {
    SECTION("duplicate promoted ID in one region") {
        CHECK_FALSE(contract({region(1, {promoted(40), promoted(40, "other")})}).valid());
    }
    SECTION("duplicate promoted ID across regions") {
        CHECK_FALSE(contract({region(1, {promoted(40)}), region(2, {promoted(40)})}).valid());
    }
    SECTION("duplicate key in one region") {
        CHECK_FALSE(contract({region(1, {promoted(40), promoted(41)})}).valid());
    }
    SECTION("same key in different regions") {
        CHECK(contract({region(1, {promoted(40)}), region(2, {promoted(41)})}).valid());
    }
    SECTION("duplicate region identity") {
        CHECK_FALSE(
            contract({region(1, {promoted(40)}), region(1, {promoted(41, "other")})}).valid());
    }
    SECTION("duplicate ordinary ID") {
        const auto candidate = contract({region(1, {promoted(40)})});
        CHECK_FALSE(
            SampleRegionParameterOwner::create(std::vector{ordinary(7), ordinary(7)}, candidate));
    }
    SECTION("ordinary and promoted ID conflict") {
        const auto candidate = contract({region(1, {promoted(40)})});
        CHECK_FALSE(SampleRegionParameterOwner::create(std::vector{ordinary(40)}, candidate));
    }
}

TEST_CASE("Sample region contract rejects unsupported v1 promoted metadata",
          "[host][sample-region][parameters][negative]") {
    auto record = promoted(40);
    SECTION("audio rate") {
        record.rate = state::ParamRate::AudioRate;
    }
    SECTION("smoothing") {
        record.smoothing_ramp_seconds = 0.01f;
    }
    SECTION("negative zero smoothing") {
        record.smoothing_ramp_seconds = -0.0f;
    }
    SECTION("empty key") {
        record.key.clear();
    }
    SECTION("empty name") {
        record.name.clear();
    }
    SECTION("zero bound node") {
        record.bound_node_id = 0;
    }
    SECTION("nonzero bound port") {
        record.bound_port = 1;
    }
    SECTION("invalid range") {
        record.range.default_value = 2.0f;
    }
    CHECK_FALSE(contract({region(1, {record})}).valid());
}

TEST_CASE("Sample region binding requires exact complete manifest order and metadata",
          "[host][sample-region][parameters][binding]") {
    const auto candidate = contract({region(1, {promoted(40)})});
    auto ordinary_parameter = ordinary(7);
    ordinary_parameter.to_string = formatter_a;
    auto owner = SampleRegionParameterOwner::create(std::vector{ordinary_parameter}, candidate);
    REQUIRE(owner);
    auto manifest = std::vector<state::ParamInfo>(owner->contract().parameters().begin(),
                                                  owner->contract().parameters().end());
    SECTION("missing") {
        manifest.pop_back();
    }
    SECTION("extra") {
        manifest.push_back(ordinary(8));
    }
    SECTION("reordered") {
        std::swap(manifest[0], manifest[1]);
    }
    SECTION("one field mismatch") {
        manifest[1].unit = "%";
    }
    SECTION("different plain formatter callback") {
        manifest[0].to_string = formatter_b;
    }
    SECTION("signed zero is distinct") {
        REQUIRE(std::bit_cast<std::uint32_t>(manifest[1].range.default_value) == 0u);
        manifest[1].range.default_value = -0.0f;
    }
    auto store = store_from(manifest);
    CHECK_FALSE(owner->contract().bind(*store));
}

TEST_CASE("Sample region contracts canonicalize authored permutations",
          "[host][sample-region][parameters][ordering]") {
    auto first = promoted(40, "first");
    auto second = promoted(90, "second");
    const auto canonical = contract({region(1, {second, first}), region(2, {promoted(60)})});
    const auto permuted = contract({region(2, {promoted(60)}), region(1, {first, second})});
    REQUIRE(canonical.valid());
    REQUIRE(permuted.valid());
    CHECK(canonical.matches_promoted(permuted));

    auto changed_identity = first;
    changed_identity.bound_node_id += 1;
    CHECK_FALSE(canonical.matches_promoted(
        contract({region(2, {promoted(60)}), region(1, {changed_identity, second})})));
    auto changed_metadata = first;
    changed_metadata.name = "Different";
    CHECK_FALSE(canonical.matches_promoted(
        contract({region(2, {promoted(60)}), region(1, {changed_metadata, second})})));
}

TEST_CASE("Prepared edit publishes and later edits inherit the borrowed binding",
          "[host][sample-region][parameters][binding][transaction]") {
    const auto empty_contract = contract({});
    auto owner = SampleRegionParameterOwner::create(std::vector{ordinary(7)}, empty_contract);
    REQUIRE(owner);
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->bind_sample_region_parameters(owner->binding()).accepted);
    CHECK(edit->sample_region_parameter_binding() == &owner->binding());
    REQUIRE(edit->prepare(48000.0, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    CHECK(graph.sample_region_parameter_binding() == &owner->binding());
    const auto graph_generation = graph.sample_region_binding_generation();
    owner->store().set_value(7, 0.5f);
    CHECK(graph.sample_region_binding_generation() == graph_generation);
    auto later = graph.begin_prepared_topology_edit();
    CHECK(later->sample_region_parameter_binding() == &owner->binding());
}

TEST_CASE("Prepared edit rejects removal of the frozen promoted manifest",
          "[host][sample-region][parameters][binding][negative]") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(edit->declare_sample_region(region(1, {promoted(40)})).accepted);
    auto owner = SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
    REQUIRE(owner);
    REQUIRE(edit->bind_sample_region_parameters(owner->binding()).accepted);
    REQUIRE(edit->remove_sample_region(1).accepted);
    CHECK(edit->prepare(48000.0, 64) ==
          SignalGraph::PreparedTopologyEdit::Result::ParameterContractMismatch);
    CHECK(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::NotPrepared);
    CHECK(graph.sample_region_parameter_binding() == nullptr);
}

TEST_CASE("Prepared edit rejects a borrowed contract with different authored identity",
          "[host][sample-region][parameters][binding][negative]") {
    const auto candidate = contract({region(1, {promoted(40)})});
    auto owner = SampleRegionParameterOwner::create({}, candidate);
    REQUIRE(owner);
    SignalGraph graph;
    const auto input = graph.add_input_node(1);
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));
    const auto nodes_before = graph.nodes();
    auto edit = graph.begin_prepared_topology_edit();
    const auto rejected = edit->bind_sample_region_parameters(owner->binding());
    CHECK_FALSE(rejected.accepted);
    CHECK(rejected.reason == SampleRegionRefusalReason::ParameterContractMismatch);
    CHECK(graph.is_prepared());
    CHECK(graph.nodes().size() == nodes_before.size());
    CHECK(graph.sample_region_parameter_binding() == nullptr);
}

TEST_CASE("Baked graph publishes the frozen promoted parameter manifest",
          "[host][sample-region][parameters][baked]") {
    auto coefficient = promoted(90);
    coefficient.name = "Allpass Coefficient";
    coefficient.range = {-0.99f, 0.99f, 0.5f, 0.0f};
    auto feedback = promoted(40, "feedback");
    feedback.name = "Feedback";
    feedback.range = {0.0f, 1.0f, 0.25f, 0.0f};
    const auto definition = region(9, {coefficient, feedback});

    state::StateStore store;
    auto result = BakedGraphProcessor::create_with_sample_regions(
        {}, {}, 1, 1, "Baked", "com.test.baked", {}, {definition});
    REQUIRE(result.accepted);
    REQUIRE(result.processor);
    auto& processor = static_cast<BakedGraphProcessor&>(*result.processor);
    processor.define_parameters(store);
    processor.define_parameters(store);

    REQUIRE(processor.sample_region_parameter_contract().frozen());
    REQUIRE(processor.sample_region_parameter_binding() != nullptr);
    CHECK(&processor.sample_region_parameter_binding()->store() == &store);
    CHECK(processor.sample_region_parameter_binding()->value(90) == 0.5f);
    REQUIRE(store.param_count() == 2);
    const auto manifest = store.all_params();
    CHECK(manifest[0].id == 40);
    CHECK(manifest[0].name == "Feedback");
    CHECK(manifest[0].range.default_value == 0.25f);
    CHECK(manifest[0].rate == state::ParamRate::ControlRate);
    CHECK(manifest[0].smoothing_ramp_seconds == 0.0f);
    CHECK(manifest[1].id == 90);
    CHECK(manifest[1].name == "Allpass Coefficient");
    CHECK(manifest[1].range.default_value == 0.5f);
    CHECK(store.get_value(40) == 0.25f);
    CHECK(store.get_value(90) == 0.5f);

    std::vector<state::ParamID> begins;
    std::vector<state::ParamID> ends;
    store.set_gesture_callbacks([&](state::ParamID id) { begins.push_back(id); },
                                [&](state::ParamID id) { ends.push_back(id); });
    const auto generation = store.state_generation();
    store.begin_gesture(90);
    store.set_value(90, 0.75f);
    store.end_gesture(90);
    CHECK(store.get_value(90) == 0.75f);
    CHECK(store.state_generation() > generation);
    CHECK(begins == std::vector<state::ParamID>{90});
    CHECK(ends == std::vector<state::ParamID>{90});
    CHECK(store.open_gesture_count() == 0);
}

TEST_CASE("Baked graph without regions preserves the empty legacy manifest",
          "[host][sample-region][parameters][baked][compatibility]") {
    BakedGraphProcessor processor({}, {}, 1, 1, "Baked", "com.test.baked");
    state::StateStore store;
    processor.define_parameters(store);
    CHECK(store.param_count() == 0);
    CHECK(store.state_generation() == 0);
    CHECK(processor.sample_region_parameter_binding() == nullptr);
}

TEST_CASE("Baked graph factory rejects an invalid promoted contract before publication",
          "[host][sample-region][parameters][baked][negative]") {
    auto invalid = promoted(90);
    invalid.bound_node_id = 0;
    const auto result = BakedGraphProcessor::create_with_sample_regions(
        {}, {}, 1, 1, "Baked", "com.test.baked", {}, {region(9, {invalid})});
    CHECK_FALSE(result.accepted);
    CHECK(result.processor == nullptr);
    CHECK(result.reason == LowerRejectReason::ParameterContractMismatch);
    CHECK_FALSE(result.message.empty());
}

TEST_CASE("Baked graph rejects an unexpected prepopulated adapter manifest",
          "[host][sample-region][parameters][baked][negative]") {
    auto result = BakedGraphProcessor::create_with_sample_regions(
        {}, {}, 1, 1, "Baked", "com.test.baked", {}, {region(9, {promoted(90)})});
    REQUIRE(result.accepted);
    REQUIRE(result.processor);
    auto& processor = static_cast<BakedGraphProcessor&>(*result.processor);
    state::StateStore store;
    store.add_parameter(ordinary(7));
    processor.define_parameters(store);
    CHECK(processor.sample_region_parameter_binding() == nullptr);
    REQUIRE(store.param_count() == 1);
    CHECK(store.all_params()[0].id == 7);
    CHECK(store.state_generation() == 0);
}

TEST_CASE("Baked region contract metadata survives plan reload in canonical order",
          "[host][sample-region][parameters][baked][reload]") {
    auto definition = region(9, {promoted(40, "feedback")});
    definition.region_id = 17;
    definition.promoted_parameters[0].range = state::ParamRange::linear(0.0f, 1.0f, 0.5f);
    definition.promoted_parameters[0].bound_node_id = 5;
    BakedPlan plan;
    plan.format_version = kBakedMaxSupportedFormatVersion;
    plan.input_channels = 1;
    plan.output_channels = 1;
    plan.nodes = {{1, NodeType::AudioInput, 0, 1},
                  {2, NodeType::AudioOutput, 1, 0},
                  {3, NodeType::Custom, 1, 1, 1.0f, "pulp.core.sample-region.input", 1},
                  {4, NodeType::Custom, 1, 1, 1.0f, "pulp.core.sample-region.output", 1},
                  {5, NodeType::Custom, 0, 1, 1.0f, "pulp.core.sample-region.parameter", 1},
                  {6, NodeType::Custom, 0, 1, 1.0f, "pulp.core.sample-region.constant", 1},
                  {7, NodeType::Custom, 2, 1, 1.0f, "pulp.core.sample-region.add", 1},
                  {8, NodeType::Custom, 1, 1, 1.0f, "pulp.core.unit-delay", 1},
                  {9, NodeType::Custom, 2, 1, 1.0f, "pulp.core.sample-region.multiply", 1}};
    plan.connections = {{1, 0, 3, 0, false}, {3, 0, 7, 0, false}, {8, 0, 7, 1, false},
                        {7, 0, 8, 0, false}, {7, 0, 9, 0, false}, {5, 0, 9, 1, false},
                        {9, 0, 4, 0, false}, {4, 0, 2, 0, false}};
    auto reload_definition = definition;
    for (auto& parameter : reload_definition.promoted_parameters)
        parameter.range.skew = 1.0f;
    reload_definition.members = {
        {3, "pulp.core.sample-region.input", 1, {SampleKernelConfigKind::BoundaryIndex, 0, 0.0f}},
        {4, "pulp.core.sample-region.output", 1, {SampleKernelConfigKind::BoundaryIndex, 0, 0.0f}},
        {5,
         "pulp.core.sample-region.parameter",
         1,
         {SampleKernelConfigKind::PromotedParameterId, 40, 0.0f}},
        {6,
         "pulp.core.sample-region.constant",
         1,
         {SampleKernelConfigKind::FiniteConstant, 0, 0.5f}},
        {7, "pulp.core.sample-region.add", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
        {8, "pulp.core.unit-delay", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
        {9, "pulp.core.sample-region.multiply", 1, {SampleKernelConfigKind::None, 0, 0.0f}},
    };
    reload_definition.input_boundaries = {3};
    reload_definition.output_boundaries = {4};
    plan.sample_regions.push_back(reload_definition);
    const auto bytes = detail::serialize_plan(plan);
    const auto reloaded = detail::parse_plan_bounded(bytes);
    REQUIRE(reloaded);
    const auto authored = SampleRegionParameterContract::from_regions(plan.sample_regions);
    const auto restored = SampleRegionParameterContract::from_regions(reloaded->sample_regions);
    REQUIRE(authored.valid());
    REQUIRE(restored.valid());
    CHECK(authored.matches_promoted(restored));
    REQUIRE(restored.entries().size() == 1);
    CHECK(restored.entries()[0].info.id == 40);
}

TEST_CASE("Baked parameter binding does not outlive its adapter store",
          "[host][sample-region][parameters][baked][lifetime]") {
    auto store = std::make_unique<state::StateStore>();
    const auto definition = region(9, {promoted(90)});
    {
        auto result = BakedGraphProcessor::create_with_sample_regions(
            {}, {}, 1, 1, "Baked", "com.test.baked", {}, {definition});
        REQUIRE(result.accepted);
        REQUIRE(result.processor);
        auto& processor = static_cast<BakedGraphProcessor&>(*result.processor);
        processor.define_parameters(*store);
        REQUIRE(processor.sample_region_parameter_binding() != nullptr);
        CHECK(&processor.sample_region_parameter_binding()->store() == store.get());
    }
    CHECK(store->param_count() == 1);
}
