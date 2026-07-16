# Sampler/looper transport quantization helper. Schedules block-relative
# offsets for immediate, beat, bar, grid, and host-loop boundaries from
# ProcessContext.
pulp_add_test_suite(pulp-test-transport-quantizer LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-timebase LIBRARIES pulp::timebase TIMEOUT 60)

# Sample asset drop target adapter over cheap extension classification.
pulp_add_test_suite(pulp-test-sample-asset-drop-target LIBRARIES pulp::view)

# Additive process-block contract for graph/offline/sampler runtime paths.
pulp_add_test_suite(pulp-test-process-block LIBRARIES pulp::format)

# Release-safe no-allocation probes for graph/event/sampler DSP hot paths.
pulp_add_test_suite(pulp-test-dsp-runtime-no-alloc
    SOURCES test_dsp_runtime_no_alloc.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format pulp::audio pulp::graph pulp::midi pulp::state pulp::signal)

# Deterministic multi-block offline rendering over HeadlessHost.
pulp_add_test_suite(pulp-test-offline-render-host LIBRARIES pulp::format)

# Portable host/runtime matrix for automation, buses, events, state, latency, and offline bounce.
pulp_add_test_suite(pulp-test-host-runtime-matrix LIBRARIES pulp::format)

# Offline bounce-to-sample policy and sample-slot publication.
pulp_add_test_suite(pulp-test-offline-sample-bounce LIBRARIES pulp::format)

# Backend-neutral waveform GPU/static-layer planning over WaveformOverview data.
pulp_add_test_suite(pulp-test-waveform-gpu-primitives LIBRARIES pulp::view)

# Backend-neutral waveform GPU render/upload/cache lifecycle orchestration.
pulp_add_test_suite(pulp-test-waveform-gpu-render-controller LIBRARIES pulp::view)

# Concrete CPU/headless consumer for waveform render-controller lifecycle decisions.
pulp_add_test_suite(pulp-test-waveform-headless-render-backend LIBRARIES pulp::view)

# Machine-checkable RT-safety labels for sampler/looper hot paths and off-thread helpers.
pulp_add_test_suite(pulp-test-sampler-rt-safety-contract LIBRARIES pulp::audio)

# Resident, manually-pumped, and prepared-page transport must remain
# bit-identical across callback partitions, preload/page boundaries, seeks,
# direction changes, reset, and channel mapping.
pulp_add_test_suite(pulp-test-sampler-stream-parity
    SOURCES test_sampler_stream_parity.cpp
            test_sampler_page_transport_parity.cpp
            support/sampler_parity.cpp
            support/sample_page_transport_parity.cpp
            harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sampler-loop-oracle
    SOURCES test_sampler_loop_oracle.cpp
            support/sampler_loop_parity.cpp
    LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sampler-loop-cursor-parity
    SOURCES test_sampler_loop_cursor_parity.cpp
            support/sampler_loop_parity.cpp
    LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sample-stream-service
    SOURCES test_sample_stream_service.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sample-stream-async-service
    SOURCES test_sample_stream_async_service.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sample-stream-decode-pool
    SOURCES test_sample_stream_decode_pool.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sample-stream-voice-reader
    SOURCES test_sample_stream_voice_reader.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sample-asset LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sample-starvation-envelope
    SOURCES test_sample_starvation_envelope.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)
# Sibling drift check for the core-runtime RT-safety contract registry
# (lock-free primitives, automation queue, graph walk, Processor entry).
pulp_add_test_suite(pulp-test-core-runtime-rt-safety-contract LIBRARIES pulp::audio)

# The canonical GraphRuntimeExecutor gain output must match SignalGraph
# bit-for-bit (regression baseline for the host-graph-on-executor seam).
pulp_add_test_suite(pulp-test-graph-executor-parity
    LIBRARIES pulp::host pulp::format pulp::graph)
# Off-RT scratch-slot buffer-assignment layout + reuse.
pulp_add_test_suite(pulp-test-graph-runtime-buffer-assignment LIBRARIES pulp::graph)
# Off-RT levelization (parallel-schedule levels) for static multicore.
pulp_add_test_suite(pulp-test-graph-runtime-levelization LIBRARIES pulp::graph)
# Persistent fork-join worker pool for the levelized parallel executor.
pulp_add_test_suite(pulp-test-graph-runtime-worker-pool
    SOURCES test_graph_runtime_worker_pool.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::format)
target_compile_definitions(pulp-test-graph-runtime-worker-pool
    PRIVATE PULP_GRAPH_RUNTIME_WORKER_POOL_TEST_HOOKS=1)
