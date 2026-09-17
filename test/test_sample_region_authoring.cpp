#include <catch2/catch_test_macros.hpp>

#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <type_traits>

using namespace pulp::host;

namespace {

using Edit = SignalGraph::PreparedTopologyEdit;
using Result = Edit::Result;
using Reason = SampleRegionRefusalReason;

static_assert(static_cast<int>(Result::Prepared) == 0);
static_assert(static_cast<int>(Result::AlreadyCommitted) == 18);
static_assert(static_cast<int>(Result::RegionRuntimeUnavailable) == 19);
static_assert(static_cast<int>(Result::ParameterContractMismatch) == 20);
static_assert(std::is_copy_constructible_v<SampleRegionDefinition>);
static_assert(std::is_copy_constructible_v<SampleRegionDescriptor>);
static_assert(std::is_copy_constructible_v<SampleRegionResult>);

SampleKernelConfig none() {
    return {SampleKernelConfigKind::None, 0, 0.0f};
}
SampleKernelConfig boundary(std::uint32_t index = 0) {
    return {SampleKernelConfigKind::BoundaryIndex, index, 0.0f};
}

struct Fixture {
    std::unique_ptr<SampleRegionParameterOwner> parameter_owner;
    SignalGraph graph;
    NodeId input = graph.add_input_node(1);
    NodeId gain = graph.add_gain_node();
    NodeId output = graph.add_output_node(1);
    std::unique_ptr<Edit> edit;
    NodeId region_input = 0;
    NodeId add = 0;
    NodeId delay = 0;
    NodeId region_output = 0;

    Fixture() {
        REQUIRE(graph.connect(input, 0, gain, 0));
        REQUIRE(graph.connect(gain, 0, output, 0));
        REQUIRE(graph.set_node_gain(gain, 0.5f));
        REQUIRE(graph.prepare(48000.0, 64));
        parameter_owner = SampleRegionParameterOwner::create(
            {},
            SampleRegionParameterContract::from_regions(std::span<const SampleRegionDefinition>{}));
        REQUIRE(parameter_owner);
        edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit->bind_sample_region_parameters(parameter_owner->binding()).accepted);
        REQUIRE(register_builtin_sample_region_types(*edit));
        region_input = edit->add_custom_node("pulp.core.sample-region.input");
        add = edit->add_custom_node("pulp.core.sample-region.add");
        delay = edit->add_custom_node("pulp.core.unit-delay");
        region_output = edit->add_custom_node("pulp.core.sample-region.output");
        REQUIRE(edit->connect(input, 0, region_input, 0));
        REQUIRE(edit->connect(region_input, 0, add, 0));
        REQUIRE(edit->connect(add, 0, delay, 0));
        REQUIRE(edit->connect(add, 0, region_output, 0));
        REQUIRE(edit->connect(region_output, 0, output, 0));
    }

    SampleRegionDefinition definition(SampleRegionId id = 9) const {
        SampleRegionDefinition value;
        value.region_id = id;
        value.members = {
            {region_output, "pulp.core.sample-region.output", 1, boundary()},
            {delay, "pulp.core.unit-delay", 1, none()},
            {add, "pulp.core.sample-region.add", 1, none()},
            {region_input, "pulp.core.sample-region.input", 1, boundary()},
        };
        value.input_boundaries = {region_input};
        value.output_boundaries = {region_output};
        return value;
    }

    void declare() {
        REQUIRE(edit->declare_sample_region(definition()).accepted);
        REQUIRE(edit->connect_in_sample_region(9, delay, 0, add, 1).accepted);
    }

    void check_owner() {
        CHECK(graph.nodes().size() == 3);
        CHECK(graph.connections().size() == 2);
        CHECK(graph.custom_node_type_count() == 0);
        CHECK(graph.node_gain(gain) == 0.5f);
        CHECK(graph.sample_regions().empty());
        CHECK_FALSE(graph.sample_region(9));
        CHECK(graph.prove_sample_region(9).reason == Reason::UnknownRegion);
        CHECK(graph.prepared_stats().node_count == 3);
        for (const int frames : {1, 17, 64, 3}) {
            std::array<float, 64> source{};
            std::array<float, 64> rendered{};
            for (int i = 0; i < frames; ++i)
                source[i] = static_cast<float>(i - 7) / 16.0f;
            const float* input_ptrs[] = {source.data()};
            float* output_ptrs[] = {rendered.data()};
            pulp::audio::BufferView<const float> in(input_ptrs, 1, frames);
            pulp::audio::BufferView<float> out(output_ptrs, 1, frames);
            graph.process(out, in, frames);
            for (int i = 0; i < frames; ++i)
                CHECK(rendered[i] == source[i] * 0.5f);
        }
    }
};

} // namespace

