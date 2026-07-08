add_library(pulp-format)
add_library(pulp::format ALIAS pulp-format)

target_include_directories(pulp-format
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-format PRIVATE
    src/format.cpp
    src/processor_f64.cpp
    src/registry.cpp
    src/headless.cpp
    src/offline_render_host.cpp
    src/offline_sample_bounce.cpp
    src/offline_sample_slot_bounce.cpp
    src/graph_runtime_executor.cpp
    src/graph_runtime_worker_pool.cpp
    src/plugin_state_io.cpp
    src/processor_block_adapter.cpp
    src/processor_node_adapter.cpp
    src/validation_harness.cpp
    src/aax_model.cpp
    src/host_type.cpp
    # host_type.cpp / host_version.cpp #ifdef out their definitions on
    # __APPLE__, so the Foundation-backed impls below must compile for ALL
    # Apple targets — not just macOS. `$<PLATFORM_ID:Darwin>` is `iOS` (not
    # `Darwin`) under an iphoneos/iphonesimulator cross-build, so the old
    # Darwin-only gate dropped these TUs and left detect_host_version /
    # current_auv3_wrapper_identifier undefined when the iOS AUv3 linked
    # pulp-format. Both .mm files use only Foundation and are
    # iOS-safe.
    $<$<PLATFORM_ID:Darwin,iOS,tvOS,watchOS>:src/host_type_mac.mm>
    src/host_version.cpp
    $<$<PLATFORM_ID:Darwin,iOS,tvOS,watchOS>:src/host_version_mac.mm>
    $<$<PLATFORM_ID:Windows>:src/host_version_win.cpp>
    src/host_quirks.cpp
    src/ara.cpp
    src/ara_factory.cpp
    src/ios_audio_session.cpp
    src/descriptor_validation.cpp
    src/view_bridge.cpp
    src/remote_view_session.cpp
    src/native_core_processor.cpp
    src/transport_quantizer.cpp
)

target_link_libraries(pulp-format
    PUBLIC
        pulp::audio
        pulp::events
        pulp::graph
        pulp::midi
        pulp::state
        pulp::view
        pulp::runtime
        pulp::native-components
)
if(PULP_ENABLE_GPU AND NOT ANDROID AND NOT IOS)
    # Inspector is desktop-only AND GPU-only — pulp-inspect is added in
    # the root CMakeLists.txt under `if(PULP_ENABLE_GPU AND NOT ANDROID)`.
    # Linking it here without the same guard breaks PULP_ENABLE_GPU=OFF
    # builds (the sanitizer matrix in .github/workflows/sanitizers.yml
    # disables GPU). Defined later in CMake order, so we link by name
    # rather than using if(TARGET ...).
    target_link_libraries(pulp-format PUBLIC pulp::inspect)
    # Source-side gate: standalone.cpp uses PULP_HAS_INSPECT to decide
    # whether it can include <pulp/inspect/inspector_overlay.hpp>.
    target_compile_definitions(pulp-format PRIVATE PULP_HAS_INSPECT=1)
endif()

# PULP_ENABLE_INSPECTOR gates whether standalone.cpp actually wires the
# dev inspector (creation + install_inspector_hooks + idle paint pump).
# Defined unconditionally as 0/1 so source can `#if PULP_ENABLE_INSPECTOR`
# even on builds that don't link pulp-inspect. Release/standalone-ship
# builds configure with -DPULP_ENABLE_INSPECTOR=OFF to keep the dev
# inspector out of shipped apps.
if(PULP_ENABLE_INSPECTOR)
    target_compile_definitions(pulp-format PRIVATE PULP_ENABLE_INSPECTOR=1)
else()
    target_compile_definitions(pulp-format PRIVATE PULP_ENABLE_INSPECTOR=0)
endif()

# PULP_ENABLE_AUDIO_PROBES wires the RT output-boundary probe into the
# standalone host (the dev Audio Inspector's data source). Unlike the
# inspector flag, this one gates MEMBER declarations in the PUBLIC
# standalone.hpp (output_probe_ / output_probe_ptrs_ / output_probe()), so it
# is defined PUBLIC — every target that includes standalone.hpp must see the
# same value or sizeof(StandaloneApp) diverges (ODR). Defined unconditionally
# as 0/1 so source can `#if PULP_ENABLE_AUDIO_PROBES`. Default ON for
# dev/examples; release/standalone-ship builds configure OFF.
if(PULP_ENABLE_AUDIO_PROBES)
    target_compile_definitions(pulp-format PUBLIC PULP_ENABLE_AUDIO_PROBES=1)
