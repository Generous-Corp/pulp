# Application UI, audio harness, platform, WebView, MPE, host, WAM, WebCLAP, and web demo tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# WAMv2 Processor->WASM bridge: native coverage of the -fno-exceptions bridge TU (compiled directly; links real pulp libs; PulpWam.cmake's lib target can replace this later).
add_executable(pulp-test-wam-adapter test_wam_adapter.cpp ${CMAKE_SOURCE_DIR}/core/format/src/wasm/wam_adapter.cpp)
target_link_libraries(pulp-test-wam-adapter PRIVATE Catch2::Catch2WithMain pulp::format pulp::state pulp::runtime pulp::events pulp::midi pulp::audio)
catch_discover_tests(pulp-test-wam-adapter)

# Parameter attachment tests
pulp_add_test_suite(pulp-test-param-attachment LIBRARIES pulp::view)

# The value engine the continuous controls share: range, interval, skew, drag
# law, gesture bracketing, and the notify policy.
pulp_add_test_suite(pulp-test-slider-core LIBRARIES pulp::view)

# The paint delegate: subtree inheritance, partial override, and the units each
# control shape reasons in.
pulp_add_test_suite(pulp-test-widget-painter LIBRARIES pulp::view)

# Bugs that shipped: a button face that never rendered, a wheel notch that moved
# a quantized control by zero, a field that collapsed to no height in a flex
# tree, and caret/selection colors no skin could reach.
pulp_add_test_suite(pulp-test-widget-regressions LIBRARIES pulp::view)

# App framework tests (KeyMapping, MenuBar, Toolbar, AppSettings)
pulp_add_test_suite(pulp-test-app-framework LIBRARIES pulp::view)

# Preset manager tests
pulp_add_test_suite(pulp-test-preset-manager LIBRARIES pulp::state)

# Waveform editor tests
pulp_add_test_suite(pulp-test-waveform-editor LIBRARIES pulp::view)

# FastMath approximation tests
pulp_add_test_suite(pulp-test-fast-math LIBRARIES pulp::signal)

# Polynomial/Matrix math tests
pulp_add_test_suite(pulp-test-poly-math LIBRARIES pulp::signal)

# Preset browser UI tests
pulp_add_test_suite(pulp-test-preset-browser LIBRARIES pulp::view)

# Plugin manager panel UI tests
add_executable(pulp-test-plugin-manager-panel test_plugin_manager_panel.cpp)
target_link_libraries(pulp-test-plugin-manager-panel PRIVATE
    pulp::view pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-plugin-manager-panel)

# DSP expansion tests (FIR, Ballistics, LogRamp, ProcessorChain, LookupTable, TPT)
pulp_add_test_suite(pulp-test-dsp-expansion LIBRARIES pulp::signal)

# Interpolator tests (Hermite, Lagrange, windowed-sinc)
pulp_add_test_suite(pulp-test-interpolator LIBRARIES pulp::signal)

# MIDI expansion tests (RPN/NRPN parser, MidiKeyboardState)
pulp_add_test_suite(pulp-test-midi-expansion LIBRARIES pulp::midi)

# Audio process load measurer tests
pulp_add_test_suite(pulp-test-load-measurer
    SOURCES test_load_measurer.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio)

# Live per-node DSP telemetry: fixed-slot ring, drain, percentiles, RT-safety
pulp_add_test_suite(pulp-test-live-dsp-telemetry
    SOURCES test_live_dsp_telemetry.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::audio pulp::runtime)

# Live per-node DSP telemetry snapshot → JSON serializer (schema contract)
pulp_add_test_suite(pulp-test-live-dsp-telemetry-json
    SOURCES test_live_dsp_telemetry_json.cpp
    LIBRARIES pulp::audio)

# FilterDesign tests
pulp_add_test_suite(pulp-test-filter-design LIBRARIES pulp::signal)

