#include <pulp/format/graph_runtime_executor.hpp>

#include <algorithm>
#include <array>
#include <cassert>
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

bool contains_node(const graph::GraphRuntimePlan& plan,
                   graph::NodeId node_id) noexcept {
    return std::any_of(plan.nodes.begin(), plan.nodes.end(),
                       [node_id](const graph::GraphRuntimeNodePlan& node) {
                           return node.id == node_id;
                       });
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

// Zero each input slot (unconnected ports stay silent) then sum every inbound
// audio connection into the dest input slot. Feedforward edges read the
// upstream output slot (topological order guarantees it is already filled this
// block); feedback edges read their dedicated previous-block slot (last block's
// captured source output), matching SignalGraph's one-block feedback_prev. Event
// edges carry no audio and are skipped.
//
// A feedforward edge whose source is less latent than the destination's
// most-latent input is delayed through a per-connection ring (sized by the
// snapshot's buffer assignment, stored in the pool) so fan-in paths time-align,
// matching the host graph's per-connection delay lines. Edges with zero delay
// take the plain sum path.
void gather_node_inputs(const graph::GraphRuntimePlan& plan,
                        const graph::GraphRuntimeBufferAssignment& assignment,
                        GraphRuntimeBufferPool& pool,
                        const graph::GraphRuntimeNodePlan& node,
                        const graph::GraphRuntimeNodeSlots& slots,
                        std::uint32_t frames) noexcept {
    for (std::uint32_t p = 0; p < node.input_ports; ++p) {
        if (float* dst = pool.slot_data(slots.input_base + p)) {
            std::fill_n(dst, frames, 0.0f);
        }
    }
    for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
        const auto conn_index =
            plan.inbound_connection_indices[node.first_inbound_connection + c];
        const auto& conn = plan.connections[conn_index];
        // Event (MIDI) and automation edges carry no audio into an input port.
        if (conn.event || conn.is_automation) continue;
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

// Clear a MIDI buffer's events, sysex, and attached UMP — the full reset the
// host walk applies before gathering a node's inbound MIDI.
void clear_routed_midi(midi::MidiBuffer& block) noexcept {
    block.clear();
    block.clear_sysex();
    if (auto* ump = block.ump()) ump->clear();
}

// True if a buffer dropped any event / sysex / UMP message (capacity limit hit),
// matching the host walk's drop check.
bool routed_midi_has_drops(const midi::MidiBuffer& block) noexcept {
    if (block.dropped_event_count() > 0 || block.dropped_sysex_count() > 0) return true;
    const auto* ump = block.ump();
    return ump != nullptr && ump->dropped_event_count() > 0;
}

// Append every event / sysex / UMP message from `src` to `dst`, the same
// whole-buffer copy the host walk uses to gather an inbound MIDI edge. Returns
// false if `src` already carried a drop or an add() dropped here (the
// incompleteness the host propagates downstream). RT-safe when both buffers are
// reserved (add() respects the realtime capacity limit).
bool copy_routed_midi(const midi::MidiBuffer& src, midi::MidiBuffer& dst) noexcept {
    bool copied_all = !routed_midi_has_drops(src);
    for (const auto& ev : src) {
        if (!dst.add(ev)) copied_all = false;
    }
    for (const auto& sx : src.sysex()) {
        if (sx.data.empty()) {
            if (!dst.add_sysex({}, sx.sample_offset, sx.timestamp)) copied_all = false;
        } else if (!dst.add_sysex_copy(sx.data.data(), sx.data.size(), sx.sample_offset,
                                       sx.timestamp)) {
            copied_all = false;
        }
    }
    const auto* src_ump = src.ump();
    auto* dst_ump = dst.ump();
    if (src_ump != nullptr && dst_ump != nullptr) {
        for (const auto& ev : *src_ump) {
            if (!dst_ump->add(ev)) copied_all = false;
        }
    } else if (src_ump != nullptr && !src_ump->empty()) {
        copied_all = false;
    }
    return copied_all;
}

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
    clear_routed_midi(*in);
    bool incomplete = false;
    for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
        const auto conn_index =
            plan.inbound_connection_indices[node.first_inbound_connection + c];
        const auto& conn = plan.connections[conn_index];
        if (!conn.event) continue;
        const midi::MidiBuffer* src = midi.out(conn.source_index);
        if (src == nullptr) continue;
        // A source whose output was incomplete, or a copy that drops here,
        // marks this node's input incomplete (matching the host walk's
        // node-to-node incompleteness propagation).
        if (midi.out_incomplete(conn.source_index)) incomplete = true;
        if (!copy_routed_midi(*src, *in)) incomplete = true;
    }
    midi.set_in_incomplete(node_index, incomplete);
}

