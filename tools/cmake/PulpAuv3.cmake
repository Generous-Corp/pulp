# PulpAuv3.cmake — Audio Unit v3 (iOS app extension) helpers.
#
# Extracted from PulpUtils.cmake in the 2026-05 Phase 3 (R2-9) refactor.
# AUv3 needs a separate iOS extension target plus a containing iOS
# app shell, which is a different shape from AU v2's component
# bundle (in PulpPluginFormats.cmake).
#
# Functions:
#   _pulp_add_auv3      — internal: per-target setup
#   pulp_add_ios_auv3   — public: scaffold the iOS app + extension
function(_pulp_add_auv3 target name bundle_id version manufacturer category plugin_code manufacturer_code accepts_midi)
    if(NOT APPLE)
        return()
    endif()

    # Map category + accepts_midi to AU type and tag. Same routing logic
    # as _pulp_add_au — AU v3 hosts also require aumf for effects that
    # want MIDI input.
    if(category STREQUAL "Instrument")
        set(au_type "aumu")
        set(au_tag "Synthesizer")
    elseif(category STREQUAL "MidiEffect")
        set(au_type "aumi")
        set(au_tag "MIDI")
    elseif(accepts_midi)
        set(au_type "aumf")
        set(au_tag "Effects")
    else()
        set(au_type "aufx")
        set(au_tag "Effects")
    endif()

    # AUv3 sources — shared adapter plus platform-specific view controller
    set(auv3_sources
        ${_PULP_FORMAT_SOURCE_DIR}/au_adapter.mm
        ${_PULP_FORMAT_SOURCE_DIR}/au_entry.mm
    )
    if(PULP_IOS)
        list(APPEND auv3_sources
            ${_PULP_FORMAT_SOURCE_DIR}/au_view_controller_ios.mm
        )
    endif()

    # AUv3 is built as a loadable module (.appex)
    add_library(${target}_AUv3 MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${auv3_sources}
    )

    # Platform-specific frameworks
    set(auv3_frameworks
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreAudioKit"
    )
    if(PULP_IOS)
        list(APPEND auv3_frameworks "-framework UIKit")
    else()
        list(APPEND auv3_frameworks "-framework Cocoa")
    endif()

    target_link_libraries(${target}_AUv3 PRIVATE
        ${target}_Core
        ${_PULP_FORMAT_TARGET}
        ${_PULP_VIEW_TARGET}
        ${auv3_frameworks}
    )
    target_include_directories(${target}_AUv3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # Configure Info.plist — use iOS template on iOS, fallback to generic otherwise
    set(PLUGIN_NAME "${name}")
    set(PLUGIN_BUNDLE_ID "${bundle_id}")
    set(PLUGIN_VERSION "${version}")
    set(PLUGIN_MANUFACTURER "${manufacturer}")
    set(PLUGIN_MANUFACTURER_CODE "${manufacturer_code}")
    set(PLUGIN_SUBTYPE_CODE "${plugin_code}")
    set(PLUGIN_AU_TYPE "${au_type}")
    set(PLUGIN_AU_TAG "${au_tag}")
    # Compute integer version (major * 65536 + minor * 256 + patch)
    string(REPLACE "." ";" _ver_parts "${version}")
    list(GET _ver_parts 0 _ver_major)
    list(GET _ver_parts 1 _ver_minor)
    list(GET _ver_parts 2 _ver_patch)
    math(EXPR PLUGIN_AU_VERSION_INT "${_ver_major} * 65536 + ${_ver_minor} * 256 + ${_ver_patch}")

    if(PULP_IOS AND EXISTS "${_PULP_IOS_AUV3_TEMPLATE_DIR}/Info.plist.in")
        configure_file(
            "${_PULP_IOS_AUV3_TEMPLATE_DIR}/Info.plist.in"
            "${CMAKE_BINARY_DIR}/AUv3/${target}.appex/Info.plist"
            @ONLY
        )
        set_target_properties(${target}_AUv3 PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/AUv3/${target}.appex/Info.plist"
        )
    elseif(EXISTS "${_PULP_AUV3_TEMPLATE_DIR}/Info.plist.template")
        configure_file(
            "${_PULP_AUV3_TEMPLATE_DIR}/Info.plist.template"
            "${CMAKE_BINARY_DIR}/AUv3/${target}.appex/Contents/Info.plist"
            @ONLY
        )
        set_target_properties(${target}_AUv3 PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/AUv3/${target}.appex/Contents/Info.plist"
        )
    endif()

    set_target_properties(${target}_AUv3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "appex"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AUv3"
        OUTPUT_NAME "${name}"
        PREFIX ""
        SUFFIX ".appex"
    )

    # iOS-specific Xcode attributes
    if(PULP_IOS)
        set_target_properties(${target}_AUv3 PROPERTIES
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
            XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.0"
        )
    endif()

    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target}_AUv3)
    endif()
endfunction()

