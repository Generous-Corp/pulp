// SignalGraph private compile seam: routed-executor build.
// Builds the serial and parallel routed snapshots for one compiled snapshot,
// selects its PDC execution domain, and prepares the anticipation lane splice
// over the routed plan. compile_ calls it once, after latencies are computed.
#include "signal_graph_internal.hpp"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <pulp/graph/graph_runtime_levelization.hpp>
#include <pulp/host/anticipation_eligibility.hpp>
#include <pulp/host/anticipation_partition.hpp>
#include <pulp/host/anticipation_subgraph.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/host/signal_graph_runtime.hpp>
#include <thread>
#include <vector>

namespace pulp::host {

// Build the canonical-executor routing for this snapshot when the topology
// is eligible. The Gain bindings resolve to THIS snapshot's own gain atomics
// (valid for cg's whole lifetime), so the embedded snapshot needs no
// keepalive. Ineligible graphs leave routed.serial.valid false and use the walk.
// Returns false when the snapshot must not be published: a spectral graph
// whose serial routed snapshot is invalid.
bool SignalGraph::build_routed_for_snapshot_(CompiledGraph& compiled, double sample_rate,
                                             int max_block_size, CompileMode mode) {
    CompiledGraph* const cg = &compiled;
    // Serial routed snapshot (compact buffer layout). The resolver set that
    // reads THIS snapshot's own runtime/plugins and the cached plugin
    // metadata lives in build_routing_snapshot_locked_, shared with the
    // parallel path so the two never drift.
    cg->routed.serial.valid =
        build_routing_snapshot_locked_(*cg, /*parallel_safe=*/false, cg->routed.serial.plugin_ctx,
                                       cg->routed.serial.custom_ctx, cg->routed.serial.snapshot);
    for (auto& context : cg->routed.serial.custom_ctx) {
        context.graph_event_lane = cg->graph_event_lane;
        context.graph_event_edge = cg->graph_event_edge;
        context.graph_event_epoch = cg->graph_event_epoch;
    }
    // Size THIS snapshot's own scratch pool (per-snapshot, retired with the
    // snapshot via RCU; never resized under an in-flight reader).
    if (cg->routed.serial.valid && max_block_size > 0) {
        cg->routed.serial.valid = cg->routed.serial.pool.reset(
            cg->routed.serial.snapshot.buffer_slot_count(),
            static_cast<std::uint32_t>(max_block_size),
            cg->routed.serial.snapshot.buffer_assignment().connection_delay_samples);
        // Resolve per-node channel pointers once; an unbound pool still
        // routes, through the per-block resolution.
        if (cg->routed.serial.valid)
            (void)cg->routed.serial.pool.bind(cg->routed.serial.snapshot);
    }
    // Per-snapshot MIDI scratch + the MidiInput/MidiOutput node index lists
    // the routed dispatch bridges to the mailboxes. Built only when the
    // routed plan carries MIDI nodes, so audio-only graphs allocate none.
    cg->routed.midi_inputs.clear();
    cg->routed.midi_outputs.clear();
    cg->routed.processor_parameter_inputs.clear();
    if (cg->routed.serial.valid) {
        const auto& plan = cg->routed.serial.snapshot.plan();
        bool plan_has_midi = false;
        for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
            const auto kind = plan.nodes[i].kind;
            if (kind == graph::GraphRuntimeNodeKind::MidiInput) {
                cg->routed.midi_inputs.push_back({i, plan.nodes[i].id});
                if (cg->trigger_state != nullptr && cg->host_event_midi_input == 0)
                    cg->host_event_midi_input = plan.nodes[i].id;
                plan_has_midi = true;
            } else if (kind == graph::GraphRuntimeNodeKind::MidiOutput) {
                cg->routed.midi_outputs.push_back({i, plan.nodes[i].id});
                plan_has_midi = true;
            } else if (plan.nodes[i].event_input_ports > 0 ||
                       plan.nodes[i].event_output_ports > 0) {
                plan_has_midi = true; // a plugin carrying MIDI edges
            }
        }
        if (plan_has_midi) {
            cg->routed.serial.valid = cg->routed.midi.reset(plan.node_count());
        }
        // Per-snapshot sparse-automation scratch, built only when the routed
        // plan carries automation connections (audio-only / MIDI graphs
        // allocate none).
        bool plan_has_automation = false;
        for (const auto& conn : plan.connections) {
            if (graph::is_automation_conn(conn)) {
                plan_has_automation = true;
                break;
            }
        }
        if (cg->routed.serial.valid) {
            for (std::uint32_t i = 0; i < plan.nodes.size(); ++i) {
                const auto id = plan.nodes[i].id;
                if (!processor_nodes_.contains(id))
                    continue;
                const auto claim = exact_parameter_event_claims_.find(id);
                if (claim == exact_parameter_event_claims_.end() || claim->second.expired())
                    continue;
                cg->routed.processor_parameter_inputs.push_back({i, id, 0});
                plan_has_automation = true;
            }
        }
        if (cg->routed.serial.valid && plan_has_automation && max_block_size > 0) {
            cg->routed.serial.valid =
                cg->routed.automation.reset(plan, cg->routed.serial.snapshot.bindings(),
                                            static_cast<std::uint32_t>(max_block_size));
        }
    }

