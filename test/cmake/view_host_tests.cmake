# View, accessibility, host scanner, platform audio-device, and host hook tests.
# Included by test/CMakeLists.txt; keep related test registrations here.

# TableModel data/sort layer
pulp_add_test_suite(pulp-test-table-model LIBRARIES pulp::view)
# ModulationMatrix data model
pulp_add_test_suite(pulp-test-modulation-matrix LIBRARIES pulp::view)
# ModulationMatrixWidget canvas widget
pulp_add_test_suite(pulp-test-modulation-matrix-widget LIBRARIES pulp::view)
# Lasso selection overlay
pulp_add_test_suite(pulp-test-lasso LIBRARIES pulp::view)
# WaveformRecorder three-state recorder widget (Ink & Signal design system)
pulp_add_test_suite(pulp-test-waveform-recorder LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-design-frame-momentary LIBRARIES pulp::view)
# Split pane layout widget
pulp_add_test_suite(pulp-test-split-view LIBRARIES pulp::view)
# CommandRegistry + KeyMappingEditor (Pulp-native names)
pulp_add_test_suite(pulp-test-command-registry LIBRARIES pulp::view pulp::state)
# Audio Inspector developer tool window
pulp_add_test_suite(pulp-test-audio-inspector-window LIBRARIES pulp::view pulp::audio)
# PluginDescriptor vendor_url + vendor_email
pulp_add_test_suite(pulp-test-vendor-url LIBRARIES pulp::format)
# Image cache + ImageView↔ImageCache wiring
pulp_add_test_suite(pulp-test-image-cache LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-image-view-cache LIBRARIES pulp::view)
# Accessibility tree snapshot
pulp_add_test_suite(pulp-test-accessibility-tree LIBRARIES pulp::view)
# Recycling virtualized list primitive
pulp_add_test_suite(pulp-test-virtual-list LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-virtual-list-sample-manager LIBRARIES pulp::view)
# Text-accessibility scaffold
pulp_add_test_suite(pulp-test-text-accessibility LIBRARIES pulp::view)
# macOS NSAccessibility backend for TextAccessibilityNode
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-text-accessibility-macos test_text_accessibility_macos.mm)
    target_link_libraries(pulp-test-text-accessibility-macos
        PRIVATE pulp::view Catch2::Catch2WithMain "-framework AppKit")
    catch_discover_tests(pulp-test-text-accessibility-macos)
endif()
# macOS hover delivery for a hosted PluginViewHost (tracking area +
# host-window acceptsMouseMovedEvents).
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-plugin-view-hover-macos test_plugin_view_host_hover_macos.mm)
    target_link_libraries(pulp-test-plugin-view-hover-macos
        PRIVATE pulp::view Catch2::Catch2WithMain "-framework AppKit")
    catch_discover_tests(pulp-test-plugin-view-hover-macos)
endif()
# Windows UIA backend — compile-gated on _WIN32 in the
# source. The sentinel test case keeps the binary present + named
# consistently on non-Windows hosts so ctest output stays stable.
pulp_add_test_suite(pulp-test-text-accessibility-windows LIBRARIES pulp::view)
# Linux AccessKit backend — compile-gated on
# __linux__ && !__ANDROID__; sentinel on other hosts.
pulp_add_test_suite(pulp-test-text-accessibility-linux LIBRARIES pulp::view)
# A/B compare
pulp_add_test_suite(pulp-test-ab-compare LIBRARIES pulp::view)
# ViewSize::aspect_ratio field
pulp_add_test_suite(pulp-test-view-size-aspect LIBRARIES pulp::format)

# WindowHost::set_design_viewport pure-math
# coverage. Locks down the scale + letterbox transform that the mac
# GPU host (and any other future platform host) uses to fit
# fixed-design content into the current window.
pulp_add_test_suite(pulp-test-view-design-viewport LIBRARIES pulp::view)

