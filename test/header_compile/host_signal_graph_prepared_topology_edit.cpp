#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <type_traits>

static_assert(std::is_enum_v<pulp::host::SignalGraph::PreparedTopologyEdit::Result>);
static_assert(!std::is_copy_constructible_v<pulp::host::SignalGraph::PreparedTopologyEdit>);
static_assert(!std::is_copy_assignable_v<pulp::host::SignalGraph::PreparedTopologyEdit>);