else()
    target_compile_definitions(pulp-format PUBLIC PULP_ENABLE_AUDIO_PROBES=0)
endif()

# Host-quirks default policy (host-quirks enforcement plan).
#
# Selects the baseline tier filter detect_quirks() / resolved_quirks() apply
# when no runtime override (PULP_HOST_QUIRKS env / set_host_quirk_policy API /
# per-quirk override) is set. Shipped default is `validated_only`: only
# bench-validated accommodations fire by default; Speculative / LessonOnly
# rows stay dormant until explicitly opted in. The currently-enforced quirks
# (clamp_latency_to_nonneg, silence_unsupported_bus_arrangements,
# synthesize_bypass_parameter — all Validated) survive this filter, so the
# default flip is behavior-neutral today and evidence-gates future
# host-specific quirks. Override at configure time:
#   -DPULP_HOST_QUIRKS_DEFAULT_POLICY=all | validated_only | off
# The define is PUBLIC so default_policy_filter() in host_quirks.cpp AND the
# policy-aware unit tests both compile against the same baseline.
set(PULP_HOST_QUIRKS_DEFAULT_POLICY "validated_only" CACHE STRING
    "Baseline host-quirks tier filter: all | validated_only | off")
set_property(CACHE PULP_HOST_QUIRKS_DEFAULT_POLICY PROPERTY STRINGS
    all validated_only off)
if(PULP_HOST_QUIRKS_DEFAULT_POLICY STREQUAL "validated_only")
    target_compile_definitions(pulp-format PUBLIC PULP_HOST_QUIRKS_DEFAULT_POLICY_VALIDATED_ONLY=1)
elseif(PULP_HOST_QUIRKS_DEFAULT_POLICY STREQUAL "off")
    target_compile_definitions(pulp-format PUBLIC PULP_HOST_QUIRKS_DEFAULT_POLICY_OFF=1)
elseif(NOT PULP_HOST_QUIRKS_DEFAULT_POLICY STREQUAL "all")
    message(FATAL_ERROR
        "PULP_HOST_QUIRKS_DEFAULT_POLICY must be all|validated_only|off, got "
        "'${PULP_HOST_QUIRKS_DEFAULT_POLICY}'")
endif()

# VST3 adapter (when SDK is available)
if(PULP_HAS_VST3)
    target_sources(pulp-format PRIVATE
        src/vst3_adapter.cpp
    )
    target_link_libraries(pulp-format PUBLIC vst3-sdk)
    target_compile_definitions(pulp-format PRIVATE PULP_VST3=1 PULP_VST3_GUI=1)
endif()

# CLAP adapter
if(PULP_HAS_CLAP)
    target_sources(pulp-format PRIVATE
        src/clap_adapter.cpp
        src/clap_remote_controls.cpp
    )
    target_link_libraries(pulp-format PUBLIC clap)
    # Keep PulpClapPlugin's layout identical between clap_adapter.cpp and the
    # per-plugin clap_entry.cpp TU. The entry target defines PULP_CLAP_GUI=1;
    # the shared adapter must do the same or GUI members shift later fields.
    target_compile_definitions(pulp-format PRIVATE PULP_CLAP=1 PULP_CLAP_GUI=1)
endif()

# LV2 adapter
if(PULP_HAS_LV2)
    target_sources(pulp-format PRIVATE
        src/lv2_adapter.cpp
    )
    target_link_libraries(pulp-format PUBLIC lv2-headers)
    target_compile_definitions(pulp-format PRIVATE PULP_LV2=1)
endif()

