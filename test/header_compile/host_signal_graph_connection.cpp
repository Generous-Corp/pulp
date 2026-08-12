#include <pulp/host/signal_graph_connection.hpp>

#include <type_traits>

static_assert(std::is_enum_v<pulp::host::AutomationMix>);
static_assert(std::is_aggregate_v<pulp::host::Connection>);
