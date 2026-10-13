# Provider decision contract: a real Dawn archive inside Skia suppresses the
# separate wgpu-native link; a missing archive keeps the fallback provider.

set(_fixture_dir "${CMAKE_CURRENT_BINARY_DIR}/gpu-provider-decision")
file(MAKE_DIRECTORY "${_fixture_dir}")
set(_helper "${CMAKE_CURRENT_LIST_DIR}/../../tools/cmake/PulpGpuProvider.cmake")
file(WRITE "${_fixture_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(gpu_provider_decision NONE)
include("@HELPER@")
set(PULP_HAS_WEBGPU TRUE)
add_library(webgpu INTERFACE)
file(WRITE "${CMAKE_BINARY_DIR}/dawn_combined.lib" "fixture")
add_library(provider-with-dawn INTERFACE)
set(PULP_HAS_SKIA TRUE)
set(DAWN_LIBRARY "${CMAKE_BINARY_DIR}/dawn_combined.lib")
pulp_link_gpu_provider(provider-with-dawn INTERFACE INTERFACE)
get_target_property(_with_dawn_links provider-with-dawn INTERFACE_LINK_LIBRARIES)
if(_with_dawn_links MATCHES "(^|;)webgpu(;|$)")
  message(FATAL_ERROR "provider-with-dawn incorrectly links wgpu-native")
endif()
add_library(provider-without-dawn INTERFACE)
set(DAWN_LIBRARY "${CMAKE_BINARY_DIR}/missing-dawn_combined.lib")
pulp_link_gpu_provider(provider-without-dawn INTERFACE INTERFACE)
get_target_property(_without_dawn_links provider-without-dawn INTERFACE_LINK_LIBRARIES)
if(NOT _without_dawn_links MATCHES "(^|;)webgpu(;|$)")
  message(FATAL_ERROR "provider-without-dawn did not link wgpu-native")
endif()
]=])
file(READ "${_fixture_dir}/CMakeLists.txt" _fixture)
string(REPLACE "@HELPER@" "${_helper}" _fixture "${_fixture}")
file(WRITE "${_fixture_dir}/CMakeLists.txt" "${_fixture}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${_fixture_dir}" -B "${_fixture_dir}/build"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "GPU provider decision fixture failed:\n${_out}\n${_err}")
endif()
message(STATUS "PASS: GPU provider decision follows DAWN_LIBRARY")
