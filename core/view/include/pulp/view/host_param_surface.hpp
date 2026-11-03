#pragma once

/// @file host_param_surface.hpp
/// Source-compatible spelling of <pulp/dataplane/host_param_surface.hpp>. The editor data
/// plane lives in the UI-free pulp::dataplane module; these names stay
/// reachable as pulp::view::* for existing view code.

#include <pulp/dataplane/host_param_surface.hpp>
#include <pulp/view/host_surfaces.hpp>

namespace pulp::view {

using dataplane::StateStoreHostParamSurface;

} // namespace pulp::view
