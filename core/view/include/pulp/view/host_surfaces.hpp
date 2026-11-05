#pragma once

/// @file host_surfaces.hpp
/// Source-compatible spelling of <pulp/dataplane/host_surfaces.hpp>. The editor data
/// plane lives in the UI-free pulp::dataplane module; these names stay
/// reachable as pulp::view::* for existing view code.

#include <pulp/dataplane/host_surfaces.hpp>

namespace pulp::view {

using dataplane::HostActionSurface;
using dataplane::HostParamSurface;
using dataplane::param_index_to_normalized;
using dataplane::param_normalized_to_index;

} // namespace pulp::view
