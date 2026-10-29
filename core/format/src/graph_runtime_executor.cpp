#include <pulp/format/graph_runtime_executor.hpp>

#include <pulp/audio/load_measurer.hpp>
#include <pulp/midi/block_ops.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

namespace pulp::format {
namespace {

bool command_requires_node(graph::GraphCommandType type) noexcept {
    switch (type) {
        case graph::GraphCommandType::SetNodeGain:
        case graph::GraphCommandType::SetNodeParameter:
        case graph::GraphCommandType::SetNodeBypass:
        case graph::GraphCommandType::ResetNode:
        case graph::GraphCommandType::InjectMidi:
            return true;
        case graph::GraphCommandType::TransportJump:
        case graph::GraphCommandType::ActivateSnapshot:
        case graph::GraphCommandType::DeactivateSnapshot:
            return false;
    }
    return true;
}

graph::GraphEvent command_event(const graph::GraphTimedCommand& command,
                                graph::GraphEventType type) noexcept {
    graph::GraphEvent event;
    event.sequence_id = command.command.sequence_id;
    event.type = type;
    event.node_id = command.command.node_id;
    event.target_id = command.command.target_id;
    event.block_offset = command.block_offset;
    event.value = command.command.value;
    event.bool_value = command.command.bool_value;
    return event;
}

// --- Routing helpers -----------------------------------------------------
//
// Factored out of process_routed so the serial loop reads as
// gather -> dispatch -> (binding | bus copy). This keeps the per-node body a
// localized seam — a parallel executor can schedule run_routed_node across
// workers, and the feedback gather stays a one-line change — instead of one
// monolith. gather/run are pure over the pool's raw mono slots: the lift point
// if routing later moves to core/graph (no ProcessBlock dependency except the
// bus copy).

// Resolve where each of a node's input ports reads from this block, and fill
// `in_ptrs` with it. A port fed by exactly one audio edge with no delay passes
// that edge's samples through unchanged, every bit intact (-0 included),
// exactly as if the two nodes were called back to back:
// - when the assignment marks the edge reads_in_place (it is the only reader of
//   its source port, and the source's region is not persistent), the port reads
//   the source's output slot, or a feedback edge's previous-block slot, in
//   place with no copy;
// - otherwise (fan-out) the port gets a private bit-exact copy, so a consumer
//   that writes through its input view cannot corrupt what a sibling reads.
// A port with no edge reads its own zeroed slot. A port fed by several edges,
// or by one through a PDC delay ring, sums them into its own slot.
//
// Reading in place cannot make an input the node's own output: the assignment
// frees a source's output region only at the END of its last reader's position
// (build_graph_runtime_buffer_assignment), so the reader's output region is
// allocated before that region is free, and a node's input and output regions
// never overlap. Feedback previous-block slots are written only by
// capture_feedback, after every node has run.
//
// A per-connection delay ring time-aligns fan-in paths, matching the host
// graph's per-connection delay lines. Event and automation edges carry no
// audio into an input port.
void gather_node_inputs(const graph::GraphRuntimePlan& plan,
                        const graph::GraphRuntimeBufferAssignment& assignment,
                        GraphRuntimeBufferPool& pool, const graph::GraphRuntimeNodePlan& node,
                        const graph::GraphRuntimeNodeSlots& slots,
                        std::span<const GraphRuntimeSnapshot::InputPortSource> ports,
                        std::uint32_t frames, const float** in_ptrs,
                        bool in_place_resolved) noexcept {
    using Mode = GraphRuntimeSnapshot::InputPortSource::Mode;
    // The snapshot resolved every port in reset() (resolve_input_ports). A node
    // without that table sums every port, which is always correct.
    const bool resolved = ports.size() == node.input_ports;
    bool any_sum = false;
    for (std::uint32_t p = 0; p < node.input_ports; ++p) {
        const Mode mode = resolved ? ports[p].mode : Mode::Sum;
        if (mode == Mode::InPlace) {
            // A bound pool already points the port at its source, which may be
            // the host input rather than a slot this block.
            if (!in_place_resolved)
                in_ptrs[p] = pool.slot_data(ports[p].slot);
            continue;
        }
        float* own = pool.slot_data(slots.input_base + p);
        in_ptrs[p] = own;
        if (own == nullptr)
            continue;
        if (mode == Mode::Copy) {
            if (const float* src = pool.slot_data(ports[p].slot))
                std::copy_n(src, frames, own);
            else
                std::fill_n(own, frames, 0.0f);
            continue;
        }
        std::fill_n(own, frames, 0.0f);
        any_sum = any_sum || mode == Mode::Sum;
    }
    if (!any_sum)
        return;

    for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
        const auto conn_index =
            plan.inbound_connection_indices[node.first_inbound_connection + c];
        const auto& conn = plan.connections[conn_index];
        if (graph::is_event(conn) || graph::is_automation_conn(conn)) continue;
        if (conn.dest_port >= node.input_ports)
            continue;
        if (resolved && ports[conn.dest_port].mode != Mode::Sum)
            continue;
        float* dst = pool.slot_data(slots.input_base + conn.dest_port);
        if (dst == nullptr) continue;
        if (conn.feedback) {
            const float* src = pool.slot_data(assignment.feedback_prev_slot[conn_index]);
            if (src == nullptr) continue;
            for (std::uint32_t f = 0; f < frames; ++f) dst[f] += src[f];
            continue;
        }
        const auto& src_slots = assignment.nodes[conn.source_index];
        const float* src = pool.slot_data(src_slots.output_base + conn.source_port);
        if (src == nullptr) continue;
        const auto ring = pool.delay_ring(conn_index);
        if (ring.data == nullptr || ring.delay == 0) {
            for (std::uint32_t f = 0; f < frames; ++f) dst[f] += src[f];
        } else {
            // Per-connection delay line: write the current sample at the write
            // head, read the sample `delay` positions behind it, accumulate.
            const int ring_size = static_cast<int>(ring.size);
            const int delay = static_cast<int>(ring.delay);
            int wp = *ring.write_pos;
            int rp = wp - delay;
            if (rp < 0) rp += ring_size;
            for (std::uint32_t f = 0; f < frames; ++f) {
                ring.data[wp] = src[f];
                dst[f] += ring.data[rp];
                if (++wp == ring_size) wp = 0;
                if (++rp == ring_size) rp = 0;
            }
            *ring.write_pos = wp;
        }
    }
}

// The routed MIDI clear / drop-check / whole-buffer copy the host reference
// walk also applies when gathering a node's inbound MIDI. Shared with the host
// layer via core/midi so routed-vs-walk output stays bit-exact across the
// host/format boundary.
using midi::clear_midi_block;
using midi::copy_midi_block;
using midi::midi_block_has_drops;

// Clear this node's gathered MIDI input, then append every inbound event
// connection's source MIDI output into it (whole-buffer copy, summed in inbound
// order). Topological order guarantees each source's output is final this block.
// The node's MIDI output is left untouched — a MIDI-emitting binding fills it,
// and a MidiInput system node's output is supplied by the host before the walk.
void gather_node_midi(const graph::GraphRuntimePlan& plan,
                      GraphRuntimeMidiScratch& midi,
                      const graph::GraphRuntimeNodePlan& node,
                      std::uint32_t node_index) noexcept {
    midi::MidiBuffer* in = midi.in(node_index);
    if (in == nullptr) return;
    clear_midi_block(*in);
    bool incomplete = false;
    for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
        const auto conn_index =
            plan.inbound_connection_indices[node.first_inbound_connection + c];
        const auto& conn = plan.connections[conn_index];
        if (!graph::is_event(conn)) continue;
        const midi::MidiBuffer* src = midi.out(conn.source_index);
        if (src == nullptr) continue;
        // A source whose output was incomplete, or a copy that drops here,
        // marks this node's input incomplete (matching the host walk's
        // node-to-node incompleteness propagation).
        if (midi.out_incomplete(conn.source_index)) incomplete = true;
        if (!copy_midi_block(*src, *in)) incomplete = true;
    }
    midi.set_in_incomplete(node_index, incomplete);
}

