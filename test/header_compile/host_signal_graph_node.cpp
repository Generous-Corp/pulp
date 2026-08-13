#include <pulp/host/signal_graph_node.hpp>

#include <type_traits>

static_assert(std::is_enum_v<pulp::host::NodeType>);
static_assert(std::is_default_constructible_v<pulp::host::GraphNode>);