    // Levelized parallel routing: when enabled (at this prepare), build a
    // PARALLEL-SAFE (reuse-free) snapshot of the same eligible graph + its
    // levelization + a dedicated scratch pool, and ensure the persistent
    // worker pool is running. The MIDI/automation scratch and MidiInput/Output
    // node lists are SHARED with the serial path (identical plan). On any
    // failure the parallel path stays invalid and process() falls back.
    cg->routed.parallel.valid = false;
    if (!cg->spectral && cg->routed.serial.valid &&
        parallel_routing_enabled_.load(std::memory_order_relaxed) && max_block_size > 0) {
        // Parallel-safe routed snapshot: same resolver set as the serial
        // path (via build_routing_snapshot_locked_) but a reuse-free buffer
        // assignment so concurrent same-level nodes never alias a recycled
        // slot. The MIDI/automation scratch and MidiInput/Output node lists
        // are SHARED with the serial path (identical plan).
        bool ok = build_routing_snapshot_locked_(
            *cg, /*parallel_safe=*/true, cg->routed.parallel.plugin_ctx,
            cg->routed.parallel.custom_ctx, cg->routed.parallel.snapshot);
        for (auto& context : cg->routed.parallel.custom_ctx) {
            context.graph_event_lane = cg->graph_event_lane;
            context.graph_event_edge = cg->graph_event_edge;
            context.graph_event_epoch = cg->graph_event_epoch;
        }
        if (ok) {
            cg->routed.parallel.levelization =
                graph::build_graph_runtime_levelization(cg->routed.parallel.snapshot.plan());
            ok = cg->routed.parallel.levelization.ok &&
                 cg->routed.parallel.pool.reset(
                     cg->routed.parallel.snapshot.buffer_slot_count(),
                     static_cast<std::uint32_t>(max_block_size),
                     cg->routed.parallel.snapshot.buffer_assignment().connection_delay_samples);
            if (ok)
                (void)cg->routed.parallel.pool.bind(cg->routed.parallel.snapshot);
        }
        if (ok && prepared_edit_origin_ == nullptr) {
            // Start the persistent worker pool off the audio thread, ONCE.
            // Hardware concurrency capped to a sane bound; participant 0 is
            // the audio thread, so the pool spawns worker_count - 1 threads.
            //
            // INVARIANT (load-bearing for audio-thread safety): the pool size
            // is fixed for the SignalGraph's lifetime and the pool is never
            // stopped/resized on a re-prepare. start()/stop() join worker
            // threads and reset epoch_/completed_/worker_count_; running them
            // concurrently with an in-flight process_parallel -> run() on the
            // audio thread would be a use-after-free. The only legal stop is
            // ~GraphRuntimeWorkerPool during ~SignalGraph, after the audio
            // thread has (by contract) stopped calling process(). So: start
            // only when not yet started (worker_count() == 0, which also retries a
            // previously failed start, safe because routed.parallel.valid was
            // false so process() never entered the parallel branch). A re-
            // prepare just re-checks running(); it must not restart the pool.
            if (worker_pool_.worker_count() == 0) {
                const unsigned hw = std::thread::hardware_concurrency();
                const std::uint32_t workers = std::clamp<std::uint32_t>(hw == 0 ? 2 : hw, 2, 16);
                ok = worker_pool_.start(workers);
            } else {
                ok = worker_pool_.running();
            }
        }
        cg->routed.parallel.valid = ok;
    }

    const bool has_pdc =
        std::any_of(cg->connection_delays.begin(), cg->connection_delays.end(),
                    [](const ConnectionDelay& delay) { return delay.delay_samples > 0; });
    if (cg->spectral) {
        if (!cg->routed.serial.valid)
            return false;
        cg->pdc_execution_domain = PdcExecutionDomain::RoutedSerial;
        cg->total_latency_samples =
            cg->routed.serial.snapshot.buffer_assignment().total_latency_samples;
    } else if (has_pdc) {
        if (cg->routed.parallel.valid &&
            parallel_routing_enabled_.load(std::memory_order_relaxed)) {
            cg->pdc_execution_domain = PdcExecutionDomain::RoutedParallel;
        } else if (cg->routed.serial.valid &&
                   canonical_executor_routing_enabled_.load(std::memory_order_relaxed)) {
            cg->pdc_execution_domain = PdcExecutionDomain::RoutedSerial;
        } else {
            cg->pdc_execution_domain = PdcExecutionDomain::Legacy;
        }
    }