# IIR design tests (Chebyshev I/II, Elliptic)
pulp_add_test_suite(pulp-test-iir-design LIBRARIES pulp::signal)

# TreeView tests
pulp_add_test_suite(pulp-test-tree-view LIBRARIES pulp::view)

# Toolbar tests (UTF-8 icon-label extraction, custom-item width, hit-testing)
pulp_add_test_suite(pulp-test-toolbar LIBRARIES pulp::view)

# File browser, file tree, and multi-document panel tests
pulp_add_test_suite(pulp-test-file-browser LIBRARIES pulp::view)

# FileChooser widget builder wrapper over pulp::platform::FileDialog
# with async callback delivery.
pulp_add_test_suite(pulp-test-file-chooser LIBRARIES pulp::view pulp::platform)

# Undo manager tests
pulp_add_test_suite(pulp-test-undo-manager LIBRARIES pulp::state)

# BufferingReader tests
pulp_add_test_suite(pulp-test-buffering-reader LIBRARIES pulp::audio)

# AudioWorkgroup tests
pulp_add_test_suite(pulp-test-workgroup LIBRARIES pulp::audio)

# AudioWorkgroup ↔ AudioDevice wiring.
pulp_add_test_suite(pulp-test-audio-workgroup-wiring
    SOURCES test_audio_workgroup_wiring.cpp
    LIBRARIES pulp::audio)

# CoreAudio follow-default output-device live switch (macOS only).
if(APPLE AND NOT PULP_IOS)
    pulp_add_test_suite(pulp-test-coreaudio-default-follow
        SOURCES test_coreaudio_default_follow.mm
        LIBRARIES pulp::audio)
    target_link_libraries(pulp-test-coreaudio-default-follow PRIVATE "-framework CoreAudio")

    # CoreAudio input-only open (capture without opening any output).
    pulp_add_test_suite(pulp-test-coreaudio-input-only
        SOURCES test_coreaudio_input_only.mm
        LIBRARIES pulp::audio)
    target_link_libraries(pulp-test-coreaudio-input-only PRIVATE "-framework CoreAudio")
endif()

# Plugin matrix tests (sample rate/buffer size/edge case sweep)
add_executable(pulp-test-matrix test_plugin_matrix.cpp)
target_link_libraries(pulp-test-matrix PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-matrix)

