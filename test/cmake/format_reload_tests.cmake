# Format, reload, validation, shipping, runtime, audio-focus, and MIDI-adjacent tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# Grouped executables for this manifest (pulp_add_test_group in
# tools/cmake/PulpTestSuite.cmake): each member keeps its own registration,
# labels and properties; only the binary behind them is shared. Members are
# grouped by the compile line they already had: the pulp::format line (plus
# hot-reload-synth, whose example include root is a per-source property),
# pulp::midi, pulp::runtime, pulp::audio, pulp::ship, the cpp-httplib line,
# the view-only reload suites, the pulp::view + pulp::format line and, on
# Apple, the C++23 AudioUnitSDK line. A suite stays on its own when it needs
# its own process or compile line: the RT allocation probe (aax-rt-safety),
# the reload suites that dlopen a fixture baked in with $<TARGET_FILE:...>
# (reload-library, reload-transaction, reload-controller, reloadable-shell),
# clap-ara-extension (PULP_CLAP_GUI changes the CLAP adapter's layout),
# plugin-state-io (registers a process-wide envelope migration), the
# AAX SDK suites (compile an adapter .cpp), the Cocoa-view and ObjC++ AU
# suites, and suites whose line no sibling shares (osc-channel,
# settings-sections, xcode-developer-path, linux-packaging).
pulp_add_test_group(pulp-test-group-fmt-format
    LIBRARIES pulp::format pulp::signal pulp::midi pulp::audio pulp::runtime)
pulp_add_test_group(pulp-test-group-fmt-midi LIBRARIES pulp::midi)
pulp_add_test_group(pulp-test-group-fmt-runtime LIBRARIES pulp::runtime)
pulp_add_test_group(pulp-test-group-fmt-audio LIBRARIES pulp::audio pulp::midi pulp::state)
pulp_add_test_group(pulp-test-group-fmt-ship LIBRARIES pulp::ship pulp::runtime)
pulp_add_test_group(pulp-test-group-fmt-httplib LIBRARIES pulp::runtime pulp-cpp-httplib)
pulp_add_test_group(pulp-test-group-fmt-reload-view LIBRARIES pulp::view)
pulp_add_test_group(pulp-test-group-fmt-format-view
    LIBRARIES pulp::view pulp::state pulp::format pulp::midi pulp::audio)

# AudioFocusRegistry — cross-platform observer for OS audio-focus
# signals (Android AudioManager.OnAudioFocusChangeListener,
# AVAudioSession interruptions). Pure C++ unit tests — no platform deps.
pulp_add_test_suite(pulp-test-audio-focus GROUP pulp-test-group-fmt-audio
    LIBRARIES pulp::audio)
# `pulp audio render` arg grammar + the device-free block stepper (no PluginSlot).
pulp_add_test_suite(pulp-test-cmd-audio-render GROUP pulp-test-group-fmt-audio
    SOURCES test_cmd_audio_render.cpp ${CMAKE_SOURCE_DIR}/tools/cli/cmd_audio_render_parse.cpp
    LIBRARIES pulp::audio pulp::midi pulp::state)
# Zero-fill helper for the Oboe input-frame-read short-read path.
# Header-only math, so no library link required beyond Catch2.
pulp_add_test_suite(pulp-test-frame-fill GROUP pulp-test-group-fmt-audio
    LIBRARIES pulp::audio)
# MonotonicMidiClock produces seconds-since-open MIDI input timestamps
# without ALSA-specific assumptions.
pulp_add_test_suite(pulp-test-midi-monotonic-clock GROUP pulp-test-group-fmt-midi
    LIBRARIES pulp::midi)

# BLE-MIDI 1.0 packet codec + cross-platform BleMidiCentral factory contract. The
# CoreBluetooth backend's live discovery / pairing path is exercised
# manually (requires a real adapter + a paired peripheral); these
# tests pin the deterministic codec + the stub central behavior.
# Keeping the codec tests host-independent lets every CI lane validate
# timestamp/running-status handling without Bluetooth hardware.
pulp_add_test_suite(pulp-test-ble-midi GROUP pulp-test-group-fmt-midi
    LIBRARIES pulp::midi)
