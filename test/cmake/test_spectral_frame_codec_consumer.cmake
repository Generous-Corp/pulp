cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/spectral-frame-codec-consumer-smoke")
set(_prefix "${_fixture_root}/prefix")
set(_consumer_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")

set(_producer_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_producer_config "${PULP_PARENT_BUILD_TYPE}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_producer_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "Spectral codec SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

set(_consumer_configure_args
    -S "${PULP_SOURCE_DIR}/examples/spectral-frame-codec-consumer"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_producer_config}")

if(PULP_PARENT_SANITIZER)
    list(APPEND _consumer_configure_args
        "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(PULP_PARENT_CXX_FLAGS)
    list(APPEND _consumer_configure_args
        "-DCMAKE_CXX_FLAGS=${PULP_PARENT_CXX_FLAGS}")
endif()
if(PULP_PARENT_EXE_LINKER_FLAGS)
    list(APPEND _consumer_configure_args
        "-DCMAKE_EXE_LINKER_FLAGS=${PULP_PARENT_EXE_LINKER_FLAGS}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_consumer_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Spectral codec SDK consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}"
            --config "${_producer_config}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Spectral codec SDK consumer build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

execute_process(
    COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${_consumer_build}"
            --output-on-failure
    RESULT_VARIABLE _test_result
    OUTPUT_VARIABLE _test_output
    ERROR_VARIABLE _test_error)
if(NOT _test_result EQUAL 0)
    message(FATAL_ERROR
        "Spectral codec SDK consumer tests failed (${_test_result})\n"
        "${_test_output}\n${_test_error}")
endif()
