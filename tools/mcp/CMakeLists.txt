# pulp-mcp — MCP server for Pulp
#
# Generates pulp_mcp_version.h from project(VERSION ...) so the MCP
# server's serverInfo.version reflects the actual SDK/CLI release. Pre
# #2067 this was hard-coded to "0.1.0" and never moved.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/pulp_mcp_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pulp_mcp_version.h"
    @ONLY)

file(READ
    "${PROJECT_SOURCE_DIR}/core/timeline/schema/timeline_mcp_tools.json"
    PULP_TIMELINE_MCP_TOOLS_JSON)
string(JSON PULP_TIMELINE_MCP_TOOL_COUNT
    LENGTH "${PULP_TIMELINE_MCP_TOOLS_JSON}" tools)
if(NOT PULP_TIMELINE_MCP_TOOL_COUNT EQUAL 5)
    message(FATAL_ERROR
        "timeline_mcp_tools.json must define exactly five generated operations")
endif()
set(_PULP_TIMELINE_MCP_EXPECTED_NAMES
    pulp_timeline_project_open
    pulp_timeline_command_apply
    pulp_timeline_validate
    pulp_timeline_explain
    pulp_timeline_render)
set(PULP_TIMELINE_MCP_TOOLS_ARRAY "")
foreach(_index RANGE 0 4)
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

add_library(pulp-mcp-core STATIC
    pulp_mcp.cpp
    mcp_compat.cpp
    mcp_tools.cpp
    mcp_tools_internal.cpp
    mcp_kit_content_tools.cpp
    mcp_audio_tools.cpp
    mcp_inspect_tools.cpp
    mcp_timeline_tools.cpp)
target_compile_features(pulp-mcp-core PRIVATE cxx_std_20)
target_include_directories(pulp-mcp-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(pulp-mcp-core
    PUBLIC
        pulp::tool-audio
        pulp::tool-timeline
    PRIVATE
        pulp::timebase
        pulp::timeline)

add_executable(pulp-mcp pulp_mcp_main.cpp)
set_target_properties(pulp-mcp PROPERTIES OUTPUT_NAME "pulp-mcp")
target_compile_features(pulp-mcp PRIVATE cxx_std_20)
target_link_libraries(pulp-mcp PRIVATE pulp-mcp-core)

# The agent-request queue is a light, choc-only unit that lives in inspect/. The
# MCP server reads it in-process, so compile just that translation unit into
# pulp-mcp rather than linking all of pulp::inspect: pulp::inspect PUBLIC-links
# pulp::view / pulp::render, which would drag the whole GPU/Skia chain into the
# MCP server just to read a JSON queue file.
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
