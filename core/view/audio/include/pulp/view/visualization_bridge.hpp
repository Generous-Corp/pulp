#pragma once

/// @file visualization_bridge.hpp
/// Source-compatible spelling of <pulp/dataplane/visualization_bridge.hpp>. The editor data
/// plane lives in the UI-free pulp::dataplane module; these names stay
/// reachable as pulp::view::* for existing view code.

#include <pulp/dataplane/visualization_bridge.hpp>

namespace pulp::view {

using dataplane::SpectrumData;
using dataplane::VisualizationBacklogPolicy;
using dataplane::VisualizationBenchCounters;
using dataplane::VisualizationBridge;
using dataplane::VisualizationConfig;
using dataplane::WaveformData;

} // namespace pulp::view
