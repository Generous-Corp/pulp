# core/format is split along one rule: pulp-format-core is exactly what
# compiles and links WITHOUT the view layer.
#
#   pulp-format-core   the plugin-authoring surface. Processor, descriptor,
#                      parameters, bus configuration, MIDI routing, state,
#                      transport, host quirks, offline render, graph runtime,
#                      plus the VST3 / AU v2 / LV2 adapters, none of which
#                      reaches pulp/view. No view includes, no view links.
#   pulp-format-view   the view-coupled surface: ViewBridge, the editor UI and
#                      chrome headers, and the CLAP adapter, whose plugin
#                      struct embeds a ViewBridge and a view::PluginViewHost by
#                      value and so cannot be compiled without them.
#   pulp-format        umbrella. Links both halves PUBLIC so every existing
#                      consumer -- in-tree and out-of-tree -- is unchanged.
#
# Consumers that only need the authoring vocabulary (pulp::dsl, pulp::sequence)
# link pulp::format-core and no longer inherit the Skia/Dawn closure that
# arrives with core/view. Everything else keeps linking pulp::format.
#
# Both halves export the same include directory, so no #include path changes.
# Header classification is therefore enforced by a gate rather than by the
# filesystem: see the format-header-view-boundary test.

add_library(pulp-format-core)
add_library(pulp::format-core ALIAS pulp-format-core)

add_library(pulp-format-view)
add_library(pulp::format-view ALIAS pulp-format-view)

# The umbrella is a real STATIC library, not INTERFACE. Two things depend on it
# having an archive artifact: the SDK export set in PulpInstallRules.cmake, and
# $<TARGET_FILE:pulp-format> in the test suite. placeholder.cpp exists to give
# it one. pulp-view (core/view/CMakeLists.txt) is the same shape.
add_library(pulp-format src/placeholder.cpp)
add_library(pulp::format ALIAS pulp-format)

