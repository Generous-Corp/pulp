cmake_minimum_required(VERSION 3.24)

# G-11 installed-SDK consumer gate: install the SDK, then configure, build and
# ctest the example project as an out-of-tree project that only
# sees the installed package. The positive test and the three marker-checked planted
# controls live in that project's CMakeLists.txt.
if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

# Defaults select the recording/publication consumer; the cookbook gates pass
# PULP_EXAMPLE_NAME and PULP_EXPECTED_TESTS for their own out-of-tree projects.
if(NOT DEFINED PULP_EXAMPLE_NAME)
    set(PULP_EXAMPLE_NAME g11-recording-publication)
endif()
if(NOT DEFINED PULP_EXPECTED_TESTS)
    set(PULP_EXPECTED_TESTS 5)
endif()

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()

set(_root "${PULP_BUILD_DIR}/${PULP_EXAMPLE_NAME}-consumer")
set(_prefix "${_root}/prefix")
set(_build "${_root}/build")
file(REMOVE_RECURSE "${_root}")

function(_g11_run description)
    execute_process(COMMAND ${ARGN}
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "G-11 consumer: ${description} failed (${_rc})\n${_out}\n${_err}")
    endif()
    message(STATUS "${_out}")
endfunction()

_g11_run("SDK install" "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
    --prefix "${_prefix}" --config "${_config}")

set(_generator_args)
if(PULP_PARENT_GENERATOR)
    set(_generator_args -G "${PULP_PARENT_GENERATOR}")
endif()
_g11_run("configure" "${CMAKE_COMMAND}"
    -S "${PULP_SOURCE_DIR}/examples/${PULP_EXAMPLE_NAME}" -B "${_build}"
    ${_generator_args}
    "-DCMAKE_BUILD_TYPE=${_config}" "-DCMAKE_PREFIX_PATH=${_prefix}")
_g11_run("build" "${CMAKE_COMMAND}" --build "${_build}" --config "${_config}")
_g11_run("ctest" "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}"
    -C "${_config}" --output-on-failure --no-tests=error)

# All four registered tests must have been discovered and run.
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_build}" -C "${_config}" -N
    OUTPUT_VARIABLE _listing)
if(NOT _listing MATCHES "Total Tests: ${PULP_EXPECTED_TESTS}([^0-9]|$)")
    message(FATAL_ERROR "G-11 consumer ${PULP_EXAMPLE_NAME} expected ${PULP_EXPECTED_TESTS} tests:\n${_listing}")
endif()
message(STATUS "g11_recording_publication_consumer_verified=true")