// Upper bound on distinct automated parameters per node for the on-stack
// accumulator. The caller (eligibility check) rejects any graph that exceeds
// this on a node and keeps it on the legacy walk, so the `continue` below is
// a defense-in-depth backstop, never hit for an eligible graph.
constexpr std::uint32_t kMaxAutomatedParamsPerNode =
    GraphRuntimeAutomationScratch::kMaxParamsPerNode;

// Build this node's parameter-automation events from its inbound SPARSE
// automation connections (audio-rate edges are handled elsewhere). Each edge
// samples its source's audio output at sample 0 and N-1, maps into the
// parameter range, applies per-source linear slew (state persisted in the
// scratch), accumulates per parameter by mix mode, then emits two control points
// per touched parameter — replicating the host walk's sparse-automation math.
void gather_node_automation(const graph::GraphRuntimePlan& plan,
                            const graph::GraphRuntimeBufferAssignment& assignment,
                            GraphRuntimeBufferPool& pool,
                            GraphRuntimeAutomationScratch& automation,
                            const graph::GraphRuntimeNodePlan& node,
                            std::uint32_t node_index,
                            std::uint32_t frames,
                            double sample_rate) noexcept {
    state::ParameterEventQueue* queue = automation.events(node_index);
    if (queue == nullptr) return;
    queue->clear();
    const int last = static_cast<int>(frames) - 1;

    struct Accum {
        std::uint32_t param_id;
        float v0;
        float vN;
        float lo;
        float hi;
        bool has_add;
    };
    std::array<Accum, kMaxAutomatedParamsPerNode> accum{};
    std::uint32_t param_count = 0;

    for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
        const auto conn_index =
            plan.inbound_connection_indices[node.first_inbound_connection + c];
        const auto& conn = plan.connections[conn_index];
        if (!conn.is_automation || conn.automation.audio_rate) continue;
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
            if (param_count >= kMaxAutomatedParamsPerNode) continue;
            accum[pi] = Accum{a.param_id, 0.0f, 0.0f, a.bounds_lo, a.bounds_hi, false};
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
    std::array<bool, kMaxAutomatedParamsPerNode> dense_replace{};
    std::array<bool, kMaxAutomatedParamsPerNode> dense_add{};
    std::array<float, kMaxAutomatedParamsPerNode> dense_lo{};
    std::array<float, kMaxAutomatedParamsPerNode> dense_hi{};
    for (std::uint32_t i = 0; i < dense_count; ++i) {
        std::fill_n(automation.dense_buffer(node_index, i), frames, 0.0f);
    }
    for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
        const auto conn_index =
            plan.inbound_connection_indices[node.first_inbound_connection + c];
        const auto& conn = plan.connections[conn_index];
        if (!conn.is_automation || !conn.automation.audio_rate) continue;
        std::uint32_t pi = 0;
        for (; pi < dense_count; ++pi) {
            if (automation.dense_param_id(node_index, pi) == conn.automation.param_id) break;
        }
        if (pi == dense_count) continue;
        const auto& src_slots = assignment.nodes[conn.source_index];
        const float* src = pool.slot_data(src_slots.output_base + conn.source_port);
        if (src == nullptr) continue;
        const auto& a = conn.automation;
        dense_lo[pi] = a.bounds_lo;
        dense_hi[pi] = a.bounds_hi;
        float* dst = automation.dense_buffer(node_index, pi);
        const auto ring = pool.delay_ring(conn_index);
        if (ring.data == nullptr || ring.delay == 0) {
            for (std::uint32_t f = 0; f < frames; ++f) {
                const float v = a.range_lo +
                    std::clamp(src[f], 0.0f, 1.0f) * (a.range_hi - a.range_lo);
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
                const float v = a.range_lo +
                    std::clamp(ring.data[rp], 0.0f, 1.0f) * (a.range_hi - a.range_lo);
                if (a.mix_add) { dst[f] += v; dense_add[pi] = true; }
                else { dst[f] = v; dense_replace[pi] = true; }
                if (++wp == ring_size) wp = 0;
                if (++rp == ring_size) rp = 0;
            }
            *ring.write_pos = wp;
        }
    }
    for (std::uint32_t i = 0; i < dense_count; ++i) {
        if (!dense_replace[i] && !dense_add[i]) continue;
        const std::uint32_t pid = automation.dense_param_id(node_index, i);
        const float* vals = automation.dense_buffer(node_index, i);
        const float lo = std::min(dense_lo[i], dense_hi[i]);
        const float hi = std::max(dense_lo[i], dense_hi[i]);
        for (std::uint32_t f = 0; f < frames; ++f) {
            float v = vals[f];
            if (dense_add[i]) v = std::clamp(v, lo, hi);
            if (!queue->push({pid, static_cast<std::int32_t>(f), v, 0})) break;
        }
    }
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
// ACCUMULATES its gathered input slots into the main output bus. The only
// format/bus-aware step in routing.
//
// AudioOutput accumulates (+=), not overwrites, because the host graph it
// mirrors (SignalGraph) zeroes the output bus once per block and lets every
// AudioOutput node sum into it — so N output nodes mix rather than last-writer-
// wins. process_routed zeroes the main output bus once before the walk to match.
void copy_io_bus(graph::GraphRuntimeNodeKind kind,
                 GraphRuntimeBufferPool& pool,
                 const graph::GraphRuntimeNodePlan& node,
                 const graph::GraphRuntimeNodeSlots& slots,
                 const BusBuffer* in_bus, BusBuffer* out_bus,
                 std::uint32_t frames) noexcept {
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
            const float* src = pool.slot_data(slots.input_base + p);
            if (src == nullptr) continue;
            float* dst = out_bus->output.channel_ptr(p);
            for (std::uint32_t f = 0; f < frames; ++f) dst[f] += src[f];
        }
    }
}