// Build this node's parameter-automation events from its inbound SPARSE
// automation connections (audio-rate edges are handled elsewhere). Each edge
// samples its source's audio output at sample 0 and N-1, maps into the
// parameter range, applies per-source linear slew (state persisted in the
// scratch), accumulates per parameter by mix mode, then emits two control points
// per touched parameter — replicating the host walk's sparse-automation math.
void gather_node_automation(const graph::GraphRuntimePlan& plan,
                            const graph::GraphRuntimeBufferAssignment& assignment,
                            GraphRuntimeBufferPool& pool, GraphRuntimeAutomationScratch& automation,
                            const graph::GraphRuntimeNodePlan& node, std::uint32_t node_index,
                            std::uint32_t frames, double sample_rate,
                            AudioRateModulationDelivery delivery,
                            bool preserve_parameter_events) noexcept {
    state::ParameterEventQueue* queue = automation.events(node_index);
    if (queue == nullptr) return;
    if (!preserve_parameter_events)
        queue->clear();
    automation.set_dense_view_count(node_index, 0);
    const int last = static_cast<int>(frames) - 1;

    // Per-node SPARSE accumulator slice (disjoint per node => parallel-safe).
    // Sized off-RT to this node's distinct sparse param count; zero its slots at
    // entry since the storage persists across blocks/nodes.
    using SparseAccum = GraphRuntimeAutomationScratch::SparseAccum;
    const std::span<SparseAccum> accum = automation.sparse_accum(node_index);
    for (auto& a : accum) a = SparseAccum{};
    std::uint32_t param_count = 0;

    for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
        const auto conn_index =
            plan.inbound_connection_indices[node.first_inbound_connection + c];
        const auto& conn = plan.connections[conn_index];
        if (!graph::is_automation_conn(conn) || conn.automation.audio_rate) continue;
        const auto& src_slots = assignment.nodes[conn.source_index];
        const float* src = pool.slot_data(src_slots.output_base + conn.source_port);
        if (src == nullptr) continue;
        const auto& a = conn.automation;
        const float s0 = std::clamp(src[0], 0.0f, 1.0f);
        const float sN = std::clamp(src[last < 0 ? 0 : last], 0.0f, 1.0f);
        float m0 = a.range_lo + s0 * (a.range_hi - a.range_lo);
        float mN = a.range_lo + sN * (a.range_hi - a.range_lo);

        if (a.smoothing_ms > 0.0f && sample_rate > 0.0) {
            const float range = std::abs(a.range_hi - a.range_lo);
            const double slew_samples =
                static_cast<double>(a.smoothing_ms) * 0.001 * sample_rate;
            const float max_step =
                slew_samples > 0.0 ? (range / static_cast<float>(slew_samples)) : range;
            if (!automation.slew_primed(conn_index)) {
                automation.slew_last(conn_index) = m0;
                automation.set_slew_primed(conn_index, true);
            }
            auto ramp_to = [max_step](float from, float target) {
                const float delta = target - from;
                if (delta > max_step) return from + max_step;
                if (delta < -max_step) return from - max_step;
                return target;
            };
            const float new_v0 = ramp_to(automation.slew_last(conn_index), m0);
            float new_vN = new_v0;
            if (last > 0) {
                const float max_block_step = max_step * static_cast<float>(last);
                const float delta = mN - new_v0;
                if (delta > max_block_step) {
                    new_vN = new_v0 + max_block_step;
                } else if (delta < -max_block_step) {
                    new_vN = new_v0 - max_block_step;
                } else {
                    new_vN = mN;
                }
            }
            automation.slew_last(conn_index) = new_vN;
            m0 = new_v0;
            mN = new_vN;
        }

        std::uint32_t pi = 0;
        for (; pi < param_count; ++pi) {
            if (accum[pi].param_id == a.param_id) break;
        }
        if (pi == param_count) {
            // Defensive backstop: the slice is sized to this node's distinct
            // sparse param count, so this never triggers for a valid plan.
            if (param_count >= accum.size()) continue;
            accum[pi] = SparseAccum{a.param_id, 0.0f, 0.0f, a.bounds_lo, a.bounds_hi, false};
            ++param_count;
        }
        if (a.mix_add) {
            accum[pi].v0 += m0;
            accum[pi].vN += mN;
            accum[pi].has_add = true;
        } else {
            accum[pi].v0 = m0;
            accum[pi].vN = mN;
        }
    }

    for (std::uint32_t pi = 0; pi < param_count; ++pi) {
        float v0 = accum[pi].v0;
        float vN = accum[pi].vN;
        if (accum[pi].has_add) {
            const float lo = std::min(accum[pi].lo, accum[pi].hi);
            const float hi = std::max(accum[pi].lo, accum[pi].hi);
            v0 = std::clamp(v0, lo, hi);
            vN = std::clamp(vN, lo, hi);
        }
        if (!queue->push({accum[pi].param_id, 0, v0, 0})) break;
        if (last > 0 && !queue->push({accum[pi].param_id, last, vN, 0})) break;
    }

    // Dense (audio-rate) modulation: per-sample map (with the same per-connection
    // PDC delay ring as audio) accumulated per parameter, emitting one event per
    // sample. Pushed after the sparse control points into the SAME queue (a final
    // stable sort orders them by sample offset), matching the host walk's
    // audio-rate path.
    const std::uint32_t dense_count = automation.dense_param_count(node_index);
    // Per-node DENSE transient gather slices (disjoint per node => parallel-safe).
    // Touch tracking keeps the RT path from clearing/emitting every predeclared
    // dense lane when this block reaches only a subset.
    const std::span<std::uint8_t> dense_replace = automation.dense_replace(node_index);
    const std::span<std::uint8_t> dense_add = automation.dense_add(node_index);
    const std::span<float> dense_lo = automation.dense_lo(node_index);
    const std::span<float> dense_hi = automation.dense_hi(node_index);
    const std::span<std::uint32_t> dense_touched = automation.dense_touched(node_index);
    const std::span<AudioRateModulationView> dense_views = automation.dense_views(node_index);
    std::uint32_t dense_touched_count = 0;
    auto touch_dense = [&](std::uint32_t pi) noexcept -> float* {
        float* dst = automation.dense_buffer(node_index, pi);
        if (!dense_replace[pi] && !dense_add[pi]) {
            dense_lo[pi] = 0.0f;
            dense_hi[pi] = 0.0f;
            std::fill_n(dst, frames, 0.0f);
            dense_touched[dense_touched_count++] = pi;
        }
        return dst;
    };
    for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
        const auto conn_index =
            plan.inbound_connection_indices[node.first_inbound_connection + c];
        const auto& conn = plan.connections[conn_index];
        if (!graph::is_automation_conn(conn) || !conn.automation.audio_rate) continue;
        std::uint32_t pi = 0;
        for (; pi < dense_count; ++pi) {
            if (automation.dense_param_id(node_index, pi) == conn.automation.param_id) break;
        }
        if (pi == dense_count) continue;
        const auto& src_slots = assignment.nodes[conn.source_index];
        const float* src = pool.slot_data(src_slots.output_base + conn.source_port);
        if (src == nullptr) continue;
        const auto& a = conn.automation;
        const float slew_range = std::abs(a.range_hi - a.range_lo);
        const double slew_samples = static_cast<double>(a.smoothing_ms) * 0.001 * sample_rate;
        const float max_step = a.smoothing_ms > 0.0f && slew_samples > 0.0
                                   ? slew_range / static_cast<float>(slew_samples)
                                   : slew_range;
        auto map_and_slew = [&](float sample) noexcept {
            const float target =
                a.range_lo + std::clamp(sample, 0.0f, 1.0f) * (a.range_hi - a.range_lo);
            if (a.smoothing_ms <= 0.0f || sample_rate <= 0.0)
                return target;
            if (!automation.slew_primed(conn_index)) {
                automation.slew_last(conn_index) = target;
                automation.set_slew_primed(conn_index, true);
                return target;
            }
            const float previous = automation.slew_last(conn_index);
            const float value = std::clamp(target, previous - max_step, previous + max_step);
            automation.slew_last(conn_index) = value;
            return value;
        };
        float* dst = touch_dense(pi);
        dense_lo[pi] = a.bounds_lo;
        dense_hi[pi] = a.bounds_hi;
        const auto ring = pool.delay_ring(conn_index);
        if (ring.data == nullptr || ring.delay == 0) {
            for (std::uint32_t f = 0; f < frames; ++f) {
                const float v = map_and_slew(src[f]);
                if (a.mix_add) { dst[f] += v; dense_add[pi] = true; }
                else { dst[f] = v; dense_replace[pi] = true; }
            }
        } else {
            const int ring_size = static_cast<int>(ring.size);
            const int delay = static_cast<int>(ring.delay);
            int wp = *ring.write_pos;
            int rp = wp - delay;
            if (rp < 0) rp += ring_size;
            for (std::uint32_t f = 0; f < frames; ++f) {
                ring.data[wp] = src[f];
                const float v = map_and_slew(ring.data[rp]);
                if (a.mix_add) { dst[f] += v; dense_add[pi] = true; }
                else { dst[f] = v; dense_replace[pi] = true; }
                if (++wp == ring_size) wp = 0;
                if (++rp == ring_size) rp = 0;
            }
            *ring.write_pos = wp;
        }
    }
    for (std::uint32_t touched = 0; touched < dense_touched_count; ++touched) {
        const std::uint32_t i = dense_touched[touched];
        if (!dense_replace[i] && !dense_add[i]) continue;
        const std::uint32_t pid = automation.dense_param_id(node_index, i);
        const float* vals = automation.dense_buffer(node_index, i);
        const float lo = std::min(dense_lo[i], dense_hi[i]);
        const float hi = std::max(dense_lo[i], dense_hi[i]);
        const bool clamp_values =
            delivery == AudioRateModulationDelivery::DenseViews || dense_add[i];
        for (std::uint32_t f = 0; f < frames; ++f) {
            float v = vals[f];
            if (clamp_values)
                v = std::clamp(v, lo, hi);
            // DenseViews exposes the bounded canonical representation directly.
            // Legacy Replace preserves the historical ParameterEventQueue values;
            // Add remains bounded after accumulation.
            automation.dense_buffer(node_index, i)[f] = v;
            if (delivery == AudioRateModulationDelivery::LegacyParameterEvents &&
                !queue->push({pid, static_cast<std::int32_t>(f), v, 0})) {
                break;
            }
        }
        if (delivery == AudioRateModulationDelivery::DenseViews) {
            dense_views[touched] = {
                pid,
                std::span<const float>(automation.dense_buffer(node_index, i), frames),
            };
        }
        dense_replace[i] = 0;
        dense_add[i] = 0;
        dense_lo[i] = 0.0f;
        dense_hi[i] = 0.0f;
    }
    automation.set_dense_view_count(node_index, dense_touched_count);
    queue->sort();
}

// After all nodes run, capture each feedback edge's current source output into
// its previous-block slot for the next block (the source's output slot is final
// once the block's walk completes). One-block delay, matching SignalGraph.
void capture_feedback(const graph::GraphRuntimePlan& plan,
                      const graph::GraphRuntimeBufferAssignment& assignment,
                      GraphRuntimeBufferPool& pool,
                      std::uint32_t frames) noexcept {
    if (!assignment.has_feedback) return;
    for (std::size_t i = 0; i < plan.connections.size(); ++i) {
        const auto& conn = plan.connections[i];
        if (!conn.feedback) continue;
        const auto& src_slots = assignment.nodes[conn.source_index];
        const float* src = pool.slot_data(src_slots.output_base + conn.source_port);
        float* prev = pool.slot_data(assignment.feedback_prev_slot[i]);
        if (src == nullptr || prev == nullptr) continue;
        std::copy_n(src, frames, prev);
    }
}

