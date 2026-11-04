#include <pulp/graph/graph_runtime_plan.hpp>

#include <array>
#include <iostream>

int main() {
    using namespace pulp::graph;
    std::array<GraphRuntimeNodeSpec, 2> nodes{};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        nodes[i].id = static_cast<NodeId>(i + 1);
        nodes[i].input_ports = nodes[i].output_ports = 1;
        nodes[i].event_input_ports = nodes[i].event_output_ports = 1;
    }
    GraphRuntimeConnectionSpec edge{1, 0, 2, 0};
    auto check = [&](GraphRuntimeConnectionKind kind, bool feedback,
                     GraphRuntimePlanErrorCode expected) {
        edge.kind = kind;
        edge.feedback = feedback;
        const auto result = build_graph_runtime_plan(nodes, std::span(&edge, 1));
        if (result.error.code != expected ||
            (expected != GraphRuntimePlanErrorCode::None &&
             (result.plan.node_count() != 0 || result.plan.connection_count() != 0))) {
            std::cerr << "admission mismatch kind=" << static_cast<int>(kind)
                      << " feedback=" << feedback << " error="
                      << static_cast<int>(result.error.code) << '\n';
            return false;
        }
        return true;
    };
    bool passed = true;
    for (auto kind : {GraphRuntimeConnectionKind::Audio,
                      GraphRuntimeConnectionKind::Event,
                      GraphRuntimeConnectionKind::Automation}) {
        passed &= check(kind, false, GraphRuntimePlanErrorCode::None);
        passed &= check(kind, true, kind == GraphRuntimeConnectionKind::Audio
                                      ? GraphRuntimePlanErrorCode::None
                                      : GraphRuntimePlanErrorCode::NonAudioFeedback);
    }
    passed &= check(GraphRuntimeConnectionKind::Trigger, false,
                    GraphRuntimePlanErrorCode::UnsupportedTrigger);
    passed &= check(GraphRuntimeConnectionKind::Trigger, true,
                    GraphRuntimePlanErrorCode::UnsupportedTrigger);
    passed &= check(static_cast<GraphRuntimeConnectionKind>(255), false,
                    GraphRuntimePlanErrorCode::InvalidConnectionKind);
    if (!passed)
        return 1;
    std::cout << "PASS: audio/MIDI/automation controls; typed/unknown/feedback refusals\n";
}