TEST_CASE("Sample region authoring proves delayed cycles without publishing a runtime",
          "[host][sample-region][authoring][transaction]") {
    Fixture fixture;
    auto sentinel = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(sentinel->prepare(48000.0, 64) == Result::Prepared);
    fixture.declare();
    auto proof = fixture.edit->prove_sample_region(9);
    REQUIRE(proof.accepted);
    CHECK(proof.resources.member_nodes == 4);
    CHECK(proof.resources.delay_nodes == 1);
    CHECK(proof.resources.state_bytes == sizeof(float));
    CHECK(std::all_of(fixture.edit->connections().begin(), fixture.edit->connections().end(),
                      [](const auto& edge) { return !edge.feedback; }));
    auto snapshot = fixture.edit->sample_region(9);
    REQUIRE(snapshot);
    CHECK(std::is_sorted(snapshot->members.begin(), snapshot->members.end(),
                         [](const auto& a, const auto& b) { return a.node < b.node; }));
    snapshot->members.front().type_id = "mutated copy";
    snapshot->members.clear();
    CHECK(fixture.edit->sample_region(9)->members.size() == 4);
    REQUIRE(fixture.edit->prepare(48000.0, 64) == Result::RegionRuntimeUnavailable);
    CHECK_FALSE(fixture.edit->routed_execution_ready(64));
    CHECK_FALSE(fixture.edit->committed_execution_snapshot());
    CHECK(fixture.edit->prove_sample_region(9).accepted);
    CHECK(fixture.edit->prove_sample_region(9).resources.logical_boundary_bytes == 512);
    CHECK(fixture.edit->commit() == Result::NotPrepared);
    fixture.check_owner();
    fixture.edit.reset();
    fixture.check_owner();
    // This was prepared before the rejected candidate: its exact owner base
    // and live snapshot must still match, or the commit returns StaleBase.
    CHECK(sentinel->commit() == Result::Committed);
    CHECK(fixture.graph.add_gain_node() == fixture.region_input);
}

TEST_CASE("Sample region quiesced preparation refuses before shared lifecycles change",
          "[host][sample-region][authoring][transaction]") {
    Fixture fixture;
    fixture.declare();
    auto sentinel = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(sentinel->prepare(48000.0, 64) == Result::Prepared);
    CHECK(fixture.edit->prepare_quiesced(96000.0, 32) == Result::RegionRuntimeUnavailable);
    CHECK(fixture.edit->commit() == Result::NotPrepared);
    fixture.check_owner();
    CHECK(sentinel->commit() == Result::Committed);
}

TEST_CASE("Removing the required sample delay exposes the instantaneous cycle",
          "[host][sample-region][authoring][causality]") {
    Fixture fixture;
    fixture.declare();
    REQUIRE(fixture.edit->prove_sample_region(9).accepted);
    auto sentinel = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(sentinel->prepare(48000.0, 64) == Result::Prepared);
    REQUIRE(fixture.edit->remove_sample_region_member(9, fixture.delay).accepted);
    REQUIRE(fixture.edit->remove_node(fixture.delay));
    REQUIRE(fixture.edit->connect_in_sample_region(9, fixture.add, 0, fixture.add, 1).accepted);
    const auto proof = fixture.edit->prove_sample_region(9);
    CHECK(proof.reason == Reason::InstantaneousCycle);
    CHECK(proof.offending_node == fixture.add);
    CHECK(fixture.edit->prepare(48000.0, 64) == Result::PreflightFailed);
    CHECK(fixture.edit->commit() == Result::NotPrepared);
    fixture.check_owner();
    CHECK(sentinel->commit() == Result::Committed);
}