// AudioInput copies the main input bus into its output slots; AudioOutput
// writes its resolved inputs into the main output bus. The only format/bus-aware
// step in routing.
//
// The host graph this mirrors (SignalGraph) zeroes the output bus once per block
// and lets every AudioOutput node sum into it, so N output nodes mix rather than
// last-writer-wins; process_routed zeroes the bus to match. When the graph has a
// single AudioOutput there is nothing to mix, and it copies instead, so the
// bus receives its input's bits exactly (a sum onto zero would turn -0 into +0).
void copy_io_bus(graph::GraphRuntimeNodeKind kind, GraphRuntimeBufferPool& pool,
                 const graph::GraphRuntimeNodePlan& node, const graph::GraphRuntimeNodeSlots& slots,
                 const float* const* in_ptrs, bool sole_output, const BusBuffer* in_bus,
                 BusBuffer* out_bus, std::uint32_t frames) noexcept {
    if (kind == graph::GraphRuntimeNodeKind::AudioInput) {
        for (std::uint32_t p = 0; p < node.output_ports; ++p) {
            float* dst = pool.slot_data(slots.output_base + p);
            if (dst == nullptr) continue;
            if (in_bus != nullptr && p < in_bus->input.num_channels()) {
                std::copy_n(in_bus->input.channel_ptr(p), frames, dst);
            } else {
                std::fill_n(dst, frames, 0.0f);
            }
        }
    } else {  // AudioOutput
        for (std::uint32_t p = 0; p < node.input_ports; ++p) {
            if (out_bus == nullptr || p >= out_bus->output.num_channels()) continue;
            const float* src = in_ptrs[p];
            float* dst = out_bus->output.channel_ptr(p);
            if (sole_output) {
                // The walk did not zero the channels a sole output writes.
                if (src != nullptr)
                    std::copy_n(src, frames, dst);
                else
                    std::fill_n(dst, frames, 0.0f);
            } else if (src != nullptr) {
                for (std::uint32_t f = 0; f < frames; ++f)
                    dst[f] += src[f];
            }
        }
    }
}

// True when the graph has exactly one AudioOutput node, so it can write the
// output bus instead of summing into it (see copy_io_bus).
bool has_sole_audio_output(const graph::GraphRuntimePlan& plan) noexcept {
    std::uint32_t outputs = 0;
    for (const auto& n : plan.nodes)
        if (n.kind == graph::GraphRuntimeNodeKind::AudioOutput && ++outputs > 1)
            return false;
    return outputs == 1;
}

// Run ONE node of the routing walk: gather its audio / MIDI / automation inputs,
// then either copy the I/O bus (AudioInput/AudioOutput) or invoke its binding.
// Returns the error code (None on success). This is the per-node seam shared by
// the serial walk and the parallel (levelized) walk, so both produce identical
// output by construction. It touches only this node's own scratch (disjoint
// output slots, per-node MIDI/automation buffers, per-connection inbound rings),
// so distinct nodes may run concurrently — EXCEPT AudioOutput, which accumulates
// into the shared output bus and must be run serially by the caller.
GraphRuntimeExecutorErrorCode
run_routed_node(std::uint32_t node_index, const graph::GraphRuntimePlan& plan,
                std::span<const GraphRuntimeNodeBinding> bindings,
                const graph::GraphRuntimeBufferAssignment& assignment, GraphRuntimeBufferPool& pool,
                GraphRuntimeMidiScratch* midi, GraphRuntimeAutomationScratch* automation,
                ProcessBlock& block, const BusBuffer* in_bus, BusBuffer* out_bus,
                std::span<const GraphRuntimeCommandDecision> command_results, std::uint32_t frames,
                bool time_node, bool sole_output,
                std::span<const GraphRuntimeSnapshot::InputPortSource> ports,
                GraphRuntimeBufferPool::BoundNode* bound) noexcept {
    const auto& node = plan.nodes[node_index];
    const auto& slots = assignment.nodes[node_index];

    // The node's channel views. A bound pool resolved them, and checked the port
    // limit, in bind(); its gather only moves samples, and only for a node with
    // a port that is not read in place. Otherwise only the node's own ports are
    // written, and the views read no further, so the arrays are left
    // uninitialized rather than zeroing kMaxRoutedPortsPerNode entries per node
    // per block.
    std::array<const float*, GraphRuntimeExecutor::kMaxRoutedPortsPerNode> in_ptrs;
    const float** inputs = in_ptrs.data();
    if (bound != nullptr) {
        inputs = bound->inputs;
        if (bound->gathers)
            gather_node_inputs(plan, assignment, pool, node, slots, ports, frames, inputs, true);
    } else {
        if (node.input_ports > GraphRuntimeExecutor::kMaxRoutedPortsPerNode ||
            node.output_ports > GraphRuntimeExecutor::kMaxRoutedPortsPerNode) {
            return GraphRuntimeExecutorErrorCode::NodePortLimitExceeded;
        }
        gather_node_inputs(plan, assignment, pool, node, slots, ports, frames, inputs, false);
    }
    if (midi != nullptr) {
        gather_node_midi(plan, *midi, node, node_index);
    }
    const auto& binding = bindings[node_index];
    if (automation != nullptr) {
        gather_node_automation(plan, assignment, pool, *automation, node, node_index, frames,
                               block.sample_rate, binding.audio_rate_modulation_delivery,
                               binding.preserve_parameter_events);
    }

    // Per-node CPU-load attribution: wrap this node's per-block produce step in
    // the node's measurer (when the host wired one) so routed execution — serial
    // OR parallel, both reach here — reports the same node_loads() telemetry the
    // legacy walk does. RAII closes the span on every return below (I/O copy,
    // missing/failed processor, success). Distinct measurer per node, so parallel
    // levels touch disjoint instances; begin()/end() are relaxed-atomic, RT-safe.
    audio::AudioProcessLoadMeasurer* const node_load =
        time_node ? bindings[node_index].load : nullptr;
    if (node_load != nullptr) {
        node_load->begin(static_cast<int>(frames), static_cast<float>(block.sample_rate));
    }
    struct LoadSpanEnd {
        audio::AudioProcessLoadMeasurer* m;
        ~LoadSpanEnd() noexcept {
            if (m != nullptr) m->end();
        }
    } load_span_end{node_load};

    if (node.kind == graph::GraphRuntimeNodeKind::AudioInput ||
        node.kind == graph::GraphRuntimeNodeKind::AudioOutput) {
        // An AudioInput node whose readers read the host input directly this
        // block has nothing to copy.
        if (bound == nullptr || !bound->host_input_aliased)
            copy_io_bus(node.kind, pool, node, slots, inputs, sole_output, in_bus, out_bus, frames);
        return GraphRuntimeExecutorErrorCode::None;
    }

    if (!binding.process) {
        return binding.required ? GraphRuntimeExecutorErrorCode::MissingRequiredProcessor
                                : GraphRuntimeExecutorErrorCode::None;
    }

    // A bound node's context keeps its node index, routed flag and channel
    // pointers across blocks; every field that can change per block is set here.
    std::array<float*, GraphRuntimeExecutor::kMaxRoutedPortsPerNode> out_ptrs;
    GraphRuntimeNodeProcessContext unbound_context;
    float** outputs = out_ptrs.data();
    if (bound != nullptr) {
        outputs = bound->outputs;
    } else {
        for (std::uint32_t p = 0; p < node.output_ports; ++p)
            out_ptrs[p] = pool.slot_data(slots.output_base + p);
        unbound_context.node_index = node_index;
        unbound_context.routed = true;
    }
    GraphRuntimeNodeProcessContext& context = bound != nullptr ? bound->context : unbound_context;
    context.plan = &plan;
    context.node = &node;
    context.command_results = command_results;
    context.node_inputs = audio::BufferView<const float>(inputs, node.input_ports, frames);
    context.node_outputs = audio::BufferView<float>(outputs, node.output_ports, frames);
    context.node_midi_in = midi != nullptr ? midi->in(node_index) : nullptr;
    context.node_midi_out = midi != nullptr ? midi->out(node_index) : nullptr;
    context.node_param_events = automation != nullptr ? automation->events(node_index) : nullptr;
    context.node_audio_rate_modulations = {};
    if (automation != nullptr &&
        binding.audio_rate_modulation_delivery == AudioRateModulationDelivery::DenseViews) {
        const auto count = automation->dense_view_count(node_index);
        context.node_audio_rate_modulations = automation->dense_views(node_index).first(count);
    }
    if (!binding.process(block, context, binding.user_data)) {
        return GraphRuntimeExecutorErrorCode::NodeProcessorFailed;
    }
    if (midi != nullptr && context.node_midi_out != nullptr) {
        midi->set_out_incomplete(
            node_index,
            midi->in_incomplete(node_index) ||
                midi_block_has_drops(*context.node_midi_out));
    }
    return GraphRuntimeExecutorErrorCode::None;
}

} // namespace

bool GraphRuntimeMidiScratch::reset(std::uint32_t node_count, std::size_t event_capacity,
                                    std::size_t sysex_capacity,
                                    std::size_t sysex_payload_capacity) {
    if (event_capacity == 0)
        return false;
    try {
        std::vector<std::size_t> event_capacities(node_count, event_capacity);
        return reset(event_capacities, sysex_capacity, sysex_payload_capacity);
    } catch (...) {
        clear();
        return false;
    }
}

