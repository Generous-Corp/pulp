# Core runtime, canvas, scheduler, and signal-graph test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpInspectorShipping.cmake)

# Analytics tests
pulp_add_test_suite(pulp-test-analytics LIBRARIES pulp::runtime)

# runtime::Slot<T> / runtime::Handoff<T> — the two real-time publication modes.
# Includes N-thread hammer tests that assert reclamation never runs on the
# reader/consumer thread.
pulp_add_test_suite(pulp-test-runtime-slot
    SOURCES test_runtime_slot.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::runtime)

# Tracing subsystem guard (Perfetto, dev-only). Asserts a default build has
# tracing OFF. When PULP_TRACING=OFF, also nm-scan the binary to prove no
# Perfetto symbols leaked (best-effort; mirrors the AssertNoJsSymbols guard).
pulp_add_test_suite(pulp-test-tracing LIBRARIES pulp::runtime)
# Session lifecycle + macro smoke. Config-agnostic: OFF verifies the no-op
# contract; ON emits spans from two threads and byte-checks the flushed trace.
pulp_add_test_suite(pulp-test-tracing-session LIBRARIES pulp::runtime)

# Perfetto auto-flush timeout: cancel-before-deadline, join-on-cancel (the
# property that makes plug-in module unload safe), re-arm replacing rather than
# adding a timer, and the session-generation guard that stops a stale timer
# truncating a later capture. Runs in the DEFAULT PULP_TRACING=OFF build.
pulp_add_test_suite(pulp-test-trace-timeout LIBRARIES pulp::runtime)
if(NOT PULP_TRACING AND NOT WIN32)
    add_custom_command(TARGET pulp-test-tracing POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DBIN=$<TARGET_FILE:pulp-test-tracing>
            -P ${CMAKE_SOURCE_DIR}/tools/cmake/AssertNoTracingSymbols.cmake
        VERBATIM
        COMMENT "Verifying default build has no Perfetto symbols")
endif()

# Crypto tests (SHA-256, MD5, AES, machine ID)
pulp_add_test_suite(pulp-test-crypto LIBRARIES pulp::runtime)

# Ed25519 (RFC 8032).
pulp_add_test_suite(pulp-test-ed25519 LIBRARIES pulp::runtime)

# IPC (InterprocessConnection) tests
add_executable(pulp-connected-child-process-fixture
    fixtures/connected_child_process_fixture.cpp)
target_link_libraries(pulp-connected-child-process-fixture PRIVATE pulp::events)

if(WIN32)
    pulp_add_test_suite(pulp-test-ipc
        LIBRARIES pulp::events pulp::runtime
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-ipc LIBRARIES pulp::events pulp::runtime)
endif()
add_dependencies(pulp-test-ipc pulp-connected-child-process-fixture)
target_compile_definitions(pulp-test-ipc PRIVATE
    "PULP_TEST_CONNECTED_CHILD_FIXTURE=\"$<TARGET_FILE:pulp-connected-child-process-fixture>\"")
catch_discover_tests(pulp-test-ipc
    TEST_SPEC "[lifecycle]"
    TEST_PREFIX "lifecycle::"
    PROPERTIES LABELS lifecycle)

pulp_add_test_suite(pulp-test-ipc-endpoints LIBRARIES pulp::events pulp::runtime)

# AttributedString and TextLayout tests
pulp_add_test_suite(pulp-test-attributed-string LIBRARIES pulp::canvas)

# i18n translation tests
pulp_add_test_suite(pulp-test-i18n LIBRARIES pulp::runtime)

# StateTree and ObservableValue tests
pulp_add_test_suite(pulp-test-state-tree LIBRARIES pulp::state pulp::runtime)

# GUI component tests (table, toolbar, concertina, buttons, lasso)
pulp_add_test_suite(pulp-test-gui-components LIBRARIES pulp::view)

pulp_add_test_suite(pulp-test-table-list-box LIBRARIES pulp::view)

# CodeEditor / FileBasedDocument / RecentlyOpenedFilesList tests
pulp_add_test_suite(pulp-test-code-editor LIBRARIES pulp::view)

# CodeEditor per-language tokenizer coverage.
pulp_add_test_suite(pulp-test-code-editor-tokenizer LIBRARIES pulp::view)

# PropertiesFile JSON persistence tests
pulp_add_test_suite(pulp-test-properties SOURCES test_properties_file.cpp LIBRARIES pulp::state pulp::runtime)

# AudioDeviceManager persistence + MIDI hub.
pulp_add_test_suite(pulp-test-audio-device-manager
    SOURCES test_audio_device_manager.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio pulp::midi pulp::state pulp::runtime)

# DSP enhancement tests (dry/wet mixer, processor duplicator, matrix, special functions)
pulp_add_test_suite(pulp-test-dsp-enhancements LIBRARIES pulp::signal)

