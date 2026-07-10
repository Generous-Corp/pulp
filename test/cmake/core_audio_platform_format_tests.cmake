# Core audio, platform, format, and utility test registrations.
# Included by test/CMakeLists.txt; keep related test registrations here.

# WAV metadata round-trip tests (BWAV / iXML / ASWG / ACID — item 6.11)
pulp_add_test_suite(pulp-test-wav-metadata LIBRARIES pulp::audio)

pulp_add_test_suite(pulp-test-offline-processor-edges LIBRARIES pulp::audio)

# Offline-render DSP tracing (Perfetto, dev-only). Config-agnostic: OFF verifies
# the no-op contract; ON byte-checks the flushed .pftrace for the dsp / dsp.node
# offline span names. Links pulp::format + the PulpCompressor example header so
# one case proves the spans wrap a real Processor::process() driven headless.
pulp_add_test_suite(pulp-test-offline-tracing
    LIBRARIES pulp::audio pulp::runtime pulp::format
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/pulp-compressor)

pulp_add_test_suite(pulp-test-ogg-reader LIBRARIES pulp::audio)

# Audio tests
# PROCESSORS reservation for suites that open the REAL CoreAudio output device.
# CoreAudioDevice::stop()/close() call AudioOutputUnitStop / AudioUnitUninitialize
# (coreaudio_device.mm), which block until the real-time I/O thread observes the
# request. Under `ctest -j8` heavy load that RT thread is CPU-starved and never
# observes it, so teardown hangs to the 120s timeout — the full-suite-only flake
# that wedged the macOS self-hosted runner. Reserving PROCESSORS == the CI -j
# makes ctest schedule these alone, so the RT thread gets the machine and
# teardown returns promptly. (Root-cause fix replacing the build.yml exclude.)
pulp_add_test_suite(pulp-test-audio LIBRARIES pulp::audio
    PROPERTIES PROCESSORS 8)

pulp_add_test_suite(pulp-test-system-volume LIBRARIES pulp::audio)

# Streaming sample source: preload window + background-filled ring + RT-safe
# sequential pull. Threaded case has an internal deadline; give headroom.
pulp_add_test_suite(pulp-test-streaming-sample-source LIBRARIES pulp::audio
    PROPERTIES TIMEOUT 60)

# Multi-mic / velocity-layer / round-robin layered zone selection.
pulp_add_test_suite(pulp-test-zone-layer-select LIBRARIES pulp::audio)

# Excerpt window enumeration tests
pulp_add_test_suite(pulp-test-audio-excerpt LIBRARIES pulp::audio)

# Repo-level audio tooling tests
pulp_add_test_suite(pulp-test-audio-tools LIBRARIES pulp::tool-audio)

# MCP server protocol tests
add_executable(pulp-test-mcp-server test_mcp_server.cpp)
# mcp_tools.cpp (included into the test TU) reads the agent-request queue via the
# choc-only agent_request_queue unit; compile that TU + its include dir in rather
# than linking all of pulp::inspect (which would drag in the GPU/Skia chain),
# matching how pulp-mcp itself consumes it.
target_sources(pulp-test-mcp-server PRIVATE
    "${CMAKE_SOURCE_DIR}/inspect/src/agent_request_queue.cpp")
target_link_libraries(pulp-test-mcp-server PRIVATE pulp::tool-audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-mcp-server PRIVATE
    "${CMAKE_SOURCE_DIR}/inspect/include")
