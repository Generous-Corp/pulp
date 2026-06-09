#include <catch2/catch_test_macros.hpp>
#include <pulp/host/graph_runtime_plan.hpp>
#include <pulp/host/graph_runtime_queue.hpp>

#include <array>
#include <type_traits>

TEST_CASE("Host graph runtime headers forward to the graph module",
          "[host][graph-runtime][compat]") {
    STATIC_REQUIRE(std::is_same_v<pulp::host::NodeId, pulp::graph::NodeId>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::PortIndex, pulp::graph::PortIndex>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimeLimits,
                                  pulp::graph::GraphRuntimeLimits>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimeNodeSpec,
                                  pulp::graph::GraphRuntimeNodeSpec>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimeConnectionSpec,
                                  pulp::graph::GraphRuntimeConnectionSpec>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimeNodePlan,
                                  pulp::graph::GraphRuntimeNodePlan>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimeConnectionPlan,
                                  pulp::graph::GraphRuntimeConnectionPlan>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimePlan,
                                  pulp::graph::GraphRuntimePlan>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimePlanError,
                                  pulp::graph::GraphRuntimePlanError>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimePlanResult,
                                  pulp::graph::GraphRuntimePlanResult>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphCommand,
                                  pulp::graph::GraphCommand>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphCommandTiming,
                                  pulp::graph::GraphCommandTiming>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphTimedCommand,
                                  pulp::graph::GraphTimedCommand>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphEvent,
                                  pulp::graph::GraphEvent>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphMidiOutputEvent,
                                  pulp::graph::GraphMidiOutputEvent>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimeQueueStats,
                                  pulp::graph::GraphRuntimeQueueStats>);
    STATIC_REQUIRE(std::is_same_v<pulp::host::GraphRuntimeQueues<1, 1, 1>,
                                  pulp::graph::GraphRuntimeQueues<1, 1, 1>>);

    const std::array nodes = {
        pulp::host::GraphRuntimeNodeSpec{
            .id = 1,
            .kind = pulp::host::GraphRuntimeNodeKind::AudioInput,
            .input_ports = 0,
            .output_ports = 1,
        },
        pulp::host::GraphRuntimeNodeSpec{
            .id = 2,
            .kind = pulp::host::GraphRuntimeNodeKind::AudioOutput,
            .input_ports = 1,
            .output_ports = 0,
        },
    };
    const std::array connections = {
        pulp::host::GraphRuntimeConnectionSpec{
            .source_node = 1,
            .source_port = 0,
            .dest_node = 2,
            .dest_port = 0,
        },
    };

    const auto result = pulp::host::build_graph_runtime_plan(nodes, connections);
    REQUIRE(result.ok());
    REQUIRE(result.plan.node_count() == 2);

    pulp::host::GraphRuntimeQueues<1, 1, 1> queues;
    pulp::host::GraphCommand command;
    command.sequence_id = 1;
    command.node_id = 1;
    REQUIRE(queues.enqueue_command(command));
}