# Elliptic / Jacobi special functions (pulp::signal::special). Kept in its
# own binary so future elliptic IIR design tests can live
# beside it without bloating pulp-test-dsp-enhancements.
pulp_add_test_suite(pulp-test-special-functions LIBRARIES pulp::signal)

# Animation & 3D math tests
pulp_add_test_suite(pulp-test-animation-3d LIBRARIES pulp::view)

# AnimatorSet / 3D types tests
pulp_add_test_suite(pulp-test-animator-set LIBRARIES pulp::view)

# Runtime utility tests (mmap, temp file, dynlib, base64, range, child process)
pulp_add_test_suite(pulp-test-runtime-utils LIBRARIES pulp::runtime pulp-cpp-httplib)

# HTTP headers, incremental response delivery, cancellation, and compatibility.
pulp_add_test_suite(pulp-test-http LIBRARIES pulp::runtime pulp-cpp-httplib)

# MAC address parser/formatter.
pulp_add_test_suite(pulp-test-mac-address LIBRARIES pulp::runtime)

# URL parser + percent-encoding + query helpers.
pulp_add_test_suite(pulp-test-url LIBRARIES pulp::runtime)

# Result<T,E> header-only utility.
pulp_add_test_suite(pulp-test-runtime-result LIBRARIES pulp::runtime)

# XML and ZIP/GZIP compression tests
pulp_add_test_suite(pulp-test-xml-zip LIBRARIES pulp::runtime)

# SIMD operations and aligned buffer tests
pulp_add_test_suite(pulp-test-simd LIBRARIES pulp::runtime pulp::signal)

# Drag-and-drop tests
pulp_add_test_suite(pulp-test-dnd
    SOURCES test_drag_drop.cpp test_drag_session_lifetime.cpp
    LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-musical-typing SOURCES test_musical_typing.cpp LIBRARIES pulp::view)

# OSC tests
pulp_add_test_suite(pulp-test-osc LIBRARIES pulp::osc)

# ImageConvolutionKernel tests
pulp_add_test_suite(pulp-test-image-convolution LIBRARIES pulp::canvas)

# RectangleList geometry tests
pulp_add_test_suite(pulp-test-rectangle-list LIBRARIES pulp::canvas)

# OSC Bundle serialization tests
pulp_add_test_suite(pulp-test-osc-bundle LIBRARIES pulp::osc)

# SVG tests
pulp_add_test_suite(pulp-test-svg LIBRARIES pulp::canvas)

# VectorScene retained scene graph. Pulp-native names; covers
# SceneRect/SceneTransform math, every SceneNode kind's local_bounds +
# paint command stream, SVG ingest into a SceneGroup, and the dirty-rect
# contract (mutating one child's opacity reports a rect bounded by that
# sub-tree only).
pulp_add_test_suite(pulp-test-vector-scene LIBRARIES pulp::canvas)

# Canvas image placement: affine transform, preserve-aspect fit, tiled fill.
pulp_add_test_suite(pulp-test-canvas-image-fit LIBRARIES pulp::canvas)

# Signal/DSP tests
pulp_add_test_suite(pulp-test-signal LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-signal-unison
    SOURCES test_signal_unison.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::audio-analysis)
# Modulation and utility toolkit. Contracts, sources, control tools, events,
# and voice-level compositions have separate owners so the alias inventory and
# behavioral suites can evolve without recreating a mixed test hotspot.
pulp_add_test_suite(pulp-test-signal-mod-contract LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-signal-mod-event-api LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-signal-mod-sources LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-signal-mod-source-api LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-signal-mod-tools LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-analysis-frontends
    SOURCES test_analysis_frontends.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-modulation-language
    SOURCES test_modulation_language.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-signal-units LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-signal-mod-events LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-signal-mod-voice LIBRARIES pulp::signal)
# Alias/passband claims here are measured with the shared tone-projection
# analyzers, hence the analysis lib alongside the DSP under test.
pulp_add_test_suite(pulp-test-oversampling-quality
    LIBRARIES pulp::signal pulp::audio-analysis)
