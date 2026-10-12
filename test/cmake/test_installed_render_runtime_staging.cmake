cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and PULP_BUILD_DIR are required")
endif()

# Installed Pulp exports the renderer as `Pulp::render` (capital P).  This
# fixture keeps the consumer-side target shape and proves that the staging
# helper follows its WebGPU link interface rather than only recognizing the
# source-build target name.
set(_root "${PULP_BUILD_DIR}/installed-render-runtime-staging-smoke")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}/src")
file(WRITE "${_root}/src/main.cpp" "int main() { return 0; }\n")
if(APPLE)
    set(_runtime_name "libinstalled-wgpu-runtime.dylib")
elseif(WIN32)
    set(_runtime_name "installed-wgpu-runtime.dll")
else()
    set(_runtime_name "libinstalled-wgpu-runtime.so")
endif()
file(WRITE "${_root}/src/${_runtime_name}" "installed WebGPU runtime fixture\n")

file(WRITE "${_root}/src/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(installed_render_runtime_staging LANGUAGES CXX)
include("${PULP_RUNTIME_STAGING_MODULE}")

# Match the installed SDK's provider target shape.  The staging helper must
# follow Pulp::render through its private gpu-device edge instead of assuming
# the provider appears directly on the renderer target.
add_library(webgpu INTERFACE IMPORTED GLOBAL)
add_library(Pulp::gpu-device INTERFACE IMPORTED GLOBAL)
set_target_properties(Pulp::gpu-device PROPERTIES
    INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:webgpu>")
add_library(Pulp::render INTERFACE IMPORTED GLOBAL)
set_target_properties(Pulp::render PROPERTIES
    INTERFACE_LINK_LIBRARIES "$<LINK_ONLY:Pulp::gpu-device>")
set(WEBGPU_RUNTIME_LIB
    "${CMAKE_CURRENT_SOURCE_DIR}/${PULP_RUNTIME_NAME}"
    CACHE FILEPATH "fixture WebGPU runtime")

add_executable(InstalledRenderConsumer MACOSX_BUNDLE main.cpp)
set_target_properties(InstalledRenderConsumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/consumer")
pulp_stage_runtime_dependencies(InstalledRenderConsumer)
pulp_verify_runtime_dependencies_staged(InstalledRenderConsumer)
]=])

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${_root}/src"
        -B "${_root}/build"
        -DCMAKE_BUILD_TYPE=Release
        "-DPULP_RUNTIME_STAGING_MODULE=${PULP_SOURCE_DIR}/tools/cmake/PulpRuntimeStaging.cmake"
        "-DPULP_RUNTIME_NAME=${_runtime_name}"
    RESULT_VARIABLE _configure_rc
    OUTPUT_VARIABLE _configure_out
    ERROR_VARIABLE _configure_err)
if(NOT _configure_rc EQUAL 0)
    message(FATAL_ERROR
        "installed Pulp::render fixture configure failed (${_configure_rc})\n"
        "${_configure_out}\n${_configure_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_root}/build" --config Release
    RESULT_VARIABLE _build_rc
    OUTPUT_VARIABLE _build_out
    ERROR_VARIABLE _build_err)
if(NOT _build_rc EQUAL 0)
    message(FATAL_ERROR
        "installed Pulp::render fixture build failed (${_build_rc})\n"
        "${_build_out}\n${_build_err}")
endif()

file(GLOB_RECURSE _staged "${_root}/build/*/${_runtime_name}")
list(LENGTH _staged _staged_count)
if(NOT _staged_count EQUAL 1)
    message(FATAL_ERROR
        "Pulp::render consumer did not stage exactly one ${_runtime_name}: "
        "${_staged}\n${_build_out}")
endif()
if(NOT _build_out MATCHES
   "InstalledRenderConsumer: verified ${_runtime_name}")
    message(FATAL_ERROR
        "Pulp::render runtime verification did not run:\n${_build_out}")
endif()

message(STATUS "installed_render_runtime_staging_verified=${_staged_count}")
