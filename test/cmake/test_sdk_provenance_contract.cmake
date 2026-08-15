cmake_minimum_required(VERSION 3.24)

if(NOT PULP_SRC_DIR OR NOT FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SRC_DIR and FIXTURE_DIR are required")
endif()

set(_module "${PULP_SRC_DIR}/tools/cmake/PulpSdkProvenance.cmake")
set(_sdk "${FIXTURE_DIR}/sdk")
file(REMOVE_RECURSE "${_sdk}")
file(MAKE_DIRECTORY "${_sdk}")
file(MAKE_DIRECTORY "${_sdk}/include/pulp/view" "${_sdk}/lib")
file(WRITE "${_sdk}/include/pulp/view/widget_bridge.hpp" "header fixture\n")
file(WRITE "${_sdk}/lib/libpulp-view-script.a" "archive fixture\n")
file(SHA256 "${_sdk}/include/pulp/view/widget_bridge.hpp" _header_hash)
file(SHA256 "${_sdk}/lib/libpulp-view-script.a" _archive_hash)
string(CONCAT _integrity
    ",\"integrity\":{\"schema\":\"pulp.sdk-integrity.v1\",\"algorithm\":\"sha256\","
    "\"files\":{\"include/pulp/view/widget_bridge.hpp\":\"${_header_hash}\","
    "\"lib/libpulp-view-script.a\":\"${_archive_hash}\"}}")

set(PULP_SDK_DIR "${_sdk}")
include("${_module}")
if(PULP_SDK_DEVELOPMENT OR PULP_SDK_DISTRIBUTION_ELIGIBLE OR
   NOT PULP_SDK_PROVENANCE_KIND STREQUAL "unmarked")
    message(FATAL_ERROR "marker-free SDK did not fail closed for distribution")
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
    "{\"schema\":\"pulp.sdk-provenance.v1\",\"kind\":\"release\","
    "\"profile\":\"official-release\",\"distribution_eligible\":true,"
    "\"sdk_version\":\"9.8.7\",\"source_git_ref\":\"v9.8.7\","
    "\"source_git_sha\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
    "\"source_git_dirty\":false,\"platform\":\"darwin-arm64\",\"build_type\":\"Release\","
    "\"features\":{\"audio_probes\":false,\"inspector\":true}${_integrity}}")
include("${_module}")
if(PULP_SDK_DEVELOPMENT OR NOT PULP_SDK_DISTRIBUTION_ELIGIBLE OR
   NOT PULP_SDK_PROVENANCE_KIND STREQUAL "release" OR
   PULP_SDK_AUDIO_PROBES_ENABLED OR NOT PULP_SDK_INSPECTOR_ENABLED OR
   NOT PULP_SDK_PLATFORM STREQUAL "darwin-arm64" OR
   NOT PULP_SDK_SOURCE_GIT_SHA STREQUAL "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
    message(FATAL_ERROR "official release SDK contract was not exported")
endif()

file(WRITE "${_sdk}/include/pulp/view/widget_bridge.hpp" "stale header fixture\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DPULP_SDK_DIR=${_sdk}"
        -P "${_module}"
    RESULT_VARIABLE _coherence_result
    OUTPUT_VARIABLE _coherence_stdout
    ERROR_VARIABLE _coherence_stderr)
if(_coherence_result EQUAL 0)
    message(FATAL_ERROR "mixed WidgetBridge SDK did not fail closed")
endif()
if(NOT "${_coherence_stdout}\n${_coherence_stderr}"
       MATCHES "coherence mismatch.*widget_bridge.hpp")
    message(FATAL_ERROR
        "mixed WidgetBridge SDK failed without the expected diagnostic:\n"
        "${_coherence_stdout}\n${_coherence_stderr}")
endif()
file(WRITE "${_sdk}/include/pulp/view/widget_bridge.hpp" "header fixture\n")

file(WRITE "${_sdk}/sdk-provenance.json"
    "{\"schema\":\"pulp.sdk-provenance.v1\",\"kind\":\"release\","
    "\"profile\":\"official-release\",\"distribution_eligible\":true,"
    "\"sdk_version\":\"0.771.0\",\"source_git_ref\":\"v0.771.0\","
    "\"source_git_sha\":\"dddddddddddddddddddddddddddddddddddddddd\","
    "\"source_git_dirty\":false,\"platform\":\"darwin-arm64\",\"build_type\":\"Release\","
    "\"features\":{\"audio_probes\":false,\"inspector\":false}}")
include("${_module}")
if(PULP_SDK_DEVELOPMENT OR NOT PULP_SDK_DISTRIBUTION_ELIGIBLE OR
   NOT PULP_SDK_PROVENANCE_KIND STREQUAL "release" OR
   PULP_SDK_AUDIO_PROBES_ENABLED OR PULP_SDK_INSPECTOR_ENABLED OR
   NOT PULP_SDK_SOURCE_GIT_SHA STREQUAL "dddddddddddddddddddddddddddddddddddddddd")
    message(FATAL_ERROR "historical release SDK contract was not exported")
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
    message(FATAL_ERROR
        "unsafe provenance failed without the expected diagnostic:\n"
        "${_unsafe_stdout}\n${_unsafe_stderr}")
endif()

file(WRITE "${_sdk}/sdk-provenance.json"
    "{\"schema\":\"pulp.sdk-provenance.v1\",\"kind\":\"release\","
    "\"profile\":\"official-release\",\"distribution_eligible\":true,"
    "\"sdk_version\":\"9.8.7\",\"source_git_ref\":\"v9.8.7\","
    "\"source_git_sha\":\"cccccccccccccccccccccccccccccccccccccccc\","
    "\"source_git_dirty\":false,\"platform\":\"darwin-arm64\",\"build_type\":\"Release\","
    "\"features\":{\"audio_probes\":false,\"inspector\":false}${_integrity}}")
execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DPULP_SDK_DIR=${_sdk}"
        -P "${_module}"
    RESULT_VARIABLE _unsafe_release_result
    OUTPUT_VARIABLE _unsafe_release_stdout
    ERROR_VARIABLE _unsafe_release_stderr)
if(_unsafe_release_result EQUAL 0)
    message(FATAL_ERROR "unsafe release provenance did not fail closed")
endif()
if(NOT "${_unsafe_release_stdout}\n${_unsafe_release_stderr}"
       MATCHES "unknown or unsafe release contract")
    message(FATAL_ERROR
        "unsafe release failed without the expected diagnostic:\n"
        "${_unsafe_release_stdout}\n${_unsafe_release_stderr}")
endif()
