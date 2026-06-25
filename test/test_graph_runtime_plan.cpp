#include <catch2/catch_test_macros.hpp>
#include <pulp/graph/graph_runtime_plan.hpp>

#include <array>
#include <vector>

namespace {

pulp::graph::GraphRuntimeNodeSpec node(pulp::graph::NodeId id,
                                      std::uint32_t inputs,
                                      std::uint32_t outputs,
                                      pulp::graph::GraphRuntimeNodeKind kind =
                                          pulp::graph::GraphRuntimeNodeKind::Processor) {
    return {id, kind, inputs, outputs};
}

pulp::graph::GraphRuntimeConnectionSpec connect(pulp::graph::NodeId source,
                                               pulp::graph::PortIndex source_port,
                                               pulp::graph::NodeId dest,
                                               pulp::graph::PortIndex dest_port,
                                               bool feedback = false) {
    return {source, source_port, dest, dest_port, feedback, false};
}

pulp::graph::GraphRuntimeConnectionSpec event_connect(pulp::graph::NodeId source,
                                                     pulp::graph::PortIndex source_port,
                                                     pulp::graph::NodeId dest,
                                                     pulp::graph::PortIndex dest_port,
                                                     bool feedback = false) {
    return {source, source_port, dest, dest_port, feedback, true};
}

pulp::graph::GraphRuntimeNodeSpec event_node(
    pulp::graph::NodeId id,
    std::uint32_t event_inputs,
    std::uint32_t event_outputs,
    pulp::graph::GraphRuntimeNodeKind kind) {
    pulp::graph::GraphRuntimeNodeSpec spec;
    spec.id = id;
    spec.kind = kind;
    spec.event_input_ports = event_inputs;
    spec.event_output_ports = event_outputs;
    return spec;
}

} // namespace

TEST_CASE("GraphRuntimePlan builds dense node and connection arrays",
          "[graph][graph-runtime][plan]") {
    const std::array nodes = {
        node(10, 0, 2, pulp::graph::GraphRuntimeNodeKind::AudioInput),
        node(20, 2, 2),
        node(30, 2, 0, pulp::graph::GraphRuntimeNodeKind::AudioOutput),
    };
    const std::array connections = {
        connect(10, 0, 20, 0),
        connect(10, 1, 20, 1),
        connect(20, 0, 30, 0),
        connect(20, 1, 30, 1),
    };

    auto result = pulp::graph::build_graph_runtime_plan(nodes, connections);

    REQUIRE(result.ok());
    REQUIRE(result.plan.node_count() == 3);
    REQUIRE(result.plan.connection_count() == 4);
    REQUIRE(result.plan.nodes[0].id == 10);
    REQUIRE(result.plan.nodes[1].id == 20);
    REQUIRE(result.plan.nodes[2].id == 30);
    REQUIRE(result.plan.connections[0].source_index == 0);
    REQUIRE(result.plan.connections[0].dest_index == 1);
    REQUIRE(result.plan.connections[2].source_index == 1);
    REQUIRE(result.plan.connections[2].dest_index == 2);

    const auto& processor = result.plan.nodes[1];
    REQUIRE(processor.inbound_connection_count == 2);
    REQUIRE(processor.outbound_connection_count == 2);
    REQUIRE(result.plan.inbound_connection_indices[processor.first_inbound_connection] == 0);
    REQUIRE(result.plan.inbound_connection_indices[processor.first_inbound_connection + 1] == 1);
    REQUIRE(result.plan.outbound_connection_indices[processor.first_outbound_connection] == 2);
    REQUIRE(result.plan.outbound_connection_indices[processor.first_outbound_connection + 1] == 3);

    REQUIRE(result.plan.processing_order_indices.size() == 3);
    REQUIRE(result.plan.processing_order_indices[0] == 0);
    REQUIRE(result.plan.processing_order_indices[1] == 1);
    REQUIRE(result.plan.processing_order_indices[2] == 2);
}