TEST_CASE("Rejected region mutation prevents an already prepared candidate from committing",
          "[host][sample-region][authoring][transaction]") {
    Fixture fixture;
    auto sentinel = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(sentinel->prepare(48000.0, 64) == Result::Prepared);
    auto prepared = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(prepared->set_node_gain(fixture.gain, 0.25f));
    SECTION("ordinary preparation") {
        REQUIRE(prepared->prepare(48000.0, 64) == Result::Prepared);
    }
    SECTION("quiesced preparation") {
        REQUIRE(prepared->prepare_quiesced(96000.0, 32) == Result::Prepared);
    }
    SampleRegionDefinition definition;
    definition.region_id = 9;
    const auto result = prepared->declare_sample_region(std::move(definition));
    CHECK_FALSE(result.accepted);
    CHECK(result.reason == Reason::PrepareFailed);
    CHECK(prepared->commit() == Result::InvalidMutation);
    fixture.check_owner();
    prepared.reset();
    fixture.check_owner();
    CHECK(sentinel->commit() == Result::Committed);
}

TEST_CASE("Sample region producer and boundary defects retain exact proof diagnostics",
          "[host][sample-region][authoring][boundary]") {
    Fixture fixture;
    fixture.declare();
    REQUIRE(fixture.edit->prove_sample_region(9).accepted);
    Reason expected = Reason::InvalidProducerCardinality;
    SECTION("unconnected input") {
        REQUIRE(fixture.edit->disconnect(fixture.region_input, 0, fixture.add, 0));
    }
    SECTION("implicit multi-producer port") {
        REQUIRE(fixture.edit->connect_in_sample_region(9, fixture.region_input, 0, fixture.add, 1)
                    .accepted);
    }
    SECTION("noncontiguous boundary index") {
        REQUIRE(
            fixture.edit->set_sample_kernel_config(9, fixture.region_input, boundary(2)).accepted);
        expected = Reason::InvalidBoundary;
    }
    SECTION("duplicate boundary index") {
        const auto extra = fixture.edit->add_custom_node("pulp.core.sample-region.input");
        REQUIRE(fixture.edit->add_sample_region_member(9, extra, boundary(0)).accepted);
        REQUIRE(fixture.edit->connect(fixture.input, 0, extra, 0));
        expected = Reason::InvalidBoundary;
    }
    SECTION("illegal outside to ordinary member crossing") {
        REQUIRE(fixture.edit->connect(fixture.input, 0, fixture.add, 0));
        expected = Reason::InvalidBoundaryCrossing;
    }
    SECTION("illegal output boundary to internal member") {
        REQUIRE(
            fixture.edit->connect_in_sample_region(9, fixture.region_output, 0, fixture.delay, 0)
                .accepted);
    }
    SECTION("legacy feedback inside the region") {
        REQUIRE(fixture.edit->disconnect(fixture.delay, 0, fixture.add, 1));
        REQUIRE(fixture.edit->connect_feedback(fixture.delay, 0, fixture.add, 1));
        expected = Reason::LegacyFeedbackInRegion;
    }
    SECTION("MIDI crossing") {
        REQUIRE(fixture.edit->connect_midi(fixture.input, fixture.add));
        expected = Reason::UnsupportedConnectionLane;
    }
    const auto proof = fixture.edit->prove_sample_region(9);
    CHECK_FALSE(proof.accepted);
    CHECK(proof.reason == expected);
    CHECK(proof.region_id == 9);
    CHECK_FALSE(proof.message.empty());
    if (expected == Reason::InvalidBoundaryCrossing || expected == Reason::LegacyFeedbackInRegion ||
        expected == Reason::UnsupportedConnectionLane)
        CHECK(proof.has_offending_connection);
    CHECK(fixture.edit->prepare(48000.0, 64) == Result::PreflightFailed);
    CHECK(fixture.edit->commit() == Result::NotPrepared);
    fixture.check_owner();
}