bool GraphRuntimeMidiScratch::reset(std::span<const std::size_t> event_capacities,
                                    std::size_t sysex_capacity,
                                    std::size_t sysex_payload_capacity) {
    clear();
    if (event_capacities.size() > std::numeric_limits<std::uint32_t>::max() ||
        std::any_of(event_capacities.begin(), event_capacities.end(),
                    [](std::size_t capacity) { return capacity == 0; }))
        return false;
    try {
        slots_.reserve(event_capacities.size());
        for (const auto event_capacity : event_capacities) {
            auto slot = std::make_unique<Slot>();
            slot->in_buffer.reserve(event_capacity, sysex_capacity, sysex_payload_capacity);
            slot->in_buffer.set_realtime_capacity_limit(true);
            slot->in_ump.reserve(event_capacity);
            slot->in_ump.set_realtime_capacity_limit(true);
            slot->in_buffer.attach_ump(&slot->in_ump);
            slot->out_buffer.reserve(event_capacity, sysex_capacity, sysex_payload_capacity);
            slot->out_buffer.set_realtime_capacity_limit(true);
            slot->out_ump.reserve(event_capacity);
            slot->out_ump.set_realtime_capacity_limit(true);
            slot->out_buffer.attach_ump(&slot->out_ump);
            slots_.push_back(std::move(slot));
        }
    } catch (...) {
        clear();
        return false;
    }
    node_count_ = static_cast<std::uint32_t>(event_capacities.size());
    return true;
}

void GraphRuntimeMidiScratch::clear() noexcept {
    slots_.clear();
    node_count_ = 0;
}

bool GraphRuntimeAutomationScratch::reset(const graph::GraphRuntimePlan& plan,
                                          std::uint32_t max_frames) {
    return reset(plan, {}, max_frames);
}

bool GraphRuntimeAutomationScratch::reset(const graph::GraphRuntimePlan& plan,
                                          std::span<const GraphRuntimeNodeBinding> bindings,
                                          std::uint32_t max_frames) {
    clear();
    const std::uint32_t node_count = plan.node_count();
    const std::uint32_t connection_count = plan.connection_count();
    if (max_frames == 0 || (!bindings.empty() && bindings.size() != node_count))
        return false;
    try {
        events_.reserve(node_count);
        for (std::uint32_t i = 0; i < node_count; ++i) {
            events_.push_back(std::make_unique<state::ParameterEventQueue>());
        }
        slew_last_.assign(connection_count, 0.0f);
        slew_primed_.assign(connection_count, 0);

        // Per-node DENSE (audio-rate) parameter layout: each node's distinct
        // audio-rate param ids in first-seen inbound-connection order, each given
        // a max_frames accumulation region (matching the host walk's per-node
        // audio_rate_param_data). Also count each node's distinct SPARSE
        // (control-rate) params the same way, so both axes get per-node scratch
        // slices sized to the actual count — no on-stack cap on either.
        node_dense_first_.assign(node_count, 0);
        node_dense_count_.assign(node_count, 0);
        node_sparse_first_.assign(node_count, 0);
        node_sparse_count_.assign(node_count, 0);
        std::uint32_t total_dense = 0;
        std::uint32_t total_dense_views = 0;
        std::uint32_t total_sparse = 0;
        for (std::uint32_t n = 0; n < node_count; ++n) {
            const auto& node = plan.nodes[n];
            const bool dense_delivery =
                !bindings.empty() && bindings[n].audio_rate_modulation_delivery ==
                                         AudioRateModulationDelivery::DenseViews;
            node_dense_first_[n] = static_cast<std::uint32_t>(dense_params_.size());
            node_sparse_first_[n] = total_sparse;
            for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
                const auto ci =
                    plan.inbound_connection_indices[node.first_inbound_connection + c];
                const auto& conn = plan.connections[ci];
                if (!graph::is_automation_conn(conn)) continue;
                if (conn.automation.audio_rate) {
                    bool seen = false;
                    for (std::uint32_t k = 0; k < node_dense_count_[n]; ++k) {
                        if (dense_params_[node_dense_first_[n] + k].param_id ==
                            conn.automation.param_id) {
                            seen = true;
                            break;
                        }
                    }
                    if (seen) continue;
                    if ((dense_delivery && (node_dense_count_[n] >= kMaxDenseLanesPerNode ||
                                            total_dense_views >= kMaxDenseLanesPerGraph ||
                                            max_frames > kMaxDenseFrames)) ||
                        total_dense > std::numeric_limits<std::uint32_t>::max() / max_frames) {
                        clear();
                        return false;
                    }
                    dense_params_.push_back({conn.automation.param_id, total_dense * max_frames});
                    ++node_dense_count_[n];
                    ++total_dense;
                    if (dense_delivery)
                        ++total_dense_views;
                } else {
                    bool seen = false;
                    for (std::uint32_t k = 0; k < node_sparse_count_[n]; ++k) {
                        if (sparse_accum_storage_[node_sparse_first_[n] + k].param_id ==
                            conn.automation.param_id) {
                            seen = true;
                            break;
                        }
                    }
                    if (seen) continue;
                    SparseAccum a{};
                    a.param_id = conn.automation.param_id;
                    sparse_accum_storage_.push_back(a);
                    ++node_sparse_count_[n];
                    ++total_sparse;
                }
            }
            constexpr std::size_t capacity = state::ParameterEventQueue::kCapacity;
            const std::size_t dense_count = node_dense_count_[n];
            const std::size_t sparse_count = node_sparse_count_[n];
            if (!dense_delivery && dense_count > capacity / max_frames) {
                clear();
                return false;
            }
            const std::size_t dense_events = dense_delivery ? 0 : dense_count * max_frames;
            if (sparse_count > (capacity - dense_events) / 2) {
                clear();
                return false;
            }
        }
        if (total_dense > 0 &&
            static_cast<std::size_t>(max_frames) >
                std::numeric_limits<std::size_t>::max() / static_cast<std::size_t>(total_dense)) {
            clear();
            return false;
        }
        const std::size_t dense_sample_count = static_cast<std::size_t>(total_dense) * max_frames;
        dense_storage_.assign(dense_sample_count, 0.0f);
        // Transient per-(node,denseparam) gather state, sized to the dense param
        // total (re-zeroed per gather). dense_lo/dense_hi are written before read.
        dense_lo_.assign(total_dense, 0.0f);
        dense_hi_.assign(total_dense, 0.0f);
        dense_replace_.assign(total_dense, 0);
        dense_add_.assign(total_dense, 0);
        dense_touched_.assign(total_dense, 0);
        dense_views_.assign(total_dense, {});
        node_dense_view_count_.assign(node_count, 0);

        prepared_stats_.dense_lane_count = total_dense;
        prepared_stats_.dense_sample_bytes = dense_sample_count * sizeof(float);
        prepared_stats_.dense_descriptor_bytes =
            static_cast<std::size_t>(total_dense) *
            (sizeof(DenseParam) + sizeof(AudioRateModulationView));
        prepared_stats_.dense_accumulator_bytes =
            static_cast<std::size_t>(total_dense) *
            (sizeof(float) * 2 + sizeof(std::uint8_t) * 2 + sizeof(std::uint32_t));
        prepared_stats_.dense_node_index_bytes =
            static_cast<std::size_t>(node_count) * sizeof(std::uint32_t) * 3;
    } catch (...) {
        clear();
        return false;
    }
    node_count_ = node_count;
    connection_count_ = connection_count;
    max_frames_ = max_frames;
    return true;
}

void GraphRuntimeAutomationScratch::clear() noexcept {
    events_.clear();
    slew_last_.clear();
    slew_primed_.clear();
    dense_params_.clear();
    node_dense_first_.clear();
    node_dense_count_.clear();
    dense_storage_.clear();
    dense_lo_.clear();
    dense_hi_.clear();
    dense_replace_.clear();
    dense_add_.clear();
    dense_touched_.clear();
    dense_views_.clear();
    node_dense_view_count_.clear();
    sparse_accum_storage_.clear();
    node_sparse_first_.clear();
    node_sparse_count_.clear();
    node_count_ = 0;
    connection_count_ = 0;
    max_frames_ = 0;
    prepared_stats_ = {};
}

bool GraphRuntimeBufferPool::reset(std::uint32_t slot_count, std::uint32_t max_frames) {
    return reset(slot_count, max_frames, {});
}

GraphRuntimeBufferPool::GraphRuntimeBufferPool(const GraphRuntimeBufferPool& other)
    : storage_(other.storage_),
      slot_count_(other.slot_count_),
      max_frames_(other.max_frames_),
      ring_(other.ring_.size()) {
    for (std::size_t i = 0; i < other.ring_.size(); ++i) {
        ring_[i].delay = other.ring_[i].delay;
        if (other.ring_[i].state) {
            ring_[i].state = std::make_shared<DelayRingState>(*other.ring_[i].state);
        }
    }
}

GraphRuntimeBufferPool& GraphRuntimeBufferPool::operator=(
    const GraphRuntimeBufferPool& other) {
    if (this == &other) return *this;
    GraphRuntimeBufferPool copy(other);
    *this = std::move(copy);
    return *this;
}

