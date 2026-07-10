#pragma once
//
// Pulp Live Kernel — S0 spike — block executor (one Plan).
//
// A deliberately tiny, worklet-fit cousin of the native
// GraphRuntimeExecutor::process_routed(): topo-sort the graph (ignoring feedback
// back-edges), then per 128-frame block, node-at-a-time:
//   gather  -> zero each input port slot, then SUM every inbound edge's source
//              buffer into it; feedback edges read the source's PREVIOUS-block
//              output slot (native's one-block feedback_prev).
//   run     -> the registry's process_node over the real signal class.
//   capture -> after all nodes run, copy each feedback source's current output
//              into its previous-block slot for the next block.
// This mirrors core/format/src/graph_runtime_executor.cpp (gather_node_inputs /
// capture_feedback) so a null test against an AOT-built twin is meaningful.
//
// ALL storage is preallocated inline; build_plan() and render_block() allocate
// nothing (the DelayLineT pool is prepared ONCE in prepare_pool()).

#include "codec.hpp"
#include "registry.hpp"

#include <cmath>
#include <cstring>

namespace pulp::live_kernel {

inline constexpr int LK_MAX_DELAY_NODES   = 8;
inline constexpr int LK_MAX_DELAY_SAMPLES = 48000; // 1.0 s @ 48 kHz
inline constexpr int LK_MAX_CHORUS_NODES  = 4;     // iter2: pooled modulated-delay nodes
inline constexpr int LK_MAX_REVERB_NODES  = 2;     // iter2: pooled FDN reverbs (4 lines each)

struct Plan {
    double sample_rate = 48000.0;
    int    num_nodes   = 0;
    int    output_node = 0;
    bool   valid       = false;

    NodeType     type[LK_MAX_NODES];
    int          num_in[LK_MAX_NODES];
    int          num_out[LK_MAX_NODES];
    NodeInstance inst[LK_MAX_NODES];

    // Per-node single-output buffers + one-block feedback memory.
    float outbuf[LK_MAX_NODES][LK_MAX_BLOCK];
    float prevbuf[LK_MAX_NODES][LK_MAX_BLOCK];

    // Reused input-gather scratch (one node at a time, so shared is fine).
    float inscratch[LK_MAX_IN][LK_MAX_BLOCK];

    // Edges (copied from the desc) + CSR inbound adjacency by dst node.
    EdgeDesc edges[LK_MAX_EDGES];
    int      num_edges = 0;
    int      inbound_off[LK_MAX_NODES + 1];
    int      inbound_idx[LK_MAX_EDGES];
    bool     has_feedback = false;

    // Topological order (feedback edges excluded).
    int order[LK_MAX_NODES];
    int order_count = 0;

    // Per-node output mean-square, refreshed every render_block (alloc-free
    // readout for the signal-flow graph). node_rms[i] holds node i's mean-square
    // this block (the readout sqrt's it to RMS); unreachable nodes read 0.
    float node_rms[LK_MAX_NODES] = {0};

    // Pools of the nodes whose DSP owns prepare-time buffers (DelayLineT). All
    // prepared ONCE at kernel init (off the audio path); build_plan only binds +
    // reset()s them, so a structural edit stays zero-alloc.
    Delay  delay_pool[LK_MAX_DELAY_NODES];
    Chorus chorus_pool[LK_MAX_CHORUS_NODES];
    Reverb reverb_pool[LK_MAX_REVERB_NODES];
    int   delay_used  = 0;
    int   chorus_used = 0;
    int   reverb_used = 0;
    bool  pool_ready  = false;