// Fixed realtime MIDI capacities for the routed per-node scratch buffers. Match
// the host graph's per-node MIDI block storage so the routed path never drops an
// event the legacy walk would have kept.
constexpr std::size_t kRoutedMidiEventCapacity = 1024;
constexpr std::size_t kRoutedMidiSysexCapacity = 128;
constexpr std::size_t kRoutedMidiSysexPayloadCapacity = 4096;

// Run ONE node of the routing walk: gather its audio / MIDI / automation inputs,
// then either copy the I/O bus (AudioInput/AudioOutput) or invoke its binding.
// Returns the error code (None on success). This is the per-node seam shared by
// the serial walk and the parallel (levelized) walk, so both produce identical
// output by construction. It touches only this node's own scratch (disjoint
// output slots, per-node MIDI/automation buffers, per-connection inbound rings),
// so distinct nodes may run concurrently — EXCEPT AudioOutput, which accumulates
// into the shared output bus and must be run serially by the caller.
GraphRuntimeExecutorErrorCode run_routed_node(
    std::uint32_t node_index,
    const graph::GraphRuntimePlan& plan,
    std::span<const GraphRuntimeNodeBinding> bindings,
    const graph::GraphRuntimeBufferAssignment& assignment,
    GraphRuntimeBufferPool& pool,
    GraphRuntimeMidiScratch* midi,
    GraphRuntimeAutomationScratch* automation,
    ProcessBlock& block,
    const BusBuffer* in_bus,
    BusBuffer* out_bus,
    std::span<const GraphRuntimeCommandDecision> command_results,
    std::uint32_t frames) noexcept {
    const auto& node = plan.nodes[node_index];
    const auto& slots = assignment.nodes[node_index];

    if (node.input_ports > GraphRuntimeExecutor::kMaxRoutedPortsPerNode ||
        node.output_ports > GraphRuntimeExecutor::kMaxRoutedPortsPerNode) {
        return GraphRuntimeExecutorErrorCode::NodePortLimitExceeded;
    }

    gather_node_inputs(plan, assignment, pool, node, slots, frames);
    if (midi != nullptr) {
        gather_node_midi(plan, *midi, node, node_index);
    }
    if (automation != nullptr) {
        gather_node_automation(plan, assignment, pool, *automation, node, node_index,
                               frames, block.sample_rate);
    }

    if (node.kind == graph::GraphRuntimeNodeKind::AudioInput ||
        node.kind == graph::GraphRuntimeNodeKind::AudioOutput) {
        copy_io_bus(node.kind, pool, node, slots, in_bus, out_bus, frames);
        return GraphRuntimeExecutorErrorCode::None;
    }

    const auto& binding = bindings[node_index];
    if (!binding.process) {
        return binding.required ? GraphRuntimeExecutorErrorCode::MissingRequiredProcessor
                                : GraphRuntimeExecutorErrorCode::None;
    }

    // Marshal the node's mono slots into contiguous channel views.
    std::array<const float*, GraphRuntimeExecutor::kMaxRoutedPortsPerNode> in_ptrs{};
    std::array<float*, GraphRuntimeExecutor::kMaxRoutedPortsPerNode> out_ptrs{};
    for (std::uint32_t p = 0; p < node.input_ports; ++p) {
        in_ptrs[p] = pool.slot_data(slots.input_base + p);
    }
    for (std::uint32_t p = 0; p < node.output_ports; ++p) {
        out_ptrs[p] = pool.slot_data(slots.output_base + p);
    }

    GraphRuntimeNodeProcessContext context;
    context.plan = &plan;
    context.node = &node;
    context.node_index = node_index;
    context.command_results = command_results;
    context.routed = true;
    context.node_inputs = audio::BufferView<const float>(
        in_ptrs.data(), node.input_ports, frames);
    context.node_outputs = audio::BufferView<float>(
        out_ptrs.data(), node.output_ports, frames);
    if (midi != nullptr) {
        context.node_midi_in = midi->in(node_index);
        context.node_midi_out = midi->out(node_index);
    }
    if (automation != nullptr) {
        context.node_param_events = automation->events(node_index);
    }
    if (!binding.process(block, context, binding.user_data)) {
        return GraphRuntimeExecutorErrorCode::NodeProcessorFailed;
    }
    if (midi != nullptr && context.node_midi_out != nullptr) {
        midi->set_out_incomplete(
            node_index,
            midi->in_incomplete(node_index) ||
                routed_midi_has_drops(*context.node_midi_out));
    }
    return GraphRuntimeExecutorErrorCode::None;
}

} // namespace

