#include <pulp/host/anticipation_eligibility.hpp>

#include <pulp/host/signal_graph.hpp>

#include <unordered_map>

namespace pulp::host {

AnticipationEligibility analyze_anticipation_eligibility(
    std::span<const GraphNode> nodes, std::span<const Connection> connections) {
    AnticipationEligibility result;
    result.node_exclusion.assign(nodes.size(), AnticipationExclusion::None);

    // NodeId -> dense index into `nodes`. Duplicate ids would collapse a later
    // node onto an earlier one's classification when edges resolve their
    // endpoints, so a stale/aliased id could escape an exclusion — reject as
    // malformed (a safety classifier must be correct by construction, not by an
    // unstated unique-id assumption on a raw span).
    std::unordered_map<NodeId, std::size_t> index_of;
    index_of.reserve(nodes.size());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (!index_of.emplace(nodes[i].id, i).second) {
            return result;  // ok = false
        }
    }

    auto find = [&](NodeId id, std::size_t& out) -> bool {
        const auto it = index_of.find(id);
        if (it == index_of.end()) return false;
        out = it->second;
        return true;
    };

    // Assign a reason only if the node has none yet (first reason wins).
    auto seed = [&](std::size_t i, AnticipationExclusion why) {
        if (result.node_exclusion[i] == AnticipationExclusion::None) {
            result.node_exclusion[i] = why;
        }
    };

    // Seed live-input roots and transport-sensitive nodes. A node that opted into
    // the host transport (cached, prepare-stable GraphNode::transport_sensitive)
    // must run live, so seed it here exactly like a hard topology exclusion; the
    // forward propagation below then carries TransportSensitive over its whole
    // downstream cone. The seed order makes a live-input/feedback/sidechain reason
    // win over TransportSensitive when a node is both (first-reason-wins, and the
    // specific reason is only diagnostic — exclusion is the load-bearing bit).
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (nodes[i].type == NodeType::AudioInput) {
            seed(i, AnticipationExclusion::LiveAudioInput);
        } else if (nodes[i].type == NodeType::MidiInput) {
            seed(i, AnticipationExclusion::LiveMidiInput);
        }
        if (nodes[i].transport_sensitive) {
            seed(i, AnticipationExclusion::TransportSensitive);
        }
    }

    // Seed the endpoints of every feedback edge and any sidechain consumer.
    // Validate every connection's node ids up front; a dangling id is malformed.
    for (const auto& c : connections) {
        std::size_t s = 0;
        std::size_t d = 0;
        if (!find(c.source_node, s) || !find(c.dest_node, d)) {
            return result;  // ok = false
        }
        if (c.feedback) {
            seed(s, AnticipationExclusion::Feedback);
            seed(d, AnticipationExclusion::Feedback);
        }
        if (c.sidechain) {
            seed(d, AnticipationExclusion::Sidechain);
        }
    }

    // Propagate each exclusion forward along feedforward (non-feedback) edges to a
    // fixpoint: anything reading an excluded source is itself non-anticipatable.
    // Monotone (a node only ever flips None -> reason, never back) and bounded by
    // node_count total flips, so it runs at most node_count + 1 passes; O(V*E)
    // worst case, acceptable off the audio thread. The find() guards are belt-and-
    // suspenders: the seed loop already rejected any dangling id with ok=false, but
    // re-checking here keeps this safety classifier correct by construction rather
    // than by a non-local invariant (a stray index would otherwise corrupt node 0).
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& c : connections) {
            if (c.feedback) continue;  // back-edge reads the previous block
            std::size_t s = 0;
            std::size_t d = 0;
            if (!find(c.source_node, s) || !find(c.dest_node, d)) continue;
            if (result.node_exclusion[s] != AnticipationExclusion::None &&
                result.node_exclusion[d] == AnticipationExclusion::None) {
                result.node_exclusion[d] = result.node_exclusion[s];
                changed = true;
            }
        }
    }

    result.ok = true;
    return result;
}

} // namespace pulp::host
