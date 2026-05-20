# pulp-mcp — MCP server for Pulp
#
# Generates pulp_mcp_version.h from project(VERSION ...) so the MCP
# server's serverInfo.version reflects the actual SDK/CLI release. Pre
# #2067 this was hard-coded to "0.1.0" and never moved.
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/pulp_mcp_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/pulp_mcp_version.h"
    @ONLY)

add_executable(pulp-mcp
    pulp_mcp.cpp
    mcp_compat.cpp)
set_target_properties(pulp-mcp PROPERTIES OUTPUT_NAME "pulp-mcp")
target_compile_features(pulp-mcp PRIVATE cxx_std_20)
target_include_directories(pulp-mcp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(pulp-mcp PRIVATE pulp::tool-audio)

# Ship pulp-mcp via `cmake --install` too — same lane the SDK tarball
# uses to deliver pulp-scan-worker. The release-CLI tarball still gets
# it via tools/scripts/package_cli.py --mcp-binary; this install rule
# means source builds + the SDK tarball pick it up consistently.
install(TARGETS pulp-mcp
    RUNTIME DESTINATION bin
)