    // Prepare every pool ONCE. Allocates (DelayLineT::prepare); MUST be called
    // off the steady-state audio path (kernel construction).
    void prepare_pool(double sr) {
        for (auto& d : delay_pool)  d.prepare(LK_MAX_DELAY_SAMPLES);
        for (auto& c : chorus_pool) c.prepare((float)sr);
        for (auto& r : reverb_pool) r.prepare((float)sr);
        pool_ready = true;
    }
};

// Kahn topological sort over non-feedback edges. Returns true if all nodes were
// ordered (false => a non-feedback cycle, which v0 forbids).
inline bool topo_sort(Plan& p) {
    int indeg[LK_MAX_NODES] = {0};
    for (int e = 0; e < p.num_edges; ++e) {
        if (p.edges[e].feedback) continue;
        indeg[p.edges[e].dst]++;
    }
    int queue[LK_MAX_NODES], head = 0, tail = 0;
    for (int i = 0; i < p.num_nodes; ++i)
        if (indeg[i] == 0) queue[tail++] = i;
    p.order_count = 0;
    while (head < tail) {
        int n = queue[head++];
        p.order[p.order_count++] = n;
        for (int e = 0; e < p.num_edges; ++e) {
            if (p.edges[e].feedback) continue;
            if (p.edges[e].src != n) continue;
            int d = p.edges[e].dst;
            if (--indeg[d] == 0) queue[tail++] = d;
        }
    }
    return p.order_count == p.num_nodes;
}

// Bind a decoded PlanDesc into this preallocated Plan. Zero-alloc (the delay pool
// is already prepared). Returns false on a non-feedback cycle.
inline bool build_plan(Plan& p, const PlanDesc& d, double sr) {
    p.sample_rate = sr;
    p.valid = false;
    p.num_nodes = d.num_nodes;
    p.output_node = d.output_node;
    p.num_edges = d.num_edges;
    p.delay_used = 0;
    p.chorus_used = 0;
    p.reverb_used = 0;
    p.has_feedback = false;

    for (int i = 0; i < d.num_nodes; ++i) {
        p.type[i] = d.nodes[i].type;
        ports_for(p.type[i], p.num_in[i], p.num_out[i]);
        NodeInstance& ni = p.inst[i];
        ni = NodeInstance{}; // reset cached scalars (Comp/Noise inline, no heap)
        if (p.type[i] == NodeType::Delay) {
            ni.delay = (p.delay_used < LK_MAX_DELAY_NODES)
                           ? &p.delay_pool[p.delay_used++] : nullptr;
        } else if (p.type[i] == NodeType::Chorus) {
            ni.chorus = (p.chorus_used < LK_MAX_CHORUS_NODES)
                            ? &p.chorus_pool[p.chorus_used++] : nullptr;
        } else if (p.type[i] == NodeType::Reverb) {
            ni.reverb = (p.reverb_used < LK_MAX_REVERB_NODES)
                            ? &p.reverb_pool[p.reverb_used++] : nullptr;
        }
        init_node(ni, p.type[i], sr);
    }

    for (int e = 0; e < d.num_edges; ++e) {
        p.edges[e] = d.edges[e];
        if (p.edges[e].feedback) p.has_feedback = true;
    }

    if (!topo_sort(p)) return false;

    // CSR inbound adjacency (edge indices grouped by dst node).
    int count[LK_MAX_NODES] = {0};
    for (int e = 0; e < p.num_edges; ++e) count[p.edges[e].dst]++;
    p.inbound_off[0] = 0;
    for (int i = 0; i < p.num_nodes; ++i) p.inbound_off[i + 1] = p.inbound_off[i] + count[i];
    int cursor[LK_MAX_NODES];
    for (int i = 0; i < p.num_nodes; ++i) cursor[i] = p.inbound_off[i];
    for (int e = 0; e < p.num_edges; ++e) {
        int d2 = p.edges[e].dst;
        p.inbound_idx[cursor[d2]++] = e;
    }

    // Zero the per-node buffers + feedback memory.
    std::memset(p.outbuf, 0, sizeof(float) * LK_MAX_NODES * LK_MAX_BLOCK);
    std::memset(p.prevbuf, 0, sizeof(float) * LK_MAX_NODES * LK_MAX_BLOCK);

    // Apply the param table.
    for (int i = 0; i < d.num_params; ++i)
        set_node_param(p.inst[d.params[i].node], p.type[d.params[i].node],
                       d.params[i].param_id, d.params[i].value, sr);

    p.valid = true;
    return true;
}

// Apply a live param edit to a node in this plan (zero-interruption edit path).
inline void plan_set_param(Plan& p, int node, int param_id, float value) {
    if (node < 0 || node >= p.num_nodes) return;
    set_node_param(p.inst[node], p.type[node], param_id, value, p.sample_rate);
}

// Render one block; returns a pointer to the output node's buffer (length n).
// Zero-alloc.
inline const float* render_block(Plan& p, int n, bool meter = true) {
    if (n > LK_MAX_BLOCK) n = LK_MAX_BLOCK;
    if (meter) for (int i = 0; i < p.num_nodes; ++i) p.node_rms[i] = 0.f; // unreachable → 0
    const float inv_n = n > 0 ? 1.f / (float)n : 0.f;
    for (int oi = 0; oi < p.order_count; ++oi) {
        const int node = p.order[oi];
        const int nin = p.num_in[node];
        // gather: mirror the native executor's (bus copy | sum) split — the
        // FIRST inbound edge on a port copies, any further edges accumulate, an
        // unconnected port stays silent. (A single-edge port never pays a
        // separate zero pass — the interpreter's trivial-chain hot path.)
        const float* ins[LK_MAX_IN];
        bool filled[LK_MAX_IN];
        for (int port = 0; port < nin; ++port) { filled[port] = false; ins[port] = p.inscratch[port]; }
        for (int k = p.inbound_off[node]; k < p.inbound_off[node + 1]; ++k) {
            const EdgeDesc& e = p.edges[p.inbound_idx[k]];
            int port = e.dport; if (port >= nin) port = nin - 1; if (port < 0) continue;
            const float* src = e.feedback ? p.prevbuf[e.src] : p.outbuf[e.src];
            float* slot = p.inscratch[port];
            if (!filled[port]) { std::memcpy(slot, src, sizeof(float) * n); filled[port] = true; }
            else { for (int i = 0; i < n; ++i) slot[i] += src[i]; }
        }
        for (int port = 0; port < nin; ++port)
            if (!filled[port]) std::memset(p.inscratch[port], 0, sizeof(float) * n);
        // run
        float* ob = p.outbuf[node];
        process_node(p.inst[node], p.type[node], ins, nin, ob, n);
        // per-node level tap (alloc-free) for the live signal-flow graph. Store
        // the mean-square here; the ~20 Hz readout (Kernel::node_levels) takes
        // the sqrt, keeping the per-block hot loop free of a sqrt per node. The
        // tap is skipped entirely in measurement mode so CPU numbers stay clean
        // (design §1.5).
        if (meter) {
            float ss = 0.f;
            for (int i = 0; i < n; ++i) ss += ob[i] * ob[i];
            p.node_rms[node] = ss * inv_n;
        }
    }
    // capture feedback (source current output -> its previous-block slot)
    if (p.has_feedback) {
        for (int e = 0; e < p.num_edges; ++e) {
            if (!p.edges[e].feedback) continue;
            std::memcpy(p.prevbuf[p.edges[e].src], p.outbuf[p.edges[e].src],
                        sizeof(float) * n);
        }
    }
    return p.outbuf[p.output_node];
}

} // namespace pulp::live_kernel
