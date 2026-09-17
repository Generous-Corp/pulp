cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and PULP_BUILD_DIR are required")
endif()

set(_root "${PULP_BUILD_DIR}/registered-runtime-staging-smoke")
file(REMOVE_RECURSE "${_root}")
file(MAKE_DIRECTORY "${_root}/src")
file(WRITE "${_root}/src/main.cpp" "int main() { return 0; }\n")
file(WRITE "${_root}/src/plugin.cpp" "extern \"C\" int fixture_plugin() { return 0; }\n")
if(APPLE)
    set(_runtime_name "libregistered-runtime.dylib")
elseif(WIN32)
    set(_runtime_name "registered-runtime.dll")
else()
    set(_runtime_name "libregistered-runtime.so")
endif()
file(WRITE "${_root}/src/${_runtime_name}" "registered runtime fixture\n")

file(WRITE "${_root}/src/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(registered_runtime_staging LANGUAGES CXX)
include("${PULP_RUNTIME_STAGING_MODULE}")

add_library(RegisteredRuntime SHARED IMPORTED GLOBAL)
set_target_properties(RegisteredRuntime PROPERTIES
    IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/${PULP_RUNTIME_NAME}")
pulp_register_runtime_dependency_target(RegisteredRuntime)
# Registration is idempotent; one runtime produces one staged sidecar.
pulp_register_runtime_dependency_target(RegisteredRuntime)

add_executable(RuntimeStandalone MACOSX_BUNDLE main.cpp)
set_target_properties(RuntimeStandalone PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/standalone")
pulp_stage_runtime_dependencies(RuntimeStandalone)
pulp_verify_runtime_dependencies_staged(RuntimeStandalone)

add_library(RuntimePlugin MODULE plugin.cpp)
set_target_properties(RuntimePlugin PROPERTIES
    BUNDLE TRUE
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugin")
pulp_stage_runtime_dependencies(RuntimePlugin)
pulp_verify_runtime_dependencies_staged(RuntimePlugin)
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
        "registered runtime fixture configure failed (${_configure_rc})\n"
        "${_configure_out}\n${_configure_err}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_root}/build" --config Release
    RESULT_VARIABLE _build_rc
    OUTPUT_VARIABLE _build_out
    ERROR_VARIABLE _build_err)
if(NOT _build_rc EQUAL 0)
    message(FATAL_ERROR
        "registered runtime fixture build failed (${_build_rc})\n"
        "${_build_out}\n${_build_err}")
endif()

file(GLOB_RECURSE _staged "${_root}/build/*/${_runtime_name}")
list(LENGTH _staged _staged_count)
if(NOT _staged_count EQUAL 2)
    message(FATAL_ERROR
        "expected exactly two staged ${_runtime_name} sidecars, found "
        "${_staged_count}: ${_staged}\n${_build_out}")
endif()
if(NOT _build_out MATCHES "RuntimeStandalone: verified ${_runtime_name}")
    message(FATAL_ERROR "standalone runtime verification did not run:\n${_build_out}")
endif()
if(NOT _build_out MATCHES "RuntimePlugin: verified ${_runtime_name}")
    message(FATAL_ERROR "plugin runtime verification did not run:\n${_build_out}")
endif()

message(STATUS "registered_runtime_staging_verified=${_staged_count}")
