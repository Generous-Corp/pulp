cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_id)
set(_fixture "${PULP_BUILD_DIR}/graph-patch-${_run_id}")
set(_prefix "${_fixture}/prefix")
set(_source "${_fixture}/source")
set(_build "${_fixture}/consumer-build")
file(MAKE_DIRECTORY "${_source}" "${PULP_BUILD_DIR}/Testing/Temporary")
set(_receipt "${PULP_BUILD_DIR}/Testing/Temporary/graph-patch-${_run_id}.log")

function(run_step label)
    execute_process(COMMAND ${ARGN}
        WORKING_DIRECTORY "${_fixture}" TIMEOUT 240
        RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    file(WRITE "${_fixture}/${label}.log" "${_out}\n${_err}")
    file(APPEND "${_receipt}" "${label}: ${_result}\n${_out}\n${_err}\n")
    if(NOT "${_result}" STREQUAL "0")
        message(FATAL_ERROR "G2 ${label} failed: ${_out}\n${_err}\nfixture=${_fixture}")
    endif()
    message(STATUS "G2 ${label}: passed")
endfunction()

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()
run_step(stage "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${_config}")
file(READ "${_prefix}/sdk-provenance.json" _provenance)
file(APPEND "${_receipt}" "Installed provenance: ${_provenance}\n")
execute_process(COMMAND git -C "${PULP_SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE _git_result OUTPUT_VARIABLE _head OUTPUT_STRIP_TRAILING_WHITESPACE)
string(JSON _installed_head GET "${_provenance}" source_git_sha)
string(JSON _installed_dirty GET "${_provenance}" source_git_dirty)
if(NOT _git_result EQUAL 0 OR NOT _installed_head STREQUAL _head OR _installed_dirty)
    message(FATAL_ERROR "Installed SDK must come from the clean current commit; reconfigure and build before this test")
endif()
file(SHA256 "${PULP_SOURCE_DIR}/core/host/include/pulp/host/graph_patch.hpp" _expected)
file(SHA256 "${_prefix}/include/pulp/host/graph_patch.hpp" _actual)
if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "G2 installed header differs from this checkout")
endif()
foreach(_file CMakeLists.txt main.cpp hello.hpp hello.pat)
    configure_file("${PULP_SOURCE_DIR}/examples/graph-patch-consumer/${_file}"
        "${_source}/${_file}" COPYONLY)
endforeach()

# Compile the documented public authoring calls verbatim, independently of the
# 100-line consumer. A changed guide must still construct the same saved graph.
file(READ "${PULP_SOURCE_DIR}/docs/guides/graph-patch-notation.md" _guide)
string(REGEX MATCH "```cpp\n([^`]+)```" _cpp_block "${_guide}")
set(_cpp_hello "${CMAKE_MATCH_1}")
string(STRIP "${_cpp_hello}" _cpp_trimmed)
string(REGEX REPLACE "[^\n]" "" _cpp_newlines "${_cpp_trimmed}")
string(LENGTH "${_cpp_newlines}" _cpp_line_count)
math(EXPR _cpp_line_count "${_cpp_line_count} + 1")
if(NOT _cpp_hello OR _cpp_line_count GREATER 15)
    message(FATAL_ERROR "Documented C++ graph hello must contain 1..15 lines")
endif()
file(WRITE "${_source}/cpp_hello.cpp" [=[
#include "hello.hpp"
#include <cmath>
int main(int argc, char** argv) {
if (argc != 2) return 1;
]=])
file(APPEND "${_source}/cpp_hello.cpp" "${_cpp_hello}")
file(APPEND "${_source}/cpp_hello.cpp" [=[
auto expected = pulp::host::load_graph_patch_file(argv[1], catalog);
auto actual = pulp::host::format_graph_patch(*graph, catalog);
if (!expected || !actual || graph->connections().size() != 4) return 2;
auto canonical = pulp::host::format_graph_patch(*expected.graph, catalog);
return canonical && canonical.text == actual.text ? 0 : 3;
}
]=])
file(APPEND "${_source}/CMakeLists.txt" [=[
add_executable(graph-patch-cpp-hello cpp_hello.cpp)
target_link_libraries(graph-patch-cpp-hello PRIVATE Pulp::host)
target_compile_features(graph-patch-cpp-hello PRIVATE cxx_std_20)
add_test(NAME graph-patch-documented-cpp-hello
    COMMAND graph-patch-cpp-hello "${CMAKE_CURRENT_SOURCE_DIR}/hello.pat")
set_tests_properties(graph-patch-documented-cpp-hello PROPERTIES TIMEOUT 30)
]=])
file(APPEND "${_receipt}" "Documented C++ authoring lines: ${_cpp_line_count}\n")

set(_args -S "${_source}" -B "${_build}" -G Ninja
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DCMAKE_BUILD_TYPE=${_config}"
    "-DCMAKE_CXX_FLAGS=${PULP_PARENT_CXX_FLAGS} ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=${PULP_PARENT_EXE_LINKER_FLAGS} ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
if(PULP_PARENT_CXX_COMPILER)
    list(APPEND _args "-DCMAKE_CXX_COMPILER=${PULP_PARENT_CXX_COMPILER}")
endif()
string(TOLOWER "${_config}" _config_lower)
if(_config_lower STREQUAL "debug")
    list(APPEND _args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()
if(PULP_PARENT_SANITIZER)
    list(APPEND _args "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(PULP_PARENT_OSX_ARCHITECTURES)
    list(APPEND _args "-DCMAKE_OSX_ARCHITECTURES=${PULP_PARENT_OSX_ARCHITECTURES}")
endif()
run_step(configure "${CMAKE_COMMAND}" ${_args})
run_step(build "${CMAKE_COMMAND}" -E env PULP_BUILD_JOBS=1
    bash "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
    "${CMAKE_COMMAND}" --build "${_build}")
run_step(consumer "${_build}/graph-patch-consumer" "${_source}/hello.pat")
run_step(ctest "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" --output-on-failure)
message(STATUS "G2 installed-SDK notation and numeric render proof complete")

if(EXISTS "${PULP_BUILD_DIR}/tools/cli/pulp-cpp" AND EXISTS "${PULP_BUILD_DIR}/tools/mcp/pulp-mcp")
    find_program(_python NAMES python3 REQUIRED)
    run_step(parity "${_python}" "${PULP_SOURCE_DIR}/test/test_graph_patch_tools.py"
        --consumer "${_build}/graph-patch-consumer"
        --cli "${PULP_BUILD_DIR}/tools/cli/pulp-cpp"
        --mcp "${PULP_BUILD_DIR}/tools/mcp/pulp-mcp" --root "${PULP_SOURCE_DIR}")
else()
    message(STATUS "Graph patch API/CLI/MCP parity unavailable: build pulp-cli and pulp-mcp to run it")
endif()
