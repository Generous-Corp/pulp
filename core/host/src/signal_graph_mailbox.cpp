// SignalGraph control-plane mailbox ingress/egress and snapshot readers.
//
// These methods deliberately keep Slot::read() pins around every snapshot
// dereference.  They are separated from graph authoring/prepare/process code
// so mailbox ownership and retry semantics have one implementation owner.

#include "signal_graph_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>

namespace pulp::host {
namespace {

std::uint64_t next_nonzero_mailbox_sequence(std::atomic<std::uint64_t>& counter) noexcept {
    auto current = counter.load(std::memory_order_relaxed);
    for (;;) {
        const auto next =
            current == std::numeric_limits<std::uint64_t>::max() ? std::uint64_t{1} : current + 1;
        // The mailbox's single-writer contract means contention is not expected;
        // CAS keeps the test seam and any accidental observer from losing an
        // update. Relaxed is sufficient because this atomic carries identity
        // only; TripleBuffer's release/acquire flag publishes the payload.
        if (counter.compare_exchange_weak(current, next, std::memory_order_relaxed,
                                          std::memory_order_relaxed)) {
            return next;
        }
    }
}

} // namespace

bool SignalGraph::inject_midi(NodeId id, const midi::MidiBuffer& events) noexcept {
    // Pin the live snapshot for the whole dereference. Without the guard a
    // concurrent prepare()/release()/invalidate could retire+free `cg` mid-use.
    auto read_guard = read_snapshot_(true);
    auto* cg = read_guard.get();
    if (!cg)
        return false;
    return inject_midi_into_snapshot_(*cg, id, events);
}

bool SignalGraph::inject_midi(NodeId id, const midi::MidiBuffer& events,
                              const midi::MpeBuffer& mpe) noexcept {
    auto read_guard = read_snapshot_(true);
    auto* cg = read_guard.get();
    if (!cg)
        return false;
    return inject_midi_into_snapshot_(*cg, id, events, &mpe);
}

bool SignalGraph::inject_midi_into_snapshot_(CompiledGraph& snapshot, NodeId id,
                                             const midi::MidiBuffer& events,
                                             const midi::MpeBuffer* mpe) noexcept {
    auto it = snapshot.runtime.find(id);
    if (it == snapshot.runtime.end())
        return false;
    auto shape_it = snapshot.shapes.find(id);
    if (shape_it == snapshot.shapes.end() || shape_it->second.type != NodeType::MidiInput ||
        !it->second.midi_input_mailbox) {
        return false;
    }

    auto& mailbox = *it->second.midi_input_mailbox;
    const uint64_t sequence = next_nonzero_mailbox_sequence(mailbox.next_sequence);
    const bool copied_all = mpe != nullptr
                                ? mailbox.writer_scratch.set_from_midi(events, *mpe, sequence)
                                : mailbox.writer_scratch.set_from_midi(events, sequence);
    mailbox.published.write(mailbox.writer_scratch);
    return copied_all;
}

bool SignalGraph::inject_parameter_events(NodeId id, const state::ParameterEventQueue& events) {
    // Same reader-pinned, single-writer publication discipline as inject_midi.
    // The fixed-capacity writer scratch and TripleBuffer were prepared with the
    // snapshot, so this path performs no allocation.
    auto read_guard = read_snapshot_(true);
    auto* cg = read_guard.get();
    if (!cg)
        return false;
    // A node the timeline binding owns for exact ingress rejects ad-hoc live
    // injection, so a UI/host param write cannot race the timeline's stream.
    const auto runtime_it = cg->runtime.find(id);
    if (runtime_it != cg->runtime.end() &&
        !runtime_it->second.exact_parameter_event_owner.expired()) {
        return false;
    }
    return inject_parameter_events_into_snapshot_(*cg, id, events);
}

bool SignalGraph::inject_parameter_events_into_snapshot_(
    CompiledGraph& snapshot, NodeId id, const state::ParameterEventQueue& events) noexcept {
    auto runtime_it = snapshot.runtime.find(id);
    if (runtime_it == snapshot.runtime.end())
        return false;
    const auto shape_it = snapshot.shapes.find(id);
    if (shape_it == snapshot.shapes.end() || shape_it->second.type != NodeType::Plugin ||
        (snapshot.plugins.find(id) == snapshot.plugins.end() &&
         snapshot.processors.find(id) == snapshot.processors.end()) ||
        !runtime_it->second.parameter_input_mailbox) {
        return false;
    }

    auto& mailbox = *runtime_it->second.parameter_input_mailbox;
    const std::uint64_t sequence = next_nonzero_mailbox_sequence(mailbox.next_sequence);
    const bool copied_all = mailbox.writer_scratch.set_from_queue(events, sequence);
    mailbox.published.write(mailbox.writer_scratch);
    return copied_all;
}

bool SignalGraph::inject_exact_parameter_events_into_snapshot_(
    CompiledGraph& snapshot, NodeId id, const state::ParameterEventQueue& events) noexcept {
    auto runtime_it = snapshot.runtime.find(id);
    if (runtime_it == snapshot.runtime.end())
        return false;
    const auto shape_it = snapshot.shapes.find(id);
    if (shape_it == snapshot.shapes.end() || shape_it->second.type != NodeType::Plugin ||
        (snapshot.plugins.find(id) == snapshot.plugins.end() &&
         snapshot.processors.find(id) == snapshot.processors.end()) ||
        !runtime_it->second.exact_parameter_input_mailbox ||
        runtime_it->second.exact_parameter_event_owner.expired()) {
        return false;
    }

    auto& mailbox = *runtime_it->second.exact_parameter_input_mailbox;
    const std::uint64_t sequence = next_nonzero_mailbox_sequence(mailbox.next_sequence);
    const bool copied_all = mailbox.writer_scratch.set_from_queue(events, sequence);
    mailbox.published.write(mailbox.writer_scratch);
    return copied_all;
}

bool SignalGraph::seed_midi_input_sequence_for_test(NodeId id, std::uint64_t predecessor) noexcept {
    auto read_guard = read_snapshot_(true);
    auto* cg = read_guard.get();
    if (!cg)
        return false;
    auto runtime_it = cg->runtime.find(id);
    const auto shape_it = cg->shapes.find(id);
    if (runtime_it == cg->runtime.end() || shape_it == cg->shapes.end() ||
        shape_it->second.type != NodeType::MidiInput || !runtime_it->second.midi_input_mailbox) {
        return false;
    }
    runtime_it->second.midi_input_mailbox->next_sequence.store(predecessor,
                                                               std::memory_order_relaxed);
    return true;
}

std::uint64_t SignalGraph::midi_input_sequence_for_test(NodeId id) const noexcept {
    auto read_guard = read_snapshot_(true);
    auto* cg = read_guard.get();
    if (!cg)
        return 0;
    const auto runtime_it = cg->runtime.find(id);
    if (runtime_it == cg->runtime.end() || !runtime_it->second.midi_input_mailbox) {
        return 0;
    }
    return runtime_it->second.midi_input_mailbox->next_sequence.load(std::memory_order_relaxed);
}

PluginBindingContext::PendingParameterEventSequences
SignalGraph::append_parameter_mailbox_events_(void* runtime,
                                              state::ParameterEventQueue& destination) noexcept {
    auto* rt = static_cast<NodeRuntime*>(runtime);
    PluginBindingContext::PendingParameterEventSequences pending;
    if (rt == nullptr)
        return pending;
    const auto append = [&destination](ParameterInputMailbox* mailbox,
                                       bool require_complete) noexcept {
        if (mailbox == nullptr)
            return std::uint64_t{0};
        const auto& injected = mailbox->published.read();
        const auto sequence_seen = mailbox->sequence_seen.load(std::memory_order_relaxed);
        if (injected.sequence == 0 || injected.sequence == sequence_seen) {
            return std::uint64_t{0};
        }
        const bool appended_all = injected.append_to(destination);
        return appended_all || !require_complete ? injected.sequence : std::uint64_t{0};
    };
    // Exact-generation events follow live events so they win stable-sort ties.
    pending.live = append(rt->parameter_input_mailbox.get(), false);
    // require_complete=true is safe because a claimed node is the SOLE writer of
    // its exact mailbox: it is refused live injection and rejected from graph
    // automation (DeviceNodeAutomationConflict), so the per-block exact queue is
    // bounded by one delivery and never exceeds capacity. The complete-or-skip
    // append therefore cannot drop events or retry-starve — a partial append
    // would only occur if that sole-writer invariant were violated.
    pending.exact = rt->exact_parameter_event_owner.expired()
                        ? 0
                        : append(rt->exact_parameter_input_mailbox.get(), true);
    return pending;
}

bool SignalGraph::extract_midi(NodeId id, midi::MidiBuffer& out) const {
    // Pin the live snapshot for the whole dereference (see inject_midi). const
    // method: Slot::read() only touches the slot's mutable reader counter.
    auto read_guard = read_snapshot_(true);
    auto* cg = read_guard.get();
    if (!cg)
        return false;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end())
        return false;
    auto shape_it = cg->shapes.find(id);
    if (shape_it == cg->shapes.end() || shape_it->second.type != NodeType::MidiOutput ||
        !it->second.midi_output_mailbox) {
        return false;
    }

