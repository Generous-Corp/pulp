// value_channel_json.hpp - one serializer for a processor's value channels.
//
// Two consumers describe the same channels: the scripted-UI bridge
// (`listValueChannels()`) and the inspector (`State.getValueChannels`). They
// share this rather than hand-rolling a payload each, for the same reason
// param_json exists — a second implementation drifts the day either side gains
// a field, and a UI built against one shape then silently mis-reads the other.

#pragma once

#include <pulp/view/value_channel_set.hpp>

#include <choc/containers/choc_Value.h>

namespace pulp::view {

/// Stable wire name for a channel's shape. Spelled out per case rather than
/// derived from the enum's order, so landing the reserved `events` shape cannot
/// renumber what a UI or a lint already matches on.
const char* value_channel_shape_name(ValueChannelShape shape) noexcept;

/// One channel as `{name, unit, shape, neutral}`.
choc::value::Value value_channel_to_value(const ValueChannelInfo& info);

/// Every declared channel, in declaration order. A null set yields an EMPTY
/// ARRAY, never undefined: no channels is the default every processor inherits,
/// so iterating the result must be safe without a guard.
choc::value::Value value_channels_to_value(const ValueChannelSet* channels);

} // namespace pulp::view