TEST_CASE("Sample region impossible identities and malformed configurations fail at mutation",
          "[host][sample-region][authoring][validation]") {
    Fixture fixture;
    auto definition = fixture.definition();
    Reason expected = Reason::UnknownMember;
    SECTION("zero region identity") {
        definition.region_id = 0;
        expected = Reason::UnknownRegion;
    }
    SECTION("missing member") {
        definition.members.front().node = 100000;
    }
    SECTION("duplicate member") {
        definition.members.push_back(definition.members.front());
    }
    SECTION("ordinary graph node") {
        definition.members.front().node = fixture.gain;
        expected = Reason::UnsupportedNodeKind;
    }
    SECTION("exact version mismatch") {
        definition.members.front().version = 2;
        expected = Reason::UnresolvedSampleKernel;
    }
    SECTION("missing exact descriptor") {
        const auto unknown =
            fixture.edit->add_unresolved_custom_node("pulp.test.unknown", 4, 1, 1, "unknown");
        definition.members.push_back({unknown, "pulp.test.unknown", 4, none()});
        expected = Reason::UnresolvedSampleKernel;
    }
    SECTION("wrong config tag") {
        definition.members.front().config = none();
        expected = Reason::InvalidKernelConfig;
    }
    SECTION("nonfinite constant") {
        const auto constant = fixture.edit->add_custom_node("pulp.core.sample-region.constant");
        definition.members.push_back(
            {constant,
             "pulp.core.sample-region.constant",
             1,
             {SampleKernelConfigKind::FiniteConstant, 0, std::numeric_limits<float>::infinity()}});
        expected = Reason::InvalidKernelConfig;
    }
    SECTION("unused config payload") {
        definition.members[1].config.constant = 1.0f;
        expected = Reason::InvalidKernelConfig;
    }
    SECTION("duplicate boundary identity") {
        definition.input_boundaries.push_back(fixture.region_input);
        expected = Reason::InvalidBoundary;
    }
    const auto result = fixture.edit->declare_sample_region(std::move(definition));
    CHECK_FALSE(result.accepted);
    CHECK(result.reason == expected);
    CHECK(fixture.edit->sample_regions().empty());
    CHECK(fixture.edit->prepare(48000.0, 64) == Result::InvalidMutation);
    fixture.check_owner();
}

TEST_CASE("Sample region membership and edge scope cannot be bypassed",
          "[host][sample-region][authoring][validation]") {
    Fixture fixture;
    fixture.declare();
    SECTION("overlapping region") {
        CHECK(fixture.edit->declare_sample_region(fixture.definition(10)).reason ==
              Reason::MemberInMultipleRegions);
    }
    SECTION("duplicate region") {
        CHECK(fixture.edit->declare_sample_region(fixture.definition()).reason ==
              Reason::UnknownRegion);
    }
    SECTION("unknown region") {
        CHECK(fixture.edit->connect_in_sample_region(18, fixture.delay, 0, fixture.add, 1).reason ==
              Reason::UnknownRegion);
    }
    SECTION("invalid scalar port") {
        CHECK(fixture.edit->connect_in_sample_region(9, fixture.delay, 1, fixture.add, 1).reason ==
              Reason::InvalidProducerCardinality);
    }
    SECTION("duplicate connection") {
        CHECK(fixture.edit->connect_in_sample_region(9, fixture.delay, 0, fixture.add, 1).reason ==
              Reason::InvalidProducerCardinality);
    }
    SECTION("region-aware crossing") {
        const auto result =
            fixture.edit->connect_in_sample_region(9, fixture.input, 0, fixture.add, 1);
        CHECK(result.reason == Reason::InvalidBoundaryCrossing);
        CHECK(result.has_offending_connection);
        CHECK(result.offending_connection.source == fixture.input);
    }
    SECTION("member removal requires explicit membership removal") {
        CHECK_FALSE(fixture.edit->remove_node(fixture.delay));
    }
    SECTION("ordinary connect retains cycle rejection") {
        REQUIRE(fixture.edit->disconnect(fixture.delay, 0, fixture.add, 1));
        CHECK_FALSE(fixture.edit->connect(fixture.delay, 0, fixture.add, 1));
    }
    CHECK(fixture.edit->prepare(48000.0, 64) == Result::InvalidMutation);
    fixture.check_owner();
}

