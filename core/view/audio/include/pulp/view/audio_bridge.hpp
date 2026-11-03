#pragma once

/// @file audio_bridge.hpp
/// Source-compatible spelling of <pulp/dataplane/audio_bridge.hpp>. The editor data
/// plane lives in the UI-free pulp::dataplane module; these names stay
/// reachable as pulp::view::* for existing view code.

#include <pulp/dataplane/audio_bridge.hpp>

namespace pulp::view {

using dataplane::MeterData;
using dataplane::MeterBallistics;
using dataplane::AudioBridge;

} // namespace pulp::view
