#include <pulp/host/signal_graph_execution_snapshot.hpp>

#include <type_traits>

static_assert(std::is_default_constructible_v<pulp::host::SignalGraph::ExecutionSnapshot>);
static_assert(std::is_copy_constructible_v<pulp::host::SignalGraph::ExecutionSnapshot>);