bool GraphRuntimeMidiScratch::reset(std::uint32_t node_count) {
    clear();
    try {
        slots_.reserve(node_count);
        for (std::uint32_t i = 0; i < node_count; ++i) {
            auto slot = std::make_unique<Slot>();
            slot->in_buffer.reserve(kRoutedMidiEventCapacity, kRoutedMidiSysexCapacity,
                                    kRoutedMidiSysexPayloadCapacity);
            slot->in_buffer.set_realtime_capacity_limit(true);
            slot->in_ump.reserve(kRoutedMidiEventCapacity);
            slot->in_ump.set_realtime_capacity_limit(true);
            slot->in_buffer.attach_ump(&slot->in_ump);
            slot->out_buffer.reserve(kRoutedMidiEventCapacity, kRoutedMidiSysexCapacity,
                                     kRoutedMidiSysexPayloadCapacity);
            slot->out_buffer.set_realtime_capacity_limit(true);
            slot->out_ump.reserve(kRoutedMidiEventCapacity);
            slot->out_ump.set_realtime_capacity_limit(true);
            slot->out_buffer.attach_ump(&slot->out_ump);
            slots_.push_back(std::move(slot));
        }
    } catch (...) {
        clear();
        return false;
    }
    node_count_ = node_count;
    return true;
}

void GraphRuntimeMidiScratch::clear() noexcept {
    slots_.clear();
    node_count_ = 0;
}

