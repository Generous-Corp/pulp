# pulp-mcp — MCP server for Pulp
#
# Generates pulp_mcp_version.h from project(VERSION ...) so the MCP
# server's serverInfo.version reflects the actual SDK/CLI release. Pre
# #2067 this was hard-coded to "0.1.0" and never moved.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/pulp_mcp_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pulp_mcp_version.h"
    @ONLY)

if(PULP_ENABLE_PROJECT_PACKAGE)
    file(READ
        "${PROJECT_SOURCE_DIR}/core/timeline/schema/timeline_mcp_tools.json"
        PULP_TIMELINE_MCP_TOOLS_JSON)
    string(JSON PULP_TIMELINE_MCP_TOOL_COUNT
        LENGTH "${PULP_TIMELINE_MCP_TOOLS_JSON}" tools)
    if(NOT PULP_TIMELINE_MCP_TOOL_COUNT EQUAL 10)
        message(FATAL_ERROR
            "timeline_mcp_tools.json must define exactly ten generated operations")
    endif()
    set(_PULP_TIMELINE_MCP_EXPECTED_NAMES
        pulp_timeline_project_open
        pulp_timeline_command_apply
        pulp_timeline_diff
        pulp_timeline_undo
        pulp_timeline_redo
        pulp_timeline_validate
        pulp_timeline_explain
        pulp_timeline_render)
    list(APPEND _PULP_TIMELINE_MCP_EXPECTED_NAMES
        pulp_timeline_export
        pulp_timeline_import)
    set(PULP_TIMELINE_MCP_TOOLS_ARRAY "")
    foreach(_index RANGE 0 9)
        string(JSON _tool_name GET "${PULP_TIMELINE_MCP_TOOLS_JSON}" tools ${_index} name)
        list(GET _PULP_TIMELINE_MCP_EXPECTED_NAMES ${_index} _expected_name)
        if(NOT _tool_name STREQUAL _expected_name)
            message(FATAL_ERROR
                "timeline MCP operation ${_index} must be ${_expected_name}, got ${_tool_name}")
        endif()
        string(JSON _tool_json GET "${PULP_TIMELINE_MCP_TOOLS_JSON}" tools ${_index})
        if(_index GREATER 0)
            string(APPEND PULP_TIMELINE_MCP_TOOLS_ARRAY ",")
        endif()
        string(APPEND PULP_TIMELINE_MCP_TOOLS_ARRAY "${_tool_json}")
        set(PULP_TIMELINE_MCP_TOOL_NAME_${_index} "${_tool_name}")
    endforeach()
    unset(_PULP_TIMELINE_MCP_EXPECTED_NAMES)
    unset(_expected_name)
    unset(_index)
    unset(_tool_json)
    unset(_tool_name)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/core/timeline/schema/timeline_mcp_tools.json")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/timeline_mcp_tools.h.in"
        "${CMAKE_CURRENT_BINARY_DIR}/timeline_mcp_tools.h"
        @ONLY)
endif()

add_library(pulp-mcp-core STATIC
    pulp_mcp.cpp
    mcp_compat.cpp
    mcp_tools.cpp
    mcp_tools_internal.cpp
    mcp_kit_content_tools.cpp
    mcp_audio_tools.cpp
    mcp_inspect_tools.cpp)
target_compile_features(pulp-mcp-core PRIVATE cxx_std_20)
target_include_directories(pulp-mcp-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(pulp-mcp-core
    PUBLIC
        pulp::tool-audio
    PRIVATE
        pulp::platform)

if(PULP_ENABLE_PROJECT_PACKAGE)
    target_sources(pulp-mcp-core PRIVATE
        mcp_timeline_tools.cpp
        timeline_session_store.cpp)
    target_compile_definitions(pulp-mcp-core PUBLIC
        PULP_MCP_ENABLE_TIMELINE_TOOLS=1)
    target_include_directories(pulp-mcp-core PRIVATE
        "${PROJECT_SOURCE_DIR}/core/timeline/src")
    target_link_libraries(pulp-mcp-core
        PUBLIC
            pulp::tool-timeline
        PRIVATE
            pulp::timebase
            pulp::timeline)
else()
    target_compile_definitions(pulp-mcp-core PUBLIC
        PULP_MCP_ENABLE_TIMELINE_TOOLS=0)
endif()

if(TARGET pulp::inspect-protocol)
    target_link_libraries(pulp-mcp-core PRIVATE pulp::inspect-protocol)
    target_link_libraries(pulp-mcp-core PRIVATE pulp::inspect-client)
    target_compile_definitions(pulp-mcp-core PUBLIC
        PULP_MCP_ENABLE_INSPECTOR_CLIENT=1)
else()
    target_sources(pulp-mcp-core PRIVATE
        "${PROJECT_SOURCE_DIR}/inspect/src/capabilities.cpp")
    target_compile_definitions(pulp-mcp-core PUBLIC
        PULP_MCP_ENABLE_INSPECTOR_CLIENT=0)
endif()

add_executable(pulp-mcp pulp_mcp_main.cpp)
set_target_properties(pulp-mcp PROPERTIES OUTPUT_NAME "pulp-mcp")
target_compile_features(pulp-mcp PRIVATE cxx_std_20)
target_link_libraries(pulp-mcp PRIVATE pulp-mcp-core)

# The agent-request queue is a light, choc-only unit that lives in inspect/. The
# MCP server reads it in-process, so compile that translation unit into
# pulp-mcp. Capability metadata comes from the CPU-only inspect-protocol target
# when enabled, or its single registry translation unit in inspector-off builds;
# do not link all of pulp::inspect, which would drag in the GPU/Skia overlay.
target_sources(pulp-mcp-core PRIVATE
    "${PROJECT_SOURCE_DIR}/inspect/src/agent_request_queue.cpp")
target_include_directories(pulp-mcp-core PRIVATE
    "${PROJECT_SOURCE_DIR}/inspect/include")

# Ship pulp-mcp via `cmake --install` too — same lane the SDK tarball
# uses to deliver pulp-scan-worker. The release-CLI tarball still gets
# it via tools/scripts/package_cli.py --mcp-binary; this install rule
# means source builds + the SDK tarball pick it up consistently.
install(TARGETS pulp-mcp
    RUNTIME DESTINATION bin
)
