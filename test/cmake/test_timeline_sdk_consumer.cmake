cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/timeline-sdk-consumer-smoke")
set(_prefix "${_fixture_root}/prefix")
set(_consumer_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")

set(_producer_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_producer_config "${PULP_PARENT_BUILD_TYPE}")
endif()
string(TOLOWER "${_producer_config}" _producer_config_lower)

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_producer_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "Timeline SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

set(_consumer_configure_args
    -S "${PULP_SOURCE_DIR}/examples/timeline-sdk-consumer"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_producer_config}")

# This fixture verifies the installed Timeline target closure from every build
# lane, including ASan's deliberate Debug SDK. The production guard remains
# covered by test_debug_sdk_guard.cmake; this one explicitly acknowledges a
# Debug producer and carries its sanitizer flags into the external consumer so
# its static Pulp libraries link against the matching sanitizer runtime.
if(_producer_config_lower STREQUAL "debug")
    list(APPEND _consumer_configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()

set(_consumer_cxx_flags "${PULP_PARENT_CXX_FLAGS}")
set(_consumer_linker_flags "${PULP_PARENT_EXE_LINKER_FLAGS}")
if(PULP_PARENT_INSTRUMENTATION_CXX_FLAGS)
    string(APPEND _consumer_cxx_flags
        " ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}")
endif()
if(PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS)
    string(APPEND _consumer_linker_flags
        " ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
endif()
string(STRIP "${_consumer_cxx_flags}" _consumer_cxx_flags)
string(STRIP "${_consumer_linker_flags}" _consumer_linker_flags)

if(PULP_PARENT_SANITIZER)
    list(APPEND _consumer_configure_args
        "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(_consumer_cxx_flags)
    list(APPEND _consumer_configure_args
        "-DCMAKE_CXX_FLAGS=${_consumer_cxx_flags}")
endif()
if(_consumer_linker_flags)
    list(APPEND _consumer_configure_args
        "-DCMAKE_EXE_LINKER_FLAGS=${_consumer_linker_flags}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_consumer_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Timeline SDK consumer configure failed (${_configure_result})\n"
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
        "Timeline SDK consumer build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

set(_executable_suffix "")
if(WIN32)
    set(_executable_suffix ".exe")
endif()
set(_executable
    "${_consumer_build}/pulp-timeline-sdk-consumer${_executable_suffix}")
if(NOT EXISTS "${_executable}")
    set(_executable
        "${_consumer_build}/${_producer_config}/pulp-timeline-sdk-consumer${_executable_suffix}")
endif()
execute_process(COMMAND "${_executable}" RESULT_VARIABLE _run_result)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR "Timeline SDK consumer exited ${_run_result}")
endif()

set(_smf_executable
    "${_consumer_build}/pulp-smf-interchange-sdk-consumer${_executable_suffix}")
if(NOT EXISTS "${_smf_executable}")
    set(_smf_executable
        "${_consumer_build}/${_producer_config}/pulp-smf-interchange-sdk-consumer${_executable_suffix}")
endif()
execute_process(COMMAND "${_smf_executable}" RESULT_VARIABLE _smf_run_result)
if(NOT _smf_run_result EQUAL 0)
    message(FATAL_ERROR "SMF interchange SDK consumer exited ${_smf_run_result}")
endif()

set(_daw_export_executable
    "${_consumer_build}/pulp-dawproject-export-sdk-consumer${_executable_suffix}")
if(NOT EXISTS "${_daw_export_executable}")
    set(_daw_export_executable
        "${_consumer_build}/${_producer_config}/pulp-dawproject-export-sdk-consumer${_executable_suffix}")
endif()
execute_process(COMMAND "${_daw_export_executable}" RESULT_VARIABLE _daw_export_run_result)
if(NOT _daw_export_run_result EQUAL 0)
    message(FATAL_ERROR "DAWproject exporter SDK consumer exited ${_daw_export_run_result}")
endif()

# The cookbook walks the authoring surface end to end -- transaction commit,
# undo/redo, scene insert, compile+publish, capture prepare -- and returns a
# distinct code per step. Building it only proves the headers and the installed
# link closure are usable; every runtime claim in the guide it backs is
# unverified unless the binary actually RUNS. So run it, and map the code back
# to the step so a failure names what broke instead of just a number.
set(_cookbook_executable
    "${_consumer_build}/pulp-timeline-cookbook-consumer${_executable_suffix}")
if(NOT EXISTS "${_cookbook_executable}")
    set(_cookbook_executable
        "${_consumer_build}/${_producer_config}/pulp-timeline-cookbook-consumer${_executable_suffix}")
endif()
execute_process(COMMAND "${_cookbook_executable}" RESULT_VARIABLE _cookbook_run_result)
if(NOT _cookbook_run_result EQUAL 0)
    set(_cookbook_steps
        "1=build project"
        "2=open DocumentSession"
        "3=register writer"
        "4=resolve the authored clip"
        "5=commit the MoveClip transaction"
        "6=undo then redo"
        "7=allocate scene and slot ids"
        "8=commit the InsertScene transaction"
        "9=compile and publish"
        "10=prepare capture")
    set(_cookbook_step "unknown step")
    foreach(_entry IN LISTS _cookbook_steps)
        if(_entry MATCHES "^${_cookbook_run_result}=(.*)$")
            set(_cookbook_step "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    message(FATAL_ERROR
        "Timeline cookbook consumer exited ${_cookbook_run_result} (${_cookbook_step})")
endif()

set(_importer_executable
    "${_consumer_build}/pulp-dawproject-import-sdk-consumer${_executable_suffix}")
if(NOT EXISTS "${_importer_executable}")
    set(_importer_executable
        "${_consumer_build}/${_producer_config}/pulp-dawproject-import-sdk-consumer${_executable_suffix}")
endif()
execute_process(COMMAND "${_importer_executable}" RESULT_VARIABLE _importer_run_result)
if(NOT _importer_run_result EQUAL 0)
    message(FATAL_ERROR
        "Installed DAWproject importer consumer exited ${_importer_run_result}")
endif()

set(_missing_source "${_fixture_root}/missing-component")
set(_missing_build "${_fixture_root}/missing-component-build")
file(MAKE_DIRECTORY "${_missing_source}")
file(WRITE "${_missing_source}/CMakeLists.txt"
    "cmake_minimum_required(VERSION 3.24)\n"
    "project(PulpMissingComponent LANGUAGES CXX)\n"
    "find_package(Pulp REQUIRED COMPONENTS timeline component-that-does-not-exist)\n")
set(_missing_configure_args
    -S "${_missing_source}"
    -B "${_missing_build}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_producer_config}")
if(_producer_config_lower STREQUAL "debug")
    list(APPEND _missing_configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_missing_configure_args}
    RESULT_VARIABLE _missing_result
    OUTPUT_VARIABLE _missing_output
    ERROR_VARIABLE _missing_error)
if(_missing_result EQUAL 0)
    message(FATAL_ERROR
        "Pulp accepted a nonexistent required component\n"
        "${_missing_output}\n${_missing_error}")
endif()