    // Anticipative rendering: carve an eligible latent interior out of the
    // graph into a lane pre-rendered ahead of the deadline, and prepare the
    // live-path splice (a skip mask over the routed plan + a map from each lane
    // output channel to the interior boundary-source output slot it fills).
    // Requires the canonical routed snapshot (the splice runs on that path).
    // Recomputed every compile: with no active anticipation no node is forced
    // exterior, so the counter must read 0 rather than keep a prior value.
    transport_suppressed_for_anticipation_.store(0, std::memory_order_relaxed);
    if (!cg->spectral && mode == CompileMode::Normal && cg->routed.serial.valid &&
        cg->processors.empty() && anticipation_enabled_.load(std::memory_order_relaxed) &&
        max_block_size > 0) {
        std::vector<NodeId> exact_parameter_input_nodes;
        exact_parameter_input_nodes.reserve(exact_parameter_event_claims_.size());
        for (const auto& [id, owner] : exact_parameter_event_claims_) {
            if (!owner.expired())
                exact_parameter_input_nodes.push_back(id);
        }
        const auto eligibility = analyze_anticipation_eligibility(
            cg->executable_nodes, cg->connections, exact_parameter_input_nodes);
        // Record how many transport-sensitive nodes anticipation forced
        // exterior: each such node was seeded TransportSensitive by compile_ and so
        // is excluded from the interior, running live to observe the host
        // transport. This is the repurposed meaning of
        // transport_suppressed_for_anticipation(): no longer "transport
        // dropped per block" (transport now stays live; the masked interior
        // is transport-insensitive by construction).
        std::uint64_t transport_forced_exterior = 0;
        for (const auto& n : cg->executable_nodes) {
            if (n.transport_sensitive)
                ++transport_forced_exterior;
        }
        transport_suppressed_for_anticipation_.store(transport_forced_exterior,
                                                     std::memory_order_relaxed);
        const auto partition =
            build_anticipation_partition(cg->executable_nodes, cg->connections, eligibility);
        const auto subgraph =
            build_anticipation_subgraph(cg->executable_nodes, cg->connections, partition);
        if (subgraph.renders_anything()) {
            CompiledGraph& cgr = *cg;
            constexpr int kLeadBlocks = 4;
            const bool prepared = cg->anticipation.lane.prepare(
                subgraph,
                [&cgr](NodeId id) -> std::atomic<float>* {
                    auto it = cgr.runtime.find(id);
                    return it == cgr.runtime.end() ? nullptr : it->second.gain.get();
                },
                [&cgr](NodeId id) -> PluginSlot* {
                    auto it = cgr.plugins.find(id);
                    return it == cgr.plugins.end() ? nullptr : it->second.get();
                },
                sample_rate, max_block_size, kLeadBlocks,
                [&cgr](NodeId id) -> ParameterEventInjectionBinding {
                    auto it = cgr.runtime.find(id);
                    if (it == cgr.runtime.end() || !it->second.parameter_input_mailbox) {
                        return {};
                    }
                    return {
                        .user_data = &it->second,
                        .append = &SignalGraph::append_parameter_mailbox_events_,
                        .live_sequence_seen = &it->second.parameter_input_mailbox->sequence_seen,
                        .exact_sequence_seen =
                            it->second.exact_parameter_input_mailbox
                                ? &it->second.exact_parameter_input_mailbox->sequence_seen
                                : nullptr,
                    };
                });
            if (prepared) {
                // Map each interior node id -> its dense index in the ROUTED
                // plan, to build the skip mask and resolve boundary output slots.
                const auto& rplan = cg->routed.serial.snapshot.plan();
                const auto& rassign = cg->routed.serial.snapshot.buffer_assignment();
                auto routed_index = [&rplan](NodeId id) -> std::uint32_t {
                    for (std::uint32_t i = 0; i < rplan.nodes.size(); ++i) {
                        if (rplan.nodes[i].id == id)
                            return i;
                    }
                    return 0xFFFFFFFFu;
                };
                cg->anticipation.skip_mask.assign(rplan.nodes.size(), 0);
                bool map_ok = true;
                for (const auto idx : partition.interior_nodes) {
                    const std::uint32_t ri = routed_index(cg->executable_nodes[idx].id);
                    if (ri == 0xFFFFFFFFu) {
                        map_ok = false;
                        break;
                    }
                    cg->anticipation.skip_mask[ri] = 1;
                }
                cg->anticipation.prefill.clear();
                for (std::uint32_t ch = 0; map_ok && ch < subgraph.outputs.size(); ++ch) {
                    const auto& out = subgraph.outputs[ch];
                    const std::uint32_t ri = routed_index(out.source_node);
                    if (ri == 0xFFFFFFFFu) {
                        map_ok = false;
                        break;
                    }
                    const std::uint32_t slot = rassign.nodes[ri].output_base + out.source_port;
                    cg->anticipation.prefill.push_back({ch, slot});
                }
                if (map_ok) {
                    cg->anticipation.consume_scratch.assign(
                        subgraph.outputs.size(),
                        std::vector<float>(static_cast<std::size_t>(max_block_size), 0.0f));
                    cg->anticipation.consume_ptrs.clear();
                    for (auto& c : cg->anticipation.consume_scratch) {
                        cg->anticipation.consume_ptrs.push_back(c.data());
                    }
                    cg->anticipation.valid = true;
                }
            }
        }
    }
    return true;
}

} // namespace pulp::host