pulp_add_test_suite(pulp-test-android-ble-midi-registry GROUP pulp-test-group-fmt-midi
    SOURCES test_android_ble_midi_registry.cpp
    LIBRARIES pulp::midi)

# UmpSession + UmpEndpoint + VirtualUmpEndpoint headless tests for the
# cross-platform session + virtual-endpoint
# registry. The macOS .mm path is exercised opportunistically; the
# suite runs on every platform via the virtual-endpoint half.
pulp_add_test_suite(pulp-test-ump-session GROUP pulp-test-group-fmt-midi
    LIBRARIES pulp::midi)

# AsyncStream + portable-exceptions macro tests
pulp_add_test_suite(pulp-test-async-stream GROUP pulp-test-group-fmt-runtime
    LIBRARIES pulp::runtime)
pulp_add_test_suite(pulp-test-exceptions GROUP pulp-test-group-fmt-runtime
    LIBRARIES pulp::runtime)

# Network Stream tests
pulp_add_test_suite(pulp-test-network-stream GROUP pulp-test-group-fmt-httplib
    LIBRARIES pulp::runtime pulp-cpp-httplib
    PROPERTIES RESOURCE_LOCK network-stream)

# MessageChannel tests
pulp_add_test_suite(pulp-test-memory-message-channel GROUP pulp-test-group-fmt-runtime
    LIBRARIES pulp::runtime)
pulp_add_test_suite(pulp-test-websocket-channel GROUP pulp-test-group-fmt-runtime
    LIBRARIES pulp::runtime)

pulp_add_test_suite(pulp-test-osc-channel LIBRARIES pulp::osc)

pulp_add_test_suite(pulp-test-json-rpc GROUP pulp-test-group-fmt-runtime
    LIBRARIES pulp::runtime)