bool GraphRuntimeBufferPool::reset(std::uint32_t slot_count, std::uint32_t max_frames,
                                   std::span<const std::uint32_t> connection_delay_samples) {
    unbind();
    if (slot_count > 0 && max_frames == 0) return false;
    try {
        storage_.assign(static_cast<std::size_t>(slot_count) * max_frames, 0.0f);

        // One independently shareable state object per delayed connection. A
        // delay of D needs D + max_frames floats, zero-filled so the leading D
        // samples read silence. The indirection lets a freshly compiled graph
        // adopt the old graph's live state without reading/copying it.
        std::vector<RingSlot> rings;
        if (!connection_delay_samples.empty()) {
            rings.assign(connection_delay_samples.size(), RingSlot{});
            for (std::size_t i = 0; i < connection_delay_samples.size(); ++i) {
                const std::uint32_t delay = connection_delay_samples[i];
                if (delay == 0) continue;
                const std::uint32_t size = delay + max_frames;
                rings[i].delay = delay;
                rings[i].state = std::make_shared<DelayRingState>();
                rings[i].state->storage.assign(size, 0.0f);
            }
        }

        ring_ = std::move(rings);
    } catch (...) {
        clear();
        return false;
    }
    slot_count_ = slot_count;
    max_frames_ = max_frames;
    return true;
}

void GraphRuntimeBufferPool::clear() noexcept {
    unbind();
    storage_.clear();
    slot_count_ = 0;
    max_frames_ = 0;
    ring_.clear();
}

void GraphRuntimeBufferPool::unbind() noexcept {
    bound_snapshot_id_ = 0;
    bound_nodes_.clear();
    bound_inputs_.clear();
    bound_outputs_.clear();
    host_input_aliases_.clear();
    host_input_node_ = 0;
    host_input_channels_ = 0;
    host_input_aliasable_ = false;
}

bool GraphRuntimeBufferPool::bind(const GraphRuntimeSnapshot& snapshot) noexcept {
    using Mode = GraphRuntimeSnapshot::InputPortSource::Mode;
    unbind();
    if (snapshot.id() == 0 || !snapshot.valid() || !fits(snapshot, max_frames_))
        return false;
    const auto& plan = snapshot.plan();
    const auto& assignment = snapshot.buffer_assignment();
    if (assignment.nodes.size() != plan.nodes.size())
        return false;
    std::size_t input_total = 0;
    std::size_t output_total = 0;
    for (const auto& node : plan.nodes) {
        if (node.input_ports > GraphRuntimeExecutor::kMaxRoutedPortsPerNode ||
            node.output_ports > GraphRuntimeExecutor::kMaxRoutedPortsPerNode)
            return false;
        input_total += node.input_ports;
        output_total += node.output_ports;
    }
    try {
        bound_inputs_.assign(input_total, nullptr);
        bound_outputs_.assign(output_total, nullptr);
        bound_nodes_.assign(plan.nodes.size(), BoundNode{});
    } catch (...) {
        unbind();
        return false;
    }
    std::size_t input_at = 0;
    std::size_t output_at = 0;
    for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
        const auto& node = plan.nodes[i];
        const auto& slots = assignment.nodes[i];
        const auto ports = snapshot.input_port_sources(i);
        const bool resolved = ports.size() == node.input_ports;
        auto& bound = bound_nodes_[i];
        bound.inputs = bound_inputs_.data() + input_at;
        bound.outputs = bound_outputs_.data() + output_at;
        input_at += node.input_ports;
        output_at += node.output_ports;
        // The same pointers gather_node_inputs() resolves each block: a port
        // read in place points at its source, every other port at its own slot.
        for (std::uint32_t p = 0; p < node.input_ports; ++p) {
            if (resolved && ports[p].mode == Mode::InPlace) {
                bound.inputs[p] = slot_data(ports[p].slot);
            } else {
                bound.inputs[p] = slot_data(slots.input_base + p);
                bound.gathers = true;
            }
        }
        for (std::uint32_t p = 0; p < node.output_ports; ++p)
            bound.outputs[p] = slot_data(slots.output_base + p);
        bound.context.node_index = i;
        bound.context.routed = true;
    }

    // The host input can stand in for the AudioInput node's slots when that
    // node is the only one and each of its readers is an audio input port that
    // reads it in place, the same block, without writing through it.
    std::uint32_t input_node = 0;
    std::uint32_t input_nodes = 0;
    for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
        if (plan.nodes[i].kind == graph::GraphRuntimeNodeKind::AudioInput) {
            input_node = i;
            ++input_nodes;
        }
    }
    bool aliasable = input_nodes == 1;
    const auto bindings = snapshot.bindings();
    // An empty snapshot is valid and has no buffer assignment.  The alias
    // analysis below only applies when the unique AudioInput node actually
    // exists; do not index the empty assignment while checking that case.
    const auto* input_slots = (input_nodes == 1 && input_node < assignment.nodes.size())
                                  ? &assignment.nodes[input_node]
                                  : nullptr;
    try {
        for (const auto& conn : plan.connections) {
            if (!aliasable)
                break;
            if (conn.source_index != input_node)
                continue;
            // An automation lane reads the AudioInput slot itself.
            if (graph::is_event(conn) || graph::is_automation_conn(conn)) {
                aliasable = false;
                break;
            }
            const auto& dest = plan.nodes[conn.dest_index];
            const auto ports = snapshot.input_port_sources(conn.dest_index);
            const bool read_only = dest.kind == graph::GraphRuntimeNodeKind::AudioOutput ||
                                   bindings[conn.dest_index].reads_inputs_only;
            if (conn.feedback || !read_only || conn.dest_port >= ports.size() ||
                ports[conn.dest_port].mode != Mode::InPlace) {
                aliasable = false;
                break;
            }
            host_input_aliases_.push_back({&bound_nodes_[conn.dest_index].inputs[conn.dest_port],
                                           slot_data(input_slots->output_base + conn.source_port),
                                           conn.source_port});
        }
    } catch (...) {
        unbind();
        return false;
    }
    if (aliasable && input_slots != nullptr) {
        host_input_node_ = input_node;
        host_input_channels_ = plan.nodes[input_node].output_ports;
        host_input_aliasable_ = true;
    } else {
        host_input_aliases_.clear();
    }
    bound_snapshot_id_ = snapshot.id();
    return true;
}

bool GraphRuntimeBufferPool::alias_host_input(const BusBuffer* input, const BusBuffer* output,
                                              std::uint32_t frames) noexcept {
    if (!host_input_aliasable_ || input == nullptr ||
        input->input.num_channels() < host_input_channels_)
        return false;
    // A host that processes in place hands the same memory as input and
    // output; the walk writes the output before every reader has run, so the
    // input then goes through the AudioInput slots as usual.
    if (output != nullptr) {
        const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(float);
        for (std::uint32_t c = 0; c < host_input_channels_; ++c) {
            const auto in_begin = reinterpret_cast<std::uintptr_t>(input->input.channel_ptr(c));
            for (std::size_t k = 0; k < output->output.num_channels(); ++k) {
                const auto out_begin =
                    reinterpret_cast<std::uintptr_t>(output->output.channel_ptr(k));
                if (in_begin < out_begin + bytes && out_begin < in_begin + bytes)
                    return false;
            }
        }
    }
    for (const auto& alias : host_input_aliases_)
        *alias.view = input->input.channel_ptr(alias.channel);
    bound_nodes_[host_input_node_].host_input_aliased = true;
    return true;
}

void GraphRuntimeBufferPool::release_host_input() noexcept {
    if (!host_input_aliasable_ || !bound_nodes_[host_input_node_].host_input_aliased)
        return;
    for (const auto& alias : host_input_aliases_)
        *alias.view = alias.slot;
    bound_nodes_[host_input_node_].host_input_aliased = false;
}

bool GraphRuntimeBufferPool::can_adopt_delay_ring_state(
    std::uint32_t index,
    const GraphRuntimeBufferPool& source,
    std::uint32_t source_index) const noexcept {
    if (index >= ring_.size() || source_index >= source.ring_.size()) return false;
    const auto& dst = ring_[index];
    const auto& src = source.ring_[source_index];
    if (dst.delay == 0 || dst.delay != src.delay || !dst.state || !src.state) {
        return false;
    }
    return dst.state->storage.size() == src.state->storage.size();
}

bool GraphRuntimeBufferPool::adopt_delay_ring_state(
    std::uint32_t index,
    const GraphRuntimeBufferPool& source,
    std::uint32_t source_index) noexcept {
    if (!can_adopt_delay_ring_state(index, source, source_index)) return false;
    ring_[index].state = source.ring_[source_index].state;
    return true;
}

