#include <pulp/host/signal_graph_execution_status.hpp>

#include <type_traits>

static_assert(std::is_trivially_copyable_v<pulp::host::SignalGraphExecutionStatus>);
static_assert(pulp::host::SignalGraphExecutionStatus{}.reference_walk_permitted);
static_assert(!pulp::host::SignalGraphExecutionStatus{}.routed_path_ready());
