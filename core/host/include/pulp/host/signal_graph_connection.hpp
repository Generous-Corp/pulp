#pragma once

// Connection vocabulary for the Pulp host signal graph.

#include <pulp/host/graph_types.hpp>

#include <cstdint>

namespace pulp::host {

// ── Connection ──────────────────────────────────────────────────────────

enum class AutomationMix : uint8_t {
    Replace = 0,  // default; graph refuses a 2nd Replace edge to same (node,param)
    Add     = 1,  // summed with other Add edges, clamped to param range
};

struct Connection {
    NodeId source_node;
    PortIndex source_port;
    NodeId dest_node;
    PortIndex dest_port;      // audio: dest port index; automation: ignored
    bool feedback = false;    // back-edge: reads previous block's audio, breaks
                              // the cycle for topological sort and PDC.
    bool midi = false;        // event-edge: routes MidiBuffer events instead of
                              // audio samples. Ports are ignored.
    bool automation = false;  // automation-edge: source audio drives a param on
                              // the dest plugin.
    bool audio_rate_modulation = false; // dense CV edge into an AudioRate param.
    bool sidechain = false;   // sidechain-edge: like a normal audio edge, but
                              // routes into one of the destination plugin's
                              // sidechain-bus input ports. The
                              // topological-sort + PDC treat sidechain as a
                              // hard edge — it is not a back-edge.

    // Parameter-modulation fields (valid when automation or
    // audio_rate_modulation is true).
    uint32_t automation_param_id  = 0;
    float automation_range_lo     = 0.0f;  // plain param domain
    float automation_range_hi     = 1.0f;  // plain param domain
    float automation_smoothing_ms = 0.0f;  // per-source pre-mix slew
    AutomationMix automation_mix  = AutomationMix::Replace;

    bool operator==(const Connection& o) const {
        return source_node == o.source_node && source_port == o.source_port
            && dest_node == o.dest_node && dest_port == o.dest_port
            && automation == o.automation
            && audio_rate_modulation == o.audio_rate_modulation
            && sidechain == o.sidechain
            && ((automation || audio_rate_modulation)
                ? automation_param_id == o.automation_param_id : true);
    }
};

}  // namespace pulp::host
