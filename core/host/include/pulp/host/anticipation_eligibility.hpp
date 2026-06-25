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
};

struct AnticipationEligibility {
    // Per node, indexed exactly like the input `nodes` span: None = passed the
    // static topology exclusions, otherwise the reason it (or something it depends
    // on) cannot be anticipated. A node inherits the exclusion of any feedforward
    // source it depends on.
    std::vector<AnticipationExclusion> node_exclusion;
    bool ok = false;

    // WARNING: `passes_static_exclusions(i)` means ONLY that node i cleared the
    // statically-detectable hard exclusions (live input / feedback / sidechain).
    // It is NOT a blanket "safe to render ahead" clearance — host-clock-sensitive
    // plugins are deliberately not detected here (see the function note). A
    // renderer must apply the host-time check (or a per-node opt-out) on top of
    // this; do not treat a true result as sufficient on its own.
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

// Classify each node's anticipation eligibility purely from the graph topology:
// seed the hard-excluded roots (live AudioInput / MidiInput nodes, the endpoints
// of every feedback edge, and any node with a sidechain inbound edge), then
// propagate each exclusion forward along feedforward (non-feedback) edges to a
// fixpoint, so a node downstream of anything excluded is excluded too. The first
// reason assigned to a node wins (its own seed reason over an inherited one);
// "excluded for some reason" is the load-bearing bit, the specific reason is
// diagnostic. Deterministic and allocation-bounded; no prepared/live graph
// required. Returns ok=false only on a malformed input (a connection naming a
// node id absent from `nodes`).
//
// NOTE: host-clock sensitivity (a plugin whose output depends on the transport
// playhead) is NOT statically detectable from the current node metadata, so it is
// NOT covered here — anticipating such a node needs either a per-node opt-out flag
// or feeding it the correct future transport. That is a deliberate follow-up; the
// renderer that consumes this analysis must treat host-time exposure separately.
AnticipationEligibility analyze_anticipation_eligibility(
    std::span<const GraphNode> nodes, std::span<const Connection> connections);

} // namespace pulp::host
