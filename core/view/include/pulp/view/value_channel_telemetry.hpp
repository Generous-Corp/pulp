#pragma once

/// @file value_channel_telemetry.hpp
/// Source-compatible spelling of <pulp/dataplane/value_channel_telemetry.hpp>. The editor data
/// plane lives in the UI-free pulp::dataplane module; these names stay
/// reachable as pulp::view::* for existing view code.

#include <pulp/dataplane/value_channel_telemetry.hpp>
#include <pulp/view/value_source.hpp>

namespace pulp::view {

using dataplane::ValueChannelShape;
using dataplane::ValueChannelInfo;
using dataplane::ValueChannelContinuousPayload;
using dataplane::ValueChannelTelemetrySnapshot;
using dataplane::ValueChannelTelemetryEventBatch;
using dataplane::ValueChannelTelemetryEventStats;
using dataplane::ValueChannelTelemetryDescriptor;
using dataplane::ValueChannelTelemetryAttachment;

} // namespace pulp::view
