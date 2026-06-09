#pragma once

#include <pulp/graph/graph_runtime_queue.hpp>
#include <pulp/host/graph_types.hpp>

#include <cstddef>

namespace pulp::host {

// Source-compatibility aliases for the old host-owned graph runtime headers.
// Canonical symbols and SDK ownership live in pulp::graph.
using ::pulp::graph::GraphCommand;
using ::pulp::graph::GraphCommandTiming;
using ::pulp::graph::GraphCommandTimingType;
using ::pulp::graph::GraphCommandType;
using ::pulp::graph::GraphEvent;
using ::pulp::graph::GraphEventType;
using ::pulp::graph::GraphMidiOutputEvent;
using ::pulp::graph::GraphRuntimeQueueStats;
using ::pulp::graph::GraphTimedCommand;

template<std::size_t CommandCapacity = 128,
         std::size_t EventCapacity = 256,
         std::size_t MidiOutputCapacity = 256>
using GraphRuntimeQueues =
    ::pulp::graph::GraphRuntimeQueues<CommandCapacity, EventCapacity, MidiOutputCapacity>;

} // namespace pulp::host
