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
            target_link_libraries(${target}_VST3 PRIVATE ${target}_Core Pulp::format)
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

    # Standalone
    if("Standalone" IN_LIST PLUGIN_FORMATS)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp")
            add_executable(${target}_Standalone main.cpp)
            target_link_libraries(${target}_Standalone PRIVATE ${target}_Core Pulp::format)
            target_include_directories(${target}_Standalone PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
            set_target_properties(${target}_Standalone PROPERTIES
                OUTPUT_NAME "${PLUGIN_PLUGIN_NAME}"
                RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            )
        endif()
    endif()

    # Test target
    set(_test_file "test_${target}.cpp")
    string(TOLOWER "${_test_file}" _test_file)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_test_file}")
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