    auto& mailbox = *it->second.midi_output_mailbox;
    mailbox.consumer_incomplete = mailbox.incomplete.exchange(false, std::memory_order_relaxed) ||
                                  mailbox.consumer_incomplete;
    if (mailbox.consumer_has_retry) {
        if (!mailbox.consumer_scratch.append_to_midi(out, mailbox.consumer_event_index,
                                                     mailbox.consumer_sysex_index,
                                                     mailbox.consumer_ump_index)) {
            return false;
        }
        mailbox.consumer_incomplete =
            mailbox.consumer_scratch.incomplete || mailbox.consumer_incomplete;
        mailbox.consumer_has_retry = false;
    }
    while (mailbox.pending.try_pop(mailbox.consumer_scratch)) {
        mailbox.consumer_event_index = 0;
        mailbox.consumer_sysex_index = 0;
        mailbox.consumer_ump_index = 0;
        mailbox.consumer_incomplete =
            mailbox.consumer_scratch.incomplete || mailbox.consumer_incomplete;
        if (!mailbox.consumer_scratch.append_to_midi(out, mailbox.consumer_event_index,
                                                     mailbox.consumer_sysex_index,
                                                     mailbox.consumer_ump_index)) {
            mailbox.consumer_has_retry = true;
            return false;
        }
    }
    const bool complete = !mailbox.consumer_incomplete;
    mailbox.consumer_incomplete = false;
    return complete;
}

