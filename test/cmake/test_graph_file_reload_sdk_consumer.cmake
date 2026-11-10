cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef _run_id)
set(_fixture "${PULP_BUILD_DIR}/graph-file-reload-${_run_id}")
set(_prefix "${_fixture}/prefix")
set(_source "${_fixture}/source")
set(_build "${_fixture}/consumer-build")
file(MAKE_DIRECTORY "${_source}" "${PULP_BUILD_DIR}/Testing/Temporary")
set(_receipt "${PULP_BUILD_DIR}/Testing/Temporary/graph-file-reload-${_run_id}.log")

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
file(SHA256 "${PULP_SOURCE_DIR}/core/host/include/pulp/host/graph_file_reload.hpp" _expected)
file(SHA256 "${_prefix}/include/pulp/host/graph_file_reload.hpp" _actual)
if(NOT _actual STREQUAL _expected)
    message(FATAL_ERROR "G2 installed header differs from this checkout")
endif()
foreach(_file CMakeLists.txt main.cpp hello.hpp)
    configure_file("${PULP_SOURCE_DIR}/examples/graph-file-reload-consumer/${_file}"
        "${_source}/${_file}" COPYONLY)
endforeach()
file(MAKE_DIRECTORY "${_source}/../graph-patch-consumer")
configure_file("${PULP_SOURCE_DIR}/examples/graph-patch-consumer/hello.hpp"
    "${_source}/../graph-patch-consumer/hello.hpp" COPYONLY)
run_step(configure "${CMAKE_COMMAND}" -S "${_source}" -B "${_build}" -G Ninja
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp" "-DCMAKE_PREFIX_PATH=${_prefix}"
    -DCMAKE_BUILD_TYPE=Release "-DCMAKE_CXX_COMPILER=${PULP_PARENT_CXX_COMPILER}")
run_step(build "${CMAKE_COMMAND}" -E env PULP_BUILD_JOBS=1 bash
    "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh" "${CMAKE_COMMAND}" --build "${_build}")
run_step(consumer "${_build}/graph-file-reload-consumer" "${_fixture}/live.pat")
