// signal_graph_ports_internal.hpp: named-port population shared by the
// SignalGraph translation units that create nodes.
//
// Every node a graph owns carries one PortHandle per declared input and
// output, so typed connect(), numeric connect() and the serializer can index
// input_ports / output_ports by port number. Node constructors outside
// signal_graph_authoring.cpp (the trigger nodes) call this too.
#pragma once

#include <pulp/host/signal_graph_node.hpp>

#include <cstdint>

namespace pulp::host::detail {

/// Rebuild `node`'s input and output handles from its port counts. MIDI
/// input/output nodes get Midi-kind ports, every other node Audio-kind ports
/// named "audio", "in1"/"out1", ...
void populate_named_ports(GraphNode& node, std::uint64_t owner, std::uint64_t incarnation);

} // namespace pulp::host::detail