TEST_CASE("GraphRuntimePlan rejects duplicate and reserved node ids",
          "[graph][graph-runtime][plan]") {
    const std::array duplicate_nodes = {
        node(1, 0, 1),
        node(1, 1, 0),
    };
    auto duplicate = pulp::graph::build_graph_runtime_plan(duplicate_nodes, {});
    REQUIRE_FALSE(duplicate.ok());
    REQUIRE(duplicate.error.code == pulp::graph::GraphRuntimePlanErrorCode::DuplicateNodeId);
    REQUIRE(duplicate.error.node_id == 1);

    const std::array reserved_nodes = {
        node(0, 0, 1),
    };
    auto reserved = pulp::graph::build_graph_runtime_plan(reserved_nodes, {});
    REQUIRE_FALSE(reserved.ok());
    REQUIRE(reserved.error.code == pulp::graph::GraphRuntimePlanErrorCode::InvalidNodeId);
}

TEST_CASE("GraphRuntimePlan enforces bounded complexity limits",
          "[graph][graph-runtime][plan]") {
    const std::array nodes = {
        node(1, 0, 1),
        node(2, 1, 0),
    };
    const std::array connections = {
        connect(1, 0, 2, 0),
    };

    pulp::graph::GraphRuntimeLimits limits;
    limits.max_nodes = 1;
    auto too_many_nodes = pulp::graph::build_graph_runtime_plan(nodes, connections, limits);
    REQUIRE_FALSE(too_many_nodes.ok());
    REQUIRE(too_many_nodes.error.code == pulp::graph::GraphRuntimePlanErrorCode::TooManyNodes);

    limits = {};
    limits.max_connections = 0;
    auto invalid_limits = pulp::graph::build_graph_runtime_plan(nodes, connections, limits);
    REQUIRE_FALSE(invalid_limits.ok());
    REQUIRE(invalid_limits.error.code == pulp::graph::GraphRuntimePlanErrorCode::InvalidLimits);

    limits = {};
    limits.max_ports_per_node = 1;
    const std::array wide_nodes = {
        node(1, 0, 2),
    };
    auto too_many_ports_per_node = pulp::graph::build_graph_runtime_plan(wide_nodes, {}, limits);
    REQUIRE_FALSE(too_many_ports_per_node.ok());
    REQUIRE(too_many_ports_per_node.error.code ==
            pulp::graph::GraphRuntimePlanErrorCode::InvalidNodePortCount);

    limits = {};
    limits.max_total_ports = 1;
    auto too_many_total_ports = pulp::graph::build_graph_runtime_plan(nodes, {}, limits);
    REQUIRE_FALSE(too_many_total_ports.ok());
    REQUIRE(too_many_total_ports.error.code == pulp::graph::GraphRuntimePlanErrorCode::TooManyPorts);
}

TEST_CASE("GraphRuntimePlan validates connection endpoints and ports",
          "[graph][graph-runtime][plan]") {
    const std::array nodes = {
        node(1, 0, 1),
        node(2, 1, 0),
    };

    const std::array unknown_source = {
        connect(99, 0, 2, 0),
    };
    auto source_result = pulp::graph::build_graph_runtime_plan(nodes, unknown_source);
    REQUIRE_FALSE(source_result.ok());
    REQUIRE(source_result.error.code == pulp::graph::GraphRuntimePlanErrorCode::UnknownSourceNode);
    REQUIRE(source_result.error.node_id == 99);

    const std::array unknown_dest = {
        connect(1, 0, 99, 0),
    };
    auto dest_result = pulp::graph::build_graph_runtime_plan(nodes, unknown_dest);
    REQUIRE_FALSE(dest_result.ok());
    REQUIRE(dest_result.error.code == pulp::graph::GraphRuntimePlanErrorCode::UnknownDestinationNode);
    REQUIRE(dest_result.error.node_id == 99);

    const std::array bad_source_port = {
        connect(1, 1, 2, 0),
    };
    auto source_port_result = pulp::graph::build_graph_runtime_plan(nodes, bad_source_port);
    REQUIRE_FALSE(source_port_result.ok());
    REQUIRE(source_port_result.error.code == pulp::graph::GraphRuntimePlanErrorCode::SourcePortOutOfRange);

    const std::array bad_dest_port = {
        connect(1, 0, 2, 1),
    };
    auto dest_port_result = pulp::graph::build_graph_runtime_plan(nodes, bad_dest_port);
    REQUIRE_FALSE(dest_port_result.ok());
    REQUIRE(dest_port_result.error.code == pulp::graph::GraphRuntimePlanErrorCode::DestinationPortOutOfRange);
}