bool GraphRuntimeAutomationScratch::reset(const graph::GraphRuntimePlan& plan,
                                          std::uint32_t max_frames) {
    clear();
    const std::uint32_t node_count = plan.node_count();
    const std::uint32_t connection_count = plan.connection_count();
    try {
        events_.reserve(node_count);
        for (std::uint32_t i = 0; i < node_count; ++i) {
            events_.push_back(std::make_unique<state::ParameterEventQueue>());
        }
        slew_last_.assign(connection_count, 0.0f);
        slew_primed_.assign(connection_count, 0);

        // Per-node dense (audio-rate) parameter layout: each node's distinct
        // audio-rate param ids in first-seen inbound-connection order, each given
        // a max_frames accumulation region (matching the host walk's per-node
        // audio_rate_param_data).
        node_dense_first_.assign(node_count, 0);
        node_dense_count_.assign(node_count, 0);
        std::uint32_t total_dense = 0;
        for (std::uint32_t n = 0; n < node_count; ++n) {
            const auto& node = plan.nodes[n];
            node_dense_first_[n] = static_cast<std::uint32_t>(dense_params_.size());
            for (std::uint32_t c = 0; c < node.inbound_connection_count; ++c) {
                const auto ci =
                    plan.inbound_connection_indices[node.first_inbound_connection + c];
                const auto& conn = plan.connections[ci];
                if (!conn.is_automation || !conn.automation.audio_rate) continue;
                bool seen = false;
                for (std::uint32_t k = 0; k < node_dense_count_[n]; ++k) {
                    if (dense_params_[node_dense_first_[n] + k].param_id ==
                        conn.automation.param_id) {
                        seen = true;
                        break;
                    }
                }
                if (seen) continue;
                dense_params_.push_back({conn.automation.param_id, total_dense * max_frames});
                ++node_dense_count_[n];
                ++total_dense;
            }
        }
        dense_storage_.assign(static_cast<std::size_t>(total_dense) * max_frames, 0.0f);
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
    node_count_ = 0;
    connection_count_ = 0;
    max_frames_ = 0;
}

bool GraphRuntimeBufferPool::reset(std::uint32_t slot_count, std::uint32_t max_frames) {
    return reset(slot_count, max_frames, {});
}

bool GraphRuntimeBufferPool::reset(std::uint32_t slot_count, std::uint32_t max_frames,
                                   std::span<const std::uint32_t> connection_delay_samples) {
    if (slot_count > 0 && max_frames == 0) return false;
    try {
        storage_.assign(static_cast<std::size_t>(slot_count) * max_frames, 0.0f);

        // Lay out one contiguous ring per delayed connection in ring_storage_.
        // A delay of D needs D + max_frames floats (the legacy per-connection
        // delay line), zero-filled so the leading D samples read silence.
        std::vector<RingSlot> rings;
        std::size_t total_ring_floats = 0;
        if (!connection_delay_samples.empty()) {
            rings.assign(connection_delay_samples.size(), RingSlot{});
            for (std::size_t i = 0; i < connection_delay_samples.size(); ++i) {
                const std::uint32_t delay = connection_delay_samples[i];
                if (delay == 0) continue;
                const std::uint32_t size = delay + max_frames;
                // Fail closed rather than truncate the 32-bit ring offset: a
                // truncated offset would index out of ring_storage_ on the RT
                // gather. Off-RT, so the bound check is free.
                if (total_ring_floats > 0xFFFFFFFFull - size) {
                    clear();
                    return false;
                }
                rings[i].offset = static_cast<std::uint32_t>(total_ring_floats);
                rings[i].size = size;
                rings[i].delay = delay;
                rings[i].write_pos = 0;
                total_ring_floats += size;
            }
        }
        std::vector<float> ring_storage(total_ring_floats, 0.0f);

        ring_ = std::move(rings);
        ring_storage_ = std::move(ring_storage);
    } catch (...) {
        clear();
        return false;
    }
    slot_count_ = slot_count;
    max_frames_ = max_frames;
    return true;
}

void GraphRuntimeBufferPool::clear() noexcept {
    storage_.clear();
    slot_count_ = 0;
    max_frames_ = 0;
    ring_storage_.clear();
    ring_.clear();
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
        assignment_ = std::move(assignment);
        parallel_safe_ = parallel_safe;
    } catch (...) {
        clear();
        return false;
    }
    return true;
}

void GraphRuntimeSnapshot::clear() noexcept {
    plan_.clear();
    bindings_.clear();
    assignment_ = {};
    parallel_safe_ = false;
}