# Shared continuous-frame predicate (needs_continuous_frames) the window /
# plugin-view hosts and a foreign-host embed tick gate repaint on.
pulp_add_test_suite(pulp-test-continuous-frames LIBRARIES pulp::view)

# Sub-view rect-level partial invalidation: View::request_repaint(Rect) →
# WindowHost dirty-region accumulation. Pins the local->root mapping, the
# bounding-box union, and the full-repaint escalations (no-arg, transform,
# empty rect, sticky-full).
pulp_add_test_suite(pulp-test-partial-invalidation LIBRARIES pulp::view)

# Host-side consumption of the accumulated dirty region: WindowHost::paint_root
# clips the canvas to pending_dirty_bounds() for a bounded frame and paints
# unclipped for a full one, clearing the pending region after each paint.
pulp_add_test_suite(pulp-test-dirty-region-consumption LIBRARIES pulp::view)

# PluginViewHost design-viewport wiring (mac CPU + GPU). The pure
# math is already pinned by `pulp-test-view-design-viewport`; covers the
# PluginViewHost virtuals + CPU-host inverse mouse mapping (GPU section
# soft-skips without Dawn; foundation for CLAP/VST3 resize negotiation)
# and the keyboard-focus host-etiquette contract (prior-responder
# restore, host-grab ends text input — keeps Musical Typing alive).
if(APPLE AND NOT PULP_IOS)
    add_executable(pulp-test-plugin-view-host-design-viewport
        test_plugin_view_host_design_viewport.mm
        test_plugin_view_host_key_focus.mm
        test_plugin_view_host_text_input.mm)
    target_link_libraries(pulp-test-plugin-view-host-design-viewport PRIVATE
        pulp::view pulp::canvas Catch2::Catch2WithMain "-framework AppKit")
    if(PULP_HAS_SKIA)
        target_link_libraries(pulp-test-plugin-view-host-design-viewport
            PRIVATE pulp::render skia::skia)
        target_compile_definitions(pulp-test-plugin-view-host-design-viewport
            PRIVATE PULP_HAS_SKIA=1)
        target_include_directories(pulp-test-plugin-view-host-design-viewport
            PRIVATE ${SKIA_INCLUDE_DIRS})
    endif()
    catch_discover_tests(pulp-test-plugin-view-host-design-viewport)
endif()

# Design-tool viewport resolver probe-layout pattern. Locks in the
# yoga-measured-height path that the design-tool
# example uses to auto-size its window for grid + row trees where
# View::intrinsic_height returns 0.
pulp_add_test_suite(pulp-test-design-tool-viewport-resolver LIBRARIES pulp::view)

# NativeViewHost widget (R1) — native child embed geometry + lifecycle
pulp_add_test_suite(pulp-test-native-view-host LIBRARIES pulp::view)
if(APPLE AND NOT PULP_IOS)
    # macOS companion: exercises the real CALayer-mask clip helper.
    target_sources(pulp-test-native-view-host PRIVATE test_native_view_host_mac.mm)
endif()

# View host bridges — screenshot/window/plugin-view
add_executable(pulp-test-view-host-bridge test_view_host_bridge.cpp)
if(APPLE AND NOT PULP_IOS)
    target_sources(pulp-test-view-host-bridge PRIVATE test_view_host_bridge_mac.mm)
