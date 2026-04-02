# PulpPlugin.cmake — pulp_add_plugin() for standalone SDK projects
#
# This is the standalone-project version of the plugin macro.
# It uses imported Pulp:: targets from find_package(Pulp) instead of
# the in-tree pulp:: aliases used by PulpUtils.cmake.
#
# Usage (in a standalone project's CMakeLists.txt):
#   find_package(Pulp REQUIRED)
#   pulp_add_plugin(MyPlugin
#       FORMATS VST3 CLAP Standalone
#       PLUGIN_NAME "MyPlugin"
#       BUNDLE_ID "com.example.myplugin"
#       MANUFACTURER "Example"
#       PLUGIN_CODE "MyPl"
#       MANUFACTURER_CODE "Exmp"
#   )

function(pulp_add_plugin target)
    cmake_parse_arguments(PLUGIN
        ""
        "PLUGIN_NAME;BUNDLE_ID;VERSION;MANUFACTURER;CATEGORY;PLUGIN_CODE;MANUFACTURER_CODE;PROCESSOR_FACTORY"
        "FORMATS;SOURCES"
        ${ARGN}
    )

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

    # Core library from processor sources
    if(PLUGIN_SOURCES)
        add_library(${target}_Core OBJECT ${PLUGIN_SOURCES})
        target_link_libraries(${target}_Core PUBLIC Pulp::format)
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE Pulp::format)
        target_include_directories(${target}_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    endif()

    if(PLUGIN_SOURCES)
        target_compile_definitions(${target}_Core PRIVATE
            PULP_PLUGIN_NAME="${PLUGIN_PLUGIN_NAME}"
            PULP_BUNDLE_ID="${PLUGIN_BUNDLE_ID}"
            PULP_PLUGIN_VERSION="${PLUGIN_VERSION}"
        )
    endif()

    # CLAP format
    if("CLAP" IN_LIST PLUGIN_FORMATS)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/clap_entry.cpp")
            add_library(${target}_CLAP MODULE clap_entry.cpp)
            target_link_libraries(${target}_CLAP PRIVATE ${target}_Core Pulp::format)
            target_include_directories(${target}_CLAP PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            if(DEFINED PULP_CLAP_INCLUDE_DIR AND EXISTS "${PULP_CLAP_INCLUDE_DIR}")
                target_include_directories(${target}_CLAP PRIVATE "${PULP_CLAP_INCLUDE_DIR}")
            endif()
            set_target_properties(${target}_CLAP PROPERTIES
                OUTPUT_NAME "${PLUGIN_PLUGIN_NAME}"
                SUFFIX ".clap"
                PREFIX ""
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/CLAP"
            )
            if(APPLE)
                set_target_properties(${target}_CLAP PROPERTIES
                    BUNDLE TRUE
                    BUNDLE_EXTENSION "clap"
                )
            endif()
        endif()
    endif()

    # VST3 format
    if("VST3" IN_LIST PLUGIN_FORMATS)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vst3_entry.cpp")
            add_library(${target}_VST3 MODULE vst3_entry.cpp)
            target_link_libraries(${target}_VST3 PRIVATE
                ${target}_Core
                Pulp::format
                Pulp::vst3-sdk
            )
            target_include_directories(${target}_VST3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            if(DEFINED PULP_VST3_INCLUDE_DIR AND EXISTS "${PULP_VST3_INCLUDE_DIR}")
                target_include_directories(${target}_VST3 PRIVATE "${PULP_VST3_INCLUDE_DIR}")
            endif()
            set_target_properties(${target}_VST3 PROPERTIES
                OUTPUT_NAME "${PLUGIN_PLUGIN_NAME}"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/VST3"
            )
            if(APPLE)
                set_target_properties(${target}_VST3 PROPERTIES
                    BUNDLE TRUE
                    BUNDLE_EXTENSION "vst3"
                )
            endif()
        endif()
    endif()

    # AU v2 format
    if("AU" IN_LIST PLUGIN_FORMATS AND APPLE)
        if(NOT PLUGIN_PLUGIN_CODE OR NOT PLUGIN_MANUFACTURER_CODE)
            message(WARNING "pulp_add_plugin(${target}): AU format requires PLUGIN_CODE and MANUFACTURER_CODE")
        elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/au_v2_entry.cpp")
            if(NOT DEFINED PULP_SOURCE_INCLUDE_DIR OR NOT EXISTS "${PULP_SOURCE_INCLUDE_DIR}/pulp/format")
                message(FATAL_ERROR "pulp_add_plugin(${target}): missing installed Pulp source tree under ${PULP_SOURCE_INCLUDE_DIR}")
            endif()

            add_library(${target}_AU MODULE au_v2_entry.cpp)
            target_link_libraries(${target}_AU PRIVATE
                ${target}_Core
                Pulp::format
                Pulp::ausdk
                "-framework AudioToolbox"
                "-framework CoreFoundation"
                "-framework CoreAudio"
            )
            target_include_directories(${target}_AU PRIVATE
                ${CMAKE_CURRENT_SOURCE_DIR}
                "${PULP_SOURCE_INCLUDE_DIR}"
            )
            if(DEFINED PULP_AUSDK_INCLUDE_DIR AND EXISTS "${PULP_AUSDK_INCLUDE_DIR}")
                target_include_directories(${target}_AU PRIVATE "${PULP_AUSDK_INCLUDE_DIR}")
            endif()
            set_target_properties(${target}_AU PROPERTIES
                BUNDLE TRUE
                BUNDLE_EXTENSION "component"
                OUTPUT_NAME "${PLUGIN_PLUGIN_NAME}"
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AU"
            )

            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.au")
                set_target_properties(${target}_AU PROPERTIES
                    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.au")
            elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/PulpInfoPlist.au.in")
                set(PULP_PLUGIN_NAME "${PLUGIN_PLUGIN_NAME}")
                set(PULP_BUNDLE_ID "${PLUGIN_BUNDLE_ID}")
                set(PULP_VERSION "${PLUGIN_VERSION}")
                set(PULP_MANUFACTURER "${PLUGIN_MANUFACTURER}")
                set(PULP_MANUFACTURER_CODE "${PLUGIN_MANUFACTURER_CODE}")
                set(PULP_PLUGIN_CODE "${PLUGIN_PLUGIN_CODE}")
                if("${PLUGIN_CATEGORY}" STREQUAL "Instrument")
                    set(PULP_AU_TYPE "aumu")
                elseif("${PLUGIN_CATEGORY}" STREQUAL "MidiEffect")
                    set(PULP_AU_TYPE "aumi")
                else()
                    set(PULP_AU_TYPE "aufx")
                endif()
                set(PULP_AU_FACTORY_NAME "${target}AUFactory")
                configure_file(
                    "${CMAKE_CURRENT_LIST_DIR}/PulpInfoPlist.au.in"
                    "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.au"
                    @ONLY)
                set_target_properties(${target}_AU PROPERTIES
                    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${target}_Info.plist.au")
            endif()

            add_custom_command(TARGET ${target}_AU POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E echo "BNDL????" >
                    "$<TARGET_BUNDLE_DIR:${target}_AU>/Contents/PkgInfo"
                COMMENT "Writing PkgInfo into ${PLUGIN_PLUGIN_NAME}.component bundle"
            )
        endif()
    endif()

    # Standalone
    if("Standalone" IN_LIST PLUGIN_FORMATS)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
            add_executable(${target}_Standalone main.cpp)
            target_link_libraries(${target}_Standalone PRIVATE ${target}_Core Pulp::standalone)
            target_include_directories(${target}_Standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            set_target_properties(${target}_Standalone PROPERTIES
                OUTPUT_NAME "${PLUGIN_PLUGIN_NAME}"
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            )
        endif()
    endif()

    # Test target
    # Standalone templates already define ${target}-test explicitly with
    # Catch2 discovery. Keep this fallback only for hand-written SDK projects
    # that do not create their own test target.
    set(_test_file "test_${target}.cpp")
    string(TOLOWER "${_test_file}" _test_file)
    if(NOT TARGET ${target}-test AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_test_file}")
        add_executable(${target}-test "${_test_file}")
        target_link_libraries(${target}-test PRIVATE ${target}_Core Pulp::format)
        target_include_directories(${target}-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        if(TARGET Catch2::Catch2WithMain)
            target_link_libraries(${target}-test PRIVATE Catch2::Catch2WithMain)
        endif()
        add_test(NAME ${target} COMMAND ${target}-test)
    endif()

    message(STATUS "Pulp plugin: ${target} (formats: ${PLUGIN_FORMATS})")
endfunction()