bool GraphRuntimeSnapshot::valid() const noexcept {
    if (bindings_.size() != plan_.nodes.size()) return false;
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

bool GraphRuntimeExecutor::drain_commands(
    ProcessBlock& block,
    const graph::GraphRuntimePlan& plan,
    std::span<const graph::GraphTimedCommand> commands,
    std::span<GraphRuntimeCommandDecision> command_results,
    GraphRuntimeCommandHandler command_handler,
    GraphRuntimeEventSink event_sink,
    GraphRuntimeExecutorResult& result) noexcept {
    if (command_results.size() < commands.size()) return false;

    result.commands_drained = static_cast<std::uint32_t>(commands.size());
    commands_drained_.fetch_add(result.commands_drained, std::memory_order_relaxed);

    for (const auto& command : commands) {
        auto status = GraphRuntimeCommandStatus::Accepted;
        if (command_requires_node(command.command.type) &&
            !contains_node(plan, command.command.node_id)) {
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
    if (!drain_commands(block, plan, commands, command_results, command_handler,
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
    if (!drain_commands(block, plan, commands, command_results, command_handler,
                        event_sink, result)) {
        return fail_command_scratch_too_small();
    }

    const BusBuffer* in_bus =
        block.buses ? block.buses->first(BusDirection::Input, BusRole::Main) : nullptr;
    BusBuffer* out_bus =
        block.buses ? block.buses->first(BusDirection::Output, BusRole::Main) : nullptr;

    // Zero the main output bus once; AudioOutput nodes accumulate into it, so N
    // output nodes mix (and any output channel no node drives stays silent),
    // matching the host graph this executor mirrors.
    if (out_bus != nullptr) {
        for (std::size_t c = 0; c < out_bus->output.num_channels(); ++c) {
            std::fill_n(out_bus->output.channel_ptr(c), frames, 0.0f);
        }
    }

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
    for (const auto node_index : plan.processing_order_indices) {
        if (node_index >= bindings.size()) return fail_invalid_snapshot();

        // Skipped nodes are not run: the caller has pre-filled their output slots
        // (anticipative rendering pre-renders the interior off-thread). They are
        // not counted as processed and their plugin state is left untouched.
        if (!skip_mask.empty() && node_index < skip_mask.size() &&
            skip_mask[node_index] != 0) {
            continue;
        }

        const auto error = run_routed_node(node_index, plan, bindings, assignment,
                                           pool, midi, automation, block, in_bus,
                                           out_bus, command_decisions, frames);
        if (error != GraphRuntimeExecutorErrorCode::None) {
            result.error = error;
            result.failed_node_index = node_index;
            node_failures_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }
        ++result.nodes_processed;
        nodes_processed_.fetch_add(1, std::memory_order_relaxed);
    }

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
    const std::uint32_t* level_nodes;          // this level's node indices
    std::atomic<std::uint32_t>* error_code;    // 0 = None, else (code + 1)
    std::atomic<std::uint32_t>* failed_node;
};

void run_level_node(void* ctx, std::uint32_t i) noexcept {
    auto* t = static_cast<ParallelLevelTask*>(ctx);
    const std::uint32_t node_index = t->level_nodes[i];
    const auto e = run_routed_node(node_index, *t->plan, t->bindings, *t->assignment,
                                   *t->pool, t->midi, t->automation, *t->block,
                                   t->in_bus, t->out_bus, t->command_results,
                                   t->frames);
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
    if (!drain_commands(block, plan, commands, command_results, command_handler,
                        event_sink, result)) {
        return fail_command_scratch_too_small();
    }

    const BusBuffer* in_bus =
        block.buses ? block.buses->first(BusDirection::Input, BusRole::Main) : nullptr;
    BusBuffer* out_bus =
        block.buses ? block.buses->first(BusDirection::Output, BusRole::Main) : nullptr;
    if (out_bus != nullptr) {
        for (std::size_t c = 0; c < out_bus->output.num_channels(); ++c) {
            std::fill_n(out_bus->output.channel_ptr(c), frames, 0.0f);
        }
    }

    const auto command_decisions = command_results.first(commands.size());
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
                const auto e = run_routed_node(node_index, plan, bindings, assignment,
                                               pool, midi, automation, block, in_bus,
                                               out_bus, command_decisions, frames);
                if (e != GraphRuntimeExecutorErrorCode::None) {
                    return report_failure(e, node_index);
                }
            }
        } else {
            ParallelLevelTask task{
                &plan, bindings, &assignment, &pool, midi, automation, &block,
                in_bus, out_bus, command_decisions, frames,
                levels.level_nodes.data() + base, &error_code, &failed_node,
            };
            workers.run(width, run_level_node, &task);
            if (const std::uint32_t code = error_code.load(std::memory_order_relaxed);
                code != 0) {
                return report_failure(
                    static_cast<GraphRuntimeExecutorErrorCode>(code - 1),
                    failed_node.load(std::memory_order_relaxed));
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
