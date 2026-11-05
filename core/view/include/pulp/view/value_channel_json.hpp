#pragma once

/// @file value_channel_json.hpp
/// Source-compatible spelling of <pulp/dataplane/value_channel_json.hpp>. The editor data
/// plane lives in the UI-free pulp::dataplane module; these names stay
/// reachable as pulp::view::* for existing view code.

#include <pulp/dataplane/value_channel_json.hpp>
#include <pulp/view/value_channel_set.hpp>

namespace pulp::view {

using dataplane::value_channel_shape_name;
using dataplane::value_channel_to_value;
using dataplane::value_channels_to_value;

} // namespace pulp::view
