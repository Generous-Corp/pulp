#pragma once

#include <pulp/host/graph_types.hpp>

#include <cstdint>
#include <span>
#include <vector>

namespace pulp::host {

struct GraphNode;
struct Connection;

// Why a node may NOT be rendered ahead of the audio deadline (anticipative
// rendering, masterwork Phase 6). Anticipation pre-computes a latent subgraph's
// output into a buffer during earlier blocks' slack so a CPU spike is absorbed
// off the critical block — but only for work whose result for a FUTURE block can
// be computed from inputs already available, with no dependence on live or
// host-clock state. A node is eligible only if it (and its entire feedforward
// dependency cone) is free of every exclusion below.
enum class AnticipationExclusion : std::uint8_t {
    None = 0,        // anticipation-eligible
    LiveAudioInput,  // is, or transitively reads, a live system AudioInput
    LiveMidiInput,   // is, or transitively reads, a live system MidiInput
                     // (a MIDI-instrument feed — the notes aren't known ahead)
    Feedback,        // participates in, or sits downstream of, a feedback edge
                     // (its own past output gates it; can't run arbitrarily ahead)
    Sidechain,       // has, or transitively reads, a sidechain input (live signal)
    TransportSensitive,  // is, or transitively reads, a node that consumes the
                         // host transport (GraphNode::transport_sensitive). Such
                         // a node must run live to observe the real playhead, so
                         // it (and its downstream cone) cannot be rendered ahead.
};

struct AnticipationEligibility {
    // Per node, indexed exactly like the input `nodes` span: None = passed the
    // static topology exclusions, otherwise the reason it (or something it depends
    // on) cannot be anticipated. A node inherits the exclusion of any feedforward
    // source it depends on.
    std::vector<AnticipationExclusion> node_exclusion;
    bool ok = false;

    // `passes_static_exclusions(i)` means node i cleared every exclusion: the
    // topology hard exclusions (live input / feedback / sidechain) AND
    // transport sensitivity (a node that opted into the host transport via
    // GraphNode::transport_sensitive, seeded as TransportSensitive and propagated
    // over its downstream cone). A true result is therefore sufficient for the
    // anticipation partition to treat node i as ahead-renderable interior.
    bool passes_static_exclusions(std::size_t node_index) const {
        return node_index < node_exclusion.size() &&
               node_exclusion[node_index] == AnticipationExclusion::None;
    }
    bool any_passes_static_exclusions() const {
        for (auto e : node_exclusion) {
            if (e == AnticipationExclusion::None) return true;
        }
        return false;
    }
};

// Classify each node's anticipation eligibility from the graph topology plus the
// per-node transport-sensitivity capability: seed the hard-excluded roots (live
// AudioInput / MidiInput nodes, the endpoints of every feedback edge, any node
// with a sidechain inbound edge, and any node with GraphNode::transport_sensitive
// set), then propagate each exclusion forward along feedforward (non-feedback)
// edges to a fixpoint, so a node downstream of anything excluded is excluded too.
// The first reason assigned to a node wins (its own seed reason over an inherited
// one); "excluded for some reason" is the load-bearing bit, the specific reason
// is diagnostic. Deterministic and allocation-bounded; no prepared/live graph
// required. Returns ok=false only on a malformed input (a connection naming a
// node id absent from `nodes`).
//
// Transport sensitivity is the per-node opt-out for host-clock-dependent nodes:
// `transport_sensitive` is resolved once at compile (from
// PluginSlot::wants_transport() or a transport-aware custom callback), so seeding
// TransportSensitive here keeps such a node — and its downstream cone — out of
// the ahead-rendered interior, letting it run live and observe the real playhead.
AnticipationEligibility analyze_anticipation_eligibility(
    std::span<const GraphNode> nodes, std::span<const Connection> connections);

} // namespace pulp::host
