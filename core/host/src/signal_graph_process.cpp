// SignalGraph real-time dispatch and pinned-snapshot process entry points.
// The routed executor and legacy reference walk remain independent backends;
// this file owns only selection, ingress/egress, telemetry, and fail-closed
// fallback policy for an already-published CompiledGraph.

#include "signal_graph_internal.hpp"

#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_execution_snapshot.hpp>
#include <pulp/host/signal_graph_executor_routing.hpp>
#include <pulp/runtime/log.hpp>

#include <algorithm>
#include <cstring>
#include <span>

namespace pulp::host {

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input, int num_samples) {
    process_impl(output, input, num_samples, /*transport=*/nullptr);
}

void SignalGraph::process(audio::BufferView<float>& output,
                          const audio::BufferView<const float>& input, int num_samples,
                          const format::ProcessContext& transport) {
    process_impl(output, input, num_samples, &transport);
}

void SignalGraph::process_impl(audio::BufferView<float>& output,
                               const audio::BufferView<const float>& input, int num_samples,
                               const format::ProcessContext* transport) {
    // See runtime::Slot: its reader count and the raw snapshot
    // pointer form one RCU-style lifetime handshake. Slot::ReadGuard is a
    // private nested struct (signal_graph.hpp) so the control-thread snapshot
    // readers can pin the same way.
    auto read_guard = live_slot_.read();
    process_snapshot_impl(output, input, num_samples, transport, read_guard.get());
}

