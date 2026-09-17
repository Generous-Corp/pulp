cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/faust-rt-sdk-consumer")
set(_prefix "${_fixture_root}/prefix")
set(_consumer_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")

set(_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_config "${PULP_PARENT_BUILD_TYPE}")
endif()
string(TOLOWER "${_config}" _config_lower)

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "Faust RT SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

set(_configure_args
    -S "${PULP_SOURCE_DIR}/examples/faust-rt-sdk-consumer"
    -B "${_consumer_build}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DCMAKE_BUILD_TYPE=${_config}")
if(_config_lower STREQUAL "debug")
    list(APPEND _configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()
if(PULP_PARENT_SANITIZER)
    list(APPEND _configure_args "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(PULP_PARENT_CXX_FLAGS)
    list(APPEND _configure_args "-DCMAKE_CXX_FLAGS=${PULP_PARENT_CXX_FLAGS}")
endif()
if(PULP_PARENT_EXE_LINKER_FLAGS)
    list(APPEND _configure_args
        "-DCMAKE_EXE_LINKER_FLAGS=${PULP_PARENT_EXE_LINKER_FLAGS}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed Faust RT example configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}"
            --target pulp-faust-rt-sdk-consumer --config "${_config}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed Faust RT example build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

set(_executable "${_consumer_build}/pulp-faust-rt-sdk-consumer")
if(WIN32)
    set(_executable "${_consumer_build}/${_config}/pulp-faust-rt-sdk-consumer.exe")
elseif(NOT EXISTS "${_executable}")
    set(_executable
        "${_consumer_build}/${_config}/pulp-faust-rt-sdk-consumer")
endif()
execute_process(COMMAND "${_executable}" RESULT_VARIABLE _run_result)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR "Installed Faust RT example exited ${_run_result}")
endif()
