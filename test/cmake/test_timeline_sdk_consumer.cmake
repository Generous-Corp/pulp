cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR OR NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR and PULP_SOURCE_DIR are required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/timeline-sdk-consumer-smoke")
set(_prefix "${_fixture_root}/prefix")
set(_consumer_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config Release
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "Timeline SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${PULP_SOURCE_DIR}/examples/timeline-sdk-consumer"
            -B "${_consumer_build}"
            "-DCMAKE_PREFIX_PATH=${_prefix}"
            "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
            -DCMAKE_BUILD_TYPE=Release
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
            --config Release --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Timeline SDK consumer build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

set(_executable "${_consumer_build}/pulp-timeline-sdk-consumer")
if(WIN32)
    set(_executable "${_consumer_build}/Release/pulp-timeline-sdk-consumer.exe")
endif()
execute_process(COMMAND "${_executable}" RESULT_VARIABLE _run_result)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR "Timeline SDK consumer exited ${_run_result}")
endif()

set(_importer_executable "${_consumer_build}/pulp-dawproject-import-sdk-consumer")
if(WIN32)
    set(_importer_executable
        "${_consumer_build}/Release/pulp-dawproject-import-sdk-consumer.exe")
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
execute_process(
    COMMAND "${CMAKE_COMMAND}"
            -S "${_missing_source}"
            -B "${_missing_build}"
            "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
            -DCMAKE_BUILD_TYPE=Release
    RESULT_VARIABLE _missing_result
    OUTPUT_VARIABLE _missing_output
    ERROR_VARIABLE _missing_error)
if(_missing_result EQUAL 0)
    message(FATAL_ERROR
        "Pulp accepted a nonexistent required component\n"
        "${_missing_output}\n${_missing_error}")
endif()
