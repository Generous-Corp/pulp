# Sampler/looper transport quantization helper. Schedules block-relative
# offsets for immediate, beat, bar, grid, and host-loop boundaries from
# ProcessContext.
pulp_add_test_suite(pulp-test-transport-quantizer LIBRARIES pulp::format)

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

# Backend-neutral waveform GPU/static-layer planning over AudioThumbnail data.
pulp_add_test_suite(pulp-test-waveform-gpu-primitives LIBRARIES pulp::view)

# Backend-neutral waveform GPU render/upload/cache lifecycle orchestration.
pulp_add_test_suite(pulp-test-waveform-gpu-render-controller LIBRARIES pulp::view)

# Concrete CPU/headless consumer for waveform render-controller lifecycle decisions.
pulp_add_test_suite(pulp-test-waveform-headless-render-backend LIBRARIES pulp::view)

# Machine-checkable RT-safety labels for sampler/looper hot paths and off-thread helpers.
pulp_add_test_suite(pulp-test-sampler-rt-safety-contract LIBRARIES pulp::audio)
# Sibling drift check for the core-runtime RT-safety contract registry
# (lock-free primitives, automation queue, graph walk, Processor entry).
pulp_add_test_suite(pulp-test-core-runtime-rt-safety-contract LIBRARIES pulp::audio)

# The canonical GraphRuntimeExecutor gain output must match SignalGraph
# bit-for-bit (regression baseline for the host-graph-on-executor seam).
pulp_add_test_suite(pulp-test-graph-executor-parity
    LIBRARIES pulp::host pulp::format pulp::graph)
# Off-RT scratch-slot buffer-assignment layout + reuse.
pulp_add_test_suite(pulp-test-graph-runtime-buffer-assignment LIBRARIES pulp::graph)
# Executor routing path moves audio between nodes (chain/diamond/feedback/
# multi-output parity vs SignalGraph) and is allocation-free on the RT thread.
pulp_add_test_suite(pulp-test-graph-executor-routing
    SOURCES test_graph_executor_routing.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# A SignalGraph translated to the executor produces bit-identical output to its
# own walk for the eligible node/connection subset.
pulp_add_test_suite(pulp-test-signal-graph-executor-parity
    SOURCES test_signal_graph_executor_parity.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)
# Differential routing parity: random audio-only DAGs driven through both
# SignalGraph (oracle) and the routed executor must agree, fuzzing the gather /
# fan-in / scratch-reuse / feedback paths the fixed shapes above only sample.
pulp_add_test_suite(pulp-test-graph-routing-differential-parity
    SOURCES test_graph_routing_differential_parity.cpp
    LIBRARIES pulp::host pulp::format pulp::graph)

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