TEST_CASE("Sample region resource proof is reachable through the private authoring candidate",
          "[host][sample-region][authoring][resources]") {
    Fixture fixture;
    auto definition = fixture.definition();
    definition.limits.max_state_bytes = sizeof(float) - 1;
    REQUIRE(fixture.edit->declare_sample_region(std::move(definition)).accepted);
    REQUIRE(fixture.edit->connect_in_sample_region(9, fixture.delay, 0, fixture.add, 1).accepted);
    const auto proof = fixture.edit->prove_sample_region(9);
    CHECK(proof.reason == Reason::StateBudgetExceeded);
    CHECK(proof.actual == sizeof(float));
    CHECK(proof.limit == sizeof(float) - 1);
    CHECK(fixture.edit->prepare(48000.0, 64) == Result::PreflightFailed);
    fixture.check_owner();
}

TEST_CASE("RegionOnly kernels cannot execute through direct or undeclared block preparation",
          "[host][sample-region][authoring][compatibility]") {
    SignalGraph graph;
    REQUIRE(register_builtin_sample_region_types(graph));
    const auto node = graph.add_custom_node("pulp.core.sample-region.constant");
    REQUIRE(node != 0);
    CHECK_FALSE(graph.prepare(48000.0, 64));
    auto edit = graph.begin_prepared_topology_edit();
    CHECK(edit->prepare(48000.0, 64) == Result::PreflightFailed);
    CHECK(edit->commit() == Result::NotPrepared);
    auto quiesced = graph.begin_prepared_topology_edit();
    CHECK(quiesced->prepare_quiesced(48000.0, 64) == Result::PreflightFailed);
    CHECK(quiesced->commit() == Result::NotPrepared);
}

TEST_CASE("Sample region quotient refuses an exterior cycle even when the flat graph is acyclic",
          "[host][sample-region][authoring][causality]") {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(register_builtin_sample_region_types(*edit));
    const auto input = edit->add_custom_node("pulp.core.sample-region.input");
    const auto constant = edit->add_custom_node("pulp.core.sample-region.constant");
    const auto first_output = edit->add_custom_node("pulp.core.sample-region.output");
    const auto second_output = edit->add_custom_node("pulp.core.sample-region.output");
    const auto outside = edit->add_gain_node();
    REQUIRE(edit->connect(constant, 0, first_output, 0));
    REQUIRE(edit->connect(first_output, 0, outside, 0));
    REQUIRE(edit->connect(outside, 0, input, 0));
    REQUIRE(edit->connect(input, 0, second_output, 0));
    SampleRegionDefinition definition;
    definition.region_id = 1;
    definition.members = {
        {input, "pulp.core.sample-region.input", 1, boundary()},
        {constant,
         "pulp.core.sample-region.constant",
         1,
         {SampleKernelConfigKind::FiniteConstant, 0, 0.25f}},
        {first_output, "pulp.core.sample-region.output", 1, boundary(0)},
        {second_output, "pulp.core.sample-region.output", 1, boundary(1)},
    };
    definition.input_boundaries = {input};
    definition.output_boundaries = {second_output, first_output};
    REQUIRE(edit->declare_sample_region(definition).accepted);
    CHECK(edit->sample_region(1)->output_boundaries ==
          std::vector<NodeId>{first_output, second_output});
    CHECK(edit->prove_sample_region(1).reason == Reason::CycleCrossesRegionBoundary);
    CHECK(edit->prepare(48000.0, 64) == Result::PreflightFailed);
    CHECK(edit->commit() == Result::NotPrepared);
    CHECK(graph.nodes().empty());
}

