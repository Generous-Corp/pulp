cmake_minimum_required(VERSION 3.24)

# Installed-SDK consumer staging of the wgpu-native runtime, both ways:
#
#   skia-off  the exported Pulp::render links webgpu (an SDK built without
#             Skia's Dawn renders through wgpu-native): the runtime must be
#             staged beside each module, and the staged-runtime check must
#             expect it, so a missing copy fails the build.
#   skia-on   the exported Pulp::render does not link webgpu (Skia's static
#             Dawn is the implementation): nothing is staged or expected.
#
# The consumer sees only what PulpConfig.cmake provides: the IMPORTED
# Pulp::render, its pulp::render alias, and the IMPORTED webgpu target from
# PulpWebGpuImportedTarget.cmake over a fake SDK prefix. An IMPORTED target
# carries only INTERFACE_LINK_LIBRARIES, which is what the staging decision
# must read; each case's expectation is the opposite of the other's, so a
# decision that cannot see the link fails one of them.

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and PULP_BUILD_DIR are required")
endif()

set(_root "${PULP_BUILD_DIR}/installed-wgpu-runtime-staging")
file(REMOVE_RECURSE "${_root}")
set(_sdk "${_root}/sdk")
if(APPLE)
    set(_runtime_name "libwgpu_native.dylib")
elseif(WIN32)
    set(_runtime_name "wgpu_native.dll")
else()
    set(_runtime_name "libwgpu_native.so")
endif()
file(MAKE_DIRECTORY "${_sdk}/lib" "${_sdk}/external/webgpu/include/webgpu" "${_root}/src")
file(WRITE "${_sdk}/lib/${_runtime_name}" "fake wgpu-native runtime\n")
if(WIN32)
    file(WRITE "${_sdk}/lib/wgpu_native.dll.lib" "fake import library\n")
endif()
file(WRITE "${_root}/src/plugin.cpp" "extern \"C\" int fixture_plugin() { return 0; }\n")

file(WRITE "${_root}/src/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(installed_wgpu_runtime_staging LANGUAGES CXX)

# The shape PulpConfig.cmake gives a find_package(Pulp) consumer.
set(PULP_SDK_DIR "${FAKE_SDK}")
set(PULP_SDK_LIBRARY_DIR "${FAKE_SDK}/lib")
set(PULP_SDK_SHARED_LIBRARY_PREFIX "${CMAKE_SHARED_LIBRARY_PREFIX}")
set(PULP_SDK_SHARED_LIBRARY_SUFFIX "${CMAKE_SHARED_LIBRARY_SUFFIX}")
include("${PULP_CMAKE_DIR}/PulpWebGpuImportedTarget.cmake")
include("${PULP_CMAKE_DIR}/PulpRuntimeStaging.cmake")
add_library(Pulp::render INTERFACE IMPORTED)
if(RENDER_LINKS_WEBGPU)
    set_target_properties(Pulp::render PROPERTIES INTERFACE_LINK_LIBRARIES "webgpu")
endif()
add_library(pulp::render ALIAS Pulp::render)

pulp_wgpu_runtime_linked(_linked)
pulp_expected_runtime_dependencies(_expected)
message(STATUS "wgpu_runtime_linked=${_linked}")
message(STATUS "wgpu_runtime_expected=${_expected}")

add_library(InstalledPlugin MODULE plugin.cpp)
set_target_properties(InstalledPlugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin")
pulp_stage_runtime_dependencies(InstalledPlugin)
pulp_verify_runtime_dependencies_staged(InstalledPlugin)
]=])

function(_run_case name links)
    set(_build "${_root}/build-${name}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${_root}/src" -B "${_build}"
            -DCMAKE_BUILD_TYPE=Release
            "-DPULP_CMAKE_DIR=${PULP_SOURCE_DIR}/tools/cmake"
            "-DFAKE_SDK=${_sdk}"
            "-DRENDER_LINKS_WEBGPU=${links}"
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${name}: configure failed (${_rc})\n${_out}\n${_err}")
    endif()
    set(_configure_out "${_out}" PARENT_SCOPE)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config Release
        RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${name}: build failed (${_rc})\n${_out}\n${_err}")
    endif()
    set(_build_out "${_out}" PARENT_SCOPE)
    file(GLOB_RECURSE _staged "${_build}/plugin/*${_runtime_name}")
    set(_staged "${_staged}" PARENT_SCOPE)
endfunction()

_run_case(skia-off ON)
if(NOT _configure_out MATCHES "wgpu_runtime_linked=TRUE")
    message(FATAL_ERROR "skia-off: the staging decision did not see the imported link\n${_configure_out}")
endif()
if(NOT _configure_out MATCHES "wgpu_runtime_expected=[^\n]*${_runtime_name}")
    message(FATAL_ERROR "skia-off: the staged-runtime check does not expect ${_runtime_name}\n${_configure_out}")
endif()
if(NOT _staged)
    message(FATAL_ERROR "skia-off: ${_runtime_name} was not staged beside the module\n${_build_out}")
endif()
if(NOT _build_out MATCHES "InstalledPlugin: verified ${_runtime_name}")
    message(FATAL_ERROR "skia-off: the staged-runtime check did not verify ${_runtime_name}\n${_build_out}")
endif()

_run_case(skia-on OFF)
if(NOT _configure_out MATCHES "wgpu_runtime_linked=FALSE")
    message(FATAL_ERROR "skia-on: the staging decision reported a link that is not there\n${_configure_out}")
endif()
if(_configure_out MATCHES "wgpu_runtime_expected=[^\n]*wgpu_native")
    message(FATAL_ERROR "skia-on: the staged-runtime check expects wgpu-native\n${_configure_out}")
endif()
if(_staged)
    message(FATAL_ERROR "skia-on: ${_runtime_name} was staged although nothing links it: ${_staged}")
endif()

message(STATUS "installed_wgpu_runtime_staging_verified=skia-off,skia-on")