if(PULP_HAS_AUSDK)
    if(APPLE)
        target_sources(pulp-format PRIVATE
            src/au_v2_adapter.cpp
            src/au_v2_instrument.cpp
        )
        # AudioUnitSDK 1.4 headers use std::expected (C++23). PUBLIC feature
        # propagation from the ausdk target isn't reliable across all Apple
        # clang toolchains when the consumer has CMAKE_CXX_STANDARD=20, so
        # pin it here explicitly for pulp-format.
        target_compile_features(pulp-format PRIVATE cxx_std_23)
    endif()
    # PRIVATE, not PUBLIC: pulp-format uses AudioUnitSDK only in its own AU v2
    # .cpp sources, so it needs the headers at build time but must NOT export
    # external/AudioUnitSDK/include onto every consumer of pulp::format. A
    # PUBLIC link leaked ausdk's $<INSTALL_INTERFACE:external/AudioUnitSDK/include>
    # to anything linking pulp::format (e.g. a JUCE plugin embedding Pulp via
    # pulp-view-embed), colliding with the host's OWN vendored, differently-
    # versioned AudioUnitSDK. Pulp's own AU plugins don't rely on this
    # propagation — _pulp_add_au links ausdk PRIVATE directly to the AU target,
    # and the only public headers that pull AudioUnitSDK (au_v2_entry.hpp et al)
    # are opt-in, compiled solely into an AU plugin entry point. Static-lib link
    # propagation still pulls ausdk's objects into consumers that need them; only
    # the stray include-dir is withheld. C++23 propagation is a separate PUBLIC
    # compile-feature below, unaffected.
    target_link_libraries(pulp-format PRIVATE ausdk)
    # AudioUnitSDK 1.4 headers `#include <expected>` (C++23). Apple clang
    # only exposes std::expected when the consuming TU is compiled with
    # -std=c++23. Set the standard explicitly (target_compile_features
    # alone is ignored when CMAKE_CXX_STANDARD pins a lower version —
    # CMake 3.24 treats CXX_STANDARD as authoritative, so we need to set
    # it per-target). Public so every TU that links pulp::format on
    # Apple also builds at C++23.
    set_target_properties(pulp-format PROPERTIES CXX_STANDARD 23)
    target_compile_features(pulp-format PUBLIC cxx_std_23)
endif()

# AU v2 adapter uses Apple AudioUnitSDK — compiled per-plugin in the entry point,
# not in the shared library. The au_adapter.mm (AUv3) works on both macOS and iOS.
if(APPLE)
    target_compile_definitions(pulp-format PRIVATE PULP_AU=1)
    # CoreMIDI supplies the MIDIPacketList builders the AU v2 MIDI-output path
    # uses to deliver kAudioUnitProperty_MIDIOutputCallback packets to the host.
    # PUBLIC so every consumer of pulp::format (the AU v2 adapter header inlines
    # the builder) resolves the symbols too.
    target_link_libraries(pulp-format PUBLIC "-framework CoreMIDI")
    if(PULP_IOS)
        # iOS uses AUv3 only (no AU v2 component manager)
        target_link_libraries(pulp-format PRIVATE
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework UIKit"
        )
        target_compile_definitions(pulp-format PRIVATE PULP_IOS=1)
    endif()
endif()

# ── ARA 2.x ──────────────────────────────────────────────────────────────────
# Celemony's ARA SDK is developer-supplied; not bundled. Point
# PULP_ARA_SDK_DIR at a local checkout of https://github.com/Celemony/ARA_SDK
# and set PULP_ENABLE_ARA=ON to enable the integration.
if(PULP_ENABLE_ARA)
    if(NOT PULP_ARA_SDK_DIR)
        message(FATAL_ERROR
            "PULP_ENABLE_ARA=ON requires PULP_ARA_SDK_DIR to point at a "
            "Celemony ARA SDK checkout")
    endif()
    if(NOT EXISTS "${PULP_ARA_SDK_DIR}/ARA_API/ARAInterface.h")
        message(FATAL_ERROR
            "PULP_ARA_SDK_DIR='${PULP_ARA_SDK_DIR}' does not look like an "
            "ARA SDK — ARA_API/ARAInterface.h not found")
    endif()
    target_compile_definitions(pulp-format PUBLIC PULP_HAS_ARA=1)
    target_include_directories(pulp-format PUBLIC
        $<BUILD_INTERFACE:${PULP_ARA_SDK_DIR}>
    )
    message(STATUS "Pulp: ARA SDK enabled (${PULP_ARA_SDK_DIR})")
endif()