foreach(_pulp_format_target pulp-format-core pulp-format-view pulp-format)
    target_include_directories(${_pulp_format_target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
endforeach()
unset(_pulp_format_target)

target_sources(pulp-format-core PRIVATE
    # Emits the Processor vtable (create_view() is the key function), so this
    # TU anchors the core half. See the comment at the top of format.cpp: the
    # vtable's slots reference processor_f64.cpp and ara.cpp, which is why
    # those two must stay in this target as well.
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
    src/host_parameter_edit.cpp
    src/ara.cpp
    src/ara_factory.cpp
    src/ios_audio_session.cpp
    src/descriptor_validation.cpp
    src/native_core_processor.cpp
    src/transport_quantizer.cpp
)

target_sources(pulp-format-view PRIVATE
    src/view_bridge.cpp
    # Processor::SettingsSection's special members need view::View complete;
    # Processor::create_view() does not, and stays in format.cpp.
    src/settings_section.cpp
)

target_link_libraries(pulp-format-core
    PUBLIC
        pulp::audio
        pulp::events
        pulp::graph
        pulp::midi
        # device_quotas.hpp is a public header that includes
        # pulp/platform/device_capability.hpp, so the platform include path is a
        # declared dependency of this target rather than one inherited through
        # pulp::view.
        pulp::platform
        pulp::state
        pulp::runtime
        pulp::timebase
        pulp::native-components
)

# PUBLIC, and required rather than merely tidy. Three things depend on it:
#   * it is true -- view_bridge.cpp and clap_adapter.cpp both use Processor,
#     whose vtable lives in the core half;
#   * it gives PULP_ENABLE_AUDIO_PROBES and PULP_HAS_ARA exactly ONE definition
#     site (on core), so every path to a consumer sees an identical value.
#     PULP_ENABLE_AUDIO_PROBES gates member declarations in standalone.hpp, so
#     a skew changes sizeof(StandaloneApp) silently;
#   * clap_adapter.cpp conditions on PULP_HAS_ARA and would otherwise never
#     see it, compiling ARA support out of the CLAP adapter with nothing red.
# The dependency is one-way: no core translation unit references pulp::view.
target_link_libraries(pulp-format-view
    PUBLIC
        pulp::format-core
        # Public ViewBridge headers expose native View types, not WebView.
        pulp::view-core
)

target_link_libraries(pulp-format
    PUBLIC
        pulp::format-core
        pulp::format-view
)

if(NOT DEFINED PULP_ENABLE_JS)
    message(FATAL_ERROR
        "PULP_ENABLE_JS must be declared before core/format is configured; "
        "otherwise ViewBridge's scripted-UI static closure is order-dependent")
endif()
if(PULP_ENABLE_JS)
    # ViewBridge's implementation reaches ScriptedUiSession. Keep that static
    # closure link-only so the final artifact chooses view-native or the legacy
    # WebView-capable view umbrella without inheriting both compositions.
    target_link_libraries(pulp-format-view PRIVATE pulp::view-script)
    get_target_property(_pulp_format_private_links pulp-format-view LINK_LIBRARIES)
    if(NOT "${_pulp_format_private_links}" MATCHES "pulp::view-script")
        message(FATAL_ERROR
            "PULP_ENABLE_JS=ON but pulp-format-view does not close over "
            "pulp::view-script: ${_pulp_format_private_links}")
    endif()
    unset(_pulp_format_private_links)
endif()

# Shipping invariant: the ordinary format SDK must never expose any inspector
# component through its link interface. Static-link dead stripping can hide
# such a regression from binary symbol scans, so enforce the target graph
# itself as well as the executable proof in the test suite.
#
# Checked on all three targets, not just the umbrella. The umbrella's own
# interface is now just the two half-names, so an inspector edge added to
# either half would no longer match a check that only inspected pulp-format --
# the assertion would keep passing while proving nothing.
foreach(_pulp_format_target pulp-format-core pulp-format-view pulp-format)
    get_target_property(_pulp_format_link_interface
        ${_pulp_format_target} INTERFACE_LINK_LIBRARIES)
    if("${_pulp_format_link_interface}" MATCHES "pulp(::|-)inspect")
        message(FATAL_ERROR
            "${_pulp_format_target} must not expose inspector link dependencies: "
            "${_pulp_format_link_interface}")
    endif()
endforeach()
unset(_pulp_format_target)
unset(_pulp_format_link_interface)

# PULP_ENABLE_AUDIO_PROBES wires the RT output-boundary probe into the
# standalone host (the dev Audio Inspector's data source). Unlike the
# inspector flag, this one gates MEMBER declarations in the PUBLIC
# standalone.hpp (output_probe_ / output_probe_ptrs_ / output_probe()), so it
# is defined PUBLIC — every target that includes standalone.hpp must see the
# same value or sizeof(StandaloneApp) diverges (ODR). Defined unconditionally
# as 0/1 so source can `#if PULP_ENABLE_AUDIO_PROBES`. Default ON for
# dev/examples; release/standalone-ship builds configure OFF.
#
# Defined on core only. pulp-format-view PUBLIC-links core, and the umbrella
# links both, so there is exactly one definition site and no path can disagree.
if(PULP_ENABLE_AUDIO_PROBES)
    target_compile_definitions(pulp-format-core PUBLIC PULP_ENABLE_AUDIO_PROBES=1)
else()
    target_compile_definitions(pulp-format-core PUBLIC PULP_ENABLE_AUDIO_PROBES=0)
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
    target_compile_definitions(pulp-format-core PUBLIC PULP_HOST_QUIRKS_DEFAULT_POLICY_VALIDATED_ONLY=1)
elseif(PULP_HOST_QUIRKS_DEFAULT_POLICY STREQUAL "off")
    target_compile_definitions(pulp-format-core PUBLIC PULP_HOST_QUIRKS_DEFAULT_POLICY_OFF=1)
elseif(NOT PULP_HOST_QUIRKS_DEFAULT_POLICY STREQUAL "all")
    message(FATAL_ERROR
        "PULP_HOST_QUIRKS_DEFAULT_POLICY must be all|validated_only|off, got "
        "'${PULP_HOST_QUIRKS_DEFAULT_POLICY}'")
endif()

# VST3 adapter (when SDK is available)
#
# Core, not view: vst3_adapter.hpp has no view includes, and the adapter's one
# GUI reference goes through make_plug_view() (declared view-free in
# vst3_adapter.hpp, defined in vst3_plug_view.cpp, which is compiled
# per-plugin rather than into this library).
if(PULP_HAS_VST3)
    target_sources(pulp-format-core PRIVATE
        src/vst3_adapter.cpp
    )
    target_link_libraries(pulp-format-core PUBLIC vst3-sdk)
    # PULP_VST3_GUI must stay defined here. It guards createView()'s body in
    # vst3_adapter.cpp; without it createView() returns nullptr, which is legal
    # VST3 and shows up only as a plugin that silently has no editor.
    # pulp-test-vst3-plugin-state covers exactly this.
    target_compile_definitions(pulp-format-core PRIVATE PULP_VST3=1 PULP_VST3_GUI=1)
endif()

# CLAP adapter
#
# View, not core: PulpClapPlugin embeds std::unique_ptr<ViewBridge>,
# std::unique_ptr<view::PluginViewHost> and a GpuSurfaceSubscription by value
# under PULP_CLAP_GUI, so clap_adapter.cpp needs those types complete for
# sizeof and for `delete self`. Severing that would mean reworking the GUI
# extension block in clap_entry.hpp, and clap_adapter.hpp would still have to
# include pulp/view/plugin_view_host.hpp for sizeof afterwards -- moving the
# dependency out of CMake and into the source rather than removing it.
if(PULP_HAS_CLAP)
    target_sources(pulp-format-view PRIVATE
        src/clap_adapter.cpp
        src/clap_remote_controls.cpp
        src/clap_note_name.cpp
    )
    target_link_libraries(pulp-format-view PUBLIC clap)
    # Keep PulpClapPlugin's layout identical between clap_adapter.cpp and the
    # per-plugin clap_entry.cpp TU. The entry target defines PULP_CLAP_GUI=1;
    # the shared adapter must do the same or GUI members shift later fields.
    target_compile_definitions(pulp-format-view PRIVATE PULP_CLAP=1 PULP_CLAP_GUI=1)
endif()

# LV2 adapter
if(PULP_HAS_LV2)
    target_sources(pulp-format-core PRIVATE
        src/lv2_adapter.cpp
    )
    target_link_libraries(pulp-format-core PUBLIC lv2-headers)
    target_compile_definitions(pulp-format-core PRIVATE PULP_LV2=1)
endif()

if(PULP_HAS_AUSDK)
    if(APPLE)
        target_sources(pulp-format-core PRIVATE
            src/au_v2_common.cpp
            src/au_v2_adapter.cpp
            src/au_v2_instrument.cpp
            src/au_v2_midi_processor.cpp
        )
        # AudioUnitSDK 1.4 headers use std::expected (C++23). PUBLIC feature
        # propagation from the ausdk target isn't reliable across all Apple
        # clang toolchains when the consumer has CMAKE_CXX_STANDARD=20, so
        # pin it here explicitly for pulp-format-core.
        target_compile_features(pulp-format-core PRIVATE cxx_std_23)
    endif()
    # PRIVATE, not PUBLIC: pulp-format-core uses AudioUnitSDK only in its own AU
    # v2 .cpp sources, so it needs the headers at build time but must NOT export
    # external/AudioUnitSDK/include onto every consumer of pulp::format. A
    # PUBLIC link leaked ausdk's $<INSTALL_INTERFACE:external/AudioUnitSDK/include>
    # to anything linking pulp::format (e.g. an existing plugin that embeds a
    # Pulp view via pulp-view-embed), colliding with that consumer's OWN
    # vendored, differently-versioned AudioUnitSDK. Pulp's own AU plugins
    # don't rely on this
    # propagation — _pulp_add_au links ausdk PRIVATE directly to the AU target,
    # and the only public headers that pull AudioUnitSDK (au_v2_entry.hpp et al)
    # are opt-in, compiled solely into an AU plugin entry point. Static-lib link
    # propagation still pulls ausdk's objects into consumers that need them; only
    # the stray include-dir is withheld. C++23 propagation is a separate PUBLIC
    # compile-feature below, unaffected.
    target_link_libraries(pulp-format-core PRIVATE ausdk)
    # AudioUnitSDK 1.4 headers `#include <expected>` (C++23). Apple clang
    # only exposes std::expected when the consuming TU is compiled with
    # -std=c++23. Set the standard explicitly (target_compile_features
    # alone is ignored when CMAKE_CXX_STANDARD pins a lower version —
    # CMake 3.24 treats CXX_STANDARD as authoritative, so we need to set
    # it per-target). Public so every TU that links pulp::format on
    # Apple also builds at C++23. Set on both halves: CXX_STANDARD is a
    # per-target property and does not propagate along the link interface
    # the way the compile feature does.
    set_target_properties(pulp-format-core pulp-format-view pulp-format
        PROPERTIES CXX_STANDARD 23)
    target_compile_features(pulp-format-core PUBLIC cxx_std_23)
endif()

# AU v2 adapter uses Apple AudioUnitSDK — compiled per-plugin in the entry point,
# not in the shared library. The au_adapter.mm (AUv3) works on both macOS and iOS.
if(APPLE)
    target_compile_definitions(pulp-format-core PRIVATE PULP_AU=1)
    # CoreMIDI supplies the MIDIPacketList builders the AU v2 MIDI-output path
    # uses to deliver kAudioUnitProperty_MIDIOutputCallback packets to the host.
    # PUBLIC so every consumer of pulp::format (the AU v2 adapter header inlines
    # the builder) resolves the symbols too.
    target_link_libraries(pulp-format-core PUBLIC "-framework CoreMIDI")
    if(PULP_IOS)
        # iOS uses AUv3 only (no AU v2 component manager)
        target_link_libraries(pulp-format-core PRIVATE
            "-framework AudioToolbox"
            "-framework AVFoundation"
            "-framework UIKit"
        )
        target_compile_definitions(pulp-format-core PRIVATE PULP_IOS=1)
    endif()
endif()

# ── ARA 2.x ──────────────────────────────────────────────────────────────────
# Celemony's ARA SDK is developer-supplied; not bundled. Point
# PULP_ARA_SDK_DIR at a local checkout of https://github.com/Celemony/ARA_SDK
# and set PULP_ENABLE_ARA=ON to enable the integration.
#
# Declared on core and PUBLIC. ara.cpp and ara_factory.cpp consume it here, and
# clap_adapter.cpp consumes it in pulp-format-view, which reaches it through
# the PUBLIC view -> core link above.
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
    target_compile_definitions(pulp-format-core PUBLIC PULP_HAS_ARA=1)
    target_include_directories(pulp-format-core PUBLIC
        $<BUILD_INTERFACE:${PULP_ARA_SDK_DIR}>
    )
    message(STATUS "Pulp: ARA SDK enabled (${PULP_ARA_SDK_DIR})")
endif()