endif()
target_link_libraries(pulp-test-view-host-bridge PRIVATE pulp::view Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-view-host-bridge)
# Scan blacklist
add_executable(pulp-test-scan-blacklist test_scan_blacklist.cpp)
target_link_libraries(pulp-test-scan-blacklist PRIVATE pulp::host Catch2::Catch2WithMain)
# `slow`: file-mtime sleep loops (rebuilt-plugin-not-blacklisted
# flow waits on filesystem timestamp resolution). Each test ~1s.
catch_discover_tests(pulp-test-scan-blacklist PROPERTIES LABELS slow)
# Crash-isolated scanner. Skipped on iOS/Android
# where pulp-scan-worker isn't built (sandboxed platforms can't fork).
if(NOT IOS AND NOT ANDROID)
    add_executable(pulp-isolated-scanner-crash-helper
        fixtures/isolated_scanner_crash_helper.cpp)
    set_target_properties(pulp-isolated-scanner-crash-helper PROPERTIES
        OUTPUT_NAME "pulp-isolated-scanner-crash-helper")

    add_executable(pulp-test-isolated-scanner test_isolated_scanner.cpp)
    # pulp::platform comes transitively via pulp::host (it's a PRIVATE
    # dep there, but the static-archive link still pulls in the symbols
    # the test needs through pulp::host's USAGE side). We only have to
    # list Catch2 + the public dep.
    target_link_libraries(pulp-test-isolated-scanner PRIVATE
        pulp::host
        Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-isolated-scanner PRIVATE
        PULP_ISOLATED_SCANNER_REAL_WORKER="$<TARGET_FILE:pulp-scan-worker>"
        PULP_ISOLATED_SCANNER_CRASH_HELPER="$<TARGET_FILE:pulp-isolated-scanner-crash-helper>")
    add_dependencies(pulp-test-isolated-scanner
        pulp-scan-worker
        pulp-isolated-scanner-crash-helper)
    catch_discover_tests(pulp-test-isolated-scanner)
endif()
# Hosted editor API
pulp_add_test_suite(pulp-test-hosted-editor LIBRARIES pulp::host)
# HostedEditor → WindowHost attachment migration
pulp_add_test_suite(pulp-test-hosted-editor-migration LIBRARIES pulp::view pulp::host)
# CFRunLoop cooperation — plugin-mode MainThreadDispatcher
pulp_add_test_suite(pulp-test-cfrunloop-cooperation LIBRARIES pulp::events)
if(APPLE)
    target_link_libraries(pulp-test-cfrunloop-cooperation PRIVATE "-framework CoreFoundation")
endif()
# High-level window classes
pulp_add_test_suite(pulp-test-document-window LIBRARIES pulp::view)
# Accessibility announcements
pulp_add_test_suite(pulp-test-announce LIBRARIES pulp::view)
# ARA core type layer
pulp_add_test_suite(pulp-test-ara-types LIBRARIES pulp::format)
# Processor on_host_transport_changed hook
pulp_add_test_suite(pulp-test-transport-hook LIBRARIES pulp::format)
# Scan cache
add_executable(pulp-test-scan-cache test_scan_cache.cpp)
target_link_libraries(pulp-test-scan-cache PRIVATE pulp::host Catch2::Catch2WithMain)
# `slow`: each scenario waits for filesystem mtime resolution
# (~1s each on every platform). Excluded from fast-CI.
catch_discover_tests(pulp-test-scan-cache PROPERTIES LABELS slow)
# Processor memory-pressure hook
pulp_add_test_suite(pulp-test-memory-pressure LIBRARIES pulp::format)
# iOS background-audio descriptor flag
pulp_add_test_suite(pulp-test-ios-background-audio-flag LIBRARIES pulp::format)