# Validation harness tests
pulp_add_test_suite(pulp-test-validation-harness GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# Reusable validation assertion helpers (validation_assertions.hpp)
pulp_add_test_suite(pulp-test-validation-assertions GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# DSP hot reload build-fingerprint gate.
pulp_add_test_suite(pulp-test-build-fingerprint GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# DSP hot reload RT-safe processor hot-swap slot.
pulp_add_test_suite(pulp-test-hotswap-slot GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# DSP hot reload parameter-contract equivalence checker.
pulp_add_test_suite(pulp-test-param-contract GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-reload-capabilities GROUP pulp-test-group-fmt-reload-view
    LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-reload-pack-build GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-reload-key-store GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-reload-remote-update GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-reload-remote-fetch GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-reload-trust-policy GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-reload-autocaps GROUP pulp-test-group-fmt-reload-view
    LIBRARIES pulp::view)

# Signed revocation list and namespaced monotonic epoch floor.
pulp_add_test_suite(pulp-test-reload-revocation GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format
    DISCOVERY_ARGS TEST_PREFIX "revocation.")
pulp_add_test_suite(pulp-test-pack-install GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-pack-signing-ui GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# DSP hot reload dynamic-library handle + leak policy. Builds a tiny
# MODULE fixture and dlopens it by absolute path (RELOAD_PROBE_PATH).
add_library(pulp-reload-probe MODULE fixtures/reload_probe.cpp)
set_target_properties(pulp-reload-probe PROPERTIES
    PREFIX "" POSITION_INDEPENDENT_CODE ON)
pulp_add_test_suite(pulp-test-reload-library LIBRARIES pulp::format)
add_dependencies(pulp-test-reload-library pulp-reload-probe)
target_compile_definitions(pulp-test-reload-library PRIVATE
    RELOAD_PROBE_PATH="$<TARGET_FILE:pulp-reload-probe>")
if(NOT WIN32)
    target_link_libraries(pulp-test-reload-library PRIVATE ${CMAKE_DL_LIBS})
endif()

# DSP hot reload verify-before-commit transaction, end to end. Two
# logic-library fixtures (compatible / incompatible parameter contract) are
# built as MODULEs and dlopen'd by the transaction; the test asserts the
# fingerprint + contract gates, the live swap, and parameter-state continuity.
add_library(pulp-reload-logic-compat MODULE fixtures/reload_logic_compatible.cpp)
add_library(pulp-reload-logic-incompat MODULE fixtures/reload_logic_incompatible.cpp)
add_library(pulp-reload-logic-throwing MODULE fixtures/reload_logic_throwing.cpp)
add_library(pulp-reload-logic-nan MODULE fixtures/reload_logic_nan.cpp)
# Static-constructor marker fixture for rejected-pack verify-before-load tests.
add_library(pulp-reload-logic-ctor-marker MODULE fixtures/reload_logic_ctor_marker.cpp)
foreach(_pulp_reload_logic pulp-reload-logic-compat pulp-reload-logic-incompat
                           pulp-reload-logic-throwing pulp-reload-logic-nan
                           pulp-reload-logic-ctor-marker)
    set_target_properties(${_pulp_reload_logic} PROPERTIES
        PREFIX "" POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(${_pulp_reload_logic} PRIVATE pulp::format)
endforeach()
pulp_add_test_suite(pulp-test-reload-transaction LIBRARIES pulp::format)
add_dependencies(pulp-test-reload-transaction
    pulp-reload-logic-compat pulp-reload-logic-incompat pulp-reload-logic-throwing
    pulp-reload-logic-nan pulp-reload-logic-ctor-marker)
target_compile_definitions(pulp-test-reload-transaction PRIVATE
    RELOAD_LOGIC_COMPATIBLE="$<TARGET_FILE:pulp-reload-logic-compat>"
    RELOAD_LOGIC_INCOMPATIBLE="$<TARGET_FILE:pulp-reload-logic-incompat>"
    RELOAD_LOGIC_THROWING="$<TARGET_FILE:pulp-reload-logic-throwing>"
    RELOAD_LOGIC_NAN="$<TARGET_FILE:pulp-reload-logic-nan>"
    RELOAD_LOGIC_CTOR_MARKER="$<TARGET_FILE:pulp-reload-logic-ctor-marker>")
if(NOT WIN32)
    target_link_libraries(pulp-test-reload-transaction PRIVATE ${CMAKE_DL_LIBS})
endif()

# DSP hot reload watch->reload dev loop. Reuses the compatible /
# incompatible logic fixtures (above); the test copies them to a watched temp
# path and drives ReloadController::poll().
pulp_add_test_suite(pulp-test-reload-controller LIBRARIES pulp::format)
add_dependencies(pulp-test-reload-controller
    pulp-reload-logic-compat pulp-reload-logic-incompat)
target_compile_definitions(pulp-test-reload-controller PRIVATE
    RELOAD_LOGIC_COMPATIBLE="$<TARGET_FILE:pulp-reload-logic-compat>"
    RELOAD_LOGIC_INCOMPATIBLE="$<TARGET_FILE:pulp-reload-logic-incompat>"
    # Watch a path inside the build tree, NOT /tmp: macOS dyld kills a process
    # that dlopens an unsigned .dylib from a world-writable temp dir.
    RELOAD_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
if(NOT WIN32)
    target_link_libraries(pulp-test-reload-controller PRIVATE ${CMAKE_DL_LIBS})
endif()

# DSP hot reload DAW-integration shell, end to end. The shell is a
# Processor (format adapters wrap it) that loads its DSP from a logic library and
# hot-swaps it via a background watcher. A "unity" (1x) fixture is the initial
# logic so a swap to the 2x "compatible" fixture is observable in the output.
add_library(pulp-reload-logic-unity MODULE fixtures/reload_logic_unity.cpp)
set_target_properties(pulp-reload-logic-unity PROPERTIES
    PREFIX "" POSITION_INDEPENDENT_CODE ON)
target_link_libraries(pulp-reload-logic-unity PRIVATE pulp::format)
pulp_add_test_suite(pulp-test-live-swap-transaction GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-swap-pack GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format pulp::runtime)
pulp_add_test_suite(pulp-test-hot-reload-synth GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/hot-reload-synth)
pulp_add_test_suite(pulp-test-hot-reload-morph GROUP pulp-test-group-fmt-format-view
    LIBRARIES pulp::format pulp::view
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/hot-reload-morph)
pulp_add_test_suite(pulp-test-reloadable-shell LIBRARIES pulp::format)
add_dependencies(pulp-test-reloadable-shell
    pulp-reload-logic-unity pulp-reload-logic-compat pulp-reload-logic-incompat)
target_compile_definitions(pulp-test-reloadable-shell PRIVATE
    RELOAD_LOGIC_UNITY="$<TARGET_FILE:pulp-reload-logic-unity>"
    RELOAD_LOGIC_COMPATIBLE="$<TARGET_FILE:pulp-reload-logic-compat>"
    RELOAD_LOGIC_INCOMPATIBLE="$<TARGET_FILE:pulp-reload-logic-incompat>"
    RELOAD_WATCH_DIR="${CMAKE_CURRENT_BINARY_DIR}")
if(NOT WIN32)
    target_link_libraries(pulp-test-reloadable-shell PRIVATE ${CMAKE_DL_LIBS})
endif()
find_package(Threads REQUIRED)
target_link_libraries(pulp-test-reloadable-shell PRIVATE Threads::Threads)

# Shipping builds must exclude the development reload watcher symbol.
if(UNIX)
    add_executable(pulp-reload-ship-fixture-dev fixtures/reload_ship_fixture.cpp)
    target_link_libraries(pulp-reload-ship-fixture-dev PRIVATE
        pulp::format Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_definitions(pulp-reload-ship-fixture-dev PRIVATE
        PULP_RELOAD_SHIP_FIXTURE_MARK=1)

    add_executable(pulp-reload-ship-fixture-ship fixtures/reload_ship_fixture.cpp)
    target_link_libraries(pulp-reload-ship-fixture-ship PRIVATE
        pulp::format Threads::Threads ${CMAKE_DL_LIBS})
    target_compile_definitions(pulp-reload-ship-fixture-ship PRIVATE
        PULP_RELOAD_DEV_WATCHER=0)

    add_test(NAME pulp-reload-ship-symbol-absence
        COMMAND ${CMAKE_COMMAND}
            -DDEV_BIN=$<TARGET_FILE:pulp-reload-ship-fixture-dev>
            -DSHIP_BIN=$<TARGET_FILE:pulp-reload-ship-fixture-ship>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/reload_ship_symbol_check.cmake)
endif()

# Plugin-contributed settings sections; SettingsPanel lives in pulp::standalone.
pulp_add_test_suite(pulp-test-settings-sections LIBRARIES pulp::standalone)

# Own process: registers a process-wide envelope migration (0 -> current)
# that cannot be unregistered, which would make restore-diagnostics' "no
# migration available" case pass or fail depending on case order.
pulp_add_test_suite(pulp-test-plugin-state-io LIBRARIES pulp::format)
pulp_add_test_suite(pulp-test-plugin-state-restore-diagnostics GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# Shared max-block-overrun contract (clamp+zero) used by every format adapter
pulp_add_test_suite(pulp-test-max-block-contract GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# AAX render-path RT-safety: MIDI-out buffers reserved off-thread so a
# supports_midi_output plugin never allocates inside the ScopedNoAlloc guard.
pulp_add_test_suite(pulp-test-aax-rt-safety
    SOURCES test_aax_rt_safety.cpp harness/rt_allocation_probe.cpp
    LIBRARIES pulp::midi)

# AAX metadata/model tests
pulp_add_test_suite(pulp-test-aax-model GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# AAX editor support — gesture touch/release routing and the editor sizing
# contract (SDK-free pure logic behind the AAX_CEffectGUI shell)
pulp_add_test_suite(pulp-test-aax-editor GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# AAX MIDI bridge — SDK-gated runtime test. test_aax_midi.cpp covers the
# reassembly/fragmentation SDK-free; this drives the thin SDK glue
# (decode_midi_node / encode_midi_node in aax_midi_node.cpp) through the real
# Avid AAX SDK types (AAX_IMIDINode / AAX_CMidiStream / AAX_CMidiPacket), so it
# only builds when the developer-supplied SDK is configured (PULP_ENABLE_AAX=ON
# + a valid PULP_AAX_SDK_DIR). The adapter source is compiled directly into the
# test because aax_midi_node.cpp is otherwise only built per-plugin.
if(PULP_HAS_AAX)
    # Ensure the AAX SDK interface/library targets exist even when no AAX plugin
    # (i.e. no example) is part of this configure. The helper is idempotent.
    pulp_ensure_aax_sdk_targets()
    pulp_add_test_suite(pulp-test-aax-midi-node
        SOURCES test_aax_midi_node.cpp
                "${CMAKE_SOURCE_DIR}/core/format/src/aax_midi_node.cpp"
        LIBRARIES pulp::format pulp-aax-sdk-interface
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/core/format/include")
    target_compile_options(pulp-test-aax-midi-node PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-multichar>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-undef-prefix>)

    # AAX custom editor — SDK-gated construction test. test_aax_editor.cpp
    # covers the gesture/sizing logic SDK-free; this drives the AAX_CEffectGUI
    # shell (aax_effect_gui.cpp), which like aax_runtime.cpp is otherwise only
    # built per-plugin, so the adapter source is compiled into the test. Needs
    # pulp-aax-library for AAX_CEffectGUI + the ACF unknown base.
    pulp_add_test_suite(pulp-test-aax-effect-gui
        SOURCES test_aax_effect_gui.cpp
                "${CMAKE_SOURCE_DIR}/core/format/src/aax_effect_gui.cpp"
        LIBRARIES pulp::format pulp::view pulp-aax-library
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/core/format/include")
    target_compile_options(pulp-test-aax-effect-gui PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-multichar>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-undef-prefix>)

    # Which editor the entry-point macros register — the parameter strip by
    # default, the custom editor only under PULP_AAX_PLUGIN_WITH_GUI. Drives
    # both macros through a recording collection, so it compiles aax_runtime.cpp
    # (for get_effect_descriptions) and aax_effect_gui.cpp (for the create proc
    # the opt-in macro takes the address of). SDK-gated because the macros are
    # written against the Avid interfaces; the plugin codes below are the
    # placeholders a real target gets from pulp_add_plugin.
    pulp_add_test_suite(pulp-test-aax-entry-registration
        SOURCES test_aax_entry_registration.cpp
                "${CMAKE_SOURCE_DIR}/core/format/src/aax_runtime.cpp"
                "${CMAKE_SOURCE_DIR}/core/format/src/aax_effect_gui.cpp"
                "${CMAKE_SOURCE_DIR}/core/format/src/aax_midi_node.cpp"
        LIBRARIES pulp::format pulp::view pulp-aax-library
        INCLUDE_DIRS "${CMAKE_SOURCE_DIR}/core/format/include")
    target_compile_definitions(pulp-test-aax-entry-registration PRIVATE
        PULP_MANUFACTURER_CODE="PULP"
        PULP_AAX_PRODUCT_CODE="PGAN"
        PULP_AAX_NATIVE_CODE="PGN1")
    target_compile_options(pulp-test-aax-entry-registration PRIVATE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-multichar>
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-undef-prefix>)
endif()

# AAX MIDI/SysEx packet reassembly + fragmentation (SDK-free pure logic)
pulp_add_test_suite(pulp-test-aax-midi GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)

# Appcast/auto-update tests
pulp_add_test_suite(pulp-test-appcast GROUP pulp-test-group-fmt-ship
    LIBRARIES pulp::ship)

# Sparkle Ed25519 sign/verify
pulp_add_test_suite(pulp-test-sparkle-verify GROUP pulp-test-group-fmt-ship
    LIBRARIES pulp::ship pulp::runtime)

# Android package/signing tests (host-side fake SDK/toolchain only)
pulp_add_test_suite(pulp-test-android-package GROUP pulp-test-group-fmt-ship
    LIBRARIES pulp::ship)

# V3 gaps tests (Bias, MidiSequence, SubsectionReader, HostType, Primes, Expression)
pulp_add_test_suite(pulp-test-v3-gaps GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::signal pulp::midi pulp::audio pulp::format pulp::runtime)

# Codesign tests
if(APPLE)
    pulp_add_test_suite(pulp-test-codesign GROUP pulp-test-group-fmt-ship
        LIBRARIES pulp::ship
        TEST_SPEC "~[hdiutil]")
    pulp_add_test_suite(pulp-test-codesign GROUP pulp-test-group-fmt-ship
        LIBRARIES pulp::ship
        TEST_SPEC "[hdiutil]"
        PROPERTIES RESOURCE_LOCK pulp_hdiutil)
elseif(WIN32)
    pulp_add_test_suite(pulp-test-codesign GROUP pulp-test-group-fmt-ship
        LIBRARIES pulp::ship
        LABELS "windows-pr-quarantine")
else()
    pulp_add_test_suite(pulp-test-codesign GROUP pulp-test-group-fmt-ship
        LIBRARIES pulp::ship)
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(pulp-test-linux-packaging test_linux_packaging.cpp)
    target_link_libraries(pulp-test-linux-packaging PRIVATE
        pulp::ship
        pulp::platform
        Catch2::Catch2WithMain)
    catch_discover_tests(pulp-test-linux-packaging)
endif()

# Sync primitives tests (SeqLock, TripleBuffer)
# `slow`: hammer tests run a writer + readers for 500 ms each
# to surface ordering bugs. Sanitizers.yml exercises the same code
# under TSan; fast-CI can skip the redundant smoke.
pulp_add_test_suite(pulp-test-sync GROUP pulp-test-group-fmt-runtime
    SOURCES test_sync_primitives.cpp
    LIBRARIES pulp::runtime
    LABELS slow)

# License and BigInteger tests
pulp_add_test_suite(pulp-test-license GROUP pulp-test-group-fmt-httplib
    LIBRARIES pulp::runtime pulp-cpp-httplib)

# License v1 -> v2 migration
pulp_add_test_suite(pulp-test-license-migration GROUP pulp-test-group-fmt-runtime
    LIBRARIES pulp::runtime)

# MIDI CI tests
pulp_add_test_suite(pulp-test-midi-ci GROUP pulp-test-group-fmt-midi
    LIBRARIES pulp::midi)

# MIDI CI Property Exchange (PE) tests
pulp_add_test_suite(pulp-test-midi-ci-pe GROUP pulp-test-group-fmt-midi
    LIBRARIES pulp::midi)

# AU v2 effect adapter MIDI-input tests — guards the 2026-04-22 fix that
# wired HandleMIDIEvent / HandleSysEx into `PulpAUEffect` and flipped the
# component type from aufx to aumf for descriptor.accepts_midi=true.
# Apple-only because the adapter header pulls in AudioUnitSDK (which is
# not linked on non-Apple lanes).
if(APPLE AND NOT PULP_IOS AND PULP_HAS_AUSDK)
    # One executable for the C++23 AudioUnitSDK suites. They link ausdk
    # directly: these tests include AudioUnitSDK headers, which pulp::format
    # no longer exports (ausdk is PRIVATE on pulp-format to stop leaking those
    # headers onto every consumer). AU-header access is opt-in via linking
    # ausdk, same as _pulp_add_au does for real AU plugins. pulp::format on
    # Apple compiles at C++23 (AudioUnitSDK 1.4 requires std::expected), so
    # the group does too, so the adapter header parses.
    pulp_add_test_group(pulp-test-group-fmt-au-v2
        LIBRARIES pulp::format pulp::midi ausdk
                  "-framework AudioToolbox"
                  "-framework CoreFoundation"
                  "-framework CoreMIDI")
    set_target_properties(pulp-test-group-fmt-au-v2 PROPERTIES CXX_STANDARD 23)

    pulp_add_test_suite(pulp-test-au-v2-effect GROUP pulp-test-group-fmt-au-v2
        LIBRARIES pulp::format pulp::midi ausdk)

    # AU v2 MIDI-processor (`aumi`) adapter. Drives the real adapter end to end:
    # host MIDI in through the SDK entry points, a host-installed
    # kAudioUnitProperty_MIDIOutputCallback, and Render() to advance the block.
    pulp_add_test_suite(pulp-test-au-v2-midi-processor GROUP pulp-test-group-fmt-au-v2
        LIBRARIES pulp::format pulp::midi ausdk
                  "-framework AudioToolbox"
                  "-framework CoreFoundation"
                  "-framework CoreMIDI")

    # AU v2 multi-bus contract — guards the multi-output-element lift (instrument
    # advertises one output element per declared bus) and the effect's Sidechain
    # input surface. Pure descriptor->bus helpers, so no live AU host needed.
    pulp_add_test_suite(pulp-test-au-v2-busses GROUP pulp-test-group-fmt-au-v2
        LIBRARIES pulp::format pulp::midi ausdk)

    # AU v2 continuous-parameter display — GetParameterInfo ValuesHaveStrings
    # flag + kAudioUnitProperty_ParameterStringFromValue / ...ValueFromString
    # round-tripping through ParamInfo::to_string / from_string. Instantiates a
    # live PulpAUEffect, so it links the AU/CoreFoundation frameworks like the
    # other .mm AU tests.
    add_executable(pulp-test-au-v2-param-display test_au_v2_param_display.mm)
    target_link_libraries(pulp-test-au-v2-param-display PRIVATE
        pulp::format pulp::midi ausdk Catch2::Catch2WithMain
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework Foundation")
    set_target_properties(pulp-test-au-v2-param-display PROPERTIES
        CXX_STANDARD 23 OBJCXX_STANDARD 23)
    catch_discover_tests(pulp-test-au-v2-param-display)

    # Cross-format outbound-MIDI sample-offset parity. Exercises the real AU v2
    # MidiOutputPacketBuilder (hence ausdk + C++23) plus the shared VST3/CLAP
    # offset helpers the adapters route through.
    pulp_add_test_suite(pulp-test-midi-out-offset-parity GROUP pulp-test-group-fmt-au-v2
        LIBRARIES pulp::format pulp::midi ausdk)

    # AU v2 Cocoa editor-view advertisement (kAudioUnitProperty_CocoaUI). Compiles
    # the Cocoa view module with PULP_AU_GUI + a test-unique factory class name and
    # asserts the cross-TU filler registers + advertises the right, resolvable class.
    add_executable(pulp-test-au-v2-cocoa-ui
        test_au_v2_cocoa_ui.mm
        ${CMAKE_SOURCE_DIR}/core/format/src/au_v2_cocoa_view.mm)
    target_link_libraries(pulp-test-au-v2-cocoa-ui PRIVATE
        pulp::format pulp::view ausdk Catch2::Catch2WithMain
        "-framework AppKit" "-framework AudioToolbox" "-framework QuartzCore")
    target_compile_definitions(pulp-test-au-v2-cocoa-ui PRIVATE
        PULP_AU_GUI=1 PULP_AU_COCOA_VIEW_CLASS=PulpAUCocoaViewFactory_Test)
    set_target_properties(pulp-test-au-v2-cocoa-ui PROPERTIES CXX_STANDARD 23)
    # AudioUnitSDK 1.4 headers use std::expected (C++23). The root sets
    # CMAKE_CXX_STANDARD=20 which wins for ObjC++ over the target CXX_STANDARD,
    # so force C++23 explicitly on the OBJCXX sources (both .mm here).
    target_compile_options(pulp-test-au-v2-cocoa-ui PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-std=gnu++23>)
    catch_discover_tests(pulp-test-au-v2-cocoa-ui)
endif()

# iOS AVAudioSession bridge
pulp_add_test_suite(pulp-test-ios-audio-session GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
if(NOT PULP_IOS)
    pulp_add_test_suite(pulp-test-pulp-bridge GROUP pulp-test-group-fmt-format
        LIBRARIES pulp::format)
endif()
# ARA scaffolding
pulp_add_test_suite(pulp-test-ara GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
# CLAP ARA extension end-to-end. This suite instantiates
# PulpClapPlugin directly, so it must see the same layout as pulp-format's
# desktop CLAP adapter TU.
pulp_add_test_suite(pulp-test-clap-ara-extension
    LIBRARIES pulp::format
    COMPILE_DEFINITIONS PULP_CLAP_GUI=1)
# Processor ARA hook
pulp_add_test_suite(pulp-test-processor-ara-hook GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
# Processor bus-layout validation + processBlock precision contract +
# latency / tail change notification.
pulp_add_test_suite(pulp-test-processor-layout-latency GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
# AudioPlayHead transport-extension helpers used by every
# format adapter (VST3, AU v2 / v3, CLAP) to derive `ProcessContext::bar`
# and the three change flags (tempo_changed / time_sig_changed /
# transport_changed) once per block.
pulp_add_test_suite(pulp-test-playhead-diff GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
# Field-validity mapping is format-neutral and does not require a platform SDK.
pulp_add_test_suite(pulp-test-host-transport-validity GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
# VST3 SMPTE → FrameRate mapping (pure helper, no Steinberg SDK
# dependency). Covers the 59.94 vs 60 fps mapping edge case.
pulp_add_test_suite(pulp-test-vst3-frame-rate GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::format)
# `pulp ship auv3-xcodeproj` developer-path validation helper.
# Header-only; no extra libraries needed.
add_executable(pulp-test-xcode-developer-path
    test_xcode_developer_path.cpp
)
target_include_directories(pulp-test-xcode-developer-path PRIVATE
    ${CMAKE_SOURCE_DIR}/tools/cli
)
target_link_libraries(pulp-test-xcode-developer-path PRIVATE Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-xcode-developer-path)

# StepSequencer end-to-end proof: a StepGridView (UI) submits a cell edit through
# a SequencerStateChannel, the Processor drains it and republishes state, and the
# view reflects it. This is the synth-class control path for state that is NOT a
# host parameter — a step grid is too big and too structured to expose as one
# automatable param per cell, so it travels its own channel instead.
pulp_add_test_suite(pulp-test-step-sequencer-roundtrip GROUP pulp-test-group-fmt-format-view
    LIBRARIES pulp::view pulp::state pulp::format pulp::midi pulp::audio)

# The gate that keeps a host state restore off a Processor while the audio
# thread is inside process(). Header-only primitive plus its buffer helpers, so
# it needs no adapter or SDK — pulp::audio supplies BufferView.
pulp_add_test_suite(pulp-test-state-restore-gate GROUP pulp-test-group-fmt-format
    LIBRARIES pulp::audio pulp::format)
