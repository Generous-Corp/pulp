#include <pulp/host/signal_graph.hpp>

#include <type_traits>

static_assert(std::is_default_constructible_v<pulp::host::CustomNodeType>);
static_assert(std::is_default_constructible_v<pulp::host::SignalGraph>);
