#include <pulp/inspect/control_manifest.hpp>
#include <pulp/inspect/control_protocol.hpp>

#include <catch2/catch_test_macros.hpp>
#include <choc/text/choc_JSON.h>
#include <cstdint>

using namespace pulp::inspect;

namespace {
bool accepts(std::string_view operation, std::string_view json) {
    const auto* descriptor = resolve_control_operation(operation, 1);
    REQUIRE(descriptor != nullptr);
    ControlJsonSchemaDiagnostics diagnostics;
    const auto accepted = validate_control_json_schema(json, descriptor->input_schema_json,
                                                        &diagnostics);
    CAPTURE(diagnostics.explanation);
    return accepted;
}
constexpr auto read_operation = "dev.pulp.graph/sample-region.read@1";
constexpr auto edit_operation = "dev.pulp.graph/sample-region.edit@1";
}

TEST_CASE("region read input is bounded and closed", "[inspect][sample-region][schema]") {
    CHECK(accepts(read_operation, "{}"));
    CHECK(accepts(read_operation, R"({"region_id":1,"include_definition":true})"));
    for (const auto* invalid : {R"({"region_id":0})", R"({"region_id":4294967296})",
                               R"({"region_id":null})", R"({"include_definition":null})",
                               R"({"include_definition":1})", R"({"unknown":false})"}) {
        CAPTURE(invalid);
        CHECK_FALSE(accepts(read_operation, invalid));
    }
}

TEST_CASE("region edits require a bounded ordered action union", "[inspect][sample-region][schema]") {
    const auto request = [](std::string_view action) {
        return std::string("{\"region_id\":1,\"expected_graph_generation\":1,\"actions\":[") +
               std::string(action) + "]}";
    };
    CHECK(accepts(edit_operation, request(R"({"op":"set_finite_constant","node_id":2,"value":0.5})")));
    CHECK(accepts(edit_operation, request(R"({"op":"connect","source_temporary_node_id":"t1","source_port":0,"destination_node_id":3,"destination_port":0})")));
    for (const auto* action : {
             R"({"op":"set_finite_constant","node_id":0,"value":0.5})",
             R"({"op":"set_finite_constant","node_id":2,"value":null})",
             R"({"op":"remove_kernel","node_id":2,"extra":true})",
             R"({"op":"create_region","node_id":2})",
             R"({"op":"connect","source_node_id":2,"source_temporary_node_id":"t1","source_port":0,"destination_node_id":3,"destination_port":0})",
             R"({"op":"connect","source_node_id":2,"source_port":64,"destination_node_id":3,"destination_port":0})",
             R"({"op":"change_boundary_mapping","node_id":2,"boundary_kind":"audio","boundary_index":0})"}) {
        CAPTURE(action);
        CHECK_FALSE(accepts(edit_operation, request(action)));
    }
    CHECK_FALSE(accepts(edit_operation, R"({"region_id":1,"expected_graph_generation":1,"actions":[]})"));
    auto many = choc::json::parse(request(R"({"op":"remove_kernel","node_id":2})"));
    auto actions = choc::value::createEmptyArray();
    for (int i = 0; i < 65; ++i)
        actions.addArrayElement(choc::json::parse(R"({"op":"remove_kernel","node_id":2})"));
    many.setMember("actions", actions);
    CHECK_FALSE(accepts(edit_operation, choc::json::toString(many)));
    many = choc::json::parse(request(R"({"op":"remove_kernel","node_id":2})"));
    many.setMember("expected_graph_generation", int64_t{9007199254740992});
    CHECK_FALSE(accepts(edit_operation, choc::json::toString(many)));
}

#include <pulp/inspect/control_sample_region_read_executor.hpp>
#include <pulp/inspect/control_sample_region_edit_executor.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <pulp/host/graph_serializer.hpp>
#include <pulp/state/store.hpp>

