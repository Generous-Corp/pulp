#include <pulp/host/signal_graph_control.hpp>

#include <type_traits>

static_assert(std::is_enum_v<pulp::host::SignalGraphRouteCommand::Kind>);
static_assert(std::is_trivially_copyable_v<pulp::host::SignalGraphRouteResult>);
static_assert(pulp::host::SignalGraphControlAuthority::kMaxDenseCommands > 0);