bool GraphRuntimeSnapshot::reset(
    graph::GraphRuntimePlan plan,
    std::span<const GraphRuntimeNodeBinding> bindings,
    bool parallel_safe) {
    clear();
    if (bindings.size() != plan.nodes.size()) return false;
    if (plan.processing_order_indices.size() != plan.nodes.size()) return false;
    for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
        if (bindings[i].node_id != plan.nodes[i].id) return false;
    }
    for (const auto node_index : plan.processing_order_indices) {
        if (node_index >= plan.nodes.size()) return false;
    }

    try {
        std::vector<graph::NodeId> node_ids;
        node_ids.reserve(plan.nodes.size());
        for (const auto& node : plan.nodes) node_ids.push_back(node.id);
        std::sort(node_ids.begin(), node_ids.end());

        // The buffer assignment is a pure function of the plan, so it is built
        // here off the audio thread alongside plan validation. parallel_safe
        // disables slot reuse so concurrent same-level nodes never alias a
        // recycled slot. Per-worker scratch sizing for a parallel executor
        // depends on worker count, not the plan — it belongs to the pool.
        auto assignment =
            graph::build_graph_runtime_buffer_assignment(plan, /*allow_reuse=*/!parallel_safe);
        if (!assignment.ok) {
            clear();
            return false;
        }
        plan_ = std::move(plan);
        bindings_.assign(bindings.begin(), bindings.end());
        node_ids_sorted_ = std::move(node_ids);
        assignment_ = std::move(assignment);
        parallel_safe_ = parallel_safe;
        valid_ = compute_valid();
        sole_audio_output_ = has_sole_audio_output(plan_);
        sole_audio_output_ports_ = 0;
        for (const auto& n : plan_.nodes)
            if (sole_audio_output_ && n.kind == graph::GraphRuntimeNodeKind::AudioOutput)
                sole_audio_output_ports_ = n.input_ports;
        resolve_input_ports();
    } catch (...) {
        clear();
        return false;
    }
    static std::atomic<std::uint64_t> next_id{1};
    id_ = next_id.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void GraphRuntimeSnapshot::resolve_input_ports() {
    using Mode = InputPortSource::Mode;
    input_port_base_.assign(plan_.nodes.size() + 1, 0);
    for (std::size_t n = 0; n < plan_.nodes.size(); ++n)
        input_port_base_[n + 1] = input_port_base_[n] + plan_.nodes[n].input_ports;
    input_ports_.assign(input_port_base_.back(), InputPortSource{});

    // A source port whose every reader only reads (a reads_inputs_only binding,
    // an AudioOutput, an automation tap) can be read in place by all of them,
    // fan-out and persistent sources included: nobody writes the bytes.
    std::vector<std::uint32_t> output_base(plan_.nodes.size() + 1, 0);
    for (std::size_t n = 0; n < plan_.nodes.size(); ++n)
        output_base[n + 1] = output_base[n] + plan_.nodes[n].output_ports;
    std::vector<std::uint8_t> read_only_readers(output_base.back(), 1);
    for (const auto& conn : plan_.connections) {
        if (graph::is_event(conn) || graph::is_automation_conn(conn))
            continue;
        if (conn.source_index >= plan_.nodes.size() || conn.dest_index >= plan_.nodes.size() ||
            conn.source_port >= plan_.nodes[conn.source_index].output_ports)
            continue;
        const bool reads_only =
            plan_.nodes[conn.dest_index].kind == graph::GraphRuntimeNodeKind::AudioOutput ||
            bindings_[conn.dest_index].reads_inputs_only;
        if (!reads_only)
            read_only_readers[output_base[conn.source_index] + conn.source_port] = 0;
    }
    for (std::uint32_t n = 0; n < plan_.nodes.size(); ++n) {
        const auto& node = plan_.nodes[n];
        auto* ports = input_ports_.data() + input_port_base_[n];
        for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
            const auto conn_index =
                plan_.inbound_connection_indices[node.first_inbound_connection + c];
            const auto& conn = plan_.connections[conn_index];
            if (graph::is_event(conn) || graph::is_automation_conn(conn))
                continue;
            if (conn.dest_port >= node.input_ports)
                continue;
            auto& port = ports[conn.dest_port];
            const bool delayed = !conn.feedback &&
                                 conn_index < assignment_.connection_delay_samples.size() &&
                                 assignment_.connection_delay_samples[conn_index] != 0;
            if (port.mode != Mode::Zero || delayed) {
                port.mode = Mode::Sum;
                continue;
            }
            port.slot = conn.feedback
                            ? assignment_.feedback_prev_slot[conn_index]
                            : assignment_.nodes[conn.source_index].output_base + conn.source_port;
            const bool in_place =
                (conn_index < assignment_.reads_in_place.size() &&
                 assignment_.reads_in_place[conn_index] != 0) ||
                (!conn.feedback &&
                 read_only_readers[output_base[conn.source_index] + conn.source_port] != 0);
            port.mode = in_place ? Mode::InPlace : Mode::Copy;
        }
    }
}

void GraphRuntimeSnapshot::clear() noexcept {
    plan_.clear();
    bindings_.clear();
    node_ids_sorted_.clear();
    assignment_ = {};
    parallel_safe_ = false;
    valid_ = true;
    sole_audio_output_ = false;
    sole_audio_output_ports_ = 0;
    input_ports_.clear();
    input_port_base_.clear();
    id_ = 0;
}

bool GraphRuntimeSnapshot::valid() const noexcept {
    return valid_;
}

bool GraphRuntimeSnapshot::compute_valid() const noexcept {
    if (bindings_.size() != plan_.nodes.size()) return false;
    if (node_ids_sorted_.size() != plan_.nodes.size()) return false;
    if (plan_.processing_order_indices.size() != plan_.nodes.size()) return false;
    if (assignment_.nodes.size() != plan_.nodes.size()) return false;
    for (std::uint32_t i = 0; i < plan_.nodes.size(); ++i) {
        if (bindings_[i].node_id != plan_.nodes[i].id) return false;
    }
    for (const auto node_index : plan_.processing_order_indices) {
        if (node_index >= plan_.nodes.size()) return false;
    }
    return true;
}

bool GraphRuntimeSnapshot::contains_node(graph::NodeId node_id) const noexcept {
    return std::binary_search(node_ids_sorted_.begin(), node_ids_sorted_.end(), node_id);
}

