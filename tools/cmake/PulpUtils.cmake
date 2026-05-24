# PulpUtils.cmake — Build utilities for Pulp projects
#
# Provides:
#   pulp_add_plugin()  — Create a plugin target with format adapters
#   pulp_add_app()     — Create a standalone application target
#   pulp_app_icon()    — Attach a generated app icon to a target

include("${CMAKE_CURRENT_LIST_DIR}/PulpAppIcon.cmake")

function(_pulp_pick_target out_var)
    foreach(_candidate IN LISTS ARGN)
        if(TARGET "${_candidate}")
            set(${out_var} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    set(${out_var} "" PARENT_SCOPE)
endfunction()

_pulp_pick_target(_PULP_FORMAT_TARGET Pulp::format pulp::format)
_pulp_pick_target(_PULP_VIEW_TARGET Pulp::view pulp::view)
_pulp_pick_target(_PULP_AUDIO_TARGET Pulp::audio pulp::audio)
_pulp_pick_target(_PULP_MIDI_TARGET Pulp::midi pulp::midi)
_pulp_pick_target(_PULP_STANDALONE_TARGET Pulp::standalone pulp::standalone)
_pulp_pick_target(_PULP_VST3_SDK_TARGET Pulp::vst3-sdk vst3-sdk)
_pulp_pick_target(_PULP_CLAP_TARGET Pulp::clap clap)
_pulp_pick_target(_PULP_LV2_TARGET Pulp::lv2-headers lv2-headers)
_pulp_pick_target(_PULP_AUSDK_TARGET Pulp::ausdk ausdk)
_pulp_pick_target(_PULP_AAX_LIBRARY_TARGET Pulp::aax-library pulp-aax-library)

if(NOT _PULP_FORMAT_TARGET)
    message(FATAL_ERROR
        "PulpUtils.cmake requires Pulp targets to exist first. "
        "Use add_subdirectory(Pulp) or find_package(Pulp) before including it.")
endif()

# Resolve PulpUtils.cmake's sibling helper dirs without reaching into
# CMAKE_SOURCE_DIR (which is the *consumer's* source tree when this
# file is loaded via find_package, not Pulp's). Pulp #2087 piggyback —
# CMAKE_CURRENT_LIST_DIR at top level is this file's own dir, so we
# probe both possible layouts (in-tree source build + installed SDK)
# before falling back. The fallback path is kept as a last-resort
# escape hatch for stale checkouts that haven't run `cmake --install`.
#
#   In-tree:    tools/cmake/PulpUtils.cmake
#               -> ../../core/format/src    (Pulp source tree)
#               -> ../../tools/templates/auv3
#               -> ../../templates/ios-auv3
#   Installed:  <prefix>/lib/cmake/Pulp/PulpUtils.cmake
#               -> ../../../src/pulp/format (matches root install(FILES) DESTINATION src/pulp/format)
#               -> ../../../templates/auv3
#               -> ../../../templates/ios-auv3
if(DEFINED PULP_FORMAT_SOURCE_DIR AND EXISTS "${PULP_FORMAT_SOURCE_DIR}")
    set(_PULP_FORMAT_SOURCE_DIR "${PULP_FORMAT_SOURCE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../core/format/src")
    set(_PULP_FORMAT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../core/format/src")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/format")
    set(_PULP_FORMAT_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src/pulp/format")
else()
    # Last resort: leave unset so plugin targets fail with a clear "missing
    # source" error at link-time rather than silently picking up the
    # consumer's source tree. Override with -DPULP_FORMAT_SOURCE_DIR=... if
    # the SDK install is incomplete.
    set(_PULP_FORMAT_SOURCE_DIR "")
endif()

if(DEFINED PULP_VST3_INCLUDE_DIR AND EXISTS "${PULP_VST3_INCLUDE_DIR}")
    set(_PULP_VST3_SDK_DIR "${PULP_VST3_INCLUDE_DIR}")
elseif(DEFINED VST3_SDK_DIR AND EXISTS "${VST3_SDK_DIR}")
    set(_PULP_VST3_SDK_DIR "${VST3_SDK_DIR}")
else()
    set(_PULP_VST3_SDK_DIR "")
endif()

if(DEFINED PULP_AUV3_TEMPLATE_DIR AND EXISTS "${PULP_AUV3_TEMPLATE_DIR}")
    set(_PULP_AUV3_TEMPLATE_DIR "${PULP_AUV3_TEMPLATE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../tools/templates/auv3")
    set(_PULP_AUV3_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../tools/templates/auv3")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../templates/auv3")
    set(_PULP_AUV3_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../templates/auv3")
else()
    set(_PULP_AUV3_TEMPLATE_DIR "")
endif()

if(DEFINED PULP_IOS_AUV3_TEMPLATE_DIR AND EXISTS "${PULP_IOS_AUV3_TEMPLATE_DIR}")
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "${PULP_IOS_AUV3_TEMPLATE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../templates/ios-auv3")
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../templates/ios-auv3")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../../templates/ios-auv3")
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../templates/ios-auv3")
else()
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "")
endif()

