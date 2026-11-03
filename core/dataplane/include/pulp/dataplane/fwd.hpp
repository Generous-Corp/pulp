#pragma once

/// @file fwd.hpp
/// Forward declarations for the editor data plane, plus their pulp::view
/// spellings. Include this instead of writing `namespace pulp::view { class
/// ValueChannelSet; }`: the view names are using-declarations of the
/// pulp::dataplane classes, and a class forward declaration in pulp::view
/// would declare a second, unrelated class.

namespace pulp::dataplane {

class HostParamSurface;
class HostActionSurface;
class StateStoreHostParamSurface;
class MeterSource;
class ScalarSource;
class VectorSource;
class EventSource;
struct MeterFrame;
struct VectorFrame;
struct EventFrame;
class ValueChannelSet;
class ValueChannelTelemetryAttachment;

} // namespace pulp::dataplane

namespace pulp::view {

using dataplane::HostParamSurface;
using dataplane::HostActionSurface;
using dataplane::StateStoreHostParamSurface;
using dataplane::MeterSource;
using dataplane::ScalarSource;
using dataplane::VectorSource;
using dataplane::EventSource;
using dataplane::MeterFrame;
using dataplane::VectorFrame;
using dataplane::EventFrame;
using dataplane::ValueChannelSet;
using dataplane::ValueChannelTelemetryAttachment;

} // namespace pulp::view
