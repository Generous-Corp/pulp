# PulpPluginFormats.cmake — per-format plugin packaging helpers.
#
# Extracted from PulpUtils.cmake in the 2026-05 Phase 3 (R2-9) refactor.
# Each `_pulp_add_<format>` function is called by `pulp_add_plugin()` (in
# PulpUtils.cmake) when the requested format is enabled. They handle
# bundle structure, manifest generation, code-signing entitlements, and
# format-specific install rules.
#
# Formats covered here:
#   _pulp_add_vst3   — VST3 bundle (.vst3 / .vst3.dll)
#   _pulp_add_clap   — CLAP bundle (.clap)
#   _pulp_add_lv2    — LV2 bundle (.lv2 directory)
#   _pulp_add_aax    — AAX bundle (optional, vendor SDK)
#   _pulp_add_au     — AU v2 component (.component)
#
# AUv3 (which has its own iOS extension shape) lives in PulpAuv3.cmake.
# Standalone + app targets live in PulpAppTargets.cmake.
#
# Codex R2-9 risk callout: external consumers configure via
# `find_package(Pulp)` which loads PulpUtils.cmake; that file must
# `include()` this one transitively so the helpers are visible to
# user-side `pulp_add_plugin(... FORMATS vst3 clap)` calls.

function(_pulp_add_vst3 target name bundle_id version manufacturer category)
    if(NOT _PULP_VST3_SDK_DIR OR NOT _PULP_VST3_SDK_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): VST3 requested but the VST3 SDK is unavailable")
    endif()
    if(NOT _PULP_VIEW_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): VST3 GUI targets require ${_PULP_VIEW_TARGET}")
    endif()

    # Platform entry point sources
    set(vst3_platform_src "")
    if(APPLE)
        list(APPEND vst3_platform_src
            ${_PULP_VST3_SDK_DIR}/public.sdk/source/main/macmain.cpp)
    elseif(WIN32)
        list(APPEND vst3_platform_src
            ${_PULP_VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp)
    elseif(UNIX)
        list(APPEND vst3_platform_src
            ${_PULP_VST3_SDK_DIR}/public.sdk/source/main/linuxmain.cpp)
    endif()

    # Find VST3 entry point (convention: vst3_entry.cpp in source dir)
    set(vst3_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst3_entry.cpp")
        set(vst3_entry "${CMAKE_CURRENT_SOURCE_DIR}/vst3_entry.cpp")
    endif()

    add_library(${target}_VST3 MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${vst3_entry}
        ${vst3_platform_src}
        ${_PULP_FORMAT_SOURCE_DIR}/vst3_plug_view.cpp
        ${_PULP_VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp
        ${_PULP_VST3_SDK_DIR}/public.sdk/source/main/moduleinit.cpp
    )
    target_link_libraries(${target}_VST3 PRIVATE
        ${target}_Core
        ${_PULP_FORMAT_TARGET}
        ${_PULP_VIEW_TARGET}
        ${_PULP_VST3_SDK_TARGET}
    )
    target_compile_definitions(${target}_VST3 PRIVATE PULP_VST3_GUI=1)
    _pulp_apply_ui_script_definition(${target}_VST3 "${PULP_${target}_UI_SCRIPT}")
    target_include_directories(${target}_VST3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # VST3 bundle structure
    set_target_properties(${target}_VST3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "vst3"
        OUTPUT_NAME "${name}"
        ARCHIVE_OUTPUT_NAME "${name}_vst3"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3-import"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3"
    )

    # Info.plist: use custom if available, otherwise generate from template
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.vst3")
        set_target_properties(${target}_VST3 PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.vst3")
    elseif(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpInfoPlist.vst3.in")
        set(PULP_PLUGIN_NAME "${name}")
        set(PULP_BUNDLE_ID "${bundle_id}")
        set(PULP_VERSION "${version}")
        configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpInfoPlist.vst3.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.vst3"
            @ONLY)
        set_target_properties(${target}_VST3 PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.vst3")
    endif()

    # Write PkgInfo so Finder treats the .vst3 as an opaque bundle (not a folder)
    if(APPLE)
        add_custom_command(TARGET ${target}_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "BNDL????" >
                "$<TARGET_BUNDLE_DIR:${target}_VST3>/Contents/PkgInfo"
            COMMENT "Writing PkgInfo into ${name}.vst3 bundle"
        )
    endif()

    # Copy moduleinfo.json if available
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/moduleinfo.json")
        add_custom_command(TARGET ${target}_VST3 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/moduleinfo.json"
                "${CMAKE_BINARY_DIR}/VST3/${name}.vst3/Contents/moduleinfo.json"
            COMMENT "Copying moduleinfo.json into ${name}.vst3 bundle"
        )
    endif()

    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target}_VST3)
    endif()
endfunction()

# ── Internal: CLAP target ───────────────────────────────────────────────
function(_pulp_add_clap target name bundle_id version manufacturer category)
    if(NOT _PULP_CLAP_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): CLAP requested but the CLAP SDK is unavailable")
    endif()
    if(NOT _PULP_VIEW_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): CLAP GUI targets require Pulp::view")
    endif()

    # Find CLAP entry point (convention: clap_entry.cpp in source dir)
    set(clap_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/clap_entry.cpp")
        set(clap_entry "${CMAKE_CURRENT_SOURCE_DIR}/clap_entry.cpp")
    endif()

    add_library(${target}_CLAP MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${clap_entry}
    )
    target_link_libraries(${target}_CLAP PRIVATE
        ${target}_Core
        ${_PULP_FORMAT_TARGET}
        ${_PULP_VIEW_TARGET}
        ${_PULP_CLAP_TARGET}
    )
    target_compile_definitions(${target}_CLAP PRIVATE PULP_CLAP_GUI=1)
    _pulp_apply_ui_script_definition(${target}_CLAP "${PULP_${target}_UI_SCRIPT}")
    target_include_directories(${target}_CLAP PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(${target}_CLAP PROPERTIES
        OUTPUT_NAME "${name}"
        ARCHIVE_OUTPUT_NAME "${name}_clap"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CLAP-import"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CLAP"
        PREFIX ""
        SUFFIX ".clap"
    )

    if(APPLE)
        set_target_properties(${target}_CLAP PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "clap"
        )
    endif()

    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target}_CLAP)
    endif()
endfunction()

# ── Internal: LV2 target ───────────────────────────────────────────────
function(_pulp_add_lv2 target name bundle_id version manufacturer category)
    if(NOT _PULP_LV2_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): LV2 requested but LV2 headers are unavailable")
    endif()

    # Find LV2 entry point (convention: lv2_entry.cpp in source dir)
    set(lv2_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lv2_entry.cpp")
        set(lv2_entry "${CMAKE_CURRENT_SOURCE_DIR}/lv2_entry.cpp")
    endif()

    add_library(${target}_LV2 MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${lv2_entry}
    )
    target_link_libraries(${target}_LV2 PRIVATE
        ${target}_Core
        ${_PULP_FORMAT_TARGET}
        ${_PULP_LV2_TARGET}
    )
    target_include_directories(${target}_LV2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # LV2 bundles: <name>.lv2/ directory containing the .so and .ttl files
    set_target_properties(${target}_LV2 PROPERTIES
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LV2/${name}.lv2"
        PREFIX ""
    )
endfunction()

# ── Internal: AAX target ────────────────────────────────────────────────
function(_pulp_add_aax target name bundle_id version manufacturer category manufacturer_code product_code native_code)
    if(NOT APPLE AND NOT WIN32)
        return()
    endif()

    if(NOT PULP_HAS_AAX)
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): AAX requested but PULP_HAS_AAX is false. "
            "Configure with -DPULP_ENABLE_AAX=ON and an out-of-tree PULP_AAX_SDK_DIR.")
    endif()

    pulp_ensure_aax_sdk_targets()

    set(aax_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/aax_entry.cpp")
        set(aax_entry "${CMAKE_CURRENT_SOURCE_DIR}/aax_entry.cpp")
    else()
        message(FATAL_ERROR
            "pulp_add_plugin(${target}): AAX requested but no aax_entry.cpp was found "
            "in ${CMAKE_CURRENT_SOURCE_DIR}")
    endif()

    add_library(${target}_AAX MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${aax_entry}
        ${_PULP_FORMAT_SOURCE_DIR}/aax_runtime.cpp
        $<TARGET_OBJECTS:pulp-aax-export>
    )
    target_link_libraries(${target}_AAX PRIVATE
        ${target}_Core
        ${_PULP_FORMAT_TARGET}
        pulp-aax-library
    )
    target_include_directories(${target}_AAX PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target}_AAX PRIVATE
        PULP_AAX=1
        PULP_MANUFACTURER_CODE="${manufacturer_code}"
        PULP_AAX_PRODUCT_CODE="${product_code}"
        PULP_AAX_NATIVE_CODE="${native_code}"
    )
    set_target_properties(${target}_AAX PROPERTIES
        OUTPUT_NAME "${name}"
        PREFIX ""
    )

    if(APPLE)
        set(PULP_PLUGIN_NAME "${name}")
        set(PULP_BUNDLE_ID "${bundle_id}")
        set(PULP_VERSION "${version}")
        configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpInfoPlist.aax.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.aax"
            @ONLY
        )
        set_target_properties(${target}_AAX PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "aaxplugin"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AAX"
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.aax"
        )
        add_custom_command(TARGET ${target}_AAX POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E echo "TDMwPTul" >
                "$<TARGET_BUNDLE_DIR:${target}_AAX>/Contents/PkgInfo"
            COMMENT "Writing PkgInfo into ${name}.aaxplugin bundle"
        )
    else()
        set_target_properties(${target}_AAX PROPERTIES
            SUFFIX ".aaxplugin"
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AAX/${name}.aaxplugin/Contents/x64"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AAX/${name}.aaxplugin/Contents/x64"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AAX-import"
        )
    endif()

    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target}_AAX)
    endif()
endfunction()

# ── Internal: AU v2 target ──────────────────────────────────────────────
function(_pulp_add_au target name bundle_id version manufacturer category plugin_code manufacturer_code accepts_midi)
    if(NOT _PULP_AUSDK_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): AU requested but AudioUnitSDK is unavailable")
    endif()
    if(NOT _PULP_VIEW_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): AU GUI targets require Pulp::view")
    endif()

    # Find AU entry point (convention: au_v2_entry.cpp in source dir)
    set(au_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/au_v2_entry.cpp")
        set(au_entry "${CMAKE_CURRENT_SOURCE_DIR}/au_v2_entry.cpp")
    endif()

    add_library(${target}_AU MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${au_entry}
        ${_PULP_FORMAT_SOURCE_DIR}/au_v2_cocoa_view.mm
    )
    target_link_libraries(${target}_AU PRIVATE
        ${target}_Core
        ${_PULP_FORMAT_TARGET}
        ${_PULP_VIEW_TARGET}
        ${_PULP_AUSDK_TARGET}
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework Cocoa"
    )
    target_compile_definitions(${target}_AU PRIVATE PULP_AU_GUI=1)
    # AudioUnitSDK 1.4 headers use std::expected (C++23). CMAKE_CXX_STANDARD=20
    # at the repo root is authoritative under CMake 3.24's policy — target
    # compile_features are ignored when a lower CXX_STANDARD is already set.
    # Pin the per-target property AND the feature (PUBLIC so loaders linking
    # this target also inherit).
    set_target_properties(${target}_AU PROPERTIES CXX_STANDARD 23)
    target_compile_features(${target}_AU PUBLIC cxx_std_23)
    _pulp_apply_ui_script_definition(${target}_AU "${PULP_${target}_UI_SCRIPT}")
    target_include_directories(${target}_AU PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # AudioUnitSDK 1.4 headers (`AUUtility.h`) use `std::expected` /
    # `std::unexpected` (C++23). The host pulp-format static lib already
    # marks `cxx_std_23` PUBLIC so downstream C++ TUs inherit it, but
    # per-plugin `.mm` sources added directly to ${target}_AU compile with
    # that target's own CXX_STANDARD. Apple clang's Objective-C++ mode
    # only exposes <expected> at -std=c++23, so pin it explicitly here
    # (same rationale as core/format/CMakeLists.txt L76).
    set_target_properties(${target}_AU PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        OBJCXX_STANDARD 23
        OBJCXX_STANDARD_REQUIRED ON
        BUNDLE TRUE
        BUNDLE_EXTENSION "component"
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AU"
    )

    # Info.plist: use custom if available, otherwise generate from template
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.au")
        set_target_properties(${target}_AU PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.au")
    elseif(EXISTS "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpInfoPlist.au.in")
        set(PULP_PLUGIN_NAME "${name}")
        set(PULP_BUNDLE_ID "${bundle_id}")
        set(PULP_VERSION "${version}")
        set(PULP_MANUFACTURER "${manufacturer}")
        set(PULP_MANUFACTURER_CODE "${manufacturer_code}")
        set(PULP_PLUGIN_CODE "${plugin_code}")
        # Determine AU component type from category + accepts_midi flag.
        #   aumu — kAudioUnitType_MusicDevice   (instrument that accepts MIDI)
        #   aumf — kAudioUnitType_MusicEffect   (effect that accepts MIDI;
        #                                       hosts only route MIDI here)
        #   aumi — kAudioUnitType_MIDIProcessor (MIDI in, MIDI out, no audio)
        #   aufx — kAudioUnitType_Effect        (audio-only effect)
        #
        # AU hosts (Logic, MainStage, GarageBand, etc.) never deliver MIDI
        # to an aufx-typed plug-in. When a descriptor declares
        # ``accepts_midi = true`` but we still emitted aufx, the adapter
        # would silently never receive HandleMIDIEvent callbacks — a
        # packaging bug, not a code bug. Flip to aumf so the host wires
        # the MIDI lane. See the auv2 skill for the full rationale and
        # the user-facing DAW cache note.
        if("${category}" STREQUAL "Instrument")
            set(PULP_AU_TYPE "aumu")
        elseif("${category}" STREQUAL "MidiEffect")
            set(PULP_AU_TYPE "aumi")
        elseif(accepts_midi)
            set(PULP_AU_TYPE "aumf")
        else()
            set(PULP_AU_TYPE "aufx")
        endif()
        # Factory function name follows the AUSDK_COMPONENT_ENTRY convention
        set(PULP_AU_FACTORY_NAME "${target}AUFactory")
        # Compute integer version (major * 65536 + minor * 256 + patch)
        string(REPLACE "." ";" _au_ver_parts "${version}")
        list(LENGTH _au_ver_parts _au_ver_len)
        list(GET _au_ver_parts 0 _au_ver_major)
        if(_au_ver_len GREATER 1)
            list(GET _au_ver_parts 1 _au_ver_minor)
        else()
            set(_au_ver_minor 0)
        endif()
        if(_au_ver_len GREATER 2)
            list(GET _au_ver_parts 2 _au_ver_patch)
        else()
            set(_au_ver_patch 0)
        endif()
        math(EXPR PULP_AU_VERSION_INT "${_au_ver_major} * 65536 + ${_au_ver_minor} * 256 + ${_au_ver_patch}")
        configure_file(
            "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpInfoPlist.au.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.au"
            @ONLY)
        set_target_properties(${target}_AU PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.au")
    endif()

    # Write PkgInfo so Finder treats the .component as an opaque bundle (not a folder)
    add_custom_command(TARGET ${target}_AU POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "BNDL????" >
            "$<TARGET_BUNDLE_DIR:${target}_AU>/Contents/PkgInfo"
        COMMENT "Writing PkgInfo into ${name}.component bundle"
    )

    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target}_AU)
    endif()
endfunction()

# ── Internal: AUv3 app extension target ────────────────────────────────