# MidiBuffer UMP sidecar
pulp_add_test_suite(pulp-test-midi-buffer-ump LIBRARIES pulp::midi)
# MidiBuffer SysEx sidecar — full MIDI vocabulary
pulp_add_test_suite(pulp-test-midi-buffer-sysex LIBRARIES pulp::midi)
# Win MIDI MIM_LONGDATA SysEx + QPC timestamps. Cross-platform, with
# Windows-only cases guarded by #ifdef _WIN32.
pulp_add_test_suite(pulp-test-winmidi-sysex LIBRARIES pulp::midi)
# WinRT MIDI 2.0 (UMP) backend — W3 (UMP↔MIDI1 + shared sysex7 reassembly + F0..F7 framing; WinRT TU opt-in via PULP_HAS_WINRT_MIDI, #error-guarded off non-Windows).
pulp_add_test_suite(pulp-test-winrt-midi LIBRARIES pulp::midi)
# Accessibility provider cross-platform entry
pulp_add_test_suite(pulp-test-accessibility-provider LIBRARIES pulp::view)
# PluginDescriptor validation
pulp_add_test_suite(pulp-test-descriptor-validation LIBRARIES pulp::format)
# PluginInfo metadata shape
pulp_add_test_suite(pulp-test-plugin-info-metadata LIBRARIES pulp::host)
# A11y role mapping tables — UIA + AT-SPI
pulp_add_test_suite(pulp-test-uia-mapping LIBRARIES pulp::view)
pulp_add_test_suite(pulp-test-atspi-mapping LIBRARIES pulp::view)
# AudioSystem hotplug base plumbing
pulp_add_test_suite(pulp-test-audio-system-hotplug LIBRARIES pulp::audio)

# WASAPI capture path. Windows-only; cross-platform stub keeps
# CI green elsewhere. Skips at runtime on hosts without a default
# capture endpoint.
add_executable(pulp-test-wasapi-input test_wasapi_input.cpp)
target_link_libraries(pulp-test-wasapi-input PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-wasapi-input PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-wasapi-input)

# WASAPI share-mode coverage contract. Pins "shared-mode only"
# surface; static_asserts fire on every platform so a Windows-only
# DeviceConfig extension still trips this on macOS/Linux CI.
add_executable(pulp-test-wasapi-share-modes test_wasapi_share_modes.cpp)
target_link_libraries(pulp-test-wasapi-share-modes PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-wasapi-share-modes PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-wasapi-share-modes)

# ALSA capture + real device metadata. Linux-only;
# cross-platform stub keeps CI green elsewhere. Skips at runtime on
# hosts without an ALSA-routable default device.
add_executable(pulp-test-alsa-input test_alsa_input.cpp)
target_link_libraries(pulp-test-alsa-input PRIVATE pulp::audio Catch2::Catch2WithMain)
target_include_directories(pulp-test-alsa-input PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-alsa-input)

# Cross-platform device-hotplug monitor. Lives in pulp::runtime so
# both audio (AlsaSystem) and MIDI (AlsaMidiSystem) share it; libudev is the
# Linux backend, no-op elsewhere — so this test runs on every platform.
add_executable(pulp-test-udev-monitor test_udev_monitor.cpp)
target_link_libraries(pulp-test-udev-monitor PRIVATE pulp::runtime Catch2::Catch2WithMain)
target_include_directories(pulp-test-udev-monitor PRIVATE ${CMAKE_SOURCE_DIR})
catch_discover_tests(pulp-test-udev-monitor)

# JACK device-enumeration contract. Registered only when CMake
# detected a JACK dev package and compiled the backend into pulp-audio — the
# test calls JackSystem symbols that exist only in that configuration, and it
# is the regression guard for the enumerate_devices() build break.
if(PULP_JACK_AVAILABLE)
    add_executable(pulp-test-jack-device test_jack_device.cpp)
    target_link_libraries(pulp-test-jack-device PRIVATE pulp::audio Catch2::Catch2WithMain)
    target_include_directories(pulp-test-jack-device PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(pulp-test-jack-device PRIVATE PULP_HAS_JACK=1)
    catch_discover_tests(pulp-test-jack-device)
endif()
# Processor on_host_tempo_changed hook.
pulp_add_test_suite(pulp-test-tempo-hook LIBRARIES pulp::format)
# Resizable plugin shell.
pulp_add_test_suite(pulp-test-resizable-shell LIBRARIES pulp::view)
# ATK / AT-SPI mapping.
pulp_add_test_suite(pulp-test-atk-mapping LIBRARIES pulp::view)
# Background scanner.
pulp_add_test_suite(pulp-test-background-scanner LIBRARIES pulp::host)
