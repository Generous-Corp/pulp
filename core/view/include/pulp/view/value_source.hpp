#pragma once

/// @file value_source.hpp
/// Source-compatible spelling of <pulp/dataplane/value_source.hpp>. The editor data
/// plane lives in the UI-free pulp::dataplane module; these names stay
/// reachable as pulp::view::* for existing view code.

#include <pulp/dataplane/value_source.hpp>

namespace pulp::view {

using dataplane::EventFrame;
using dataplane::EventSource;
using dataplane::MeterFrame;
using dataplane::MeterSource;
using dataplane::PublishCounter;
using dataplane::ScalarSource;
using dataplane::ValueEvent;
using dataplane::VectorFrame;
using dataplane::VectorSource;

} // namespace pulp::view
