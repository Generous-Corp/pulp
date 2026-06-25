#include <pulp/graph/graph_runtime_buffer_assignment.hpp>

#include <algorithm>
#include <array>
#include <vector>

namespace pulp::graph {
namespace {

// Free-list of contiguous scratch regions bucketed by size. A region of size
// k is recycled only as a size-k region, which keeps each node's input/output
// region contiguous (so the executor can keep addressing slots as base + port)
// while still reusing scratch across non-overlapping lifetimes. Region sizes
// are port counts, bounded by GraphRuntimeLimits::max_ports_per_node.
class RegionAllocator {
public:
    // Allocate a contiguous region of `size` slots, reusing a freed same-size
    // region if available, else bumping the high-water mark.
    std::uint32_t alloc(std::uint32_t size) {
        if (size == 0) return high_water_;  // empty region: no storage
        if (size < free_by_size_.size() && !free_by_size_[size].empty()) {
            const std::uint32_t base = free_by_size_[size].back();
            free_by_size_[size].pop_back();
            return base;
        }
        const std::uint32_t base = high_water_;
        high_water_ += size;
        return base;
    }
    // Allocate fresh slots above the high-water mark, never reusing a freed
    // region. Used for a persistent output region, which must be a slot no other
    // node ever writes — reusing an earlier node's freed slot would let that
    // node overwrite the persistent contents every block, clobbering the tail
    // before the next block reads it.
    std::uint32_t alloc_fresh(std::uint32_t size) {
        if (size == 0) return high_water_;
        const std::uint32_t base = high_water_;
        high_water_ += size;
        return base;
    }
    void free(std::uint32_t base, std::uint32_t size) {
        if (size == 0 || size >= free_by_size_.size()) return;
        free_by_size_[size].push_back(base);
    }
    std::uint32_t high_water() const noexcept { return high_water_; }

private:
    // Index by region size; sizes are bounded by max_ports_per_node (<= 64),
    // but use a generous fixed bound so an out-of-spec plan never indexes OOB.
    static constexpr std::size_t kMaxRegionSize = 256;
    std::array<std::vector<std::uint32_t>, kMaxRegionSize> free_by_size_{};
    std::uint32_t high_water_ = 0;
};

} // namespace

GraphRuntimeBufferAssignment build_graph_runtime_buffer_assignment(
    const GraphRuntimePlan& plan) {
    GraphRuntimeBufferAssignment assignment;
    const std::size_t node_count = plan.nodes.size();
    try {
        assignment.nodes.resize(node_count);
        assignment.feedback_prev_slot.assign(plan.connections.size(), kGraphRuntimeNoSlot);
        assignment.connection_delay_samples.assign(plan.connections.size(), 0);
    } catch (...) {
        assignment = {};
        assignment.ok = false;
        return assignment;
    }

    // A node's output region is live from when it is produced until its last
    // feedforward consumer runs. A feedback source's output must survive until
    // the end-of-block capture, so it is pinned past every position. An input
    // region is live only while its own node runs. We free regions at the END
    // of the position that last needs them, so a freed region is reused only by
    // strictly-later nodes — never aliasing a value still being read this block.
    if (plan.processing_order_indices.size() != node_count) {
        // Defensive: a malformed plan (validated elsewhere) — fall back to the
        // unique-slot layout rather than mis-free.
        assignment = {};
        assignment.ok = false;
        return assignment;
    }

    try {
        std::vector<std::uint32_t> pos(node_count, 0);
        for (std::uint32_t t = 0; t < plan.processing_order_indices.size(); ++t) {
            pos[plan.processing_order_indices[t]] = t;
        }

        // last_use[n] = topo position after which node n's OUTPUT region is dead.
        // Default to the node's own position (no consumer -> dead immediately).
        // A persistent-output node is pinned: its region is never freed, so it
        // never aliases another node and its contents survive across blocks
        // (the backing pool persists), matching a per-node persistent output
        // buffer for nodes that may not fully overwrite their outputs.
        const std::uint32_t pinned = static_cast<std::uint32_t>(node_count);  // never freed in-block
        std::vector<std::uint32_t> last_use(node_count);
        for (std::size_t n = 0; n < node_count; ++n) {
            last_use[n] = plan.nodes[n].persistent_output ? pinned : pos[n];
        }
        // Connection plans store dense node indices in source_index/dest_index.
        for (const auto& conn : plan.connections) {
            if (conn.event) continue;
            if (conn.feedback) {
                last_use[conn.source_index] = pinned;
            } else {
                last_use[conn.source_index] =
                    std::max(last_use[conn.source_index], pos[conn.dest_index]);
            }
        }

        // Regions to free at the end of each topo position.
        struct FreeReq { std::uint32_t base; std::uint32_t size; };
        std::vector<std::vector<FreeReq>> frees_at(node_count + 1);

        RegionAllocator alloc;
        for (std::uint32_t t = 0; t < plan.processing_order_indices.size(); ++t) {
            const auto node_index = plan.processing_order_indices[t];
            const auto& node = plan.nodes[node_index];
            auto& slots = assignment.nodes[node_index];

            slots.input_base = alloc.alloc(node.input_ports);
            // A persistent-output node's output region must be dedicated: fresh
            // slots that no other node ever writes (see alloc_fresh). Pairs with
            // last_use == pinned below, which also keeps it from being recycled
            // by a later node.
            slots.output_base = node.persistent_output
                                    ? alloc.alloc_fresh(node.output_ports)
                                    : alloc.alloc(node.output_ports);

            // Input region dies after this node runs.
            if (node.input_ports > 0) {
                frees_at[t].push_back({slots.input_base, node.input_ports});
            }
            // Output region dies after its last consumer (or never, if pinned).
            if (node.output_ports > 0) {
                const std::uint32_t death = last_use[node_index];
                if (death < node_count) {
                    frees_at[death].push_back({slots.output_base, node.output_ports});
                }
            }

            for (const auto& f : frees_at[t]) alloc.free(f.base, f.size);
        }

        std::uint32_t cursor = alloc.high_water();
        // Append one persistent previous-block slot per feedback edge (cross-
        // block state — never recycled).
        for (std::size_t i = 0; i < plan.connections.size(); ++i) {
            if (plan.connections[i].feedback) {
                assignment.feedback_prev_slot[i] = cursor++;
                assignment.has_feedback = true;
            }
        }
        assignment.slot_count = cursor;
    } catch (...) {
        assignment = {};
        assignment.ok = false;
        return assignment;
    }

    // Plug-in delay compensation. Propagate each node's added latency through the
    // topology in processing order (every source resolves before its consumers),
    // then derive each feedforward connection's required delay so fan-in paths of
    // differing latency time-align — matching the host graph's per-connection
    // delay lines. Feedback and event connections carry no delay.
    try {
        std::vector<std::uint32_t> input_latency(node_count, 0);
        std::vector<std::uint32_t> output_latency(node_count, 0);
        for (const auto node_index : plan.processing_order_indices) {
            const auto& node = plan.nodes[node_index];
            std::uint32_t max_upstream = 0;
            for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
                const auto ci = plan.inbound_connection_indices[
                    node.first_inbound_connection + c];
                const auto& conn = plan.connections[ci];
                if (conn.feedback || conn.event) continue;
                max_upstream = std::max(max_upstream, output_latency[conn.source_index]);
            }
            input_latency[node_index] = max_upstream;
            output_latency[node_index] = max_upstream + node.latency_samples;
        }
        for (std::size_t i = 0; i < plan.connections.size(); ++i) {
            const auto& conn = plan.connections[i];
            if (conn.feedback || conn.event) continue;
            const std::uint32_t dst_in = input_latency[conn.dest_index];
            const std::uint32_t src_out = output_latency[conn.source_index];
            const std::uint32_t want = dst_in > src_out ? dst_in - src_out : 0;
            assignment.connection_delay_samples[i] = want;
            if (want > 0) assignment.has_delay = true;
        }
    } catch (...) {
        assignment = {};
        assignment.ok = false;
        return assignment;
    }

    assignment.ok = true;
    return assignment;
}

} // namespace pulp::graph