int SignalGraph::node_latency_samples(NodeId id) const {
    // Pin the live snapshot for the whole dereference (see inject_midi).
    auto read_guard = read_snapshot_(true);
    const auto* cg = read_guard.get();
    if (!cg)
        return 0;
    auto it = cg->runtime.find(id);
    if (it == cg->runtime.end())
        return 0;
    return (int)it->second.input_latency;
}

std::optional<SignalGraph::PreparedNodeLatency>
SignalGraph::PreparedLatencySnapshot::node(NodeId id) const {
    for (const auto& value : nodes) {
        if (value.node_id == id)
            return value;
    }
    return std::nullopt;
}

std::optional<std::int64_t>
SignalGraph::PreparedLatencySnapshot::connection_delay(const Connection& query) const {
    for (const auto& value : connections) {
        const auto& candidate = value.connection;
        // Connection::operator== intentionally ignores several routing flags
        // for legacy authoring equivalence. Latency identity needs the full
        // edge kind so a query cannot accidentally resolve a different edge.
        if (candidate.source_node != query.source_node ||
            candidate.source_port != query.source_port || candidate.dest_node != query.dest_node ||
            candidate.dest_port != query.dest_port || candidate.feedback != query.feedback ||
            candidate.midi != query.midi || candidate.automation != query.automation ||
            candidate.audio_rate_modulation != query.audio_rate_modulation ||
            candidate.sidechain != query.sidechain ||
            candidate.automation_param_id != query.automation_param_id ||
            candidate.spectral != query.spectral) {
            continue;
        }
        return value.delay_samples;
    }
    return std::nullopt;
}

std::optional<SignalGraph::PreparedLatencySnapshot> SignalGraph::prepared_latency_snapshot() const {
    // Keep the published snapshot pinned while copying every value. The
    // resulting object is independent of the graph and remains stable across
    // later authoring mutations; a subsequent call observes the invalidated
    // (null) publication until prepare() succeeds again.
    auto read_guard = read_snapshot_(true);
    const auto* cg = read_guard.get();
    if (!cg)
        return std::nullopt;

    PreparedLatencySnapshot result;
    result.total_latency_samples = cg->total_latency_samples;
    result.sample_rate = cg->sample_rate;
    result.max_block_size = cg->max_block_size;
    result.nodes.reserve(cg->order.size());
    for (const auto id : cg->order) {
        const auto it = cg->runtime.find(id);
        if (it == cg->runtime.end())
            continue;
        PreparedNodeLatency node{
            .node_id = id,
            .input_latency_samples = it->second.input_latency,
            .output_latency_samples = it->second.output_latency,
            .artifact_latency_samples =
                std::max<std::int64_t>(0, it->second.output_latency - it->second.input_latency),
        };
        if (const auto delay = cg->custom_intentional_delay_samples.find(id);
            delay != cg->custom_intentional_delay_samples.end())
            node.intentional_delay_samples = delay->second;
        if (const auto ports = cg->custom_output_port_latency_samples.find(id);
            ports != cg->custom_output_port_latency_samples.end())
            node.output_port_latency_samples.assign(ports->second.begin(), ports->second.end());
        result.nodes.push_back(std::move(node));
    }

    result.connections.reserve(cg->connections.size());
    for (std::size_t i = 0; i < cg->connections.size(); ++i) {
        const auto delay =
            i < cg->connection_delays.size() ? cg->connection_delays[i].delay_samples : 0;
        result.connections.push_back(PreparedConnectionDelay{
            .connection = cg->connections[i],
            .delay_samples = delay,
        });
    }
    return result;
}

} // namespace pulp::host