# True-peak look-ahead limiting: independent higher-rate reconstruction oracle,
# latency, channel-link, deterministic partitioning, and realtime storage.
pulp_add_test_suite(pulp-test-true-peak-limiter
    SOURCES test_true_peak_limiter.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
target_compile_definitions(pulp-test-true-peak-limiter
    PRIVATE PULP_TRUE_PEAK_LIMITER_TEST_SEAMS=1)
# Fundamental-frequency estimator for harmonically-dense oscillator output plus
# the f0(t) trajectory extractor — proven accurate to well under a cent, and
# proven to beat the shipped zero-crossing detector on dense material.
pulp_add_test_suite(pulp-test-pitch-track LIBRARIES pulp::audio-analysis)
pulp_add_test_suite(pulp-test-transition-mixer LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-routing-primitives
    SOURCES test_routing_primitives.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
# Signal filter tests extracted from test_signal.cpp.
# Biquad / SVF / LadderFilter / LinkwitzRiley TEST_CASE clusters moved
# verbatim into a sibling TU to keep test_signal.cpp under ~1,200 lines.
pulp_add_test_suite(pulp-test-signal-filters LIBRARIES pulp::signal)
# Fixed-capacity N-way LR4 crossover: independent complex-response and
# time-domain reconstruction oracles, automation, fault recovery, and RT proof.
pulp_add_test_suite(pulp-test-nway-linkwitz-riley
    SOURCES test_nway_linkwitz_riley.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
# Filter analysis: coefficients -> magnitude response -> sampled curve. Asserts
# the SHAPE of each filter type (a lowpass rolls off, a notch nulls, a shelf
# plateaus), which is what distinguishes a real response from an approximation.
pulp_add_test_suite(pulp-test-frequency-response LIBRARIES pulp::signal)
# Signal spectral tests extracted from test_signal.cpp.
# WindowFunction / FFT / Convolver TEST_CASE clusters moved verbatim.
pulp_add_test_suite(pulp-test-signal-spectral LIBRARIES pulp::signal)
# Window coefficient, spectral-shape, STFT-consumer, and RT apply contracts.
pulp_add_test_suite(pulp-test-windowing
    SOURCES test_windowing.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
# Fixed-capacity single-thread history and its STFT consumer contract.
pulp_add_test_suite(pulp-test-mirrored-history-buffer
    SOURCES test_mirrored_history_buffer.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)

# Public signal headers are consumed by WAM/WebCLAP translation units compiled
# without an exception runtime. This small executable instantiates allocating
# prepare paths so exception-only syntax in template bodies fails natively too.
add_executable(pulp-test-signal-no-exceptions test_signal_no_exceptions.cpp)
target_link_libraries(pulp-test-signal-no-exceptions PRIVATE pulp::signal)
if(MSVC)
    target_compile_options(pulp-test-signal-no-exceptions PRIVATE /EHs-c- /GR-)
else()
    target_compile_options(pulp-test-signal-no-exceptions PRIVATE
        -fno-exceptions -fno-rtti)
endif()
add_test(NAME signal-public-headers-no-exceptions
    COMMAND pulp-test-signal-no-exceptions)
# Spectral primitives: STFT/WOLA engine, pitch/time, formant, smoothing.
pulp_add_test_suite(pulp-test-spectral-primitives
    SOURCES test_spectral_frame_engine.cpp test_realtime_pitch_time.cpp
            test_realtime_pitch_time_stream_contract.cpp test_finite_stretch_builder.cpp
            test_transient_freeze_delay.cpp test_spectral_matrix.cpp test_stn_stretch.cpp
            test_sinc_pitch.cpp
    LIBRARIES pulp::signal)
# Frame-domain spectral gate and bounded causal magnitude blur. The test owns
# hand-computed oracles and an allocation probe rather than reusing production
# FFT or smoothing code as its reference.
pulp_add_test_suite(pulp-test-spectral-gate-blur
    SOURCES test_spectral_gate_blur.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
# Live two-input frame-domain magnitude/phase morph with independent arithmetic
# oracles, wrap-seam negative controls, partition equivalence, and RT proof.
pulp_add_test_suite(pulp-test-spectral-morph
    SOURCES test_spectral_morph.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-stn-decomposer LIBRARIES pulp::signal)
# Offline time-stretch/pitch engine (orchestrates the spectral primitives).
pulp_add_test_suite(pulp-test-offline-stretch LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-noise-morpher LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-sinc-resampler LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-freeze-loop-sampler LIBRARIES pulp::signal)
# Multi-backend FFT facade (vdsp / kissfft / fftw3 / mkl). Verifies
# selector helpers, env-var routing, round-trip on each available
# backend, and cross-backend numerical equivalence.
pulp_add_test_suite(pulp-test-fft-backends LIBRARIES pulp::signal-fft-backend)
# Signal meter tests extracted from test_signal.cpp.
# MultiChannelMeter / MultiChannelBallistics TEST_CASE clusters moved
# verbatim. Distinct from pulp-test-multi-channel-meter (test_multi_
# channel_meter.cpp), which keeps its own focused edge-case coverage.
pulp_add_test_suite(pulp-test-signal-meter LIBRARIES pulp::signal)
# Biquad filter tests
pulp_add_test_suite(pulp-test-biquad LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-osc-phase LIBRARIES pulp::signal)
# The BLEP/BLAMP kernels are gated on measured alias rejection, so this suite
# links the analysis lib for the shared tone-projection analyzers.
pulp_add_test_suite(pulp-test-osc-blep LIBRARIES pulp::signal pulp::audio-analysis)
# The causal minBLEP accumulator is gated on deterministic RT behavior and
# measured alias residuals against the current polyBLEP path.
pulp_add_test_suite(pulp-test-osc-minblep
    SOURCES test_osc_minblep.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::audio-analysis)
if(Python3_Interpreter_FOUND)
    add_test(NAME minblep-table-reproducible
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tools/scripts/generate_minblep_table.py
            --check
            --output ${CMAKE_SOURCE_DIR}/core/signal/include/pulp/signal/osc/detail/minblep_table.hpp)
endif()
# The VA shapes are gated on measured alias rejection, hence the analysis lib.
pulp_add_test_suite(pulp-test-osc-va LIBRARIES pulp::signal pulp::audio-analysis)
# Sync and through-zero FM are gated on measured alias rejection too.
pulp_add_test_suite(pulp-test-osc-sync LIBRARIES pulp::signal pulp::audio-analysis)
# The circuit-flavored VCO's core is gated on measured alias rejection, and its
# deterministic character stages on level/DC/pitch correctness.
pulp_add_test_suite(pulp-test-osc-vco LIBRARIES pulp::signal pulp::audio-analysis)
# The divider-clocked DCO front-end: pitch quantization asserted on the derived
# integer N / rational period AND on the rendered pitch (hence the analysis lib),
# with the fractional-N jitter and the shared-path alias rejection measured too.
pulp_add_test_suite(pulp-test-osc-dco LIBRARIES pulp::signal pulp::audio-analysis)
# The modern wavetable tier is gated on alias rejection swept to the top of every
# band, a click-free band-switch seam, and a zipper-free scan.
pulp_add_test_suite(pulp-test-osc-wt LIBRARIES pulp::signal pulp::audio-analysis)
# The lo-fi wavetable tier is a dedicated variable-clock ZOH engine, gated on the
# pitch-tracking n·L·f0 image ladder matching the analytic sinc model, odd-
# harmonic 8-bit grit, a reconstruction stage that kills the naive in-band fold,
# and a faithful stepped wave-scan.
pulp_add_test_suite(pulp-test-osc-wt-lofi LIBRARIES pulp::signal pulp::audio-analysis)
# SF-2 crossfade unification: live_kernel structural-swap fade now matches the
# native signal::TransitionMixer (EqualPower) law bit-for-bit — an intended,
# documented behavior change (the fade previously used a linear theta).
pulp_add_test_suite(pulp-test-live-kernel-crossfade-null
    SOURCES test_live_kernel_crossfade_null.cpp
    LIBRARIES pulp::signal
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/experimental)
# SF-2 crossfade unification: the ONE fixture covering every SIGNAL-side fade —
# shared-law invariants + TransitionMixer / live_kernel / LoopRenderer parity.
pulp_add_test_suite(pulp-test-crossfade
    SOURCES test_crossfade.cpp
    LIBRARIES pulp::signal pulp::audio
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/experimental)
# DSL processor contract tests (FaustProcessor + PulpFaustUI + PulpFaustMeta)
add_executable(pulp-test-dsl-processor test_dsl_processor.cpp)
target_link_libraries(pulp-test-dsl-processor PRIVATE
    pulp::dsl pulp::format pulp::state pulp::audio pulp::midi
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-dsl-processor)

# Convolution engine tests
pulp_add_test_suite(pulp-test-convolver LIBRARIES pulp::signal)

# Background-IR-swap tests for PartitionedConvolver. Lock-free
# pointer-shuttle hand-off from worker thread → audio thread, including
# a concurrent stage/swap hammer test.
pulp_add_test_suite(pulp-test-convolver-bg-swap
    SOURCES test_convolver_bg_swap.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::signal pulp::runtime)

# Non-uniform partitioned convolver tests. Two-stage Gardner-style
# head (small block) + tail (K× larger block) with zero-latency mixing.
pulp_add_test_suite(pulp-test-convolver-non-uniform LIBRARIES pulp::signal)

# GPU audio runtime: real-time transport (fixed-latency proxy + non-RT worker
# pump + lock-free rings + miss policy). GPU-agnostic scheduling logic, so it
# runs on no-GPU CI too.
pulp_add_test_suite(pulp-test-gpu-audio-transport
    SOURCES test_gpu_audio_transport.cpp
    LIBRARIES pulp::gpu-audio pulp::audio)

# Flow pans: pure per-room constant-power pan math + GpuMultiConvolver::set_flow
# (an atomic store). GPU-agnostic, so it runs — and keeps the flow math covered —
# in the no-GPU coverage build too.
pulp_add_test_suite(pulp-test-flow-pans
    SOURCES test_flow_pans.cpp
    LIBRARIES pulp::gpu-audio pulp::audio)

# GPU convolver node: golden test vs direct convolution when GPU/render is
# available, plus CPU-fallback coverage in GPU-off builds.
# These suites create real Dawn/Metal devices + blocking GPU readbacks, so they
# carry RESOURCE_LOCK pulp_gpu (matching the render/GPU manifests) to serialize
# against every other GPU test under parallel ctest — the cross-process
# contention that intermittently trips their numeric checks after a 2s readback
# timeout leaves a stale frame. (The GPU-agnostic transport/flow-pans suites
# above do NOT create devices and are intentionally left parallel.)
pulp_add_test_suite(pulp-test-gpu-convolver
    SOURCES test_gpu_convolver.cpp
    LIBRARIES pulp::gpu-audio pulp::audio
    PROPERTIES RESOURCE_LOCK pulp_gpu)

if(PULP_HAS_SKIA)
    # GPU STFT primitive: window+FFT analyze, inverse-FFT synthesize,
    # and COLA overlap-add reconstruction.
    pulp_add_test_suite(pulp-test-gpu-stft
        SOURCES test_gpu_stft.cpp
        LIBRARIES pulp::gpu-audio pulp::audio pulp::signal
        PROPERTIES RESOURCE_LOCK pulp_gpu)
    # Spectral freeze: capture and sustain a spectral frame.
    pulp_add_test_suite(pulp-test-gpu-spectral-freeze
        SOURCES test_gpu_spectral_freeze.cpp
        LIBRARIES pulp::gpu-audio pulp::audio pulp::signal
        PROPERTIES RESOURCE_LOCK pulp_gpu)
    # Spectral morph: blend between two captured spectra.
    pulp_add_test_suite(pulp-test-gpu-spectral-morph
        SOURCES test_gpu_spectral_morph.cpp
        LIBRARIES pulp::gpu-audio pulp::audio pulp::signal
        PROPERTIES RESOURCE_LOCK pulp_gpu)
    # Spectral stack: multi-layer frozen stack, weighted morph, and spectral
    # smear — the batched engine that superseded the retired GpuHyperFreeze.
    pulp_add_test_suite(pulp-test-gpu-spectral-stack
        SOURCES test_gpu_spectral_stack.cpp
        LIBRARIES pulp::gpu-audio pulp::audio pulp::signal
        PROPERTIES RESOURCE_LOCK pulp_gpu)
endif()

# Test signal source (sine tone, file playback)
pulp_add_test_suite(pulp-test-test-signal LIBRARIES pulp::standalone)
# Standalone editor chrome helpers
# These construct a StandaloneApp, which opens the real audio device on init
# (standalone.cpp create_device/open/start) — same RT-thread-starved teardown
# hazard as pulp-test-audio above, so they get the same PROCESSORS reservation.
pulp_add_test_suite(pulp-test-standalone-editor-chrome LIBRARIES pulp::standalone
    PROPERTIES PROCESSORS 8)
if(TARGET pulp::inspect AND NOT IOS)
    pulp_add_test_suite(pulp-test-standalone-inspector
        SOURCES test_standalone_inspector.cpp
        LIBRARIES pulp::standalone-inspector-runtime-eval
        PROPERTIES PROCESSORS 8)
    target_compile_definitions(pulp-standalone-inspector-runtime-eval PRIVATE
        PULP_STANDALONE_INSPECTOR_TEST_HOOKS=1)
    target_compile_definitions(pulp-standalone-inspector PRIVATE
        PULP_STANDALONE_INSPECTOR_TEST_HOOKS=1)
    target_compile_definitions(pulp-test-standalone-inspector PRIVATE
        PULP_TEST_STANDALONE_INSPECTOR=1
        PULP_STANDALONE_INSPECTOR_TEST_HOOKS=1)
    target_link_libraries(pulp-test-standalone-inspector PRIVATE
        pulp::inspect-client)

    pulp_add_test_suite(pulp-test-standalone-runtime-eval
        SOURCES test_standalone_runtime_eval.cpp
        LIBRARIES pulp::standalone-inspector-runtime-eval pulp::inspect-client
        PROPERTIES PROCESSORS 8)
    target_compile_definitions(pulp-test-standalone-runtime-eval PRIVATE
        PULP_TEST_STANDALONE_INSPECTOR=1
        PULP_STANDALONE_INSPECTOR_TEST_HOOKS=1)
else()
    pulp_add_test_suite(pulp-test-standalone-inspector
        SOURCES test_standalone_inspector.cpp
        LIBRARIES pulp::standalone
        PROPERTIES PROCESSORS 8)
    target_compile_definitions(pulp-test-standalone-inspector PRIVATE
        PULP_TEST_STANDALONE_INSPECTOR=0)
endif()
if(APPLE AND PULP_ENABLE_GPU AND PULP_HAS_SKIA AND TARGET pulp::inspect
   AND PROJECT_IS_TOP_LEVEL)
    set_source_files_properties(
        fixtures/standalone_inspector_process_fixture.cpp
        PROPERTIES LANGUAGE OBJCXX)
    add_executable(pulp-standalone-inspector-process-fixture
        fixtures/standalone_inspector_process_fixture.cpp)
    set(PULP_pulp-standalone-inspector-process-fixture_CONTROL_PROFILE
        developer-local)
    set(PULP_pulp-standalone-inspector-process-fixture_CONTROL_CAPABILITIES
        dev.pulp.instance/read@1 dev.pulp.state/read@1
        dev.pulp.ui/observe@1 dev.pulp.diagnostics/read@1
        dev.pulp.logs/read@1 dev.pulp.ui/capture@1
        dev.pulp.telemetry/subscribe@1)
    set(PULP_pulp-standalone-inspector-process-fixture_INSPECTOR_MANIFEST_DIRECTORY
        "${CMAKE_BINARY_DIR}/pulp-inspector-test-manifests")
    _pulp_configure_inspector_shipping(
        pulp-standalone-inspector-process-fixture
        "com.pulp.test.inspector-developer-edition"
        "Inspector Developer Edition")
    target_compile_definitions(pulp-standalone-inspector-process-fixture PRIVATE
        PULP_STANDALONE_INSPECTOR_TEST_HOOKS=1)
    target_link_libraries(pulp-standalone-inspector-process-fixture PRIVATE
        pulp::standalone-inspector)
    _pulp_attach_inspector_shipping(
        pulp-standalone-inspector-process-fixture
        pulp-standalone-inspector-process-fixture)

    pulp_add_test_suite(pulp-test-standalone-inspector-process
        SOURCES test_standalone_inspector_process.cpp
        LIBRARIES pulp::standalone pulp::inspect-client pulp::platform pulp::view
        PROPERTIES RESOURCE_LOCK pulp_gpu PROCESSORS 8)
    target_compile_definitions(pulp-test-standalone-inspector-process PRIVATE
        "PULP_STANDALONE_INSPECTOR_PROCESS_FIXTURE=\"$<TARGET_FILE:pulp-standalone-inspector-process-fixture>\"")
    add_dependencies(pulp-test-standalone-inspector-process
        pulp-standalone-inspector-process-fixture)

    set_source_files_properties(
        fixtures/standalone_inspector_workflow_process_fixture.cpp
        PROPERTIES LANGUAGE OBJCXX)
    add_executable(pulp-standalone-inspector-workflow-process-fixture
        fixtures/standalone_inspector_workflow_process_fixture.cpp)
    set(PULP_pulp-standalone-inspector-workflow-process-fixture_SHIP_INSPECTOR TRUE)
    set(PULP_pulp-standalone-inspector-workflow-process-fixture_SHIP_INSPECTOR_RUNTIME_EVAL TRUE)
    set(PULP_pulp-standalone-inspector-workflow-process-fixture_INSPECTOR_CAPABILITIES
        session.describe session.control state.read ui.read diagnostics.read
        logs.read capture.image state.write test.input authoring.tweaks
        telemetry.stream runtime.eval)
    set(PULP_pulp-standalone-inspector-workflow-process-fixture_INSPECTOR_MANIFEST_DIRECTORY
        "${CMAKE_BINARY_DIR}/pulp-inspector-test-manifests")
    _pulp_configure_inspector_shipping(
        pulp-standalone-inspector-workflow-process-fixture
        "com.pulp.test.standalone-inspector-workflow"
        "Standalone Inspector Workflow Fixture")
    target_link_libraries(pulp-standalone-inspector-workflow-process-fixture PRIVATE
        pulp::standalone-inspector-runtime-eval pulp::inspect-client)
    _pulp_attach_inspector_shipping(
        pulp-standalone-inspector-workflow-process-fixture
        pulp-standalone-inspector-workflow-process-fixture)

    pulp_add_test_suite(pulp-test-standalone-inspector-workflow-process
        SOURCES test_standalone_inspector_workflow_process.cpp
        LIBRARIES pulp::standalone pulp::inspect-client pulp::platform pulp::runtime pulp::view
        PROPERTIES RESOURCE_LOCK pulp_gpu PROCESSORS 8)
    target_compile_definitions(pulp-test-standalone-inspector-workflow-process PRIVATE
        "PULP_STANDALONE_INSPECTOR_WORKFLOW_PROCESS_FIXTURE=\"$<TARGET_FILE:pulp-standalone-inspector-workflow-process-fixture>\"")
    add_dependencies(pulp-test-standalone-inspector-workflow-process
        pulp-standalone-inspector-workflow-process-fixture)

    function(_pulp_register_packaged_inspector_clients_test)
        get_property(python_executable GLOBAL
            PROPERTY PULP_PACKAGED_INSPECTOR_CLIENTS_PYTHON)
        if(NOT CMAKE_CROSSCOMPILING
           AND python_executable
           AND TARGET pulp-rust-cli
           AND TARGET pulp-cli
           AND TARGET pulp-mcp)
            add_custom_target(pulp-test-packaged-inspector-clients
                DEPENDS
                    pulp-rust-cli
                    pulp-cli
                    pulp-mcp
                    pulp-standalone-inspector-workflow-process-fixture)
            add_test(
                NAME packaged-inspector-clients-reach-real-standalone-workflow
                COMMAND "${python_executable}"
                    "${PROJECT_SOURCE_DIR}/test/test_packaged_inspector_clients.py"
                    --rust "${CMAKE_BINARY_DIR}/pulp${CMAKE_EXECUTABLE_SUFFIX}"
                    --cpp "$<TARGET_FILE:pulp-cli>"
                    --mcp "$<TARGET_FILE:pulp-mcp>"
                    --fixture "$<TARGET_FILE:pulp-standalone-inspector-workflow-process-fixture>"
                    --packager "${PROJECT_SOURCE_DIR}/tools/scripts/package_cli.py"
                    --mcp-config "${PROJECT_SOURCE_DIR}/.mcp.json"
                    --build-dir "${CMAKE_BINARY_DIR}")
            set_tests_properties(
                packaged-inspector-clients-reach-real-standalone-workflow
                PROPERTIES
                    LABELS "slow;installed;inspect;workflow;gpu"
                    PROCESSORS 8
                    RESOURCE_LOCK pulp_gpu
                    TIMEOUT 120)
        endif()
    endfunction()
    if(Python3_Interpreter_FOUND)
        # CLI and MCP targets are declared after the test subtree, so defer the
        # cross-artifact registration until the root directory is complete.
        set_property(GLOBAL PROPERTY PULP_PACKAGED_INSPECTOR_CLIENTS_PYTHON
            "${Python3_EXECUTABLE}")
        cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}"
            CALL _pulp_register_packaged_inspector_clients_test)
    endif()
endif()
pulp_add_test_suite(pulp-test-standalone-apply-config LIBRARIES pulp::standalone
    PROPERTIES PROCESSORS 8)
# Screenshot-only launches skip the audio backend; the device lifecycle is
# asserted through an injected fake system, so this one opens no real device
# and needs no PROCESSORS reservation.
pulp_add_test_suite(pulp-test-standalone-capture-audio LIBRARIES pulp::standalone pulp::audio)
pulp_add_test_suite(pulp-test-standalone-audio-capture-wav LIBRARIES pulp::standalone pulp::audio PROPERTIES PROCESSORS 8)
pulp_add_test_suite(pulp-test-standalone-audio-capture-rolling-wav LIBRARIES pulp::standalone pulp::audio PROPERTIES PROCESSORS 8)
pulp_add_test_suite(pulp-test-standalone-transport-midi LIBRARIES pulp::standalone
    PROPERTIES PROCESSORS 8)
pulp_add_test_suite(pulp-test-standalone-musical-typing LIBRARIES pulp::standalone)
pulp_add_test_suite(pulp-test-standalone-audio-inspector
    LIBRARIES pulp::standalone pulp::view PROPERTIES PROCESSORS 8)
# Headless screenshot capture state machine
pulp_add_test_suite(pulp-test-screenshot-capture LIBRARIES pulp::standalone
    PROPERTIES PROCESSORS 8)
# STFT and audio visualization signal tests
pulp_add_test_suite(pulp-test-stft LIBRARIES pulp::signal)
# Visualization bridge and widget tests
pulp_add_test_suite(pulp-test-visualization LIBRARIES pulp::view)

# Clipboard tests
add_executable(pulp-test-clipboard test_clipboard.cpp)
target_link_libraries(pulp-test-clipboard PRIVATE pulp::platform Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-clipboard
    PROPERTIES RESOURCE_LOCK system-clipboard)

# FileDialog backend-registration tests
pulp_add_test_suite(pulp-test-file-dialog LIBRARIES pulp::platform)

# D-Bus client + Linux xdg-desktop-portal file-dialog backend
pulp_add_test_suite(pulp-test-dbus LIBRARIES pulp::platform)

# Platform tests
pulp_add_test_suite(pulp-test-platform LIBRARIES pulp::platform)

# Device capability tier and thermal ladder. The quota table they index is
# policy and lives above the engine, so it is registered with the format tests
# instead. Header-only over pulp::platform — linked for the include path only.
pulp_add_test_suite(pulp-test-device-capability LIBRARIES pulp::platform)

# Permissions tests
pulp_add_test_suite(pulp-test-permissions LIBRARIES pulp::platform)

# Environment API tests
pulp_add_test_suite(pulp-test-environment LIBRARIES pulp::platform)

# Runtime tests
pulp_add_test_suite(pulp-test-runtime
    SOURCES test_runtime.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::runtime)

pulp_add_test_suite(pulp-test-durable-file-replacement LIBRARIES pulp::runtime)

# Lock-free realtime/control-thread occurrence signals (pad flashes, UI triggers).
pulp_add_test_suite(pulp-test-activity-channel
    LIBRARIES pulp::runtime
    PROPERTIES LABELS lifecycle)

# SpscRingIndex (index-pair SPSC ring-buffer cursor management)
pulp_add_test_suite(pulp-test-spsc-ring-index LIBRARIES pulp::runtime)

# FileSearchPath (ordered directory search list)
pulp_add_test_suite(pulp-test-file-search-path LIBRARIES pulp::runtime)

# Sync-primitive hammer for the TSan-focused race regression.
# Runs under the tag-scoped TSan subset via the [concurrent][race]
# tags embedded in each TEST_CASE (matches the 'Race|Concurrent'
# alternation in sanitizers.yml's --tests-regex).
add_executable(pulp-test-sync-race-hammer test_sync_race_hammer.cpp)
target_link_libraries(pulp-test-sync-race-hammer PRIVATE pulp::runtime Catch2::Catch2WithMain)
# `slow`: N-thread race smoke. Sanitizer builds also pick this
# up via the dedicated sanitizers.yml workflow, so fast-CI doesn't
# need to re-run it.
catch_discover_tests(pulp-test-sync-race-hammer PROPERTIES LABELS slow)

# Events tests
pulp_add_test_suite(pulp-test-events LIBRARIES pulp::events)

pulp_add_test_suite(pulp-test-events-async-helpers LIBRARIES pulp::events)

# PushNotifications cross-platform smoke + headless-mock coverage.
pulp_add_test_suite(pulp-test-push-notifications LIBRARIES pulp::events)

# IapClient cross-platform smoke + headless-mock coverage.
pulp_add_test_suite(pulp-test-in-app-purchase LIBRARIES pulp::events)

# Message-loop integration cross-platform introspection surface.
pulp_add_test_suite(pulp-test-message-loop-integration LIBRARIES pulp::events)

add_executable(pulp-test-events-timer-helpers test_events_timer_helpers.cpp)
target_link_libraries(pulp-test-events-timer-helpers PRIVATE pulp::events Catch2::Catch2WithMain)
# `slow`: Timer hammer + UAF-free destroy-while-dispatched
# tests deliberately exercise message-loop pacing (~0.3-1.5 sec each
# depending on platform). Excluded from fast-CI; sanitizers.yml
# still covers Timer under TSan.
catch_discover_tests(pulp-test-events-timer-helpers PROPERTIES LABELS slow)

# Audio file I/O tests
pulp_add_test_suite(pulp-test-audio-file LIBRARIES pulp::audio pulp::signal)
pulp_add_test_suite(pulp-test-wav-memory-decoder
    SOURCES test_wav_memory_decoder.cpp wav_decoder_consumer_drwav.cpp
    LIBRARIES pulp::audio
    INCLUDE_DIRS "${PROJECT_SOURCE_DIR}/external/dr_libs")
if(CMAKE_NM)
    add_custom_command(TARGET pulp-test-wav-memory-decoder POST_BUILD
        COMMAND "${CMAKE_COMMAND}"
            "-DNM=${CMAKE_NM}"
            "-DARCHIVE=$<TARGET_FILE:pulp-audio>"
            -P "${CMAKE_CURRENT_LIST_DIR}/check_no_exported_drwav.cmake"
        COMMENT "Checking pulp-audio does not export private dr_wav symbols")
endif()

# Phase-vocoder offline time-stretch / pitch-shift.
pulp_add_test_suite(pulp-test-phase-vocoder LIBRARIES pulp::signal)

# Click-free soft-bypass wrapper.
pulp_add_test_suite(pulp-test-soft-bypass LIBRARIES pulp::signal)
pulp_add_test_suite(pulp-test-sample-resource
    SOURCES test_sample_resource.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio pulp::runtime)

# WaveformOverview + WaveformOverviewCache (item 6.12).
# Links pulp::view so the WaveformView integration smoke compiles.
pulp_add_test_suite(pulp-test-waveform-overview LIBRARIES pulp::audio pulp::view)

# WaveformOverviewView: Canvas (non-GPU) min/max-column rendering over a
# caller-set viewport, asserted headlessly through RecordingCanvas.
pulp_add_test_suite(pulp-test-waveform-overview-view LIBRARIES pulp::audio pulp::view)

# Memory-mapped reader: true ranged (seek-based) decode, no whole-file decode.
pulp_add_test_suite(pulp-test-mmap-reader-ranged LIBRARIES pulp::audio)

# SearchIndex — pure ranking/matching core of the off-UI-thread query service (R7).
pulp_add_test_suite(pulp-test-search-index LIBRARIES pulp::runtime)
