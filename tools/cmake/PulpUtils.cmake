# PulpUtils.cmake — Build utilities for Pulp projects
#
# Provides:
#   pulp_add_plugin()  — Create a plugin target with format adapters
#   pulp_add_app()     — Create a standalone application target

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
        ""
        "PLUGIN_NAME;BUNDLE_ID;VERSION;MANUFACTURER;CATEGORY;PLUGIN_CODE;MANUFACTURER_CODE;PROCESSOR_FACTORY"
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

    # ── Core object library ──────────────────────────────────────────────
    # Shared processor code compiled once, linked into each format target
    add_library(${target}_Core OBJECT ${PLUGIN_SOURCES})
    target_link_libraries(${target}_Core PUBLIC pulp::format)
    target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target}_Core PRIVATE
        PULP_PLUGIN_NAME="${PLUGIN_PLUGIN_NAME}"
        PULP_BUNDLE_ID="${PLUGIN_BUNDLE_ID}"
        PULP_PLUGIN_VERSION="${PLUGIN_VERSION}"
    )

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
                          "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}")
        endif()
    endif()

    # ── Standalone ───────────────────────────────────────────────────────
    if("Standalone" IN_LIST PLUGIN_FORMATS)
        _pulp_add_standalone(${target} "${PLUGIN_PLUGIN_NAME}")
    endif()

    message(STATUS "Pulp plugin: ${target} (formats: ${PLUGIN_FORMATS})")
endfunction()

# ── Internal: VST3 target ────────────────────────────────────────────────
function(_pulp_add_vst3 target name bundle_id version manufacturer category)
    # Platform entry point sources
    set(vst3_platform_src "")
    if(APPLE)
        list(APPEND vst3_platform_src
            ${VST3_SDK_DIR}/public.sdk/source/main/macmain.cpp)
    elseif(WIN32)
        list(APPEND vst3_platform_src
            ${VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp)
    elseif(UNIX)
        list(APPEND vst3_platform_src
            ${VST3_SDK_DIR}/public.sdk/source/main/linuxmain.cpp)
    endif()

    # Find VST3 entry point (convention: vst3_entry.cpp in source dir)
    set(vst3_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst3_entry.cpp")
        set(vst3_entry "${CMAKE_CURRENT_SOURCE_DIR}/vst3_entry.cpp")
    endif()

    add_library(${target}_VST3 MODULE
        $<TARGET_OBJECTS:${target}_Core>
        ${vst3_entry}
        ${vst3_platform_src}
        ${VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp
        ${VST3_SDK_DIR}/public.sdk/source/main/moduleinit.cpp
    )
    target_link_libraries(${target}_VST3 PRIVATE pulp::format vst3-sdk)
    target_include_directories(${target}_VST3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # VST3 bundle structure
    set_target_properties(${target}_VST3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "vst3"
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3"
    )

    # Info.plist: use custom if available, otherwise generate from template
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.vst3")
        set_target_properties(${target}_VST3 PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.vst3")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/tools/cmake/PulpInfoPlist.vst3.in")
        set(PULP_PLUGIN_NAME "${name}")
        set(PULP_BUNDLE_ID "${bundle_id}")
        set(PULP_VERSION "${version}")
        configure_file(
            "${CMAKE_SOURCE_DIR}/tools/cmake/PulpInfoPlist.vst3.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.vst3"
            @ONLY)
        set_target_properties(${target}_VST3 PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.vst3")
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
endfunction()

# ── Internal: CLAP target ───────────────────────────────────────────────
function(_pulp_add_clap target name bundle_id version manufacturer category)
    # Find CLAP entry point (convention: clap_entry.cpp in source dir)
    set(clap_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/clap_entry.cpp")
        set(clap_entry "${CMAKE_CURRENT_SOURCE_DIR}/clap_entry.cpp")
    endif()

    add_library(${target}_CLAP MODULE
        $<TARGET_OBJECTS:${target}_Core>
        ${clap_entry}
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(${target}_CLAP PRIVATE pulp::format clap)
    target_include_directories(${target}_CLAP PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(${target}_CLAP PROPERTIES
        OUTPUT_NAME "${name}"
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
endfunction()

# ── Internal: AU v2 target ──────────────────────────────────────────────
function(_pulp_add_au target name bundle_id version manufacturer category plugin_code manufacturer_code)
    # Find AU entry point (convention: au_v2_entry.cpp in source dir)
    set(au_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/au_v2_entry.cpp")
        set(au_entry "${CMAKE_CURRENT_SOURCE_DIR}/au_v2_entry.cpp")
    endif()

    add_library(${target}_AU MODULE
        $<TARGET_OBJECTS:${target}_Core>
        ${au_entry}
    )
    target_link_libraries(${target}_AU PRIVATE
        pulp::format
        ausdk
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
    )
    target_include_directories(${target}_AU PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    set_target_properties(${target}_AU PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "component"
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AU"
    )

    # Info.plist: use custom if available, otherwise generate from template
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.au")
        set_target_properties(${target}_AU PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.au")
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/tools/cmake/PulpInfoPlist.au.in")
        set(PULP_PLUGIN_NAME "${name}")
        set(PULP_BUNDLE_ID "${bundle_id}")
        set(PULP_VERSION "${version}")
        set(PULP_MANUFACTURER "${manufacturer}")
        set(PULP_MANUFACTURER_CODE "${manufacturer_code}")
        set(PULP_PLUGIN_CODE "${plugin_code}")
        # Determine AU type from category
        if("${category}" STREQUAL "Instrument")
            set(PULP_AU_TYPE "aumu")
        elseif("${category}" STREQUAL "MidiEffect")
            set(PULP_AU_TYPE "aumi")
        else()
            set(PULP_AU_TYPE "aufx")
        endif()
        # Factory function name follows the AUSDK_COMPONENT_ENTRY convention
        set(PULP_AU_FACTORY_NAME "${target}AUFactory")
        configure_file(
            "${CMAKE_SOURCE_DIR}/tools/cmake/PulpInfoPlist.au.in"
            "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.au"
            @ONLY)
        set_target_properties(${target}_AU PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.au")
    endif()
endfunction()

# ── Internal: Standalone target ─────────────────────────────────────────
function(_pulp_add_standalone target name)
    # Find standalone entry (convention: main.cpp in source dir)
    set(standalone_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
        set(standalone_entry "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
    endif()

    add_executable(${target}_Standalone
        $<TARGET_OBJECTS:${target}_Core>
        ${standalone_entry}
        ${CMAKE_SOURCE_DIR}/core/format/src/standalone.cpp
    )
    target_link_libraries(${target}_Standalone PRIVATE pulp::format pulp::audio pulp::midi)
    target_include_directories(${target}_Standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    set_target_properties(${target}_Standalone PROPERTIES
        OUTPUT_NAME "${name}"
    )
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

    add_executable(${target})

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

# ── pulp_add_binary_data ────────────────────────────────────────────────
# Embed binary assets as C++ arrays (images, fonts, etc.)
function(pulp_add_binary_data target)
    cmake_parse_arguments(DATA "" "" "SOURCES" ${ARGN})
    message(STATUS "Pulp binary data: ${target} (${DATA_SOURCES})")
endfunction()
