cmake_minimum_required(VERSION 3.24)

# The macOS AUv3 container app embeds the framework and the .appex after it
# links. Its control-shipping scan reads every binary in the .app, so it must
# run after the embedding in both places it runs: the host's POST_BUILD list,
# and the persisted-stamp edge, which would otherwise sit beside the embed
# sentinel edge in the graph and read the bundle while it is rewritten.
# Configure-only: the Ninja graph is the evidence.

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()
if(NOT NINJA)
    # A fresh variable: NINJA arrives defined (empty) from -DNINJA=, and
    # find_program does not search into a variable that is already set.
    find_program(_pulp_scan_order_ninja NAMES ninja ninja-build)
    if(_pulp_scan_order_ninja)
        set(NINJA "${_pulp_scan_order_ninja}")
    endif()
endif()
if(NOT NINJA)
    # The check reads the Ninja graph, so it cannot run without ninja. That is
    # a skip, not a pass: say which tool is missing. A Ninja-configured build
    # passes its own CMAKE_MAKE_PROGRAM, so this only fires on another
    # generator with no ninja on PATH.
    message("SKIPPED: cmake-auv3-host-scan-order needs ninja to read the build graph, "
            "and no ninja was found (not passed in, not on PATH)")
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.29)
        cmake_language(EXIT 77)
    endif()
    return()
endif()

file(REMOVE_RECURSE "${FIXTURE_DIR}")
file(MAKE_DIRECTORY "${FIXTURE_DIR}/source")
file(WRITE "${FIXTURE_DIR}/source/main.cpp" "int main() { return 0; }\n")
file(WRITE "${FIXTURE_DIR}/source/framework.cpp" "int fixture_framework() { return 0; }\n")
file(WRITE "${FIXTURE_DIR}/source/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(Auv3HostScanOrder LANGUAGES CXX OBJCXX)

include("@PULP_SOURCE_DIR@/tools/cmake/PulpControlShipping.cmake")
include("@PULP_SOURCE_DIR@/tools/cmake/PulpAuv3.cmake")
set(_PULP_AUV3_TEMPLATE_DIR "@PULP_SOURCE_DIR@/tools/templates/auv3")

# The framework and the extension stand in for the real ones; the host, its
# embedding and its scan are the production helpers under test.
_pulp_cache_control_declarations(FixtureGain production-stripped "" FALSE)
_pulp_configure_control_shipping(FixtureGain com.pulp.test.fixturegain FixtureGain)
add_library(FixtureGain_AUv3Framework SHARED framework.cpp)
set_target_properties(FixtureGain_AUv3Framework PROPERTIES
    FRAMEWORK TRUE
    OUTPUT_NAME FixtureGainAUv3Framework
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AUv3")
add_executable(FixtureGain_AUv3 MACOSX_BUNDLE main.cpp)
set_target_properties(FixtureGain_AUv3 PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION appex
    OUTPUT_NAME FixtureGain
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AUv3")
_pulp_add_auv3_macos_host(FixtureGain FixtureGain com.pulp.test.fixturegain 1.0.0)
]=])

file(READ "${FIXTURE_DIR}/source/CMakeLists.txt" _fixture_cmake)
string(REPLACE "@PULP_SOURCE_DIR@" "${PULP_SOURCE_DIR}" _fixture_cmake "${_fixture_cmake}")
file(WRITE "${FIXTURE_DIR}/source/CMakeLists.txt" "${_fixture_cmake}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/source" -B "${FIXTURE_DIR}/build"
            -G Ninja "-DCMAKE_MAKE_PROGRAM=${NINJA}" -DCMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "AUv3 host scan-order fixture failed to configure: "
        "${_configure_output}${_configure_error}")
endif()

set(_build "${FIXTURE_DIR}/build")
set(_scan_stamp "pulp-control-shipping-stamps/FixtureGain_AUv3Host.AUV3HOST.Release.sidecars-v2.stamp")
set(_embed_stamp "AUv3/FixtureGain_embed.stamp")

# 1. The persisted-stamp scan edge takes the embed sentinel as an input.
execute_process(
    COMMAND "${NINJA}" -C "${_build}" -t query "${_scan_stamp}"
    RESULT_VARIABLE _query_result
    OUTPUT_VARIABLE _query
    ERROR_VARIABLE _query_error)