# Executor routing path moves audio between nodes (chain/diamond/feedback/
# multi-output parity vs SignalGraph) and is allocation-free on the RT thread.
pulp_add_test_suite(pulp-test-graph-executor-routing
    SOURCES test_graph_executor_routing.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# A SignalGraph translated to the executor produces bit-identical output to its
# own walk for the eligible node/connection subset.
pulp_add_test_suite(pulp-test-crossfade-plugin-slot
    SOURCES test_crossfade_plugin_slot.cpp
    LIBRARIES pulp::host pulp::signal pulp::audio)
pulp_add_test_suite(pulp-test-live-swap-admission
    SOURCES test_live_swap_admission.cpp
    LIBRARIES pulp::host pulp::audio)
pulp_add_test_suite(pulp-test-signal-graph-live-swap-staging
    SOURCES test_signal_graph_live_swap_staging.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio)
pulp_add_test_suite(pulp-test-signal-graph-executor-parity
    SOURCES test_signal_graph_executor_parity.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# SignalGraph's multi-path routing renders an arithmetic oracle bit-exactly on
# the legacy walk, the routed serial path, and the routed parallel path, and the
# paths stay independent across block partitionings and an aborted swap edit.
# audio_signal_generators.cpp is compiled directly rather than linking
# pulp-audio-test-support: that library is defined in app_audio_host_tests.cmake,
# which test/CMakeLists.txt includes AFTER this manifest.
pulp_add_test_suite(pulp-test-signal-graph-audio-parity
    SOURCES test_signal_graph_audio_parity.cpp support/audio_signal_generators.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio pulp::audio-analysis)
# Offline-equals-online: OfflineSignalGraphHost renders a deterministic graph at
# different block partitionings and the harness proves block-partitioning
# invariance (with a declared-exemption path for block-size-dependent nodes).
pulp_add_test_suite(pulp-test-signal-graph-offline-parity
    SOURCES test_signal_graph_offline_parity.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# A SignalGraph baked into one shippable Processor (BakedGraphProcessor) drives a
# frozen plan through the SAME GraphRuntimeExecutor::process_routed as the live
# graph, so its output is bit-identical for the lowerable subset; non-lowerable
# graphs are refused loudly.
pulp_add_test_suite(pulp-test-baked-codec
    SOURCES test_baked_codec.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio pulp::runtime)
pulp_add_test_suite(pulp-test-baked-graph-processor-parity
    SOURCES test_baked_graph_processor_parity.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::runtime)
# Parallel SignalGraph plugin bindings must not share fallback MIDI/parameter
# scratch when the routed executor runs same-level Plugin nodes concurrently.
pulp_add_test_suite(pulp-test-signal-graph-parallel-plugin-scratch
    SOURCES test_signal_graph_parallel_plugin_scratch.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# Host-facing coverage for the parallel executor's break-even threshold wiring.
pulp_add_test_suite(pulp-test-signal-graph-parallel-cost
    LIBRARIES pulp::host pulp::format pulp::graph)
# Differential routing parity: random audio-only DAGs driven through both
# SignalGraph (oracle) and the routed executor must agree, fuzzing the gather /
# fan-in / scratch-reuse / feedback paths the fixed shapes above only sample.
pulp_add_test_suite(pulp-test-graph-routing-differential-parity
    SOURCES test_graph_routing_differential_parity.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# Pins the single connection-classification surface (classify): every host
# Connection variant maps to one runtime lane, so the routed gather and the
# reference-walk bucketer can never drift apart.
pulp_add_test_suite(pulp-test-connection-classify
    SOURCES test_connection_classify.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# Anticipative-rendering safety contract: the static eligibility analysis must
# exclude every live-input / feedback / sidechain-dependent node and propagate
# those exclusions downstream, so no unsafe subgraph is ever rendered ahead.
pulp_add_test_suite(pulp-test-anticipation-eligibility
    SOURCES test_anticipation_eligibility.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# The eligible interior + boundary edges carved out for anticipative rendering:
# live sinks stay out of the interior, every interior->outside edge is a splice
# point, and a live-only graph yields nothing worth anticipating.
pulp_add_test_suite(pulp-test-anticipation-partition
    SOURCES test_anticipation_partition.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# The renderable sub-graph carved from a partition: interior nodes + internal
# edges + one synthesized AudioOutput sink whose input ports carry the distinct
# boundary outputs, with a fresh non-colliding id.
pulp_add_test_suite(pulp-test-anticipation-subgraph
    SOURCES test_anticipation_subgraph.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# Render-path proof: the extracted sub-graph, driven through the real executor,
# reproduces the full graph's boundary signals with each captured port on its own
# output channel (the guard the structural extraction tests cannot give).
pulp_add_test_suite(pulp-test-anticipation-subgraph-render
    SOURCES test_anticipation_subgraph_render.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# Render-ahead lane: pre-render the eligible sub-graph into a ring off the audio
# thread and consume pre-rendered blocks on it — consumed output matches a
# synchronous render, underruns cleanly, allocates nothing on consume, and the
# producer/consumer pair is race-free.
pulp_add_test_suite(pulp-test-anticipation-lane
    SOURCES test_anticipation_lane.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio)
# Acceptance: anticipation wired into SignalGraph::process — the pre-rendered
# interior splice is bit-identical to the canonical interior-live render, and the
# interior is advanced exactly once per block (by the producer pump), never twice.
pulp_add_test_suite(pulp-test-signal-graph-anticipation
    SOURCES test_signal_graph_anticipation.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio)
# 2.2a no-silence-swap contract: compile_() runs race-free against a live
# process() (TSan). Proves compile_() never mutates state the audio thread reads —
# the invariant the compile-first prepared-swap (2.2b) is built on.
pulp_add_test_suite(pulp-test-signal-graph-prepared-swap
    SOURCES test_signal_graph_prepared_swap.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio)
# 2.2b prerequisite (H2): compile_()/routing read cached plugin metadata, never
# the live PluginSlot — a CountingSlot asserts zero metadata calls after prepare()
# + a compile-vs-process race check on a plugin-bearing graph.
pulp_add_test_suite(pulp-test-signal-graph-metadata-cache
    SOURCES test_signal_graph_metadata_cache.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio)
# Per-node live-DSP telemetry wired through SignalGraph: default-off, per-node
# recording on the reference walk, live toggle without recompile, no audio-path alloc.
pulp_add_test_suite(pulp-test-live-dsp-telemetry-graph
    SOURCES test_live_dsp_telemetry_graph.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio pulp::runtime)
# 2.2b: the no-silence LIVE swap — begin_swap_edit/prepare_swap publishes a
# reinit-free gain-graph edit with no silent block under a concurrent render, and
# rejects non-reinit-free edits (SR change / removed node / anticipation) as
# NeedsEagerPrepare.
pulp_add_test_suite(pulp-test-signal-graph-prepared-swap-live
    SOURCES test_signal_graph_prepared_swap_live.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio)
# Feed-forward PDC delay history survives a gap-free structural edit in the
# legacy walk and both routed execution domains. A 97-sample delay rendered in
# 64-frame blocks catches cursor resets that block-aligned fixtures would miss.
pulp_add_test_suite(pulp-test-signal-graph-pdc-swap-continuity
    SOURCES test_signal_graph_pdc_swap_continuity.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio)
# Transport plumbing for SignalGraph::process: the transport-aware overload is
# bit-identical for transport-inert routed nodes, populates the routed block so a
# ProcessorNode consumer receives the host transport / process_mode / render-speed
# hint, and suppresses (and counts) the transport whenever anticipation is active.
pulp_add_test_suite(pulp-test-signal-graph-transport
    SOURCES test_signal_graph_transport.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio pulp::state)

# Live plugin-instance swap produces no dropout/xrun and a sample-continuous
# output across the swap block, for every hosted format. CI-runnable mirror of
# the local-only REAPER live-plugin-swap smoke.
pulp_add_test_suite(pulp-test-signal-graph-live-swap-continuity
    SOURCES test_signal_graph_live_swap_continuity.cpp
    LIBRARIES pulp::host pulp::format pulp::graph pulp::audio)

# First sampler/looper storage primitives split by ownership so failures point
# to the actual layer instead of a catch-all primitive bucket.
pulp_add_test_suite(pulp-test-planar-audio-ring-buffer LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-rolling-audio-capture LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-sample-slot-bank-store LIBRARIES pulp::audio)
pulp_add_test_suite(pulp-test-realtime-sample-recorder LIBRARIES pulp::audio)

# Resampled generated-audio handoff accounting and split-pull regressions.
pulp_add_test_suite(pulp-test-audio-stream-handoff LIBRARIES pulp::audio)

# Sampler key/pitch/slice mapping policy kept out of renderers and slot storage.
pulp_add_test_suite(pulp-test-sample-key-map LIBRARIES pulp::audio)

# Undoable sample edit document metadata kept separate from audio storage/import/export.
pulp_add_test_suite(pulp-test-sample-edit-document LIBRARIES pulp::audio)

# Sample asset import/export policy and platform-neutral drop classification.
pulp_add_test_suite(pulp-test-sample-asset-io LIBRARIES pulp::audio)

# Prepared sample-pool resolver over existing published sample stores.
pulp_add_test_suite(pulp-test-sample-pool LIBRARIES pulp::audio)

# Prepared planar page/window storage for streaming sample playback.
pulp_add_test_suite(pulp-test-sample-stream-window LIBRARIES pulp::audio)

# Sampler zone selection policy for key/velocity ranges, round-robin, slices, and keytracking.
pulp_add_test_suite(pulp-test-sample-zone-map LIBRARIES pulp::audio)

# Scalar sample voice rendering from sample-pool resolutions.
pulp_add_test_suite(pulp-test-sample-voice-renderer LIBRARIES pulp::audio)

# Prepared per-voice modulation lane storage for sampler/instrument renderers.
pulp_add_test_suite(pulp-test-voice-modulation-buffer LIBRARIES pulp::audio)

# SIMD-backed voice scratch summing for sampler/instrument renderers.
pulp_add_test_suite(pulp-test-voice-sum-mixer LIBRARIES pulp::audio)

# Pool-backed instrument trigger resolver over sample zones.
pulp_add_test_suite(pulp-test-instrument-runtime LIBRARIES pulp::audio)

# AHDSR/ADSR envelope primitive for future sample voices.
pulp_add_test_suite(pulp-test-instrument-envelope LIBRARIES pulp::audio)

# Prepared voice-slot allocation, stealing, release, and choke-group policy.
pulp_add_test_suite(pulp-test-instrument-voice-allocator LIBRARIES pulp::audio)

# Loop metadata validation and off-RT loop candidate analysis.
pulp_add_test_suite(pulp-test-loop-analysis LIBRARIES pulp::audio)

# Built-in onset detection and slice-map analysis primitives.
pulp_add_test_suite(pulp-test-onset-slice-analysis LIBRARIES pulp::audio)

# Analysis provider descriptors, package availability policy, and provenance sidecars.
pulp_add_test_suite(pulp-test-analyzer-provider LIBRARIES pulp::audio)

# Built-in package-free key/tempo analyzer baseline.
pulp_add_test_suite(pulp-test-built-in-key-tempo-analyzer LIBRARIES pulp::audio)

# Built-in package-free transient classification baseline.
pulp_add_test_suite(pulp-test-built-in-transient-classifier LIBRARIES pulp::audio)

# Optional time/pitch processor contract and Signalsmith Stretch package adapter.
pulp_add_test_suite(pulp-test-time-pitch-processor LIBRARIES pulp::audio)

# Loop reader and renderer primitives, including interpolation, fades, and crossfades.
pulp_add_test_suite(pulp-test-loop-rendering LIBRARIES pulp::audio)
# PF-2 null test: wrap-crossfade output is bit-exact after hoisting the
# equal-power gains to one per-frame CrossfadePlan.
pulp_add_test_suite(pulp-test-loop-renderer-crossfade-null
    SOURCES test_loop_renderer_crossfade_null.cpp LIBRARIES pulp::audio)

# End-to-end synthetic generated looper harness over core primitives only.
pulp_add_test_suite(pulp-test-sampler-looper-integration LIBRARIES pulp::audio)

# Fixed-capacity graph command/event queues for runtime v2 handoff.
pulp_add_test_suite(pulp-test-graph-runtime-queue LIBRARIES pulp::graph)

# Dense graph runtime plan and bounded topology validation for runtime v2.
pulp_add_test_suite(pulp-test-graph-runtime-plan LIBRARIES pulp::graph)

if(TARGET pulp-host)
    # Compatibility coverage for legacy host graph-runtime include paths.
    pulp_add_test_suite(pulp-test-graph-runtime-host-compat
        SOURCES test_graph_runtime_host_compat.cpp
        LIBRARIES pulp::host)
endif()

# Additive ProcessBlock graph executor/snapshot primitive for runtime v2.
pulp_add_test_suite(pulp-test-graph-runtime-executor
    LIBRARIES pulp::format)

# ProcessBlock to legacy Processor::process() adapter for migration compatibility.
pulp_add_test_suite(pulp-test-processor-block-adapter
    LIBRARIES pulp::format)

# I2 parity: the same Processor produces identical output standalone (HeadlessHost)
# and as an in-graph ProcessorNode driven through the routed GraphRuntimeExecutor.
pulp_add_test_suite(pulp-test-processor-node-adapter
    SOURCES test_processor_node_adapter.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)

# A generated/native DSP core (C-ABI native_core) reaches a graph through the
# SAME ProcessorNode path as any Processor — no separate generated-DSP runtime.
# Standalone (HeadlessHost) and in-graph (ProcessorNode) output must be bit-exact.
pulp_add_test_suite(pulp-test-generated-dsp-graph-parity
    SOURCES test_generated_dsp_graph_parity.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