TEST_CASE("GraphRuntimePlan validates event connections separately from audio ports",
          "[graph][graph-runtime][plan][midi]") {
    const std::array nodes = {
        event_node(1, 0, 1, pulp::graph::GraphRuntimeNodeKind::MidiInput),
        event_node(2, 1, 0, pulp::graph::GraphRuntimeNodeKind::MidiOutput),
    };
    const std::array event_connections = {
        event_connect(1, 0, 2, 0),
    };

    auto accepted = pulp::graph::build_graph_runtime_plan(nodes, event_connections);
    REQUIRE(accepted.ok());
    REQUIRE(accepted.plan.node_count() == 2);
    REQUIRE(accepted.plan.nodes[0].event_output_ports == 1);
    REQUIRE(accepted.plan.nodes[1].event_input_ports == 1);
    REQUIRE(accepted.plan.connections[0].event);
    REQUIRE(accepted.plan.connections[0].source_index == 0);
    REQUIRE(accepted.plan.connections[0].dest_index == 1);

    const std::array audio_connections = {
        connect(1, 0, 2, 0),
    };
    auto audio_rejected = pulp::graph::build_graph_runtime_plan(nodes, audio_connections);
    REQUIRE_FALSE(audio_rejected.ok());
    REQUIRE(audio_rejected.error.code ==
            pulp::graph::GraphRuntimePlanErrorCode::SourcePortOutOfRange);

    const std::array bad_event_connections = {
        event_connect(1, 1, 2, 0),
    };
    auto event_rejected = pulp::graph::build_graph_runtime_plan(nodes, bad_event_connections);
    REQUIRE_FALSE(event_rejected.ok());
    REQUIRE(event_rejected.error.code ==
            pulp::graph::GraphRuntimePlanErrorCode::SourcePortOutOfRange);
}

TEST_CASE("GraphRuntimePlan rejects cycles unless an edge is explicit feedback",
          "[graph][graph-runtime][plan]") {
    const std::array nodes = {
        node(1, 1, 1),
        node(2, 1, 1),
    };
    const std::array cycle = {
        connect(1, 0, 2, 0),
        connect(2, 0, 1, 0),
    };

    auto rejected = pulp::graph::build_graph_runtime_plan(nodes, cycle);
    REQUIRE_FALSE(rejected.ok());
    REQUIRE(rejected.error.code == pulp::graph::GraphRuntimePlanErrorCode::CycleDetected);

    const std::array feedback_cycle = {
        connect(1, 0, 2, 0),
        connect(2, 0, 1, 0, true),
    };
    auto accepted = pulp::graph::build_graph_runtime_plan(nodes, feedback_cycle);
    REQUIRE(accepted.ok());
    REQUIRE(accepted.plan.connection_count() == 2);
    REQUIRE(accepted.plan.connections[1].feedback);
    REQUIRE(accepted.plan.processing_order_indices.size() == 2);
    REQUIRE(accepted.plan.processing_order_indices[0] == 0);
    REQUIRE(accepted.plan.processing_order_indices[1] == 1);
}

TEST_CASE("GraphRuntimePlan handles feedback self-loops explicitly",
          "[graph][graph-runtime][plan]") {
    const std::array nodes = {
        node(1, 1, 1),
    };

    const std::array self_loop = {
        connect(1, 0, 1, 0),
    };
    auto rejected = pulp::graph::build_graph_runtime_plan(nodes, self_loop);
    REQUIRE_FALSE(rejected.ok());
    REQUIRE(rejected.error.code == pulp::graph::GraphRuntimePlanErrorCode::CycleDetected);

    const std::array feedback_self_loop = {
        connect(1, 0, 1, 0, true),
    };
    auto accepted = pulp::graph::build_graph_runtime_plan(nodes, feedback_self_loop);
    REQUIRE(accepted.ok());
    REQUIRE(accepted.plan.connection_count() == 1);
    REQUIRE(accepted.plan.connections[0].feedback);
    REQUIRE(accepted.plan.nodes[0].inbound_connection_count == 1);
    REQUIRE(accepted.plan.nodes[0].outbound_connection_count == 1);
    REQUIRE(accepted.plan.inbound_connection_indices[0] == 0);
    REQUIRE(accepted.plan.outbound_connection_indices[0] == 0);
    REQUIRE(accepted.plan.processing_order_indices.size() == 1);
    REQUIRE(accepted.plan.processing_order_indices[0] == 0);
}

