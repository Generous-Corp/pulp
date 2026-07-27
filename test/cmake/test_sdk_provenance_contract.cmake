cmake_minimum_required(VERSION 3.24)

if(NOT PULP_SRC_DIR OR NOT FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SRC_DIR and FIXTURE_DIR are required")
endif()

set(_module "${PULP_SRC_DIR}/tools/cmake/PulpSdkProvenance.cmake")
set(_sdk "${FIXTURE_DIR}/sdk")
file(REMOVE_RECURSE "${_sdk}")
file(MAKE_DIRECTORY "${_sdk}")

set(PULP_SDK_DIR "${_sdk}")
include("${_module}")
if(PULP_SDK_DEVELOPMENT OR NOT PULP_SDK_DISTRIBUTION_ELIGIBLE OR
   NOT PULP_SDK_PROVENANCE_KIND STREQUAL "release")
    message(FATAL_ERROR "marker-free release compatibility contract is wrong")
endif()

file(WRITE "${_sdk}/sdk-provenance.json"
    "{\"schema\":\"pulp.sdk-provenance.v1\",\"kind\":\"development\","
    "\"distribution_eligible\":false,\"source_git_sha\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}")
include("${_module}")
if(NOT PULP_SDK_DEVELOPMENT OR PULP_SDK_DISTRIBUTION_ELIGIBLE OR
   NOT PULP_SDK_PROVENANCE_KIND STREQUAL "development" OR
   NOT PULP_SDK_SOURCE_GIT_SHA STREQUAL "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa")
    message(FATAL_ERROR "development SDK contract was not exported")
endif()

file(WRITE "${_sdk}/sdk-provenance.json"
    "{\"schema\":\"pulp.sdk-provenance.v1\",\"kind\":\"development\","
    "\"distribution_eligible\":true,\"source_git_sha\":\"bad\"}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DPULP_SDK_DIR=${_sdk}"
        -P "${_module}"
    RESULT_VARIABLE _unsafe_result
    OUTPUT_VARIABLE _unsafe_stdout
    ERROR_VARIABLE _unsafe_stderr)
if(_unsafe_result EQUAL 0)
    message(FATAL_ERROR "unsafe development provenance did not fail closed")
endif()
if(NOT "${_unsafe_stdout}\n${_unsafe_stderr}" MATCHES "unknown or unsafe contract")
    message(FATAL_ERROR "unsafe provenance failed without the expected diagnostic")
endif()