# ── pulp_add_ios_auv3 ────────────────────────────────────────────────────
# Public helper for iOS AUv3 app-extension targets.
#
# Wraps the internal `_pulp_add_auv3` and builds a Core OBJECT library from
# the supplied SOURCES so example and user projects can declare a single
# target without the `FORMATS AUv3` boilerplate of `pulp_add_plugin`.
#
# This helper builds the `.appex` bundle only. The host container app that
# the App Store requires is still authored separately; a generator for the
# SwiftUI host template in `templates/ios-auv3/HostApp/` is follow-on work.
#
# Usage:
#   pulp_add_ios_auv3(
#       NAME               PulpSineSynth
#       BUNDLE_ID          com.pulp.examples.sinesynth
#       MANUFACTURER       Pulp
#       MANUFACTURER_CODE  Pulp        # exactly 4 characters
#       SUBTYPE_CODE       PsSn        # exactly 4 characters
#       AU_TYPE            aumu        # aumu (instrument) | aufx (audio-only effect) |
#                                      # aumf (effect that accepts MIDI) | aumi (MIDI processor)
#       VERSION            0.1.0
#       SOURCES            src/sine_synth.cpp src/sine_synth.hpp
#   )
#
# Creates:
#   ${NAME}_Core  — OBJECT library with the processor sources
#   ${NAME}_AUv3  — AUv3 app-extension bundle (.appex)
function(pulp_add_ios_auv3)
    cmake_parse_arguments(AUV3
        "ACCEPTS_MIDI"
        "NAME;BUNDLE_ID;MANUFACTURER;MANUFACTURER_CODE;SUBTYPE_CODE;AU_TYPE;VERSION"
        "SOURCES"
        ${ARGN}
    )

    if(NOT PULP_IOS)
        message(FATAL_ERROR
            "pulp_add_ios_auv3(${AUV3_NAME}): iOS-only helper. Configure with "
            "-DCMAKE_SYSTEM_NAME=iOS (optionally -DCMAKE_OSX_SYSROOT=iphonesimulator) "
            "before invoking it.")
    endif()

    foreach(_req IN ITEMS NAME BUNDLE_ID MANUFACTURER MANUFACTURER_CODE SUBTYPE_CODE VERSION)
        if(NOT AUV3_${_req})
            message(FATAL_ERROR "pulp_add_ios_auv3: ${_req} is required")
        endif()
    endforeach()

    string(LENGTH "${AUV3_MANUFACTURER_CODE}" _mfr_len)
    if(NOT _mfr_len EQUAL 4)
        message(FATAL_ERROR
            "pulp_add_ios_auv3(${AUV3_NAME}): MANUFACTURER_CODE must be exactly "
            "4 characters (got '${AUV3_MANUFACTURER_CODE}', length ${_mfr_len}).")
    endif()
    string(LENGTH "${AUV3_SUBTYPE_CODE}" _sub_len)
    if(NOT _sub_len EQUAL 4)
        message(FATAL_ERROR
            "pulp_add_ios_auv3(${AUV3_NAME}): SUBTYPE_CODE must be exactly "
            "4 characters (got '${AUV3_SUBTYPE_CODE}', length ${_sub_len}).")
    endif()

    if(NOT AUV3_AU_TYPE)
        set(AUV3_AU_TYPE "aufx")
    endif()
    if(AUV3_AU_TYPE STREQUAL "aumu")
        set(_category "Instrument")
    elseif(AUV3_AU_TYPE STREQUAL "aumi")
        set(_category "MidiEffect")
    elseif(AUV3_AU_TYPE STREQUAL "aufx")
        set(_category "Effect")
    else()
        message(FATAL_ERROR
            "pulp_add_ios_auv3(${AUV3_NAME}): AU_TYPE must be aumu, aumi, or "
            "aufx (got '${AUV3_AU_TYPE}').")
    endif()

    set(target "${AUV3_NAME}")

    if(AUV3_SOURCES)
        add_library(${target}_Core OBJECT ${AUV3_SOURCES})
        target_link_libraries(${target}_Core PUBLIC ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(${target}_Core PRIVATE
            PULP_PLUGIN_NAME="${AUV3_NAME}"
            PULP_BUNDLE_ID="${AUV3_BUNDLE_ID}"
            PULP_PLUGIN_VERSION="${AUV3_VERSION}"
        )
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    set(PULP_${target}_CORE_OBJECTS "" CACHE INTERNAL "")

    # Mirror pulp_add_plugin: resolve the ACCEPTS_MIDI flag into the
    # TRUE/FALSE string that _pulp_add_auv3 expects as its 9th arg. Added
    # to _pulp_add_auv3's signature alongside the aufx→aumf fix; without
    # it cmake_parse_arguments leaves AUV3_ACCEPTS_MIDI unset on iOS
    # callers and _pulp_add_auv3 is invoked with 8 args (seen on CI as
    # "_pulp_add_auv3 Function invoked with incorrect arguments" at
    # examples/ios-auv3-synth/CMakeLists.txt:17).
    if(AUV3_ACCEPTS_MIDI)
        set(_auv3_accepts_midi "TRUE")
    else()
        set(_auv3_accepts_midi "FALSE")
    endif()

    _pulp_add_auv3(${target}
        "${AUV3_NAME}"
        "${AUV3_BUNDLE_ID}"
        "${AUV3_VERSION}"
        "${AUV3_MANUFACTURER}"
        "${_category}"
        "${AUV3_SUBTYPE_CODE}"
        "${AUV3_MANUFACTURER_CODE}"
        "${_auv3_accepts_midi}"
    )

    if(TARGET ${target}_AUv3)
        target_link_libraries(${target}_AUv3 PRIVATE ${target}_Core)
    endif()

    message(STATUS "Pulp iOS AUv3: ${target} (type: ${AUV3_AU_TYPE}, subtype: ${AUV3_SUBTYPE_CODE})")
endfunction()

# ── Internal: Standalone target ─────────────────────────────────────────