TEST_CASE("Sample region promoted metadata is canonical and checked against its fixed manifest",
          "[host][sample-region][authoring][parameters]") {
    Fixture fixture;
    const auto parameter = fixture.edit->add_custom_node("pulp.core.sample-region.parameter");
    const auto multiply = fixture.edit->add_custom_node("pulp.core.sample-region.multiply");
    REQUIRE(fixture.edit->disconnect(fixture.add, 0, fixture.region_output, 0));
    REQUIRE(fixture.edit->connect(fixture.add, 0, multiply, 0));
    REQUIRE(fixture.edit->connect(parameter, 0, multiply, 1));
    REQUIRE(fixture.edit->connect(multiply, 0, fixture.region_output, 0));
    auto definition = fixture.definition();
    definition.members.push_back({parameter,
                                  "pulp.core.sample-region.parameter",
                                  1,
                                  {SampleKernelConfigKind::PromotedParameterId, 29, 0.0f}});
    definition.members.push_back({multiply, "pulp.core.sample-region.multiply", 1, none()});
    SampleRegionPromotedParameter promoted;
    promoted.param_id = 29;
    promoted.key = "coefficient";
    promoted.name = "Coefficient";
    promoted.range = pulp::state::ParamRange::linear(-0.99f, 0.99f, 0.5f);
    promoted.bound_node_id = parameter;
    definition.promoted_parameters = {promoted};
    bool valid = false;
    bool bind = true;
    bool change_after_binding = false;
    SECTION("exact control rate manifest") {
        valid = true;
    }
    SECTION("missing binding") {
        valid = true;
        bind = false;
    }
    SECTION("inherited binding rejects changed metadata") {
        valid = true;
        change_after_binding = true;
    }
    SECTION("unsupported parameter rate") {
        definition.promoted_parameters[0].rate = pulp::state::ParamRate::AudioRate;
    }
    SECTION("smoothing is not implicit") {
        definition.promoted_parameters[0].smoothing_ramp_seconds = 0.01f;
    }
    SECTION("wrong bound identity") {
        definition.promoted_parameters[0].bound_node_id = fixture.add;
    }
    SECTION("nonfinite range") {
        definition.promoted_parameters[0].range.max = std::numeric_limits<float>::infinity();
    }
    SECTION("missing manifest entry") {
        definition.promoted_parameters.clear();
    }
    REQUIRE(fixture.edit->declare_sample_region(definition).accepted);
    REQUIRE(fixture.edit->connect_in_sample_region(9, fixture.delay, 0, fixture.add, 1).accepted);
    std::unique_ptr<SampleRegionParameterOwner> promoted_owner;
    if (valid && bind) {
        promoted_owner = SampleRegionParameterOwner::create(
            {}, fixture.edit->sample_region_parameter_contract());
        REQUIRE(promoted_owner);
        REQUIRE(fixture.edit->bind_sample_region_parameters(promoted_owner->binding()).accepted);
    }
    if (change_after_binding) {
        REQUIRE(fixture.edit->remove_sample_region(9).accepted);
        definition.promoted_parameters[0].name = "Changed coefficient";
        REQUIRE(fixture.edit->declare_sample_region(definition).accepted);
    }
    const auto proof = fixture.edit->prove_sample_region(9);
    CHECK(proof.accepted == valid);
    CHECK(proof.reason == (valid ? Reason::None : Reason::ParameterContractMismatch));
    const auto expected_prepare = !valid ? Result::PreflightFailed
                                  : (!bind || change_after_binding)
                                      ? Result::ParameterContractMismatch
                                      : Result::RegionRuntimeUnavailable;
    CHECK(fixture.edit->prepare(48000.0, 64) == expected_prepare);
    CHECK(fixture.edit->commit() == Result::NotPrepared);
    fixture.check_owner();
}

TEST_CASE("Incomplete sample region declarations can be assembled and explicitly removed",
          "[host][sample-region][authoring][transaction]") {
    Fixture fixture;
    SampleRegionDefinition empty;
    empty.region_id = 9;
    REQUIRE(fixture.edit->declare_sample_region(empty).accepted);
    CHECK(fixture.edit->prove_sample_region(9).reason == Reason::InvalidBoundary);
    REQUIRE(fixture.edit->add_sample_region_member(9, fixture.region_input, boundary()).accepted);
    REQUIRE(fixture.edit->add_sample_region_member(9, fixture.add, none()).accepted);
    REQUIRE(fixture.edit->add_sample_region_member(9, fixture.delay, none()).accepted);
    REQUIRE(fixture.edit->add_sample_region_member(9, fixture.region_output, boundary()).accepted);
    REQUIRE(fixture.edit->connect_in_sample_region(9, fixture.delay, 0, fixture.add, 1).accepted);
    REQUIRE(fixture.edit->prove_sample_region(9).accepted);
    REQUIRE(fixture.edit->remove_sample_region(9).accepted);
    CHECK(fixture.edit->sample_regions().empty());
    CHECK(fixture.edit->prove_sample_region(9).reason == Reason::UnknownRegion);
    REQUIRE(fixture.edit->remove_node(fixture.region_input));
    REQUIRE(fixture.edit->remove_node(fixture.add));
    REQUIRE(fixture.edit->remove_node(fixture.delay));
    REQUIRE(fixture.edit->remove_node(fixture.region_output));
    REQUIRE(fixture.edit->prepare(48000.0, 64) == Result::Prepared);
    REQUIRE(fixture.edit->commit() == Result::Committed);
    CHECK(fixture.graph.sample_regions().empty());
    CHECK(fixture.graph.nodes().size() == 3);
}