void SignalGraph::process_snapshot_impl(audio::BufferView<float>& output,
                                        const audio::BufferView<const float>& input,
                                        int num_samples, const format::ProcessContext* transport,
                                        CompiledGraph* cg) {
    // Negative or zero block sizes mean "nothing to do" — return without
    // touching output (a memset with size_t(negative) wraps to a huge size).
    if (num_samples <= 0)
        return;
    if (!cg || num_samples > cg->max_block_size) {
        for (std::size_t c = 0; c < output.num_channels(); ++c)
            std::memset(output.channel_ptr(c), 0, sizeof(float) * static_cast<size_t>(num_samples));
        if (routed_only_execution_owners_.load(std::memory_order_relaxed) != 0)
            routed_only_execution_failures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    SampleRegionExecutionDomain::Admission sample_region_admission;
    if (cg->sample_region_bank) {
        sample_region_admission = cg->sample_region_bank->domain().try_admit(
            cg->sample_region_bank->binding_generation());
        // Contention and stale-snapshot re-entry fail without touching retained
        // state cells. The caller still receives a deterministic silent block.
        if (!sample_region_admission) {
            for (std::size_t c = 0; c < output.num_channels(); ++c)
                std::fill_n(output.channel_ptr(c), num_samples, 0.0f);
            routed_only_execution_failures_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (transport != nullptr && transport->reset_requested)
            cg->sample_region_bank->reset();
    }

    // Bracket the whole block with the graph-level load measurer (RT-safe: begin()/
    // end() are relaxed-atomic timestamps, no alloc/lock). The RAII guard's end()
    // covers every return path below. graph_load() reads this for live-swap admission.
    if (graph_load_)
        graph_load_->begin(num_samples, static_cast<float>(cg->sample_rate));

    // Record one live-DSP telemetry block per process() call, path-agnostic: the
    // routed executor and the legacy walk BOTH time each node into its persistent
    // AudioProcessLoadMeasurer (node_load_) and the whole block into graph_load_.
    // This guard reads those already-populated measurers after the block and pushes
    // a single fixed-slot record — so per-node p50/p95/p99 + jitter + over-budget
    // attribution work on every execution path with no per-node hook in either the
    // executor or the walk. Declared BEFORE graph_load_end_guard so it destructs
    // AFTER it (reverse order): graph_load_->end() has stamped this block's graph
    // elapsed by the time this runs. Inactive at one-branch cost when telemetry is
    // off; the record path is allocation-free (pre-sized scratch + drop-on-full ring).
    struct TelemetryRecordGuard {
        CompiledGraph* cg;
        audio::AudioProcessLoadMeasurer* graph_measurer;
        int num_samples;
        ~TelemetryRecordGuard() {
            auto& store = cg->live_dsp_telemetry;
            if (!store.enabled() || !store.prepared())
                return;
            std::int64_t* scratch = store.external_record_scratch();
            if (scratch == nullptr)
                return;
            const std::uint32_t n = store.node_count();
            for (std::uint32_t i = 0; i < n && i < cg->ordered_runtime.size(); ++i) {
                auto* rt = cg->ordered_runtime[i].runtime;
                auto* m = rt ? rt->load : nullptr;
                scratch[i] = m ? m->last_elapsed_ns() : 0;
            }
            const std::int64_t graph_ns = graph_measurer ? graph_measurer->last_elapsed_ns() : 0;
            store.inject_block(std::span<const std::int64_t>(scratch, n), graph_ns,
                               static_cast<std::uint32_t>(num_samples), cg->sample_rate);
        }
    } telemetry_record_guard{cg, graph_load_.get(), num_samples};

    struct GraphLoadEndGuard {
        audio::AudioProcessLoadMeasurer* m;
        ~GraphLoadEndGuard() {
            if (m)
                m->end();
        }
    } graph_load_end_guard{graph_load_.get()};

    // Routed dispatch (opt-in): try the levelized PARALLEL executor first (if
    // enabled), then the SERIAL executor, then fall through to the legacy walk.
    // Both routed paths run GraphRuntimeExecutor (which zeroes the output bus and
    // accumulates AudioOutput itself, so a successful routed call returns before
    // the legacy zero+walk below) and SHARE the MIDI mailbox bridge (the MIDI
    // scratch + MidiInput/Output node lists are shared — identical plan). The
    // dispatch stays inside the Slot reader-pin, so `cg` (snapshots, pools, gain
    // atomics) is pinned for the whole call. Output is bit-identical across paths.
    {
        const auto frames32 = static_cast<std::uint32_t>(num_samples);
        const bool has_midi = cg->routed.midi.node_count() > 0;
        const bool has_automation = cg->routed.automation.node_count() > 0;

        pulp::format::BusBufferSet buses;
        const bool buses_ok = buses.add_input("main", input, pulp::format::BusRole::Main) &&
                              buses.add_output("main", output, pulp::format::BusRole::Main);
        if (buses_ok) {
            // Anticipation safety (per-node, not blanket): a transport-sensitive
            // node opts in via GraphNode::transport_sensitive, which seeds
            // AnticipationExclusion::TransportSensitive and so excludes that node
            // (and its downstream cone) from the ahead-rendered interior. Every
            // node that IS ahead-rendered is therefore transport-insensitive by
            // construction and ignores block.transport. So the live transport can
            // stay populated even while anticipation is active: forwarding it is
            // inert for the masked interior, and the transport-sensitive nodes
            // that need it always run live/exterior. (Resolved at compile; the
            // count of nodes forced exterior is in
            // transport_suppressed_for_anticipation().)
            pulp::format::ProcessBlock block;
            block.sample_rate = cg->sample_rate;
            block.frame_count = frames32;
            block.buses = &buses;
            if (transport != nullptr) {
                // Deliver transport + process_mode + render_speed_hint to
                // Processors via *block.transport. block.sample_rate stays sourced
                // from cg (the prepared rate is authoritative). render_speed is left
                // at 1.0: render_speed_hint is a categorical hint that reaches
                // Processors through *block.transport, not a numeric multiplier.
                block.transport = transport;
                block.mode = transport->process_mode;
            }

            // Run one routed path with the shared MIDI mailbox bridge around it.
            // `run` returns the executor result; this returns true iff routing
            // succeeded (took the path). On failure the consumed MIDI sequences
            // are NOT committed, so a fallback path re-consumes the same block.
            auto dispatch_routed = [&](auto&& run) -> bool {
                if (!block.validate())
                    return false;
                reset_plugin_parameter_event_sequences(cg->routed.serial.plugin_ctx);
                reset_plugin_parameter_event_sequences(cg->routed.parallel.plugin_ctx);
                for (auto& processor : cg->routed.processor_parameter_inputs) {
                    processor.pending_seq = 0;
                    auto* queue = cg->routed.automation.events(processor.plan_index);
                    auto runtime_it = cg->runtime.find(processor.id);
                    if (queue == nullptr || runtime_it == cg->runtime.end())
                        continue;
                    queue->clear();
                    const auto pending =
                        append_parameter_mailbox_events_(&runtime_it->second, *queue);
                    processor.pending_seq = pending.exact;
                }
                if (has_midi) {
                    for (auto& mi : cg->routed.midi_inputs) {
                        mi.pending_seq = 0;
                        midi::MidiBuffer* out_buf = cg->routed.midi.out(mi.plan_index);
                        if (out_buf == nullptr)
                            continue;
                        clear_midi_block(*out_buf);
                        cg->routed.midi.set_out_incomplete(mi.plan_index, false);
                        auto rt_it = cg->runtime.find(mi.id);
                        if (rt_it == cg->runtime.end() || !rt_it->second.midi_input_mailbox) {
                            continue;
                        }
                        const auto& injected = rt_it->second.midi_input_mailbox->published.read();
                        const auto sequence_seen =
                            rt_it->second.midi_input_mailbox->sequence_seen.load(
                                std::memory_order_relaxed);
                        if (injected.sequence != 0 && injected.sequence != sequence_seen) {
                            cg->routed.midi.set_out_incomplete(mi.plan_index,
                                                               !injected.copy_to_midi(*out_buf));
                            mi.pending_seq = injected.sequence;
                        }
                    }
                }
                if (!run().ok())
                    return false;
                commit_plugin_parameter_event_sequences(cg->routed.serial.plugin_ctx);
                commit_plugin_parameter_event_sequences(cg->routed.parallel.plugin_ctx);
                for (const auto& processor : cg->routed.processor_parameter_inputs) {
                    if (processor.pending_seq == 0)
                        continue;
                    auto runtime_it = cg->runtime.find(processor.id);
                    if (runtime_it != cg->runtime.end() &&
                        runtime_it->second.exact_parameter_input_mailbox) {
                        runtime_it->second.exact_parameter_input_mailbox->sequence_seen.store(
                            processor.pending_seq, std::memory_order_relaxed);
                    }
                }
                if (has_midi) {
                    for (const auto& mi : cg->routed.midi_inputs) {
                        if (mi.pending_seq == 0)
                            continue;
                        auto rt_it = cg->runtime.find(mi.id);
                        if (rt_it != cg->runtime.end() && rt_it->second.midi_input_mailbox) {
                            rt_it->second.midi_input_mailbox->sequence_seen.store(
                                mi.pending_seq, std::memory_order_relaxed);
                        }
                    }
                    for (const auto& mo : cg->routed.midi_outputs) {
                        midi::MidiBuffer* in_buf = cg->routed.midi.in(mo.plan_index);
                        auto rt_it = cg->runtime.find(mo.id);
                        if (in_buf == nullptr || rt_it == cg->runtime.end() ||
                            !rt_it->second.midi_output_mailbox) {
                            continue;
                        }
                        cg->midi_publish_scratch.set_from_midi(
                            *in_buf, 0, cg->routed.midi.in_incomplete(mo.plan_index));
                        if (cg->midi_publish_scratch.has_payload() &&
                            !rt_it->second.midi_output_mailbox->pending.try_push(
                                cg->midi_publish_scratch)) {
                            rt_it->second.midi_output_mailbox->incomplete.store(
                                true, std::memory_order_relaxed);
                        }
                    }
                }
                return true;
            };

            // worker_pool_.running() is a plain flag read here; it is safe without
            // a drain handshake only because running_ is flipped false exactly
            // once, by ~GraphRuntimeWorkerPool, when no audio-thread reader exists
            // (see the compile_ start invariant). Each dispatch is idempotent: it
            // re-clears the MIDI ingress buffers and does not commit consumed
            // sequences until run() succeeds, and every executor zeroes the output
            // bus before accumulating — so a parallel attempt that returns false
            // (a node failed, or it does not fit) can fall through to the serial
            // executor and then the legacy walk re-rendering the same block with
            // no double-consumed MIDI and no doubled output.
            // Anticipative rendering (tried first): the lane has pre-rendered the
            // interior off the audio thread. Consume one block, fill the interior
            // boundary-source output slots with it (or zero them on an underrun /
            // block-size mismatch — NEVER re-run the interior, whose plugin state
            // the producer owns), then run the routed walk with the interior masked
            // so only the exterior runs live. Uses the serial routed snapshot.
            if (anticipation_enabled_.load(std::memory_order_relaxed) && cg->anticipation.valid &&
                cg->anticipation.lane.output_channels() == cg->anticipation.prefill.size()) {
                // STRUCTURALLY TERMINAL once anticipation is valid: the interior's
                // plugin state is advanced solely by the producer (pump_anticipation),
                // so the live path must NEVER run the interior — not via a fallback,
                // and not even when the pool can't fit the block. The fits() check is
                // INSIDE the branch (not an entry gate) precisely so a future change
                // that made it false could not let control fall through to the
                // parallel/legacy paths below, which would run the masked interior
                // live and double-advance producer-owned state. Worst case here is a
                // silent block, never a double-render.
                bool ok = false;
                if (cg->routed.serial.pool.fits(cg->routed.serial.snapshot, frames32)) {
                    const bool size_ok = frames32 == static_cast<std::uint32_t>(
                                                         cg->anticipation.lane.block_frames());
                    bool hit = false;
                    if (size_ok) {
                        pulp::audio::BufferView<float> cap(cg->anticipation.consume_ptrs.data(),
                                                           cg->anticipation.consume_ptrs.size(),
                                                           frames32);
                        hit = cg->anticipation.lane.consume(cap);
                    }
                    for (const auto& pf : cg->anticipation.prefill) {
                        float* dst = cg->routed.serial.pool.slot_data(pf.slot);
                        if (dst == nullptr)
                            continue;
                        if (hit) {
                            std::copy_n(cg->anticipation.consume_ptrs[pf.out_channel], num_samples,
                                        dst);
                        } else {
                            std::fill_n(dst, num_samples, 0.0f);
                        }
                    }
                    ok = dispatch_routed([&] {
                        return executor_.process_routed(
                            block, cg->routed.serial.snapshot, cg->routed.serial.pool,
                            has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr, {}, {}, {}, {},
                            cg->anticipation.skip_mask);
                    });
                }
                if (!ok) {
                    for (std::size_t c = 0; c < output.num_channels(); ++c) {
                        std::memset(output.channel_ptr(c), 0,
                                    sizeof(float) * static_cast<std::size_t>(num_samples));
                    }
                }
                return;
            }

            // Reached only when anticipation is NOT valid (the branch above is
            // terminal whenever it is) — REQUIRED for safety: the parallel and
            // legacy paths run every node, including any interior the producer
            // owns, so they must never execute while anticipation is active.
            // Observability for a SILENT degradation the parity test cannot see:
            // when a routed path is ELIGIBLE (enabled + a valid routed snapshot +
            // the pool fits) but its dispatch returns false, control falls through
            // to the legacy walk. Because the walk is BOTH the oracle and the
            // fallback, that produces no divergence — so an eligible graph that
            // stopped routing would be invisible. Flag it: count it (relaxed,
            // RT-safe) and warn in debug builds. A normal fallback (routing
            // disabled, ineligible, or build failure → routed.serial.valid false) does
            // NOT set this, so the counter isolates the should-have-routed case.
            bool routed_eligible_dispatch_failed = false;

            const bool use_parallel =
                cg->pdc_execution_domain == PdcExecutionDomain::RoutedParallel ||
                (cg->pdc_execution_domain == PdcExecutionDomain::Dynamic &&
                 parallel_routing_enabled_.load(std::memory_order_relaxed));
            const bool use_serial =
                !cg->processors.empty() ||
                cg->pdc_execution_domain == PdcExecutionDomain::RoutedSerial ||
                (cg->pdc_execution_domain == PdcExecutionDomain::Dynamic &&
                 canonical_executor_routing_enabled_.load(std::memory_order_relaxed));

            if (use_parallel && cg->routed.parallel.valid && worker_pool_.running() &&
                cg->routed.parallel.pool.fits(cg->routed.parallel.snapshot, frames32)) {
                if (dispatch_routed([&] {
                        return executor_.process_parallel(
                            block, cg->routed.parallel.snapshot, cg->routed.parallel.levelization,
                            cg->routed.parallel.pool, worker_pool_,
                            has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr);
                    })) {
                    return;
                }
                routed_eligible_dispatch_failed = true; // eligible but did not route
            }

            if (use_serial && cg->routed.serial.valid &&
                cg->routed.serial.pool.fits(cg->routed.serial.snapshot, frames32)) {
                if (dispatch_routed([&] {
                        return executor_.process_routed(
                            block, cg->routed.serial.snapshot, cg->routed.serial.pool,
                            has_midi ? &cg->routed.midi : nullptr,
                            has_automation ? &cg->routed.automation : nullptr);
                    })) {
                    return;
                }
                routed_eligible_dispatch_failed = true; // eligible but did not route
            }

            if (routed_eligible_dispatch_failed &&
                routed_only_execution_owners_.load(std::memory_order_relaxed) == 0) {
                const std::uint64_t prev =
                    routed_walk_fallbacks_.fetch_add(1, std::memory_order_relaxed);
                (void)prev;
#ifndef NDEBUG
                // Warn ONCE per graph (the counter carries the full tally) — the
                // log path is not RT-safe, so don't repeat it every block.
                if (prev == 0) {
                    runtime::log_warn(
                        "SignalGraph: routed dispatch failed for an eligible graph; "
                        "falling back to the legacy walk (see routed_walk_fallbacks())");
                }
#endif
            }
        }
        // Any setup failure / disabled path falls through to the legacy walk.
    }

    // Authored Processor nodes have only the routed ProcessBlock binding. Never
    // reinterpret one as the unresolved-plugin pass-through used by the legacy
    // reference walk; a routed failure is a fail-closed silent block.
    if (!cg->processors.empty()) {
        output.clear();
        routed_only_execution_failures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (routed_only_execution_owners_.load(std::memory_order_relaxed) != 0) {
        output.clear();
        routed_only_execution_failures_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // No routed path took this block — run the legacy serial reference walk,
    // the hand-maintained bit-exact oracle/fallback (see
    // signal_graph_reference_walk.cpp).
    run_reference_walk_(output, input, num_samples, cg);
}

void SignalGraph::ExecutionSnapshot::process(audio::BufferView<float>& output,
                                             const audio::BufferView<const float>& input,
                                             int num_samples) const noexcept {
    if (owner_ == nullptr || snapshot_ == nullptr) {
        if (num_samples > 0)
            output.clear();
        return;
    }
    owner_->process_snapshot_impl(output, input, num_samples, nullptr, snapshot_.get());
}

void SignalGraph::ExecutionSnapshot::process(
    audio::BufferView<float>& output, const audio::BufferView<const float>& input, int num_samples,
    const format::ProcessContext& transport) const noexcept {
    if (owner_ == nullptr || snapshot_ == nullptr) {
        if (num_samples > 0)
            output.clear();
        return;
    }
    owner_->process_snapshot_impl(output, input, num_samples, &transport, snapshot_.get());
}

} // namespace pulp::host