bool GraphRuntimeExecutor::drain_commands(
    ProcessBlock& block,
    const GraphRuntimeSnapshot& snapshot,
    std::span<const graph::GraphTimedCommand> commands,
    std::span<GraphRuntimeCommandDecision> command_results,
    GraphRuntimeCommandHandler command_handler,
    GraphRuntimeEventSink event_sink,
    GraphRuntimeExecutorResult& result) noexcept {
    if (command_results.size() < commands.size()) return false;
    // Most blocks carry no commands: nothing to count or report.
    if (commands.empty())
        return true;

    result.commands_drained = static_cast<std::uint32_t>(commands.size());
    commands_drained_.fetch_add(result.commands_drained, std::memory_order_relaxed);
    const auto& plan = snapshot.plan();

    for (const auto& command : commands) {
        auto status = GraphRuntimeCommandStatus::Accepted;
        if (command_requires_node(command.command.type) &&
            !snapshot.contains_node(command.command.node_id)) {
            status = GraphRuntimeCommandStatus::Rejected;
        } else if (command_handler.apply && command.block_offset != 0) {
            status = GraphRuntimeCommandStatus::Rejected;
        } else if (command_handler.apply) {
            status = command_handler.apply(block, plan, command, command_handler.user_data);
        }

        const auto accepted = status == GraphRuntimeCommandStatus::Accepted;
        if (accepted) {
            ++result.commands_accepted;
            commands_accepted_.fetch_add(1, std::memory_order_relaxed);
        } else {
            ++result.commands_rejected;
            commands_rejected_.fetch_add(1, std::memory_order_relaxed);
        }

        command_results[result.commands_accepted + result.commands_rejected - 1] = {
            command,
            status,
        };

        const auto event_type = accepted ? graph::GraphEventType::CommandAccepted
                                         : graph::GraphEventType::CommandRejected;
        if (!event_sink.push(command_event(command, event_type))) {
            ++result.events_dropped;
            events_dropped_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return true;
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::process(
    ProcessBlock& block,
    const GraphRuntimeSnapshot& snapshot,
    std::span<const graph::GraphTimedCommand> commands,
    std::span<GraphRuntimeCommandDecision> command_results,
    GraphRuntimeCommandHandler command_handler,
    GraphRuntimeEventSink event_sink) noexcept {
    if (!block.validate()) return fail_invalid_block();
    if (!snapshot.valid()) return fail_invalid_snapshot();

    const auto& plan = snapshot.plan();
    const auto bindings = snapshot.bindings();

    GraphRuntimeExecutorResult result;
    if (!drain_commands(block, snapshot, commands, command_results, command_handler,
                        event_sink, result)) {
        return fail_command_scratch_too_small();
    }

    for (const auto node_index : plan.processing_order_indices) {
        if (node_index >= bindings.size()) return fail_invalid_snapshot();

        const auto& binding = bindings[node_index];
        if (!binding.process) {
            if (binding.required) {
                result.error = GraphRuntimeExecutorErrorCode::MissingRequiredProcessor;
                result.failed_node_index = node_index;
                node_failures_.fetch_add(1, std::memory_order_relaxed);
                return result;
            }
            continue;
        }

        GraphRuntimeNodeProcessContext context;
        context.plan = &plan;
        context.node = &plan.nodes[node_index];
        context.node_index = node_index;
        context.command_results = command_results.first(commands.size());
        if (!binding.process(block, context, binding.user_data)) {
            result.error = GraphRuntimeExecutorErrorCode::NodeProcessorFailed;
            result.failed_node_index = node_index;
            node_failures_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        ++result.nodes_processed;
        nodes_processed_.fetch_add(1, std::memory_order_relaxed);
    }

    blocks_processed_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::process_routed(
    ProcessBlock& block,
    const GraphRuntimeSnapshot& snapshot,
    GraphRuntimeBufferPool& pool,
    GraphRuntimeMidiScratch* midi,
    GraphRuntimeAutomationScratch* automation,
    std::span<const graph::GraphTimedCommand> commands,
    std::span<GraphRuntimeCommandDecision> command_results,
    GraphRuntimeCommandHandler command_handler,
    GraphRuntimeEventSink event_sink,
    std::span<const std::uint8_t> skip_mask) noexcept {
    if (!block.validate()) return fail_invalid_block();
    if (!snapshot.valid()) return fail_invalid_snapshot();

    const auto& plan = snapshot.plan();
    const auto bindings = snapshot.bindings();
    const auto& assignment = snapshot.buffer_assignment();
    const auto frames = block.frame_count;

    if (!pool.fits(snapshot, frames)) {
        GraphRuntimeExecutorResult fail;
        fail.error = GraphRuntimeExecutorErrorCode::BufferPoolTooSmall;
        return fail;
    }
    // An automation scratch, when supplied, must cover every node + connection so
    // the gather can index any node's queue and any connection's slew state.
    if (automation != nullptr &&
        !automation->fits(plan.node_count(), plan.connection_count(), frames)) {
        GraphRuntimeExecutorResult fail;
        fail.error = GraphRuntimeExecutorErrorCode::BufferPoolTooSmall;
        return fail;
    }
    // A MIDI scratch, when supplied, must cover every node so the gather can
    // index any node's in/out buffer; an undersized one fails closed rather than
    // routing MIDI past the end of the scratch.
    if (midi != nullptr && !midi->fits(plan.node_count())) {
        GraphRuntimeExecutorResult fail;
        fail.error = GraphRuntimeExecutorErrorCode::BufferPoolTooSmall;
        return fail;
    }

    GraphRuntimeExecutorResult result;
    if (!drain_commands(block, snapshot, commands, command_results, command_handler,
                        event_sink, result)) {
        return fail_command_scratch_too_small();
    }

    const BusBuffer* in_bus =
        block.buses ? block.buses->first(BusDirection::Input, BusRole::Main) : nullptr;
    BusBuffer* out_bus =
        block.buses ? block.buses->first(BusDirection::Output, BusRole::Main) : nullptr;

    const bool bound = pool.bound_to(snapshot);
    // A bound pool reads the host input in place for this block when the graph
    // allows it, undone on every return below. This must happen before clearing
    // the output bus: host input and output may intentionally alias.
    struct HostInputRelease {
        GraphRuntimeBufferPool* pool;
        ~HostInputRelease() {
            if (pool != nullptr)
                pool->release_host_input();
        }
    } host_input_release{
        bound && skip_mask.empty() && pool.alias_host_input(in_bus, out_bus, frames) ? &pool
                                                                                     : nullptr};
    // An overlapping host bus may still be safe to defer when an AudioInput
    // node will run and clear it after copying.  If aliasing was disabled by a
    // skip mask, only an unskipped AudioInput can perform that clear.
    bool can_defer_output_clear = false;
    for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
        if (plan.nodes[i].kind == graph::GraphRuntimeNodeKind::AudioInput &&
            (skip_mask.empty() || i >= skip_mask.size() || skip_mask[i] == 0)) {
            can_defer_output_clear = true;
            break;
        }
    }

    // Zero the main output bus once; AudioOutput nodes accumulate into it, so N
    // output nodes mix (and any output channel no node drives stays silent),
    // matching the host graph this executor mirrors. A sole AudioOutput node
    // writes its channels instead of adding to them, so those are left for it
    // and are zeroed only if the block fails before it runs. With a skip mask
    // the whole bus is zeroed up front.
    const bool sole_output = snapshot.sole_audio_output();
    std::size_t sole_output_channels = 0;
    bool defer_output_clear = false;
    if (out_bus != nullptr) {
        const std::size_t channels = out_bus->output.num_channels();
        if (sole_output && skip_mask.empty())
            sole_output_channels =
                std::min<std::size_t>(channels, snapshot.sole_audio_output_ports());
        if (can_defer_output_clear && in_bus != nullptr) {
            const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(float);
            for (std::size_t c = 0; c < in_bus->input.num_channels() && !defer_output_clear; ++c) {
                const auto in_begin =
                    reinterpret_cast<std::uintptr_t>(in_bus->input.channel_ptr(c));
                for (std::size_t k = 0; k < channels; ++k) {
                    const auto out_begin =
                        reinterpret_cast<std::uintptr_t>(out_bus->output.channel_ptr(k));
                    if (in_begin < out_begin + bytes && out_begin < in_begin + bytes) {
                        defer_output_clear = true;
                        break;
                    }
                }
            }
        }
        if (!defer_output_clear) {
            for (std::size_t c = sole_output_channels; c < channels; ++c)
                std::fill_n(out_bus->output.channel_ptr(c), frames, 0.0f);
        }
    }
    const auto zero_unwritten_sole_output = [&]() noexcept {
        for (std::size_t c = 0; c < sole_output_channels; ++c)
            std::fill_n(out_bus->output.channel_ptr(c), frames, 0.0f);
    };

#ifndef NDEBUG
    // Contract for a skip_mask (caller-enforced; assert it so a future caller
    // can't regress into silent-wrong audio): a masked node must be neither an
    // AudioOutput (skipping it drops its += into the shared output bus, which no
    // pool pre-fill can restore) nor a feedback endpoint (its pre-filled slot would
    // feed stale history into capture_feedback / the next block). The anticipation
    // interior satisfies this — live sinks are never interior and 6a excludes
    // feedback endpoints.
    if (!skip_mask.empty()) {
        for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
            if (i < skip_mask.size() && skip_mask[i] != 0) {
                assert(plan.nodes[i].kind != graph::GraphRuntimeNodeKind::AudioOutput &&
                       "process_routed: a skipped node must not be an AudioOutput");
            }
        }
        for (const auto& c : plan.connections) {
            if (!c.feedback) continue;
            const bool src_skipped =
                c.source_index < skip_mask.size() && skip_mask[c.source_index] != 0;
            const bool dst_skipped =
                c.dest_index < skip_mask.size() && skip_mask[c.dest_index] != 0;
            assert(!src_skipped && !dst_skipped &&
                   "process_routed: a skipped node must not be a feedback endpoint");
        }
    }
#endif

    const auto command_decisions = command_results.first(commands.size());
    const bool time_nodes = node_load_timing();
    for (const auto node_index : plan.processing_order_indices) {
        if (node_index >= bindings.size()) {
            zero_unwritten_sole_output();
            return fail_invalid_snapshot();
        }

        // Skipped nodes are not run: the caller has pre-filled their output slots
        // (anticipative rendering pre-renders the interior off-thread). They are
        // not counted as processed and their plugin state is left untouched.
        if (!skip_mask.empty() && node_index < skip_mask.size() &&
            skip_mask[node_index] != 0) {
            continue;
        }

        const auto error =
            run_routed_node(node_index, plan, bindings, assignment, pool, midi, automation, block,
                            in_bus, out_bus, command_decisions, frames, time_nodes, sole_output,
                            snapshot.input_port_sources(node_index),
                            bound ? &pool.bound_node(node_index) : nullptr);
        if (error != GraphRuntimeExecutorErrorCode::None) {
            zero_unwritten_sole_output();
            result.error = error;
            result.failed_node_index = node_index;
            node_failures_.fetch_add(1, std::memory_order_relaxed);
            nodes_processed_.fetch_add(result.nodes_processed, std::memory_order_relaxed);
            return result;
        }
        if (plan.nodes[node_index].kind == graph::GraphRuntimeNodeKind::AudioOutput)
            sole_output_channels = 0;
        if (defer_output_clear &&
            plan.nodes[node_index].kind == graph::GraphRuntimeNodeKind::AudioInput &&
            out_bus != nullptr) {
            for (std::size_t c = sole_output_channels; c < out_bus->output.num_channels(); ++c)
                std::fill_n(out_bus->output.channel_ptr(c), frames, 0.0f);
            defer_output_clear = false;
        }
        ++result.nodes_processed;
    }
    // One shared-counter update per block rather than one per node.
    nodes_processed_.fetch_add(result.nodes_processed, std::memory_order_relaxed);

    capture_feedback(plan, assignment, pool, frames);

    blocks_processed_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

namespace {

// Worker-pool task context for one level's parallel nodes. Holds the shared
// (read-mostly) walk state plus this level's node-index slice and an error sink.
struct ParallelLevelTask {
    const graph::GraphRuntimePlan* plan;
    std::span<const GraphRuntimeNodeBinding> bindings;
    const graph::GraphRuntimeBufferAssignment* assignment;
    GraphRuntimeBufferPool* pool;
    GraphRuntimeMidiScratch* midi;
    GraphRuntimeAutomationScratch* automation;
    ProcessBlock* block;
    const BusBuffer* in_bus;
    BusBuffer* out_bus;
    std::span<const GraphRuntimeCommandDecision> command_results;
    std::uint32_t frames;
    bool time_nodes;
    bool sole_output;
    const std::uint32_t* level_nodes;          // this level's node indices
    std::atomic<std::uint32_t>* error_code;    // 0 = None, else (code + 1)
    std::atomic<std::uint32_t>* failed_node;
    const GraphRuntimeSnapshot* snapshot;
    bool bound; // pool.bound_to(*snapshot)
};

void run_level_node(void* ctx, std::uint32_t i) noexcept {
    auto* t = static_cast<ParallelLevelTask*>(ctx);
    const std::uint32_t node_index = t->level_nodes[i];
    const auto e = run_routed_node(node_index, *t->plan, t->bindings, *t->assignment, *t->pool,
                                   t->midi, t->automation, *t->block, t->in_bus, t->out_bus,
                                   t->command_results, t->frames, t->time_nodes, t->sole_output,
                                   t->snapshot->input_port_sources(node_index),
                                   t->bound ? &t->pool->bound_node(node_index) : nullptr);
    if (e != GraphRuntimeExecutorErrorCode::None) {
        std::uint32_t expected = 0;
        if (t->error_code->compare_exchange_strong(
                expected, static_cast<std::uint32_t>(e) + 1,
                std::memory_order_relaxed)) {
            t->failed_node->store(node_index, std::memory_order_relaxed);
        }
    }
}

} // namespace

GraphRuntimeExecutorResult GraphRuntimeExecutor::process_parallel(
    ProcessBlock& block,
    const GraphRuntimeSnapshot& snapshot,
    const graph::GraphRuntimeLevelization& levels,
    GraphRuntimeBufferPool& pool,
    GraphRuntimeWorkerPool& workers,
    GraphRuntimeMidiScratch* midi,
    GraphRuntimeAutomationScratch* automation,
    std::span<const graph::GraphTimedCommand> commands,
    std::span<GraphRuntimeCommandDecision> command_results,
    GraphRuntimeCommandHandler command_handler,
    GraphRuntimeEventSink event_sink) noexcept {
    if (!block.validate()) return fail_invalid_block();
    if (!snapshot.valid()) return fail_invalid_snapshot();

    const auto& plan = snapshot.plan();
    const auto bindings = snapshot.bindings();
    const auto& assignment = snapshot.buffer_assignment();
    const auto frames = block.frame_count;

    // The levelization must describe THIS plan (a stale one would mis-schedule),
    // and the snapshot must use the reuse-free layout — a serial (reused) layout
    // would let concurrent same-level nodes alias a recycled slot and race.
    if (!levels.ok || levels.node_level.size() != plan.nodes.size() ||
        !snapshot.parallel_safe()) {
        return fail_invalid_snapshot();
    }
    if (!pool.fits(snapshot, frames) ||
        (automation != nullptr &&
         !automation->fits(plan.node_count(), plan.connection_count(), frames)) ||
        (midi != nullptr && !midi->fits(plan.node_count()))) {
        GraphRuntimeExecutorResult fail;
        fail.error = GraphRuntimeExecutorErrorCode::BufferPoolTooSmall;
        return fail;
    }

    GraphRuntimeExecutorResult result;
    if (!drain_commands(block, snapshot, commands, command_results, command_handler,
                        event_sink, result)) {
        return fail_command_scratch_too_small();
    }

    const BusBuffer* in_bus =
        block.buses ? block.buses->first(BusDirection::Input, BusRole::Main) : nullptr;
    BusBuffer* out_bus =
        block.buses ? block.buses->first(BusDirection::Output, BusRole::Main) : nullptr;
    const bool bound = pool.bound_to(snapshot);
    struct HostInputRelease {
        GraphRuntimeBufferPool* pool;
        ~HostInputRelease() {
            if (pool != nullptr)
                pool->release_host_input();
        }
    } host_input_release{bound && pool.alias_host_input(in_bus, out_bus, frames) ? &pool : nullptr};
    bool can_defer_output_clear = false;
    for (const auto& node : plan.nodes) {
        if (node.kind == graph::GraphRuntimeNodeKind::AudioInput) {
            can_defer_output_clear = true;
            break;
        }
    }
    bool defer_output_clear = false;
    if (out_bus != nullptr) {
        if (can_defer_output_clear && in_bus != nullptr) {
            const auto bytes = static_cast<std::uintptr_t>(frames) * sizeof(float);
            for (std::size_t c = 0; c < in_bus->input.num_channels() && !defer_output_clear; ++c) {
                const auto in_begin =
                    reinterpret_cast<std::uintptr_t>(in_bus->input.channel_ptr(c));
                for (std::size_t k = 0; k < out_bus->output.num_channels(); ++k) {
                    const auto out_begin =
                        reinterpret_cast<std::uintptr_t>(out_bus->output.channel_ptr(k));
                    if (in_begin < out_begin + bytes && out_begin < in_begin + bytes) {
                        defer_output_clear = true;
                        break;
                    }
                }
            }
        }
        if (!defer_output_clear) {
            for (std::size_t c = 0; c < out_bus->output.num_channels(); ++c)
                std::fill_n(out_bus->output.channel_ptr(c), frames, 0.0f);
        }
    }

    const auto command_decisions = command_results.first(commands.size());
    const bool time_nodes = node_load_timing();
    const bool sole_output = snapshot.sole_audio_output();
    std::atomic<std::uint32_t> error_code{0};
    std::atomic<std::uint32_t> failed_node{0};

    auto report_failure = [&](GraphRuntimeExecutorErrorCode e,
                              std::uint32_t node) -> GraphRuntimeExecutorResult {
        result.error = e;
        result.failed_node_index = node;
        node_failures_.fetch_add(1, std::memory_order_relaxed);
        return result;
    };

    for (std::uint32_t level = 0; level < levels.level_count; ++level) {
        const std::uint32_t base = levels.level_offsets[level];
        const std::uint32_t width = levels.level_offsets[level + 1] - base;
        if (width == 0) continue;

        // A level runs serially when it can't safely parallelize: a single node
        // (no benefit), no worker threads, or it contains an AudioOutput node
        // (which accumulates into the shared output bus — see the header).
        bool serial = width == 1 || workers.worker_count() <= 1;
        for (std::uint32_t k = 0; !serial && k < width; ++k) {
            if (plan.nodes[levels.level_nodes[base + k]].kind ==
                graph::GraphRuntimeNodeKind::AudioOutput) {
                serial = true;
            }
        }
        // Cost gate: skip the fork/join when the level's static work-weight x this
        // block's frame count is below the break-even threshold. level_work_weight
        // is precomputed off-RT (per the levelization); the audio-thread check is a
        // single multiply-compare. A zero threshold parallelizes every eligible
        // level. (Empty level_work_weight — a plan with no precomputed weights —
        // leaves `serial` as-is rather than reading out of bounds.)
        if (!serial && level < levels.level_work_weight.size()) {
            const std::uint64_t work =
                levels.level_work_weight[level] * static_cast<std::uint64_t>(frames);
            if (work < parallel_min_work_units_.load(std::memory_order_relaxed)) {
                serial = true;
            }
        }
        if (serial) {
            serial_levels_run_.fetch_add(1, std::memory_order_relaxed);
        } else {
            parallel_levels_dispatched_.fetch_add(1, std::memory_order_relaxed);
        }

        if (serial) {
            for (std::uint32_t k = 0; k < width; ++k) {
                const std::uint32_t node_index = levels.level_nodes[base + k];
                const auto e =
                    run_routed_node(node_index, plan, bindings, assignment, pool, midi, automation,
                                    block, in_bus, out_bus, command_decisions, frames, time_nodes,
                                    sole_output, snapshot.input_port_sources(node_index),
                                    bound ? &pool.bound_node(node_index) : nullptr);
                if (e != GraphRuntimeExecutorErrorCode::None) {
                    return report_failure(e, node_index);
                }
            }
        } else {
            ParallelLevelTask task{
                &plan,       bindings,
                &assignment, &pool,
                midi,        automation,
                &block,      in_bus,
                out_bus,     command_decisions,
                frames,      time_nodes,
                sole_output, levels.level_nodes.data() + base,
                &error_code, &failed_node,
                &snapshot,   bound,
            };
            workers.run(width, run_level_node, &task);
            if (const std::uint32_t code = error_code.load(std::memory_order_relaxed);
                code != 0) {
                return report_failure(
                    static_cast<GraphRuntimeExecutorErrorCode>(code - 1),
                    failed_node.load(std::memory_order_relaxed));
            }
        }

        if (defer_output_clear) {
            bool has_audio_input = false;
            for (std::uint32_t k = 0; k < width; ++k) {
                if (plan.nodes[levels.level_nodes[base + k]].kind ==
                    graph::GraphRuntimeNodeKind::AudioInput) {
                    has_audio_input = true;
                    break;
                }
            }
            if (has_audio_input && out_bus != nullptr) {
                for (std::size_t c = 0; c < out_bus->output.num_channels(); ++c)
                    std::fill_n(out_bus->output.channel_ptr(c), frames, 0.0f);
                defer_output_clear = false;
            }
        }

        result.nodes_processed += width;
        nodes_processed_.fetch_add(width, std::memory_order_relaxed);
    }

    capture_feedback(plan, assignment, pool, frames);

    blocks_processed_.fetch_add(1, std::memory_order_relaxed);
    return result;
}

GraphRuntimeExecutorStats GraphRuntimeExecutor::stats() const noexcept {
    return {
        blocks_processed_.load(std::memory_order_relaxed),
        nodes_processed_.load(std::memory_order_relaxed),
        commands_drained_.load(std::memory_order_relaxed),
        commands_accepted_.load(std::memory_order_relaxed),
        commands_rejected_.load(std::memory_order_relaxed),
        events_dropped_.load(std::memory_order_relaxed),
        invalid_blocks_.load(std::memory_order_relaxed),
        invalid_snapshots_.load(std::memory_order_relaxed),
        command_scratch_failures_.load(std::memory_order_relaxed),
        node_failures_.load(std::memory_order_relaxed),
        parallel_levels_dispatched_.load(std::memory_order_relaxed),
        serial_levels_run_.load(std::memory_order_relaxed),
    };
}

void GraphRuntimeExecutor::reset_stats() noexcept {
    blocks_processed_.store(0, std::memory_order_relaxed);
    nodes_processed_.store(0, std::memory_order_relaxed);
    commands_drained_.store(0, std::memory_order_relaxed);
    commands_accepted_.store(0, std::memory_order_relaxed);
    commands_rejected_.store(0, std::memory_order_relaxed);
    events_dropped_.store(0, std::memory_order_relaxed);
    invalid_blocks_.store(0, std::memory_order_relaxed);
    invalid_snapshots_.store(0, std::memory_order_relaxed);
    command_scratch_failures_.store(0, std::memory_order_relaxed);
    node_failures_.store(0, std::memory_order_relaxed);
    parallel_levels_dispatched_.store(0, std::memory_order_relaxed);
    serial_levels_run_.store(0, std::memory_order_relaxed);
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::fail_invalid_block() noexcept {
    invalid_blocks_.fetch_add(1, std::memory_order_relaxed);
    GraphRuntimeExecutorResult result;
    result.error = GraphRuntimeExecutorErrorCode::InvalidProcessBlock;
    return result;
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::fail_invalid_snapshot() noexcept {
    invalid_snapshots_.fetch_add(1, std::memory_order_relaxed);
    GraphRuntimeExecutorResult result;
    result.error = GraphRuntimeExecutorErrorCode::InvalidSnapshot;
    return result;
}

GraphRuntimeExecutorResult GraphRuntimeExecutor::fail_command_scratch_too_small() noexcept {
    command_scratch_failures_.fetch_add(1, std::memory_order_relaxed);
    GraphRuntimeExecutorResult result;
    result.error = GraphRuntimeExecutorErrorCode::CommandScratchTooSmall;
    return result;
}

} // namespace pulp::format