namespace {
struct RegionFixture {
    pulp::state::StateStore store;
    pulp::host::SampleRegionParameterContract contract;
    std::unique_ptr<pulp::host::SampleRegionParameterBinding> binding;
    pulp::host::SignalGraph graph;
    ControlSampleRegionGeneration generation;
    std::shared_ptr<ControlSampleRegionTarget> target;
    pulp::host::NodeId constant = 0;
    pulp::host::NodeId parameter = 0;
    pulp::host::NodeId input_kernel = 0;
    pulp::host::NodeId multiply = 0;
    pulp::host::NodeId delay = 0;
    pulp::host::NodeId sum = 0;
    pulp::host::NodeId output = 0;
    RegionFixture() {
        using namespace pulp::host;
        auto edit = graph.begin_prepared_topology_edit();
        REQUIRE(edit);
        REQUIRE(register_builtin_sample_region_types(*edit));
        const auto input = edit->add_input_node(1);
        const auto graph_output = edit->add_output_node(1);
        const auto x = edit->add_custom_node("pulp.core.sample-region.input", 1);
        input_kernel = x;
        const auto p = edit->add_custom_node("pulp.core.sample-region.parameter", 1);
        parameter = p;
        multiply = edit->add_custom_node("pulp.core.sample-region.multiply", 1);
        delay = edit->add_custom_node("pulp.core.unit-delay", 1);
        constant = edit->add_custom_node("pulp.core.sample-region.constant", 1);
        sum = edit->add_custom_node("pulp.core.sample-region.add", 1);
        output = edit->add_custom_node("pulp.core.sample-region.output", 1);
        REQUIRE(edit->connect(input, 0, x, 0));
        REQUIRE(edit->connect(x, 0, multiply, 0));
        REQUIRE(edit->connect(p, 0, multiply, 1));
        REQUIRE(edit->connect(multiply, 0, delay, 0));
        REQUIRE(edit->connect(delay, 0, sum, 0));
        REQUIRE(edit->connect(constant, 0, sum, 1));
        REQUIRE(edit->connect(sum, 0, output, 0));
        REQUIRE(edit->connect(output, 0, graph_output, 0));
        SampleRegionDefinition region;
        region.region_id = 77;
        const SampleKernelConfig none{SampleKernelConfigKind::None};
        const SampleKernelConfig boundary{SampleKernelConfigKind::BoundaryIndex};
        region.members = {{x, "pulp.core.sample-region.input", 1, boundary},
            {p, "pulp.core.sample-region.parameter", 1, {SampleKernelConfigKind::PromotedParameterId, 29}},
            {multiply, "pulp.core.sample-region.multiply", 1, none},
            {delay, "pulp.core.unit-delay", 1, none},
            {constant, "pulp.core.sample-region.constant", 1, {SampleKernelConfigKind::FiniteConstant, 0, 0.5f}},
            {sum, "pulp.core.sample-region.add", 1, none},
            {output, "pulp.core.sample-region.output", 1, boundary}};
        region.input_boundaries = {x}; region.output_boundaries = {output};
        SampleRegionPromotedParameter parameter;
        parameter.param_id = 29; parameter.key = "gain"; parameter.name = "Gain";
        parameter.range = pulp::state::ParamRange::linear(-1, 1, 0.5f);
        parameter.bound_node_id = p;
        region.promoted_parameters = {parameter};
        REQUIRE(edit->declare_sample_region(region).accepted);
        contract = edit->sample_region_parameter_contract().freeze({});
        REQUIRE(contract.valid());
        for (const auto& info : contract.parameters()) store.add_parameter(info);
        binding = contract.bind(store); REQUIRE(binding);
        REQUIRE(edit->bind_sample_region_parameters(*binding).accepted);
        REQUIRE(edit->prepare(48000, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
        REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
        target = ControlSampleRegionTarget::editable(graph, store, generation);
        target->set_preparation_context(48000, 64);
    }
    ControlExecutionOutcome change(std::string value, ControlExecutionContext context = {
        .checkpoint = [] { return ControlExecutionCheckpoint::Continue; }}) {
        ControlAdmissionPlan plan;
        plan.registration_id = ControlRegistrationId{"r1"}; plan.receipt_id = ControlReceiptId{"receipt-1"};
        ControlRequestEnvelope request{.registration_id = "r1", .operation_id = edit_operation,
            .operation_version = 1, .params_json = "{\"region_id\":77,\"expected_graph_generation\":1,\"actions\":[{\"op\":\"set_finite_constant\",\"node_id\":" + std::to_string(constant) + ",\"value\":" + value + "}]}"};
        return make_control_sample_region_edit_executor([t=target](const ControlAdmissionPlan&) { return t; })(plan, request, context);
    }
    ControlExecutionOutcome actions(std::string action_json, std::uint64_t expected = 1) {
        ControlAdmissionPlan plan;
        plan.registration_id = ControlRegistrationId{"r1"};
        plan.receipt_id = ControlReceiptId{"receipt-actions"};
        ControlRequestEnvelope request{.registration_id = "r1", .operation_id = edit_operation,
            .operation_version = 1,
            .params_json = "{\"region_id\":77,\"expected_graph_generation\":" +
                std::to_string(expected) + ",\"actions\":[" + action_json + "]}"};
        return make_control_sample_region_edit_executor([t=target](const ControlAdmissionPlan&) { return t; })(
            plan, request, {.checkpoint = [] { return ControlExecutionCheckpoint::Continue; }});
    }
};
}

TEST_CASE("region publication isolates graph and parameter generations", "[inspect][sample-region][transaction]") {
    RegionFixture f;
    const auto old_state = f.store.state_generation();
    const auto old_catalog = f.store.parameter_display_revision();
    int calls = 0;
    const auto outcome = f.change("0.25", {.checkpoint = [&] {
        if (++calls == 2) {
            const auto applied = f.store.apply_normalized_gesture_if_generation(old_state, 29, 0.75f);
            REQUIRE(applied.status == pulp::state::ParameterGestureApplyStatus::Applied);
        }
        return ControlExecutionCheckpoint::Continue;
    }});
    INFO(outcome.result.explanation);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto receipt = choc::json::parse(outcome.result.detail_json);
    CHECK(receipt["old_graph_generation"].getInt64() == 1);
    CHECK(receipt["new_graph_generation"].getInt64() == 2);
    CHECK(receipt["state_retained_count"].getInt64() == 1);
    CHECK(receipt["state_reset_count"].getInt64() == 0);
    CHECK(f.generation.value == 2);
    CHECK(f.store.state_generation() == old_state + 1);
    CHECK(f.store.parameter_display_revision() == old_catalog);
    CHECK(f.change("0.5").result.result_code == ControlResultCode::StateConflict);
}

TEST_CASE("region cancellation after prepare leaves topology and state untouched", "[inspect][sample-region][transaction]") {
    RegionFixture f;
    const auto before = pulp::host::GraphSerializer::to_json(f.graph);
    const auto state = f.store.state_generation();
    int calls = 0;
    const auto outcome = f.change("0.25", {.checkpoint = [&] {
        return ++calls == 2 ? ControlExecutionCheckpoint::AuthorityRevoked : ControlExecutionCheckpoint::Continue;
    }});
    CHECK(outcome.terminal_state == ControlReceiptState::Cancelled);
    CHECK(calls == 2);
    CHECK(f.generation.value == 1);
    CHECK(f.store.state_generation() == state);
    CHECK(pulp::host::GraphSerializer::to_json(f.graph) == before);
    CHECK(f.graph.is_prepared());
    CHECK(f.change("0.1").result.result_code == ControlResultCode::InvalidRequest);
    CHECK(pulp::host::GraphSerializer::to_json(f.graph) == before);
}

TEST_CASE("full region read returns its frozen promoted contract", "[inspect][sample-region][read]") {
    RegionFixture f;
    ControlAdmissionPlan plan; plan.registration_id = ControlRegistrationId{"r1"};
    ControlRequestEnvelope request{.registration_id="r1", .operation_id=read_operation,
        .operation_version=1, .params_json=R"({"region_id":77,"include_definition":true})"};
    auto executor = make_control_sample_region_read_executor([t=f.target](const ControlAdmissionPlan&) { return t; });
    auto outcome = executor(plan, request, {.checkpoint=[] { return ControlExecutionCheckpoint::Continue; }});
    INFO(outcome.result.explanation);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto result = choc::json::parse(outcome.result.detail_json);
    REQUIRE(result["regions"].size() == 1);
    const auto promoted = result["regions"][0]["definition"]["promoted_parameters"];
    REQUIRE(promoted.size() == 1);
    CHECK(promoted[0]["param_id"].getInt64() == 29);
    CHECK(promoted[0]["smoothing_ramp_seconds"].getInt64() == 0);
}

TEST_CASE("region edit preserves ordered temporary mapping", "[inspect][sample-region][transaction]") {
    RegionFixture f;
    const auto outcome = f.actions(
        std::string(R"({"op":"add_supported_kernel","temporary_node_id":"t1","type_id":"pulp.core.unit-delay","type_version":1,"config":{"kind":"none"}},)") +
        "{\"op\":\"disconnect\",\"source_node_id\":" + std::to_string(f.sum) +
            ",\"source_port\":0,\"destination_node_id\":" + std::to_string(f.output) +
            ",\"destination_port\":0}," +
        "{\"op\":\"connect\",\"source_node_id\":" + std::to_string(f.sum) +
            ",\"source_port\":0,\"destination_temporary_node_id\":\"t1\",\"destination_port\":0}," +
        "{\"op\":\"connect\",\"source_temporary_node_id\":\"t1\",\"source_port\":0,\"destination_node_id\":" +
            std::to_string(f.output) + ",\"destination_port\":0}");
    INFO(outcome.result.explanation);
    REQUIRE(outcome.terminal_state == ControlReceiptState::Completed);
    const auto receipt = choc::json::parse(outcome.result.detail_json);
    REQUIRE(receipt["node_mapping"].size() == 1);
    CHECK(receipt["node_mapping"][0]["temporary_node_id"].getString() == "t1");
    CHECK(receipt["applied_action_count"].getInt64() == 4);
    CHECK(receipt["state_retained_count"].getInt64() == 1);
    CHECK(receipt["state_reset_count"].getInt64() == 1);
}

TEST_CASE("region edit rejects promotion mutation and instantaneous cycles", "[inspect][sample-region][transaction]") {
    RegionFixture f;
    const auto promotion = f.actions(
        "{\"op\":\"set_finite_constant\",\"node_id\":" + std::to_string(f.parameter) +
        ",\"value\":0.25}");
    CHECK(promotion.terminal_state == ControlReceiptState::Failed);
    CHECK(promotion.result.result_code == ControlResultCode::InvalidRequest);
    const auto cycle = f.actions(
        "{\"op\":\"disconnect\",\"source_node_id\":" + std::to_string(f.input_kernel) +
        ",\"source_port\":0,\"destination_node_id\":" + std::to_string(f.multiply) +
        ",\"destination_port\":0}," +
        "{\"op\":\"connect\",\"source_node_id\":" + std::to_string(f.multiply) +
        ",\"source_port\":0,\"destination_node_id\":" + std::to_string(f.multiply) +
        ",\"destination_port\":0}");
    CHECK(cycle.terminal_state == ControlReceiptState::Failed);
    CHECK(cycle.result.result_code == ControlResultCode::InvalidRequest);
    CHECK(f.generation.value == 1);
}

TEST_CASE("region edit rejects stale graph generation", "[inspect][sample-region][transaction]") {
    RegionFixture f;
    const auto stale = f.actions(
        "{\"op\":\"set_finite_constant\",\"node_id\":" + std::to_string(f.constant) +
        ",\"value\":0.25}", 2);
    CHECK(stale.terminal_state == ControlReceiptState::Failed);
    CHECK(stale.result.result_code == ControlResultCode::StateConflict);
    CHECK(f.generation.value == 1);
}