TEST_CASE("GraphRuntimePlan accepts empty graphs as valid no-op plans",
          "[graph][graph-runtime][plan]") {
    auto result = pulp::graph::build_graph_runtime_plan({}, {});
    REQUIRE(result.ok());
    REQUIRE(result.plan.node_count() == 0);
    REQUIRE(result.plan.connection_count() == 0);
    REQUIRE(result.plan.processing_order_indices.empty());
}

TEST_CASE("GraphRuntimePlan carries parameter-automation metadata",
          "[graph][graph-runtime][plan][automation]") {
    using namespace pulp::graph;
    // in(0->2) -> plugin(2->2); plus an automation edge in:0 -> plugin param 42.
    const std::array nodes = {
        node(1, 0, 2, GraphRuntimeNodeKind::AudioInput),
        node(2, 2, 2),
    };
    GraphRuntimeConnectionSpec audio = connect(1, 0, 2, 0);
    GraphRuntimeConnectionSpec autom{};
    autom.source_node = 1;
    autom.source_port = 1;
    autom.dest_node = 2;
    autom.dest_port = 0;  // conventional; a parameter target, not an audio port
    autom.is_automation = true;
    autom.automation = GraphRuntimeAutomationSpec{
        /*param_id=*/42, /*range_lo=*/-1.0f, /*range_hi=*/1.0f,
        /*smoothing_ms=*/25.0f, /*mix_add=*/true, /*audio_rate=*/false,
        /*bounds_lo=*/-2.0f, /*bounds_hi=*/2.0f};
    const std::array conns = {audio, autom};

    const auto result = build_graph_runtime_plan(nodes, conns);
    REQUIRE(result.ok());
    REQUIRE(result.plan.connections.size() == 2);

    // The audio edge is unchanged; the automation edge carries its full spec.
    const auto& a = result.plan.connections[1];
    CHECK(a.is_automation);
    CHECK_FALSE(a.event);
    CHECK_FALSE(a.feedback);
    CHECK(a.automation.param_id == 42u);
    CHECK(a.automation.range_lo == -1.0f);
    CHECK(a.automation.range_hi == 1.0f);
    CHECK(a.automation.smoothing_ms == 25.0f);
    CHECK(a.automation.mix_add);
    CHECK_FALSE(a.automation.audio_rate);
    CHECK(a.automation.bounds_lo == -2.0f);
    CHECK(a.automation.bounds_hi == 2.0f);

    // Automation still orders the graph: source (in) before dest (plugin).
    REQUIRE(result.plan.processing_order_indices.size() == 2);
    CHECK(result.plan.processing_order_indices[0] == 0);
    CHECK(result.plan.processing_order_indices[1] == 1);
}

TEST_CASE("GraphRuntimePlan skips the input-port check for automation edges",
          "[graph][graph-runtime][plan][automation]") {
    using namespace pulp::graph;
    // The destination plugin has ZERO audio input ports; an automation edge to
    // its parameter must still be accepted (dest_port targets a parameter, not
    // an audio input), whereas a plain audio edge to it would be rejected.
    const std::array nodes = {
        node(1, 0, 2, GraphRuntimeNodeKind::AudioInput),
        node(2, 0, 2),  // 0 audio inputs
    };
    GraphRuntimeConnectionSpec autom{};
    autom.source_node = 1;
    autom.source_port = 0;
    autom.dest_node = 2;
    autom.dest_port = 0;
    autom.is_automation = true;
    autom.automation.param_id = 7;
    const std::array ok_conns = {autom};
    CHECK(build_graph_runtime_plan(nodes, ok_conns).ok());

    // A plain audio edge to the same (absent) input port 0 is rejected.
    const std::array bad_conns = {connect(1, 0, 2, 0)};
    const auto bad = build_graph_runtime_plan(nodes, bad_conns);
    CHECK_FALSE(bad.ok());
    CHECK(bad.error.code == GraphRuntimePlanErrorCode::DestinationPortOutOfRange);
}
