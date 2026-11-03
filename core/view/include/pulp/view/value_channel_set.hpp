#pragma once

/// @file value_channel_set.hpp
/// Source-compatible spelling of <pulp/dataplane/value_channel_set.hpp>. The editor data
/// plane lives in the UI-free pulp::dataplane module; these names stay
/// reachable as pulp::view::* for existing view code.

#include <pulp/dataplane/value_channel_set.hpp>
#include <pulp/view/value_channel_telemetry.hpp>

namespace pulp::view {

using dataplane::ValueChannelVisitor;
using dataplane::ValueChannelAccess;
using dataplane::ValueChannelSet;

} // namespace pulp::view
