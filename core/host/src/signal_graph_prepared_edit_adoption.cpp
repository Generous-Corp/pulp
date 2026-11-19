// SPDX-License-Identifier: MIT
//
// Runtime-state adoption for PreparedTopologyEdit::prepare().
//
// After the candidate compiles, prepare() carries forward the live snapshot's
// stable runtime state into the new one before publication: parameter and MIDI
// ingress mailboxes for nodes whose type is unchanged, the plugin contexts'
// sequence pointers into those mailboxes, and feed-forward PDC delay rings whose
// connection identity, delay and ring shape match. A sample-rate or block-size
// change keeps the mailboxes but starts fresh DSP history. Feedback is never
// adopted.

#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pulp::host {
namespace {

bool has_feedback(const std::vector<Connection>& connections) {
    return std::any_of(connections.begin(), connections.end(),
                       [](const Connection& c) { return c.feedback; });
}

} // namespace

// `old` is the live snapshot the candidate replaces and `next` the freshly
// compiled candidate; both are non-null. std::nullopt means `next` may be
// published as prepared, including when the dimensions changed and only the
// mailboxes were adopted.
std::optional<SignalGraph::PreparedTopologyEdit::Result>
SignalGraph::PreparedTopologyEdit::adopt_live_runtime_state_(const CompiledGraph* old,
                                                             CompiledGraph* next,
                                                             bool dimensions_changed) {
    // Adopt stable runtime ingress/egress identities before publication. A
    // control-thread MIDI/parameter batch published during compilation is
    // therefore visible to exactly one audio block across the boundary.
    for (auto& [id, rt] : next->runtime) {
        const auto live_rt = old->runtime.find(id);
        const auto live_shape = old->shapes.find(id);
        const auto next_shape = next->shapes.find(id);
        if (live_rt == old->runtime.end() || live_shape == old->shapes.end() ||
            next_shape == next->shapes.end() ||
            live_shape->second.type != next_shape->second.type) {
            continue;
        }
        if (rt.parameter_input_mailbox && live_rt->second.parameter_input_mailbox &&
            rt.exact_parameter_event_owner.lock() ==
                live_rt->second.exact_parameter_event_owner.lock()) {
            rt.parameter_input_mailbox = live_rt->second.parameter_input_mailbox;
        }
        if (rt.midi_input_mailbox && live_rt->second.midi_input_mailbox) {
            rt.midi_input_mailbox = live_rt->second.midi_input_mailbox;
        }
    }
    const auto rebind_parameter_sequence = [&](CompiledGraph::RoutedPath& path) {
        for (auto& ctx : path.plugin_ctx) {
            const auto rt = next->runtime.find(ctx.node_id);
            ctx.parameter_events_live_sequence_seen =
                rt != next->runtime.end() && rt->second.parameter_input_mailbox
                    ? &rt->second.parameter_input_mailbox->sequence_seen
                    : nullptr;
            ctx.parameter_events_exact_sequence_seen =
                rt != next->runtime.end() && rt->second.exact_parameter_input_mailbox
                    ? &rt->second.exact_parameter_input_mailbox->sequence_seen
                    : nullptr;
        }
    };
    rebind_parameter_sequence(next->routed.serial);
    rebind_parameter_sequence(next->routed.parallel);

    // A sample-rate/block-size transition intentionally starts fresh DSP
    // history. Stable mailbox identities above still preserve any pending
    // MIDI/parameter publication, but differently-sized PDC/feedback rings
    // are never copied or shared across the boundary.
    if (dimensions_changed)
        return std::nullopt;

    // Feedback owns previous-block samples per snapshot and is not safely
    // adoptable. Feed-forward PDC carries only the unchanged identity+shape
    // intersection: removed rings retire with the old snapshot, newly
    // delayed or reshaped rings start fresh, and no stale history can attach
    // to a disconnect+reconnect edge that minted a new identity.
    if (has_feedback(old->connections) || has_feedback(next->connections) ||
        old->connection_identities.size() != old->connection_delays.size() ||
        next->connection_identities.size() != next->connection_delays.size()) {
        return Result::RuntimeAdoptionFailed;
    }

    std::unordered_map<std::uint64_t, std::size_t> old_delayed;
    for (std::size_t i = 0; i < old->connection_delays.size(); ++i) {
        if (old->connection_delays[i].delay_samples > 0) {
            old_delayed.emplace(old->connection_identities[i], i);
        }
    }
    std::vector<std::pair<std::size_t, std::size_t>> matched;
    for (std::size_t i = 0; i < next->connection_delays.size(); ++i) {
        const auto& delay = next->connection_delays[i];
        if (delay.delay_samples <= 0)
            continue;
        const auto found = old_delayed.find(next->connection_identities[i]);
        if (found == old_delayed.end())
            continue;
        const auto& live_delay = old->connection_delays[found->second];
        if (delay.delay_samples != live_delay.delay_samples || !delay.state || !live_delay.state ||
            delay.state->ring.size() != live_delay.state->ring.size()) {
            continue;
        }
        matched.emplace_back(i, found->second);
    }
    for (const auto [candidate_index, live_index] : matched) {
        bool routed_state_compatible = next->pdc_execution_domain == old->pdc_execution_domain;
        if (routed_state_compatible &&
            next->pdc_execution_domain == PdcExecutionDomain::RoutedSerial) {
            routed_state_compatible =
                next->routed.serial.valid && old->routed.serial.valid &&
                next->routed.serial.pool.can_adopt_delay_ring_state(
                    static_cast<std::uint32_t>(candidate_index), old->routed.serial.pool,
                    static_cast<std::uint32_t>(live_index));
        } else if (routed_state_compatible &&
                   next->pdc_execution_domain == PdcExecutionDomain::RoutedParallel) {
            routed_state_compatible =
                next->routed.parallel.valid && old->routed.parallel.valid &&
                next->routed.parallel.pool.can_adopt_delay_ring_state(
                    static_cast<std::uint32_t>(candidate_index), old->routed.parallel.pool,
                    static_cast<std::uint32_t>(live_index));
        }
        if (!routed_state_compatible)
            continue;
        next->connection_delays[candidate_index].state = old->connection_delays[live_index].state;
        if (next->pdc_execution_domain == PdcExecutionDomain::RoutedSerial) {
            (void)next->routed.serial.pool.adopt_delay_ring_state(
                static_cast<std::uint32_t>(candidate_index), old->routed.serial.pool,
                static_cast<std::uint32_t>(live_index));
        }
        if (next->pdc_execution_domain == PdcExecutionDomain::RoutedParallel) {
            (void)next->routed.parallel.pool.adopt_delay_ring_state(
                static_cast<std::uint32_t>(candidate_index), old->routed.parallel.pool,
                static_cast<std::uint32_t>(live_index));
        }
    }
    return std::nullopt;
}

} // namespace pulp::host