# Per-plugin matrix tests (separate TUs to avoid enum collisions)
add_executable(pulp-test-matrix-effect test_matrix_effect.cpp)
target_link_libraries(pulp-test-matrix-effect PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-effect PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-matrix-effect)
add_executable(pulp-test-matrix-compressor test_matrix_compressor.cpp)
target_link_libraries(pulp-test-matrix-compressor PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-compressor PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-compressor)
catch_discover_tests(pulp-test-matrix-compressor)
add_executable(pulp-test-matrix-tone test_matrix_tone.cpp)
target_link_libraries(pulp-test-matrix-tone PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-tone PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-matrix-tone)
add_executable(pulp-test-matrix-drums test_matrix_drums.cpp)
target_link_libraries(pulp-test-matrix-drums PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-drums PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpDrums)
catch_discover_tests(pulp-test-matrix-drums)
add_executable(pulp-test-matrix-synth test_matrix_synth.cpp)
target_link_libraries(pulp-test-matrix-synth PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-synth PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpSynth)
catch_discover_tests(pulp-test-matrix-synth)
add_executable(pulp-test-matrix-sampler test_matrix_sampler.cpp)
target_link_libraries(pulp-test-matrix-sampler PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-matrix-sampler PRIVATE ${CMAKE_SOURCE_DIR}/examples/PulpSampler)
catch_discover_tests(pulp-test-matrix-sampler)
# Harness support lib: Processor-driven helpers; file-analysis lives in pulp::audio-analysis (tools/audio/analysis). See test/support/README.md.
add_library(pulp-audio-test-support STATIC support/audio_signal_generators.cpp support/render_scenario.cpp support/audio_contracts.cpp support/audio_doctor.cpp)
target_link_libraries(pulp-audio-test-support PUBLIC pulp::format pulp::signal pulp::audio-analysis)
add_executable(pulp-test-golden test_golden_audio.cpp)
target_link_libraries(pulp-test-golden PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-golden)
add_executable(pulp-test-audio-tone-regression test_audio_tone_regression.cpp)
target_link_libraries(pulp-test-audio-tone-regression PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-tone-regression PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-audio-tone-regression)
add_executable(pulp-test-render-scenario test_render_scenario.cpp)
target_link_libraries(pulp-test-render-scenario PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-render-scenario PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-render-scenario)
add_executable(pulp-test-audio-support test_audio_support.cpp)
target_link_libraries(pulp-test-audio-support PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-support PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-audio-support)
add_executable(pulp-test-audio-contracts test_audio_contracts.cpp test_audio_contracts_effect.cpp)
target_link_libraries(pulp-test-audio-contracts PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-contracts PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-audio-contracts)
add_executable(pulp-test-audio-doctor test_audio_doctor.cpp)
target_link_libraries(pulp-test-audio-doctor PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-doctor PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-audio-doctor)
if(PULP_HAS_VST3)
    # The VST3 sibling of the CLAP null above: same deterministic Processor,
    # same stimulus, driven through the real PulpVst3Processor::process()
    # IAudioProcessor surface (ProcessData / AudioBusBuffers / IEventList /
    # IParameterChanges) instead of a C struct, and required to match the
    # HeadlessHost render bit-for-bit. Compiles vst3_plug_view.cpp for the GUI
    # symbols PulpVst3Processor::createView() references, mirroring
    # pulp-test-vst3-plugin-state.
    add_executable(pulp-test-vst3-audio-parity
        test_vst3_audio_parity.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/vst3_plug_view.cpp
    )
    target_link_libraries(pulp-test-vst3-audio-parity
        PRIVATE pulp-audio-test-support pulp::format Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-vst3-audio-parity PRIVATE
        PULP_VST3=1
        PULP_VST3_GUI=1
    )
    catch_discover_tests(pulp-test-vst3-audio-parity)
endif()
# Measured-versus-reported latency. Its fixture is a source-owned delay line, so
# it needs no example plugin.
add_executable(pulp-test-latency-contract test_latency_contract.cpp)
target_link_libraries(pulp-test-latency-contract PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-latency-contract)

add_executable(pulp-test-oversampling-latency-contract
    test_oversampling_latency_contract.cpp)
target_link_libraries(pulp-test-oversampling-latency-contract
    PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-oversampling-latency-contract)
add_executable(pulp-test-cli-audio-validate test_cli_audio_validate.cpp)
target_link_libraries(pulp-test-cli-audio-validate PRIVATE pulp::audio pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-audio-validate)
catch_discover_tests(pulp-test-cli-audio-validate)

add_executable(pulp-test-cli-audio-compare test_cli_audio_compare.cpp)
target_link_libraries(pulp-test-cli-audio-compare PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-audio-compare)
catch_discover_tests(pulp-test-cli-audio-compare)

