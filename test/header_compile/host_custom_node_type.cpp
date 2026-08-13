#include <pulp/host/custom_node_type.hpp>

#include <type_traits>

static_assert(std::is_default_constructible_v<pulp::host::CustomNodeType>);
