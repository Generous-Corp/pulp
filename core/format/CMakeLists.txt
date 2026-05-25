add_library(pulp-format)
add_library(pulp::format ALIAS pulp-format)

target_include_directories(pulp-format
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-format PRIVATE
    src/format.cpp
    src/registry.cpp
    src/headless.cpp
    src/plugin_state_io.cpp
    src/validation_harness.cpp
    src/aax_model.cpp
    src/host_type.cpp
    src/host_version.cpp
    $<$<PLATFORM_ID:Darwin>:src/host_version_mac.mm>
    $<$<PLATFORM_ID:Windows>:src/host_version_win.cpp>
    src/host_quirks.cpp
    src/ara.cpp
    src/ara_factory.cpp
    src/ios_audio_session.cpp
    src/descriptor_validation.cpp
    src/view_bridge.cpp
    src/remote_view_session.cpp
)

target_link_libraries(pulp-format
    PUBLIC
        pulp::audio
        pulp::midi
        pulp::state
        pulp::view
        pulp::runtime
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
    )
    target_link_libraries(pulp-format PUBLIC clap)
    target_compile_definitions(pulp-format PRIVATE PULP_CLAP=1)
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
    target_link_libraries(pulp-format PUBLIC ausdk)
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

# ── ARA 2.x (workstream 06 slice 6.1) ────────────────────────────────────────
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