if(NOT _query_result EQUAL 0)
    message(FATAL_ERROR "ninja -t query ${_scan_stamp} failed: ${_query}${_query_error}")
endif()
string(FIND "${_query}" "  input:" _input_at)
string(FIND "${_query}" "  outputs:" _outputs_at)
if(_input_at LESS 0)
    message(FATAL_ERROR "no inputs listed for ${_scan_stamp}:\n${_query}")
endif()
if(_outputs_at LESS 0)
    string(SUBSTRING "${_query}" ${_input_at} -1 _inputs)
else()
    math(EXPR _inputs_length "${_outputs_at} - ${_input_at}")
    string(SUBSTRING "${_query}" ${_input_at} ${_inputs_length} _inputs)
endif()
# Control: the host binary itself is an input, so the listing was read.
string(FIND "${_inputs}" "AUv3/FixtureGain.app/Contents/MacOS/FixtureGain" _host_input)
if(_host_input LESS 0)
    message(FATAL_ERROR "the scan edge's inputs do not list the host binary:\n${_query}")
endif()
string(FIND "${_inputs}" "${_embed_stamp}" _embed_input)
if(_embed_input LESS 0)
    message(FATAL_ERROR
        "the AUv3 host scan stamp does not depend on ${_embed_stamp}: the embed step "
        "can rewrite the .app while the scan reads it.\n${_query}")
endif()

# 2. The freshness shortcut also sees the sentinel, so a re-embed rescans.
file(READ "${_build}/build.ninja" _ninja)
string(FIND "${_ninja}" "build ${_scan_stamp}" _scan_edge)
if(_scan_edge LESS 0)
    message(FATAL_ERROR "no build edge for ${_scan_stamp} in build.ninja")
endif()
string(SUBSTRING "${_ninja}" ${_scan_edge} 4000 _scan_rule)
string(FIND "${_scan_rule}" "-DSCAN_AFTER_INPUTS=" _after_flag)
string(FIND "${_scan_rule}" "FixtureGain_embed.stamp" _after_value)
if(_after_flag LESS 0 OR _after_value LESS 0)
    message(FATAL_ERROR
        "the scan edge does not pass the embed sentinel to the freshness check:\n${_scan_rule}")
endif()

# 3. In the host link's POST_BUILD list, the scan runs after both copies.
string(FIND "${_ninja}" "build AUv3/FixtureGain.app/Contents/MacOS/FixtureGain:" _link_edge)
if(_link_edge LESS 0)
    message(FATAL_ERROR "no link edge for the host app in build.ninja")
endif()
string(SUBSTRING "${_ninja}" ${_link_edge} -1 _link_rule)
string(FIND "${_link_rule}" "POST_BUILD = " _post_at)
string(FIND "${_link_rule}" "\nbuild " _next_edge)
if(_post_at LESS 0 OR (_next_edge GREATER_EQUAL 0 AND _post_at GREATER _next_edge))
    message(FATAL_ERROR "the host link has no POST_BUILD commands:\n${_link_rule}")
endif()
string(SUBSTRING "${_link_rule}" ${_post_at} -1 _post)
string(FIND "${_post}" "\n" _post_end)
string(SUBSTRING "${_post}" 0 ${_post_end} _post)
string(FIND "${_post}" "check_control_shipping_artifact.cmake" _scan_at)
string(FIND "${_post}" "Contents/Frameworks/" _framework_copy_at REVERSE)
string(FIND "${_post}" "Contents/PlugIns/" _appex_copy_at REVERSE)
if(_scan_at LESS 0 OR _framework_copy_at LESS 0 OR _appex_copy_at LESS 0)
    message(FATAL_ERROR "the host POST_BUILD list lacks the scan or a copy:\n${_post}")
endif()
if(_scan_at LESS _framework_copy_at OR _scan_at LESS _appex_copy_at)
    message(FATAL_ERROR
        "the host's POST_BUILD scan runs before the framework and appex are embedded, "
        "so it checks a bundle without them:\n${_post}")
endif()
