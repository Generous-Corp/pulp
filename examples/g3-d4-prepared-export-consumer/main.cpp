#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <iostream>

int main() {
    pulp::host::SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    if (!edit)
        return 1;
    if (!edit->bind_authored_identity("{\"assets\":{},\"edges\":{},\"interfaces\":[],\"nodes\":{},"
                                      "\"policies\":{},\"profile\":\"pulp.graph.identity.v1\"}",
                                      {}))
        return 2;
    if (edit->prepare(48000.0, 128) !=
        pulp::host::SignalGraph::PreparedTopologyEdit::Result::Prepared)
        return 3;
    if (edit->commit() != pulp::host::SignalGraph::PreparedTopologyEdit::Result::Committed)
        return 4;
    const auto snapshot = edit->committed_execution_snapshot();
    const auto description = snapshot.export_prepared_description();
    if (!description)
        return 5;
    std::cout << description.value->canonical_bytes() << '\n';
    return 0;
}