TEST_CASE("Rejected region preparation never enters a retained custom lifecycle",
          "[host][sample-region][authoring][lifecycle]") {
    SignalGraph graph;
    int prepare_calls = 0;
    int release_calls = 0;
    CustomNodeType custom;
    custom.type_id = "pulp.test.lifecycle";
    custom.num_input_ports = 1;
    custom.num_output_ports = 1;
    custom.create = []() -> void* { return new int(0); };
    custom.destroy = [](void* value) { delete static_cast<int*>(value); };
    custom.prepare = [&](void*, double, int) { ++prepare_calls; };
    custom.release = [&](void*) { ++release_calls; };
    custom.process_instance = [](void*, pulp::audio::BufferView<float>& output,
                                 const pulp::audio::BufferView<const float>& input, int frames) {
        for (int i = 0; i < frames; ++i)
            output.channel(0)[i] = input.channel(0)[i];
    };
    REQUIRE(graph.register_custom_node_type(std::move(custom)));
    const auto input = graph.add_input_node(1);
    const auto retained = graph.add_custom_node("pulp.test.lifecycle");
    const auto output = graph.add_output_node(1);
    REQUIRE(graph.connect(input, 0, retained, 0));
    REQUIRE(graph.connect(retained, 0, output, 0));
    REQUIRE(graph.prepare(48000.0, 64));
    REQUIRE(prepare_calls == 1);
    auto edit = graph.begin_prepared_topology_edit();
    REQUIRE(register_builtin_sample_region_types(*edit));
    const auto region_input = edit->add_custom_node("pulp.core.sample-region.input");
    const auto region_output = edit->add_custom_node("pulp.core.sample-region.output");
    REQUIRE(edit->connect(input, 0, region_input, 0));
    REQUIRE(edit->connect(region_input, 0, region_output, 0));
    REQUIRE(edit->connect(region_output, 0, output, 0));
    SampleRegionDefinition definition;
    definition.region_id = 1;
    definition.members = {
        {region_input, "pulp.core.sample-region.input", 1, boundary()},
        {region_output, "pulp.core.sample-region.output", 1, boundary()},
    };
    definition.input_boundaries = {region_input};
    definition.output_boundaries = {region_output};
    REQUIRE(edit->declare_sample_region(definition).accepted);
    REQUIRE(edit->prove_sample_region(1).accepted);
    auto parameter_owner =
        SampleRegionParameterOwner::create({}, edit->sample_region_parameter_contract());
    REQUIRE(parameter_owner);
    REQUIRE(edit->bind_sample_region_parameters(parameter_owner->binding()).accepted);
    CHECK(edit->prepare_quiesced(96000.0, 128) == Result::RegionRuntimeUnavailable);
    CHECK(prepare_calls == 1);
    CHECK(release_calls == 0);
    edit.reset();
    CHECK(prepare_calls == 1);
    CHECK(release_calls == 0);
    graph.release();
    CHECK(release_calls == 1);
}

TEST_CASE("Id-scoped sample region proof reports the graph-wide region ceiling",
          "[host][sample-region][authoring][resources]") {
    Fixture fixture;
    fixture.declare();
    REQUIRE(fixture.edit->prove_sample_region(9).accepted);
    for (SampleRegionId id = 10; id < 26; ++id) {
        SampleRegionDefinition definition;
        definition.region_id = id;
        REQUIRE(fixture.edit->declare_sample_region(definition).accepted);
    }
    const auto proof = fixture.edit->prove_sample_region(9);
    CHECK_FALSE(proof.accepted);
    CHECK(proof.reason == Reason::RegionLimitExceeded);
    CHECK(proof.region_id == 9);
    CHECK(proof.actual == 17);
    CHECK(proof.limit == 16);
    CHECK(fixture.edit->prepare(48000.0, 64) == Result::PreflightFailed);
    fixture.check_owner();
}
