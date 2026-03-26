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

    # ── Core library ────────────────────────────────────────────────────
    # For header-only processors (no SOURCES), create INTERFACE library.
    # For compiled processors, create OBJECT library.
    if(PLUGIN_SOURCES)
        add_library(${target}_Core OBJECT ${PLUGIN_SOURCES})
        target_link_libraries(${target}_Core PUBLIC pulp::format)
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        set(_PULP_CORE_OBJECTS "${PULP_${target}_CORE_OBJECTS}")
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE pulp::format)
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
                          "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}")
        endif()
    endif()

    # ── LV2 ───────────────────────────────────────────────────────────────
    if("LV2" IN_LIST PLUGIN_FORMATS AND PULP_HAS_LV2)
        _pulp_add_lv2(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                       "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}" "${PLUGIN_CATEGORY}")
    endif()

    # ── AUv3 ──────────────────────────────────────────────────────────────
    if("AUv3" IN_LIST PLUGIN_FORMATS AND APPLE)
        if(NOT PLUGIN_PLUGIN_CODE OR NOT PLUGIN_MANUFACTURER_CODE)
            message(WARNING "pulp_add_plugin(${target}): AUv3 format requires PLUGIN_CODE and MANUFACTURER_CODE")
        else()
            _pulp_add_auv3(${target} "${PLUGIN_PLUGIN_NAME}" "${PLUGIN_BUNDLE_ID}"
                           "${PLUGIN_VERSION}" "${PLUGIN_MANUFACTURER}"
                           "${PLUGIN_CATEGORY}" "${PLUGIN_PLUGIN_CODE}" "${PLUGIN_MANUFACTURER_CODE}")
        endif()
    endif()

    # ── Standalone ───────────────────────────────────────────────────────
    if("Standalone" IN_LIST PLUGIN_FORMATS)
        _pulp_add_standalone(${target} "${PLUGIN_PLUGIN_NAME}")
    endif()

    # ── Install targets ────────────────────────────────────────────────
    # Platform-appropriate install locations for each format
    if(APPLE)
        set(_vst3_dir "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
        set(_clap_dir "$ENV{HOME}/Library/Audio/Plug-Ins/CLAP")
        set(_au_dir "$ENV{HOME}/Library/Audio/Plug-Ins/Components")
    elseif(WIN32)
        set(_vst3_dir "$ENV{COMMONPROGRAMFILES}/VST3")
        set(_clap_dir "$ENV{COMMONPROGRAMFILES}/CLAP")
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

    if(_install_commands)
        add_custom_target(pulp-install-${target}
            ${_install_commands}
            COMMENT "Installing ${PLUGIN_PLUGIN_NAME} to system plugin folders"
        )
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
        ${PULP_${target}_CORE_OBJECTS}
        ${vst3_entry}
        ${vst3_platform_src}
        ${CMAKE_SOURCE_DIR}/core/format/src/vst3_plug_view.cpp
        ${VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp
        ${VST3_SDK_DIR}/public.sdk/source/main/moduleinit.cpp
    )
    target_link_libraries(${target}_VST3 PRIVATE ${target}_Core pulp::format pulp::view vst3-sdk)
    target_compile_definitions(${target}_VST3 PRIVATE PULP_VST3_GUI=1)
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
endfunction()

# ── Internal: CLAP target ───────────────────────────────────────────────
function(_pulp_add_clap target name bundle_id version manufacturer category)
    # Find CLAP entry point (convention: clap_entry.cpp in source dir)
    set(clap_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/clap_entry.cpp")
        set(clap_entry "${CMAKE_CURRENT_SOURCE_DIR}/clap_entry.cpp")
    endif()

    add_library(${target}_CLAP MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${clap_entry}
        ${CMAKE_SOURCE_DIR}/core/format/src/clap_adapter.cpp
    )
    target_link_libraries(${target}_CLAP PRIVATE ${target}_Core pulp::format pulp::view clap)
    target_compile_definitions(${target}_CLAP PRIVATE PULP_CLAP_GUI=1)
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

# ── Internal: LV2 target ───────────────────────────────────────────────
function(_pulp_add_lv2 target name bundle_id version manufacturer category)
    # Find LV2 entry point (convention: lv2_entry.cpp in source dir)
    set(lv2_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/lv2_entry.cpp")
        set(lv2_entry "${CMAKE_CURRENT_SOURCE_DIR}/lv2_entry.cpp")
    endif()

    add_library(${target}_LV2 MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${lv2_entry}
        ${CMAKE_SOURCE_DIR}/core/format/src/lv2_adapter.cpp
    )
    target_link_libraries(${target}_LV2 PRIVATE ${target}_Core pulp::format lv2-headers)
    target_include_directories(${target}_LV2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    # LV2 bundles: <name>.lv2/ directory containing the .so and .ttl files
    set_target_properties(${target}_LV2 PROPERTIES
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/LV2/${name}.lv2"
        PREFIX ""
    )
endfunction()

# ── Internal: AU v2 target ──────────────────────────────────────────────
function(_pulp_add_au target name bundle_id version manufacturer category plugin_code manufacturer_code)
    # Find AU entry point (convention: au_v2_entry.cpp in source dir)
    set(au_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/au_v2_entry.cpp")
        set(au_entry "${CMAKE_CURRENT_SOURCE_DIR}/au_v2_entry.cpp")
    endif()

    add_library(${target}_AU MODULE
        ${PULP_${target}_CORE_OBJECTS}
        ${au_entry}
        ${CMAKE_SOURCE_DIR}/core/format/src/au_v2_cocoa_view.mm
    )
    target_link_libraries(${target}_AU PRIVATE
        ${target}_Core
        pulp::format
        pulp::view
        ausdk
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework CoreAudio"
        "-framework Cocoa"
    )
    target_compile_definitions(${target}_AU PRIVATE PULP_AU_GUI=1)
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

    # Write PkgInfo so Finder treats the .component as an opaque bundle (not a folder)
    add_custom_command(TARGET ${target}_AU POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E echo "BNDL????" >
            "$<TARGET_BUNDLE_DIR:${target}_AU>/Contents/PkgInfo"
        COMMENT "Writing PkgInfo into ${name}.component bundle"
    )
endfunction()

# ── Internal: AUv3 app extension target ────────────────────────────────
function(_pulp_add_auv3 target name bundle_id version manufacturer category plugin_code manufacturer_code)
    if(NOT APPLE)
        return()
    endif()

    # Map category to AU type and tag
    if(category STREQUAL "Instrument")
        set(au_type "aumu")
        set(au_tag "Synthesizer")
    elseif(category STREQUAL "MidiEffect")
        set(au_type "aumi")
        set(au_tag "MIDI")
    else()
        set(au_type "aufx")
        set(au_tag "Effects")
    endif()

    # AUv3 sources — shared adapter plus platform-specific view controller
    set(auv3_sources
        ${CMAKE_SOURCE_DIR}/core/format/src/au_adapter.mm
        ${CMAKE_SOURCE_DIR}/core/format/src/au_entry.mm
    )
    if(PULP_IOS)
        list(APPEND auv3_sources
            ${CMAKE_SOURCE_DIR}/core/format/src/au_view_controller_ios.mm
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
        ${target}_Core pulp::format pulp::view
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

    if(PULP_IOS AND EXISTS "${CMAKE_SOURCE_DIR}/templates/ios-auv3/Info.plist.in")
        configure_file(
            "${CMAKE_SOURCE_DIR}/templates/ios-auv3/Info.plist.in"
            "${CMAKE_BINARY_DIR}/AUv3/${target}.appex/Info.plist"
            @ONLY
        )
        set_target_properties(${target}_AUv3 PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/AUv3/${target}.appex/Info.plist"
        )
    elseif(EXISTS "${CMAKE_SOURCE_DIR}/tools/templates/auv3/Info.plist.template")
        configure_file(
            "${CMAKE_SOURCE_DIR}/tools/templates/auv3/Info.plist.template"
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
endfunction()

# ── Internal: Standalone target ─────────────────────────────────────────
function(_pulp_add_standalone target name)
    # Find standalone entry (convention: main.cpp in source dir)
    set(standalone_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
        set(standalone_entry "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
    endif()

    add_executable(${target}_Standalone
        ${PULP_${target}_CORE_OBJECTS}
        ${standalone_entry}
        ${CMAKE_SOURCE_DIR}/core/format/src/standalone.cpp
    )
    target_link_libraries(${target}_Standalone PRIVATE ${target}_Core pulp::format pulp::view pulp::audio pulp::midi)
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
# Embed binary assets as C++ arrays.
# Usage:
#   pulp_add_binary_data(MyResources
#       SOURCES logo.png preset.json font.ttf
#       NAMESPACE myresources
#   )
# Generates a header with:
#   namespace myresources { extern const unsigned char logo_png[]; extern const size_t logo_png_size; }
function(pulp_add_binary_data target)
    cmake_parse_arguments(DATA "" "NAMESPACE" "SOURCES" ${ARGN})

    if(NOT DATA_NAMESPACE)
        set(DATA_NAMESPACE "${target}")
    endif()

    set(generated_cpp "${CMAKE_CURRENT_BINARY_DIR}/${target}_data.cpp")
    set(generated_hpp "${CMAKE_CURRENT_BINARY_DIR}/${target}_data.hpp")

    # Generate the embedding source at configure time
    set(cpp_content "#include \"${target}_data.hpp\"\n\n")
    set(hpp_content "#pragma once\n#include <cstddef>\n\nnamespace ${DATA_NAMESPACE} {\n\n")

    foreach(source_file ${DATA_SOURCES})
        get_filename_component(file_name "${source_file}" NAME)
        string(MAKE_C_IDENTIFIER "${file_name}" var_name)
        get_filename_component(abs_path "${source_file}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

        # Read file as hex at configure time
        file(READ "${abs_path}" file_hex HEX)
        string(LENGTH "${file_hex}" hex_len)
        math(EXPR byte_count "${hex_len} / 2")

        # Convert hex pairs to C array initializer
        set(hex_list "")
        set(i 0)
        while(i LESS hex_len)
            string(SUBSTRING "${file_hex}" ${i} 2 hex_byte)
            string(APPEND hex_list "0x${hex_byte},")
            math(EXPR i "${i} + 2")
            math(EXPR mod "(${i} / 2) % 16")
            if(mod EQUAL 0)
                string(APPEND hex_list "\n    ")
            endif()
        endwhile()

        string(APPEND hpp_content "extern const unsigned char ${var_name}[];\n")
        string(APPEND hpp_content "extern const size_t ${var_name}_size;\n\n")

        string(APPEND cpp_content "namespace ${DATA_NAMESPACE} {\n")
        string(APPEND cpp_content "const unsigned char ${var_name}[] = {\n    ${hex_list}\n};\n")
        string(APPEND cpp_content "const size_t ${var_name}_size = ${byte_count};\n")
        string(APPEND cpp_content "} // namespace ${DATA_NAMESPACE}\n\n")
    endforeach()

    string(APPEND hpp_content "} // namespace ${DATA_NAMESPACE}\n")

    file(WRITE "${generated_cpp}" "${cpp_content}")
    file(WRITE "${generated_hpp}" "${hpp_content}")

    add_library(${target} STATIC "${generated_cpp}")
    target_include_directories(${target} PUBLIC "${CMAKE_CURRENT_BINARY_DIR}")

    message(STATUS "Pulp binary data: ${target} (${DATA_SOURCES})")
endfunction()
