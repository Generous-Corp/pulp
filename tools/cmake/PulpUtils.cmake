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

if(DEFINED PULP_FORMAT_SOURCE_DIR AND EXISTS "${PULP_FORMAT_SOURCE_DIR}")
    set(_PULP_FORMAT_SOURCE_DIR "${PULP_FORMAT_SOURCE_DIR}")
else()
    set(_PULP_FORMAT_SOURCE_DIR "${CMAKE_SOURCE_DIR}/core/format/src")
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
else()
    set(_PULP_AUV3_TEMPLATE_DIR "${CMAKE_SOURCE_DIR}/tools/templates/auv3")
endif()

if(DEFINED PULP_IOS_AUV3_TEMPLATE_DIR AND EXISTS "${PULP_IOS_AUV3_TEMPLATE_DIR}")
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "${PULP_IOS_AUV3_TEMPLATE_DIR}")
else()
    set(_PULP_IOS_AUV3_TEMPLATE_DIR "${CMAKE_SOURCE_DIR}/templates/ios-auv3")
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
        "PLUGIN_NAME;BUNDLE_ID;VERSION;MANUFACTURER;CATEGORY;PLUGIN_CODE;MANUFACTURER_CODE;AAX_PRODUCT_CODE;AAX_NATIVE_CODE;PROCESSOR_FACTORY;UI_SCRIPT"
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
function(_pulp_add_standalone target name bundle_id version)
    if(NOT _PULP_STANDALONE_TARGET)
        message(FATAL_ERROR "pulp_add_plugin(${target}): Standalone requested but Pulp::standalone is unavailable")
    endif()

    # Find standalone entry (convention: main.cpp in source dir)
    set(standalone_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
        set(standalone_entry "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
    endif()

    if(APPLE)
        add_executable(${target}_Standalone MACOSX_BUNDLE
            ${PULP_${target}_CORE_OBJECTS}
            ${standalone_entry}
        )
    else()
        add_executable(${target}_Standalone
            ${PULP_${target}_CORE_OBJECTS}
            ${standalone_entry}
        )
    endif()
    target_link_libraries(${target}_Standalone PRIVATE
        ${target}_Core
        ${_PULP_STANDALONE_TARGET}
    )
    _pulp_apply_ui_script_definition(${target}_Standalone "${PULP_${target}_UI_SCRIPT}")
    target_include_directories(${target}_Standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target}_Standalone PROPERTIES
        OUTPUT_NAME "${name}"
    )
    if(APPLE)
        set_target_properties(${target}_Standalone PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_BUNDLE_NAME "${name}"
            MACOSX_BUNDLE_GUI_IDENTIFIER "${bundle_id}"
            MACOSX_BUNDLE_BUNDLE_VERSION "${version}"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${version}"
        )
    endif()
    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target}_Standalone)
    endif()
    # Linux+GNU-ld link-order fix: libskia.a → fontconfig. Same helper
    # used for pulp-cli (#1986) and pulp-import-design (#2018). Standalone
    # transitively pulls in pulp::view → pulp::canvas → libskia.a, which
    # references Fc* symbols. Re-mention fontconfig AFTER the archive so
    # the linker resolves them. No-op on macOS/Windows/Android.
    include(${CMAKE_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake)
    pulp_link_fontconfig_after_skia(${target}_Standalone)
endfunction()

# ── pulp_add_app ────────────────────────────────────────────────────────
# Usage:
#   pulp_add_app(MyApp
#       APP_NAME "My App"
#       BUNDLE_ID "com.mycompany.myapp"
#       VERSION "1.0.0"
#   )
function(pulp_add_app target)
    cmake_parse_arguments(APP
        ""
        "APP_NAME;BUNDLE_ID;VERSION"
        ""
        ${ARGN}
    )

    if(APPLE)
        add_executable(${target} MACOSX_BUNDLE)
    else()
        add_executable(${target})
    endif()

    if(APP_APP_NAME)
        target_compile_definitions(${target} PRIVATE
            PULP_APP_NAME="${APP_APP_NAME}"
        )
    endif()

    if(APP_BUNDLE_ID)
        target_compile_definitions(${target} PRIVATE
            PULP_BUNDLE_ID="${APP_BUNDLE_ID}"
        )
    endif()

    message(STATUS "Pulp app: ${target}")
endfunction()

# pulp_add_binary_data() lives in PulpEmbedData.cmake so callers (e.g.
# core/canvas — bundled-font registration, #932) can include it before the
# Pulp targets exist. The function definition is unchanged; this PulpUtils
# include() preserves the existing public surface of `include(PulpUtils)`.
include("${CMAKE_CURRENT_LIST_DIR}/PulpEmbedData.cmake")

# pulp_register_font() — public font-registration macro for plugin authors
# (pulp #1150). Lives in its own file so SDK consumers who only want
# pulp_add_binary_data don't pay for the extra includes/parsing, and so the
# install layout can ship the font macro alongside the rest of the Pulp
# CMake helpers.
include("${CMAKE_CURRENT_LIST_DIR}/PulpFonts.cmake")