target_compile_definitions(pulp-test-mcp-server PRIVATE
    PULP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# test_mcp_server.cpp #include's ../tools/mcp/pulp_mcp.cpp directly (via
# `#define main pulp_mcp_main_for_test`), so it picks up the same
# generated version header pulp-mcp itself sees. The generated header
# lives under the binary tree's tools/mcp/ directory.
target_include_directories(pulp-test-mcp-server PRIVATE
    "${CMAKE_BINARY_DIR}/tools/mcp")
add_dependencies(pulp-test-mcp-server pulp-mcp)
catch_discover_tests(pulp-test-mcp-server)

# MIDI tests
pulp_add_test_suite(pulp-test-midi LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-midi-file LIBRARIES pulp::midi)

pulp_add_test_suite(pulp-test-tuning LIBRARIES pulp::midi)

# State tests
pulp_add_test_suite(pulp-test-state
    SOURCES test_state.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::state pulp::events)

# Binding tests
pulp_add_test_suite(pulp-test-binding LIBRARIES pulp::state)
pulp_add_test_suite(pulp-test-external-binding LIBRARIES pulp::state)

# Structured non-param state channel (sequencer/mod-matrix transport)
pulp_add_test_suite(pulp-test-sequencer-state-channel LIBRARIES pulp::state)

# Freeze coverage: parametric SequencerConfig (second/non-square/custom-cell
# configs), the shared step_edit_reducer, and the neutral non-Processor producer.
pulp_add_test_suite(pulp-test-sequencer-freeze LIBRARIES pulp::state)

# StepGridView — the UI consumer of the sequencer state channel
pulp_add_test_suite(pulp-test-step-grid-view LIBRARIES pulp::view pulp::state)

# Headless adapter tests
pulp_add_test_suite(pulp-test-headless LIBRARIES pulp::format)

# Standalone host render-path RT-safety guard. Drives the extracted
# StandaloneApp::render_audio_block() seam for one steady-state block under
# ScopedRtProcessProbe. Links the RT interposition trap TU + sets
# PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS (UNIX-only: strong operator-new +
# pthread overrides) so an allocation / blocking lock on the render path ABORTS
# the test. Mirrors the CLAP/AU-v2 RT-trap targets.
add_executable(pulp-test-standalone-rt test_standalone_rt.cpp)
target_sources(pulp-test-standalone-rt PRIVATE
    $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>)
target_link_libraries(pulp-test-standalone-rt PRIVATE
    pulp::standalone Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
target_compile_definitions(pulp-test-standalone-rt PRIVATE
    $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
catch_discover_tests(pulp-test-standalone-rt)
pulp_add_test_suite(pulp-test-audio-inspector-demo-processor SOURCES test_audio_inspector_demo_processor.cpp LIBRARIES pulp::format pulp::audio pulp::midi)
# .pulpset render/replay harness (G4)
pulp_add_test_suite(pulp-test-pulpset-replay SOURCES test_pulpset_replay.cpp LIBRARIES pulp::format)

# Diagnostic reporter tests
pulp_add_test_suite(pulp-test-diagnostic SOURCES test_diagnostic_reporter.cpp LIBRARIES pulp::format)

# CLAP entry point macro test
if(PULP_HAS_CLAP)
    add_executable(pulp-test-clap-entry test_clap_entry.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_remote_controls.cpp
    )
    target_link_libraries(pulp-test-clap-entry PRIVATE pulp::format clap Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-clap-entry PRIVATE PULP_CLAP_GUI=1)
    catch_discover_tests(pulp-test-clap-entry)

    # CLAP inbound/outbound MIDI event coverage + steady-state RT process guard.
    #
    # Unlike `pulp-test-clap-entry` above, this test DOES NOT re-compile
    # clap_adapter.cpp into its own TU. The test drives the adapter via
    # its public header surface (clap_init / clap_activate / clap_process
    # — see core/format/include/pulp/format/clap_adapter.hpp), so linking
    # against pulp::format is sufficient. Keeping the direct compilation
    # out means the coverage artifact reports exactly one TU for
    # clap_adapter.cpp (the library's), which is what CI's diff-cover
    # gate reads — avoids the split-TU problem that produced "96% local
    # / 62% CI" reports.
    add_executable(pulp-test-clap-midi-events test_clap_midi_events.cpp)
    target_sources(pulp-test-clap-midi-events PRIVATE $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>)
    target_link_libraries(pulp-test-clap-midi-events PRIVATE pulp::format clap Catch2::Catch2WithMain ${CMAKE_DL_LIBS})
    target_compile_definitions(pulp-test-clap-midi-events PRIVATE PULP_CLAP_GUI=1 $<$<BOOL:${UNIX}>:PULP_CLAP_PROCESS_RT_TRAP_TESTS=1>)
    catch_discover_tests(pulp-test-clap-midi-events)

    # Pin the CLAP host-facing API contract that Bitwig / Reaper /
    # FL Studio / Studio One depend on. Re-compiles the
    # adapter TU into its own executable so the test owns the
    # PULP_CLAP_PLUGIN(...) entry-symbol generator.
    add_executable(pulp-test-clap-host-validation test_clap_host_validation.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_remote_controls.cpp
    )
    target_link_libraries(pulp-test-clap-host-validation PRIVATE pulp::format clap Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-clap-host-validation PRIVATE PULP_CLAP_GUI=1)
    catch_discover_tests(pulp-test-clap-host-validation)

    # A Processor may reach its StateStore from its destructor, and from a worker
    # thread that destructor is about to join. Pin that every host destroys the
    # store after the Processor, not before.
    add_executable(pulp-test-store-lifetime test_store_lifetime.cpp)
    target_link_libraries(pulp-test-store-lifetime PRIVATE pulp::format clap Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-store-lifetime PRIVATE PULP_CLAP_GUI=1)
    catch_discover_tests(pulp-test-store-lifetime)

    # Empirical proof the CLAP adapter respects clamp_latency_to_nonneg
    # end-to-end (negative latency → 0 when the
    # quirk is enforced, raw-wrapped when PULP_HOST_QUIRKS=off). Drives the
    # adapter through its public header surface (init() caches the resolved
    # quirks), so link-only — no clap_adapter.cpp re-compile, keeping the
    # diff-cover TU attribution single (same rationale as the MIDI test).
    add_executable(pulp-test-clap-latency-quirk test_clap_latency_quirk.cpp)
    target_link_libraries(pulp-test-clap-latency-quirk PRIVATE pulp::format clap Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-clap-latency-quirk PRIVATE PULP_CLAP_GUI=1)
    catch_discover_tests(pulp-test-clap-latency-quirk)
endif()

if(PULP_HAS_VST3)
    add_executable(pulp-test-vst3-plugin-state
        test_vst3_plugin_state.cpp harness/rt_allocation_probe.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/vst3_plug_view.cpp
    )
    target_link_libraries(pulp-test-vst3-plugin-state PRIVATE pulp::format Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-vst3-plugin-state PRIVATE
        PULP_VST3=1
        PULP_VST3_GUI=1
    )
    catch_discover_tests(pulp-test-vst3-plugin-state)
endif()

if(APPLE AND PULP_HAS_AUSDK)
    add_executable(pulp-test-au-plugin-state
        test_au_plugin_state.mm
        ${CMAKE_SOURCE_DIR}/core/format/src/au_adapter.mm
    )
    target_link_libraries(pulp-test-au-plugin-state PRIVATE
        pulp::format
        ausdk
        Catch2::Catch2WithMain
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreAudioKit"
        "-framework Foundation"
    )
    set_target_properties(pulp-test-au-plugin-state PROPERTIES
        CXX_STANDARD 23
        OBJCXX_STANDARD 23
    )
    catch_discover_tests(pulp-test-au-plugin-state)

    # AU v2 instrument (aumu) render-path RT-safety guard. Drives
    # PulpAUInstrument::Render for one steady-state block under
    # ScopedRtProcessProbe. Links the RT interposition trap TU and sets
    # PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS so an allocation / blocking lock on
    # the render path ABORTS the test (mirrors the CLAP MIDI RT-trap target
    # above). Trap TU is UNIX-only (strong operator-new + pthread overrides);
    # APPLE satisfies ${UNIX}.
    add_executable(pulp-test-au-v2-instrument-rt
        test_au_v2_instrument_rt.mm
    )
    target_sources(pulp-test-au-v2-instrument-rt PRIVATE
        $<$<BOOL:${UNIX}>:${CMAKE_CURRENT_SOURCE_DIR}/native_components/rt_intercept_test_support.cpp>)
    target_link_libraries(pulp-test-au-v2-instrument-rt PRIVATE
        pulp::format
        ausdk
        Catch2::Catch2WithMain
        ${CMAKE_DL_LIBS}
        "-framework AudioToolbox"
        "-framework Foundation"
    )
    target_compile_definitions(pulp-test-au-v2-instrument-rt PRIVATE
        $<$<BOOL:${UNIX}>:PULP_NATIVE_CORE_PROCESS_RT_TRAP_TESTS=1>)
    set_target_properties(pulp-test-au-v2-instrument-rt PROPERTIES
        CXX_STANDARD 23
        OBJCXX_STANDARD 23
    )
    catch_discover_tests(pulp-test-au-v2-instrument-rt)
endif()

# LV2 adapter tests
if(PULP_HAS_LV2)
    add_executable(pulp-test-lv2-adapter test_lv2_adapter.cpp
        ${CMAKE_SOURCE_DIR}/core/format/src/lv2_adapter.cpp
    )
    target_link_libraries(pulp-test-lv2-adapter PRIVATE pulp::format lv2-headers Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-lv2-adapter)

    add_executable(pulp-test-lv2-host-discovery test_lv2_host_discovery.cpp)
    target_include_directories(pulp-test-lv2-host-discovery PRIVATE
        ${CMAKE_SOURCE_DIR}/core/host/src)
    target_link_libraries(pulp-test-lv2-host-discovery PRIVATE
        pulp::host
        lv2-headers
        Catch2::Catch2WithMain)

    if(UNIX)
        set(_pulp_lv2_slot_probe_bundle
            "${CMAKE_CURRENT_BINARY_DIR}/Lv2SlotProbe.lv2")
        set(_pulp_lv2_slot_probe_binary
            "${_pulp_lv2_slot_probe_bundle}/lv2-slot-probe${CMAKE_SHARED_MODULE_SUFFIX}")

        add_library(pulp-test-lv2-slot-probe MODULE
            fixtures/lv2_slot_probe.cpp)
        target_link_libraries(pulp-test-lv2-slot-probe PRIVATE lv2-headers)
        set_target_properties(pulp-test-lv2-slot-probe PROPERTIES
            PREFIX ""
            OUTPUT_NAME "lv2-slot-probe"
            LIBRARY_OUTPUT_DIRECTORY "${_pulp_lv2_slot_probe_bundle}")

        file(MAKE_DIRECTORY "${_pulp_lv2_slot_probe_bundle}")
        string(CONCAT _pulp_lv2_slot_probe_ttl [=[
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .

<http://example.com/pulp/lv2-slot-probe>
    a lv2:Plugin ;
    lv2:binary <lv2-slot-probe]=]
    "${CMAKE_SHARED_MODULE_SUFFIX}"
    [=[> ;
    lv2:port
    [
        a lv2:InputPort , lv2:AudioPort ;
        lv2:index 0 ;
        lv2:name "Input L"
    ] ,
    [
        a lv2:InputPort , lv2:AudioPort ;
        lv2:index 1 ;
        lv2:name "Input R"
    ] ,
    [
        a lv2:OutputPort , lv2:AudioPort ;
        lv2:index 2 ;
        lv2:name "Output L"
    ] ,
    [
        a lv2:OutputPort , lv2:AudioPort ;
        lv2:index 3 ;
        lv2:name "Output R"
    ] ,
    [
        a lv2:InputPort , lv2:ControlPort ;
        lv2:index 4 ;
        lv2:name "Probe Gain" ;
        lv2:default 0.5 ;
        lv2:minimum -2.0 ;
        lv2:maximum 4.0
    ] .
]=])
        file(WRITE "${_pulp_lv2_slot_probe_bundle}/plugin.ttl"
            "${_pulp_lv2_slot_probe_ttl}")

        add_dependencies(pulp-test-lv2-host-discovery
            pulp-test-lv2-slot-probe)
        target_compile_definitions(pulp-test-lv2-host-discovery PRIVATE
            PULP_TEST_LV2_SLOT_PROBE_BUNDLE="${_pulp_lv2_slot_probe_bundle}"
            PULP_TEST_LV2_SLOT_PROBE_BINARY="${_pulp_lv2_slot_probe_binary}")
    endif()

    catch_discover_tests(pulp-test-lv2-host-discovery)
endif()

# NSIS installer script generation tests (cross-platform — pure string output)
add_executable(pulp-test-nsis-installer test_nsis_installer.cpp
    ${CMAKE_SOURCE_DIR}/ship/platform/win/installer_win.cpp
)
target_link_libraries(pulp-test-nsis-installer PRIVATE pulp::ship Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-nsis-installer)