function(_pulp_normalize_ui_script_path out_var source_dir ui_script)
    if(NOT ui_script)
        set(${out_var} "" PARENT_SCOPE)
        return()
    endif()

    if(IS_ABSOLUTE "${ui_script}")
        set(_ui_script_path "${ui_script}")
    else()
        set(_ui_script_path "${source_dir}/${ui_script}")
    endif()

    get_filename_component(_ui_script_path "${_ui_script_path}" ABSOLUTE)
    file(TO_CMAKE_PATH "${_ui_script_path}" _ui_script_path)
    set(${out_var} "${_ui_script_path}" PARENT_SCOPE)
endfunction()

function(_pulp_apply_ui_script_definition target ui_script_path)
    if(NOT ui_script_path)
        return()
    endif()

    target_compile_definitions(${target} PRIVATE
        PULP_UI_SCRIPT_PATH="${ui_script_path}"
    )
endfunction()

# ── pulp_add_plugin ─────────────────────────────────────────────────────────
# Creates plugin targets for each requested format from a single declaration.
#
# Usage:
#   pulp_add_plugin(PulpGain
#       FORMATS         VST3 AU CLAP Standalone
#       PLUGIN_NAME     "PulpGain"
#       BUNDLE_ID       "com.pulp.gain"
#       MANUFACTURER    "Pulp"
#       VERSION         "1.0.0"
#       CATEGORY        Effect           # Effect | Instrument | MidiEffect
#       PLUGIN_CODE     "PGan"           # 4-char code for AU
#       MANUFACTURER_CODE "Pulp"         # 4-char code for AU
#       ACCEPTS_MIDI                     # set if descriptor.accepts_midi is true;
#                                        # flips AU component type from aufx to aumf
#                                        # so hosts route inbound MIDI to the plug-in
#       SOURCES         pulp_gain.hpp main.cpp
#       PROCESSOR_FACTORY create_pulp_gain  # Function that returns unique_ptr<Processor>
#   )
#
# This creates:
#   ${target}_Core       — Object library with shared processor code
#   ${target}_VST3       — VST3 bundle (.vst3)
#   ${target}_AU         — AU v2 component (.component)
#   ${target}_CLAP       — CLAP bundle (.clap)
#   ${target}_Standalone — Standalone executable
#
function(pulp_add_plugin target)
    cmake_parse_arguments(PLUGIN
        "ACCEPTS_MIDI;PRODUCES_MIDI"
        "PLUGIN_NAME;BUNDLE_ID;VERSION;MANUFACTURER;CATEGORY;PLUGIN_CODE;MANUFACTURER_CODE;AAX_PRODUCT_CODE;AAX_NATIVE_CODE;PROCESSOR_FACTORY;UI_SCRIPT;DESIGN_WIDTH;DESIGN_HEIGHT;DESIGN_MIN_WIDTH;DESIGN_MIN_HEIGHT;DESIGN_MAX_WIDTH;DESIGN_MAX_HEIGHT"
        "FORMATS;SOURCES"
        ${ARGN}
    )

    # Defaults
    if(NOT PLUGIN_PLUGIN_NAME)
        set(PLUGIN_PLUGIN_NAME "${target}")
    endif()
    if(NOT PLUGIN_VERSION)
        set(PLUGIN_VERSION "1.0.0")
    endif()
    if(NOT PLUGIN_MANUFACTURER)
        set(PLUGIN_MANUFACTURER "Unknown")
    endif()
    if(NOT PLUGIN_CATEGORY)
        set(PLUGIN_CATEGORY "Effect")
    endif()

    # ACCEPTS_MIDI mirrors ``PluginDescriptor::accepts_midi`` to CMake so
    # we can pick the correct AU component type (``aumf`` vs ``aufx``).
    # Hosts only route MIDI to AU v2 effects packaged as
    # ``kAudioUnitType_MusicEffect`` (``aumf``); ``aufx``-typed plug-ins
    # never see ``HandleMIDIEvent`` regardless of what the adapter wires.
    # Plug-ins that set ``accepts_midi = true`` in their descriptor MUST
    # also pass ACCEPTS_MIDI to ``pulp_add_plugin`` so the emitted
    # ``.component`` bundle's ``type`` matches.
    if(PLUGIN_ACCEPTS_MIDI)
        set(_plugin_accepts_midi "1")
    else()
        set(_plugin_accepts_midi "0")
    endif()

    _pulp_normalize_ui_script_path(_PULP_UI_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}" "${PLUGIN_UI_SCRIPT}")
    set(PULP_${target}_UI_SCRIPT "${_PULP_UI_SCRIPT}" CACHE INTERNAL "")

    if(_PULP_UI_SCRIPT AND NOT EXISTS "${_PULP_UI_SCRIPT}")
        message(WARNING
            "pulp_add_plugin(${target}): UI_SCRIPT points to a missing file: ${_PULP_UI_SCRIPT}. "
            "The editor will fall back to AutoUi until the script exists.")
    endif()

    # ── Core library ────────────────────────────────────────────────────
    # For header-only processors (no SOURCES), create INTERFACE library.
    # For compiled processors, create OBJECT library.
    if(PLUGIN_SOURCES)
        add_library(${target}_Core OBJECT ${PLUGIN_SOURCES})
        target_link_libraries(${target}_Core PUBLIC ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        set(_PULP_CORE_OBJECTS "${PULP_${target}_CORE_OBJECTS}")
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
        set(_PULP_CORE_OBJECTS "")
    endif()
    # Store for format target functions (CMake doesn't propagate local vars to functions)
    set(_PULP_CORE_OBJECTS "${_PULP_CORE_OBJECTS}" PARENT_SCOPE)
    set(PULP_${target}_CORE_OBJECTS "${_PULP_CORE_OBJECTS}" CACHE INTERNAL "")
    if(PLUGIN_SOURCES)
        target_compile_definitions(${target}_Core PRIVATE
            PULP_PLUGIN_NAME="${PLUGIN_PLUGIN_NAME}"
            PULP_BUNDLE_ID="${PLUGIN_BUNDLE_ID}"
            PULP_PLUGIN_VERSION="${PLUGIN_VERSION}"
        )
    endif()

    # ── Design dimensions (auto-sizing for imported-design plugins) ────
    # When DESIGN_WIDTH/HEIGHT are supplied, inject them as compile-defs so
    # `format::Processor::view_size()`'s default returns sensible bounds
    # (derived min = 2/3 preferred, max = 2x preferred, aspect = W/H).
    # Plugins still pull this in via target_compile_definitions on Core,
    # so all linked format adapters see the same defs. Explicit MIN/MAX
    # args override the derived values. See processor.hpp:view_size() and
    # the import-design skill. Issue #2784.
    if(PLUGIN_DESIGN_WIDTH AND PLUGIN_DESIGN_HEIGHT)
        if(NOT PLUGIN_DESIGN_MIN_WIDTH)
            set(PLUGIN_DESIGN_MIN_WIDTH 0)
        endif()
        if(NOT PLUGIN_DESIGN_MIN_HEIGHT)
            set(PLUGIN_DESIGN_MIN_HEIGHT 0)
        endif()
        if(NOT PLUGIN_DESIGN_MAX_WIDTH)
            set(PLUGIN_DESIGN_MAX_WIDTH 0)
        endif()
        if(NOT PLUGIN_DESIGN_MAX_HEIGHT)
            set(PLUGIN_DESIGN_MAX_HEIGHT 0)
        endif()
        set(_design_defs
            PULP_PLUGIN_DESIGN_W=${PLUGIN_DESIGN_WIDTH}
            PULP_PLUGIN_DESIGN_H=${PLUGIN_DESIGN_HEIGHT}
            PULP_PLUGIN_DESIGN_MIN_W=${PLUGIN_DESIGN_MIN_WIDTH}
            PULP_PLUGIN_DESIGN_MIN_H=${PLUGIN_DESIGN_MIN_HEIGHT}
            PULP_PLUGIN_DESIGN_MAX_W=${PLUGIN_DESIGN_MAX_WIDTH}
            PULP_PLUGIN_DESIGN_MAX_H=${PLUGIN_DESIGN_MAX_HEIGHT}
        )
        if(PLUGIN_SOURCES)
            target_compile_definitions(${target}_Core PUBLIC ${_design_defs})
        else()
            target_compile_definitions(${target}_Core INTERFACE ${_design_defs})
        endif()
    endif()

    # ── VST3 ─────────────────────────────────────────────────────────────
    if("VST3" IN_LIST PLUGIN_FORMATS AND PULP_HAS_VST3)
        _pulp_add_vst3(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                        "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── CLAP ─────────────────────────────────────────────────────────────
    if("CLAP" IN_LIST PLUGIN_FORMATS AND PULP_HAS_CLAP)
        _pulp_add_clap(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                        "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── AU v2 ────────────────────────────────────────────────────────────
    if("AU" IN_LIST PLUGIN_FORMATS AND APPLE AND PULP_HAS_AUSDK)
        if(NOT PLUGIN_PLUGIN_CODE OR NOT PLUGIN_MANUFACTURER_CODE)
            message(WARNING "pulp_add_plugin(${target}): AU format requires PLUGIN_CODE and MANUFACTURER_CODE")
        else()
            _pulp_add_au(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                          "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                          "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}"
                          "${_plugin_accepts_midi}")
        endif()
    endif()

    # ── LV2 ───────────────────────────────────────────────────────────────
    if("LV2" IN_LIST PLUGIN_FORMATS AND PULP_HAS_LV2)
        _pulp_add_lv2(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                       "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── AAX ───────────────────────────────────────────────────────────────
    if("AAX" IN_LIST PLUGIN_FORMATS)
        if(NOT APPLE AND NOT WIN32)
            message(FATAL_ERROR
                "pulp_add_plugin(${target}): AAX is only supported on macOS and Windows. "
                "Remove AAX from FORMATS when configuring on Linux or Ubuntu.")
        elseif(PULP_HAS_AAX)
            if(NOT PLUGIN_MANUFACTURER_CODE OR NOT PLUGIN_AAX_PRODUCT_CODE OR NOT PLUGIN_AAX_NATIVE_CODE)
                message(FATAL_ERROR
                    "pulp_add_plugin(${target}): AAX format requires "
                    "MANUFACTURER_CODE, AAX_PRODUCT_CODE, and AAX_NATIVE_CODE")
            endif()

            _pulp_add_aax(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                          "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                          "${PLUGIN_CATEGORY}" "${PLUGIN_MANUFACTURER_CODE}"
                          "${PLUGIN_AAX_PRODUCT_CODE}" "${PLUGIN_AAX_NATIVE_CODE}")
        endif()
    endif()

    # ── AUv3 ──────────────────────────────────────────────────────────────
    if("AUv3" IN_LIST PLUGIN_FORMATS AND APPLE)
        if(NOT PLUGIN_PLUGIN_CODE OR NOT PLUGIN_MANUFACTURER_CODE)
            message(WARNING "pulp_add_plugin(${target}): AUv3 format requires PLUGIN_CODE and MANUFACTURER_CODE")
        else()
            _pulp_add_auv3(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                           "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                           "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}"
                           "${_plugin_accepts_midi}")
        endif()
    endif()

    # ── Standalone ───────────────────────────────────────────────────────
    if("Standalone" IN_LIST PLUGIN_FORMATS)
        _pulp_add_standalone(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}" "${PLUGIN_VERSION}")
    endif()

    # ── Install targets ────────────────────────────────────────────────
    # Platform-appropriate install locations for each format
    if(APPLE)
        set(_vst3_dir "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
        set(_clap_dir "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")
        set(_au_dir "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
        set(_aax_dir "/Library/Application Support/Avid/Audio/Plug-Ins")
    elseif(WIN32)
        set(_vst3_dir "$ENV{COMMONPROGRAMFILES}/VST3")
        set(_clap_dir "$ENV{COMMONPROGRAMFILES}/CLAP")
        set(_aax_dir "$ENV{COMMONPROGRAMFILES}/Avid/Audio/Plug-Ins")
    elseif(UNIX)
        set(_vst3_dir "$ENV{HOME}/.vst3")
        set(_clap_dir "$ENV{HOME}/.clap")
    endif()

    # Custom install target: pulp-install-<target>
    set(_install_commands "")
    if(TARGET ${target}_VST3 AND DEFINED _vst3_dir)
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/VST3/${PLUGIN_PLUGIN_NAME}.vst3"
                "${_vst3_dir}/${PLUGIN_PLUGIN_NAME}.vst3")
    endif()
    if(TARGET ${target}_CLAP AND DEFINED _clap_dir)
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/CLAP/${PLUGIN_PLUGIN_NAME}.clap"
                "${_clap_dir}/${PLUGIN_PLUGIN_NAME}.clap")
    endif()
    if(TARGET ${target}_AU AND DEFINED _au_dir)
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/AU/${PLUGIN_PLUGIN_NAME}.component"
                "${_au_dir}/${PLUGIN_PLUGIN_NAME}.component")
    endif()
    if(TARGET ${target}_AAX AND DEFINED _aax_dir)
        list(APPEND _install_commands
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_BINARY_DIR}/AAX/${PLUGIN_PLUGIN_NAME}.aaxplugin"
                "${_aax_dir}/${PLUGIN_PLUGIN_NAME}.aaxplugin")
    endif()

    if(_install_commands)
        add_custom_target(pulp-install-${target}
            ${_install_commands}
            COMMENT "Installing ${PLUGIN_PLUGIN_NAME} to system plugin folders"
        )
    endif()

    set(_built_formats)
    foreach(_fmt VST3 CLAP AU AUv3 LV2 AAX Standalone)
        if(TARGET ${target}_${_fmt})
            list(APPEND _built_formats ${_fmt})
        endif()
    endforeach()
    if(_built_formats)
        list(JOIN _built_formats ";" _built_formats_display)
    else()
        set(_built_formats_display "none")
    endif()
    message(STATUS "Pulp plugin: ${target} (formats: ${_built_formats_display})")
endfunction()

# ── Internal: VST3 target ────────────────────────────────────────────────

# Phase 3 (R2-9) split: per-format / AUv3 / app target helpers
# live in focused modules now. PulpUtils.cmake stays the
# public include shim so external `find_package(Pulp)` users
# get the full surface via a single include.
include("${CMAKE_CURRENT_LIST_DIR}/PulpPluginFormats.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpAuv3.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/PulpAppTargets.cmake")
