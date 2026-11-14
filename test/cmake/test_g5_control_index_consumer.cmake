cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()
set(_root "${PULP_BUILD_DIR}/g5-control-index-filter-consumer")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
file(REMOVE_RECURSE "${_root}")

function(_g5_run description)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "G-5 consumer: ${description} failed (${_rc})\n${_out}\n${_err}")
    endif()
    message(STATUS "G-5 consumer: ${description} passed")
endfunction()

_g5_run("SDK install" "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${_config}")
set(_generator_args)
if(PULP_PARENT_GENERATOR)
    set(_generator_args -G "${PULP_PARENT_GENERATOR}")
endif()
_g5_run("configure" "${CMAKE_COMMAND}"
    -S "${PULP_SOURCE_DIR}/examples/g5-control-index-filter" -B "${_build}"
    ${_generator_args}
    "-DCMAKE_BUILD_TYPE=${_config}" "-DCMAKE_PREFIX_PATH=${_prefix}")
_g5_run("build" "${PULP_SOURCE_DIR}/tools/ci/governed-build.sh"
    "${CMAKE_COMMAND}" --build "${_build}" --config "${_config}")
_g5_run("ctest" "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}"
    -C "${_config}" --output-on-failure --no-tests=error)
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" -C "${_config}" -N
    OUTPUT_VARIABLE _listing)
if(NOT _listing MATCHES "Total Tests: 3([^0-9]|$)")
    message(FATAL_ERROR "G-5 consumer expected three tests:\n${_listing}")
endif()
message(STATUS "g5_control_index_filter_consumer_verified=true")
