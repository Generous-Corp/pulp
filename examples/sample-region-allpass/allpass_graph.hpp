#pragma once

#include <pulp/host/signal_graph_prepared_topology_edit.hpp>
#include <string>

namespace pulp::examples {

inline constexpr state::ParamID kAllpassCoefficient = 2901;
inline constexpr host::SampleRegionId kAllpassRegion = 901;

// Build the recurrence y[n] = a*x[n] + x[n-1] - a*y[n-1] in an
// unpublished edit. The caller freezes the parameter contract before exposure.
bool build_allpass_region(host::SignalGraph::PreparedTopologyEdit& edit, std::string& error);

} // namespace pulp::examples