add_executable(pulp-test-cli-audio-latency test_cli_audio_latency.cpp)
target_link_libraries(pulp-test-cli-audio-latency PRIVATE pulp::platform Catch2::Catch2WithMain)
pulp_bind_cli_shellout_target(pulp-test-cli-audio-latency)
catch_discover_tests(pulp-test-cli-audio-latency)
# Audio-probe RT output-boundary allocation counters.
add_executable(pulp-test-audio-probe test_audio_probe.cpp harness/rt_allocation_probe.cpp)
target_link_libraries(pulp-test-audio-probe PRIVATE pulp::audio pulp::runtime Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-audio-probe)
# Audio determinism matrix: SR × block sweep, framework invariants (pass-through, silence, block invariance, re-prepare).
add_executable(pulp-test-audio-matrix test_audio_determinism_matrix.cpp)
target_link_libraries(pulp-test-audio-matrix PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-matrix PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-audio-matrix)
# Audio edge-case corpus: zero blocks, denormals, impulse pos, DC extremes, mid-block param change, re-prepare. Framework contract only.
add_executable(pulp-test-audio-edge-cases test_audio_edge_cases.cpp)
target_link_libraries(pulp-test-audio-edge-cases PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
target_include_directories(pulp-test-audio-edge-cases PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain)
catch_discover_tests(pulp-test-audio-edge-cases)
# Golden-file tests for PulpEffect (separate to avoid enum collision with PulpGain)
add_executable(pulp-test-golden-effect test_golden_effect.cpp)
target_link_libraries(pulp-test-golden-effect PRIVATE pulp-audio-test-support Catch2::Catch2WithMain)
target_include_directories(pulp-test-golden-effect PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-effect)
catch_discover_tests(pulp-test-golden-effect)
# Negative-path tests (NaN, Inf, extreme sample rates, edge cases)
add_executable(pulp-test-negative-path test_negative_path.cpp)
target_link_libraries(pulp-test-negative-path PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-negative-path PRIVATE ${CMAKE_SOURCE_DIR}/examples/pulp-gain ${CMAKE_SOURCE_DIR}/examples/pulp-tone)
catch_discover_tests(pulp-test-negative-path)
# iOS foundation tests (platform detection, safe area geometry, touch events,
# AUv3 HostApp template shape).
pulp_add_test_suite(pulp-test-ios-foundation LIBRARIES pulp::view pulp::platform)
target_compile_definitions(pulp-test-ios-foundation PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# Identity/UUID tests
pulp_add_test_suite(pulp-test-identity LIBRARIES pulp::runtime)
# WebView tests (requires PULP_BUILD_WEBVIEW — WebViewPanel::create is only compiled when ON)
if(PULP_BUILD_WEBVIEW)
    add_executable(pulp-test-webview test_web_view.cpp)
    target_link_libraries(pulp-test-webview PRIVATE pulp::view Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-webview)

    # G2: examples/webview-plugin native-editor retention + host-agnostic attach.
    # Links pulp::format (ViewBridge lifecycle) + pulp::view (NativeViewHost /
    # WebViewPanel) and includes the example header directly.
    pulp_add_test_suite(pulp-test-webview-plugin-example
        SOURCES test_webview_plugin_example.cpp
        LIBRARIES pulp::format pulp::view pulp::state)
endif()

# WebView backend detection smoke — always built. Validates that
# `pulp::view::detect_webview_backend()` returns one of the documented
# identifiers ("wkwebview", "webview2", "webkitgtk", "chromium", "none")
# on every supported OS and keeps the native WebView backend identifier
# contract aligned with the public WebView guide.
if(WIN32)
    pulp_add_test_suite(pulp-test-webview-backend-detect
        LIBRARIES pulp::view
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-webview-backend-detect LIBRARIES pulp::view)
endif()

# MIDI 2.0 UMP/MPE tests
pulp_add_test_suite(pulp-test-ump-mpe LIBRARIES pulp::midi)

# MPE voice tracker tests
if(WIN32)
    pulp_add_test_suite(pulp-test-mpe-voice-tracker
        LIBRARIES pulp::midi
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-mpe-voice-tracker LIBRARIES pulp::midi)
endif()

# MPE synth voice helpers (voice, allocator, glide detector)
pulp_add_test_suite(pulp-test-mpe-synth-voice
    SOURCES test_mpe_synth_voice.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

# Plugin hosting tests (scanner, signal graph)
add_executable(pulp-test-host test_host.cpp harness/rt_allocation_probe.cpp)
target_link_libraries(pulp-test-host PRIVATE pulp::host Catch2::Catch2WithMain)
# On Apple, the AU host-slot tests (test_plugin_slot_au.mm) ride along — they
# reuse pulp::host + rt_allocation_probe here and must stay off non-Apple (.mm/MSVC).
if(APPLE)
  target_sources(pulp-test-host PRIVATE test_plugin_slot_au.mm)
endif()
# PulpGain_CLAP fixture wiring lives in the ROOT CMakeLists.txt after
# add_subdirectory(examples) (test/ registered first, so a TARGET guard here
# never sees it). The stress case stays in the binary for manual replay, out
# of the default CTest set (flaky; gated target below).
catch_discover_tests(pulp-test-host TEST_SPEC "~[flaky]")

# SignalGraph tests carved out of test_host.cpp to keep the parent
# Focused. No CLAP fixture needed — these exercise pure graph
# routing/topology, not plugin loading.
add_executable(pulp-test-host-signal-graph test_host_signal_graph.cpp)
target_sources(pulp-test-host-signal-graph PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>
    $<$<NOT:$<BOOL:${UNIX}>>:${CMAKE_CURRENT_SOURCE_DIR}/harness/rt_allocation_probe.cpp>)
target_link_libraries(pulp-test-host-signal-graph PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-host-signal-graph)

# NativeHandleVisitor pure-header pattern test. No plugin loading
# required; uses lightweight mock slots to exercise dispatch.
add_executable(pulp-test-native-handle-visitor test_native_handle_visitor.cpp)
target_link_libraries(pulp-test-native-handle-visitor PRIVATE pulp::host Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-native-handle-visitor)

# Opt-in real-plugin integration runner. Builds only when
# PULP_REAL_PLUGIN_TESTS=ON. Even when built, individual TEST_CASEs SKIP
# (via WARN + return) when their fixture binary is not on disk, so the
# binary is safe to run on machines that haven't populated the cache.
# Drive the cache with: python3 tools/scripts/fetch_real_plugins.py
if(PULP_REAL_PLUGIN_TESTS)
    add_executable(pulp-test-real-plugins integration/real_plugin_runner.cpp)
    target_link_libraries(pulp-test-real-plugins
        PRIVATE pulp::host Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-real-plugins PRIVATE
        PULP_REAL_PLUGINS_TOML="${CMAKE_CURRENT_SOURCE_DIR}/integration/real_plugins.toml")
    catch_discover_tests(pulp-test-real-plugins)
endif()

# Fixture-resolver unit test. Always built (no real plugin binaries
# required, no pulp::host link), exercises the
# pinned-vs-developer-supplied lane logic that
# `pulp-test-real-plugins` shares via integration/real_plugin_fixture.hpp.
add_executable(pulp-test-real-plugin-runner-cache
    integration/test_real_plugin_fixture.cpp)
target_link_libraries(pulp-test-real-plugin-runner-cache
    PRIVATE Catch2::Catch2WithMain)
target_compile_definitions(pulp-test-real-plugin-runner-cache PRIVATE
    PULP_REAL_PLUGINS_TOML="${CMAKE_CURRENT_SOURCE_DIR}/integration/real_plugins.toml")
catch_discover_tests(pulp-test-real-plugin-runner-cache)

# Host regression coverage — end-to-end scan / load / process /
# unload lifecycle, scanner failure modes (moduleinfo.json + manifest.ttl),
# state round-trip, and hot-reload race-cleanliness. Separate binary so a
# failing regression case fingers the guilty surface without clouding
# pulp-test-host's scope.
add_executable(pulp-test-host-regression test_host_regression.cpp)
target_link_libraries(pulp-test-host-regression
    PRIVATE pulp::host Catch2::Catch2WithMain)
# PulpGain_CLAP fixture wiring for pulp-test-host-regression lives in the ROOT
# CMakeLists.txt block after add_subdirectory(examples) (see note above).
catch_discover_tests(pulp-test-host-regression)

# Stress test for the SignalGraph hot-reload race (the tracked issue). Runs the
# specific Catch2 case 100 times in a row via a shell wrapper, exiting
# at the first failure. Gated behind PULP_STRESS_FLAKY_TESTS so routine
# CI doesn't pay the ~5s cost; nightly cron / on-demand runs flip the
# flag on. Repeated failures under stress would prove the race is real
# (vs once-in-50-runs Namespace timing-noise) and unblock a
# deterministic-repro fix.
#
# Why a wrapper instead of CTest's REPEAT property: REPEAT only takes
# effect when ctest is invoked with `--repeat until-fail:N` on the
# command line — a property alone is silently a no-op. A shell loop
# inside the test command guarantees repetition without depending on
# how the caller invokes ctest.
if(DEFINED ENV{PULP_STRESS_FLAKY_TESTS} AND NOT "$ENV{PULP_STRESS_FLAKY_TESTS}" STREQUAL "")
    if(WIN32)
        # PowerShell branch already shows full stdout; that's intentional —
        # the failing iteration's Catch2 assertion context + RNG seed need
        # to land in the ctest log so the failure is replayable. Per-
        # success noise is the cost.
        add_test(NAME pulp-test-signal-graph-hot-reload-stress
            COMMAND powershell -Command
                "for ($i = 1; $i -le 100; $i++) { & '$<TARGET_FILE:pulp-test-host-regression>' 'SignalGraph hot-reload mid-audio is race-free via snapshot publish' --rng-seed=time; if ($LASTEXITCODE -ne 0) { Write-Host \"stress failed on iteration $i\"; exit 1 } } Write-Host 'stress ok (100/100)'")
    else()
        # POSIX branch buffers each iteration's stdout into a temp file
        # and only prints it on failure. The earlier `>/dev/null` shape
        # discarded failure diagnostics; the whole point of the stress
        # harness is to capture a replayable seed when an iteration
        # fails, so silently dropping Catch2's assertion context defeats
        # it.
        # On success, the per-iteration buffer is overwritten on the
        # next loop; only the final iteration's (or first failure's)
        # contents survive in /tmp, with the loop's own "stress ok"
        # / "stress failed on iteration N" line as the visible signal.
        add_test(NAME pulp-test-signal-graph-hot-reload-stress
            COMMAND bash -c
                "set -e; out=$(mktemp); trap 'rm -f \"$out\"' EXIT; for i in $(seq 1 100); do if ! '$<TARGET_FILE:pulp-test-host-regression>' 'SignalGraph hot-reload mid-audio is race-free via snapshot publish' --rng-seed=time >\"$out\" 2>&1; then echo \"stress failed on iteration $i — Catch2 output:\"; cat \"$out\"; exit 1; fi; done; echo 'stress ok (100/100)'")
    endif()
    set_tests_properties(pulp-test-signal-graph-hot-reload-stress PROPERTIES
        LABELS "host;graph;hot-reload;race;stress;issue-669"
        TIMEOUT 600)
endif()

# GraphSerializer round-trip tests
pulp_add_test_suite(pulp-test-graph-serializer LIBRARIES pulp::host)

# NetworkServiceDiscovery backend-dispatch tests
pulp_add_test_suite(pulp-test-network-service-discovery LIBRARIES pulp::events)

# WAMv2 + WebCLAP format adapter tests
add_executable(pulp-test-wam-wclap test_wam_wclap.cpp
    ${CMAKE_SOURCE_DIR}/core/format/src/wasm/wam_adapter.cpp
)
target_link_libraries(pulp-test-wam-wclap PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-wam-wclap PRIVATE
    ${CMAKE_SOURCE_DIR}/core/format/include
)
catch_discover_tests(pulp-test-wam-wclap)

# CLAP webview extension tests
add_executable(pulp-test-clap-webview test_clap_webview.cpp
    ${CMAKE_SOURCE_DIR}/core/format/src/wasm/clap_webview.cpp
)
target_link_libraries(pulp-test-clap-webview PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-clap-webview PRIVATE
    ${CMAKE_SOURCE_DIR}/core/format/include
)
catch_discover_tests(pulp-test-clap-webview)

# Web demo plugin tests (PulpPluck, PulpChorus)
add_executable(pulp-test-web-demos test_web_demos.cpp)
target_link_libraries(pulp-test-web-demos PRIVATE pulp::format Catch2::Catch2WithMain)
target_include_directories(pulp-test-web-demos PRIVATE
    ${CMAKE_SOURCE_DIR}/examples/web-demos
    ${CMAKE_SOURCE_DIR}/examples/pulp-pluck
)
catch_discover_tests(pulp-test-web-demos)

# AI Designer: design tool layout/parity tests
pulp_add_test_suite(pulp-test-design-tool-layout LIBRARIES pulp::view pulp::state)

add_executable(pulp-test-design-debug-contracts test_design_debug_contracts.cpp)
target_link_libraries(pulp-test-design-debug-contracts PRIVATE
    pulp::view
    pulp::state
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-design-debug-contracts)

# AI Designer: style pack tests
pulp_add_test_suite(pulp-test-style-pack LIBRARIES pulp::view)

# Removed: pulp-test-token-diff (test_token_diff.cpp) and pulp-test-showcase
# (test_showcase.cpp) were placeholder TEST_CASE("...") { REQUIRE(true); }
# stubs for design-tool C++ classes that never shipped. The design tool
# landed as JS via core/view/js/web-compat* and widget_bridge's
# applyTokenDiff JS binding; there is no C++ surface to verify here.
# File a new focused test if a C++
# token-diff or Showcase class is ever added.

# WindowManager multi-window framework tests
pulp_add_test_suite(pulp-test-window-manager LIBRARIES pulp::view)

# Multi-window rendering fairness + per-window RenderLoop isolation
# Mirrors pulp-test-render-loop:
# compiles render_loop.cpp directly with PULP_RENDER_LOOP_FORCE_TIMER=1 so
# the test runs headlessly on every host (GPU off / no native vsync source).
add_executable(pulp-test-multi-window
    test_multi_window.cpp
    ${CMAKE_SOURCE_DIR}/core/render/src/render_loop.cpp)
target_compile_definitions(pulp-test-multi-window PRIVATE
    PULP_RENDER_LOOP_FORCE_TIMER=1)
target_include_directories(pulp-test-multi-window PRIVATE
    ${CMAKE_SOURCE_DIR}/core/render/include
    ${CMAKE_SOURCE_DIR}/core/render/src)
target_link_libraries(pulp-test-multi-window PRIVATE
    pulp::view
    pulp::runtime
    Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-multi-window)

# ── Theme management, widgets, asset system ─────────────────────────────────

# Theme contrast utilities (WCAG AA)
pulp_add_test_suite(pulp-test-theme-contrast LIBRARIES pulp::view)

# Theme preset library (38 tweakcn + derivation)
pulp_add_test_suite(pulp-test-theme-presets LIBRARIES pulp::view)

# Button widgets — theme-token wiring + rgba()-clamp bug-fix regression guard
pulp_add_test_suite(pulp-test-buttons LIBRARIES pulp::view)

# Gap widgets — Badge/Banner/Toast/EmptyState/Stepper/Pan/Popover/Dialog/ChannelStrip
pulp_add_test_suite(pulp-test-gap-widgets LIBRARIES pulp::view)

# Widget gallery — themed board of every primitive; builds + renders
pulp_add_test_suite(pulp-test-widget-gallery LIBRARIES pulp::view)

# The sizing delegate, and the two widgets reworked to consult it: a menu that
# can compute its whole geometry with no canvas and no paint, and a label whose
# inline editor is a real child view.
pulp_add_test_suite(pulp-test-widget-metrics LIBRARIES pulp::view)
