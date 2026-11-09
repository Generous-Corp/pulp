cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/native-sdk-consumer")
set(_prefix "${_fixture_root}/prefix")
set(_source "${_fixture_root}/src")
set(_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")
file(MAKE_DIRECTORY "${_source}")

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
        "Native-surface SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

file(WRITE "${_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpNativeSdkConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Pulp REQUIRED COMPONENTS native)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Pulp::native)
]=])

file(WRITE "${_source}/main.cpp" [=[
#include <pulp/native/frame_pump.hpp>
#include <pulp/native/input.hpp>
#include <pulp/native/metal_surface.hpp>

// This external project sees only the staged SDK prefix. It links all three
// parts of the native surface: the surface (which registers its view class
// through the platform layer), the frame pump and the input translation.
int main() {
    using namespace pulp::native;

    KeyEvent space;
    space.key = key_for_native_code(49); // kVK_Space
    if (space.key != Key::space || key_name(space.key) != "space")
        return 1;
    if (route_key(space, false) != KeyRoute::host ||
        route_key(space, true) != KeyRoute::client_first)
        return 2;

    FramePump pump([](const FrameTick&) {});
    if (pump.running())
        return 3;

    // Null only on a host with no Metal device; otherwise a live surface on it.
    auto surface = MetalSurface::create();
    if (surface && surface->device() == nullptr)
        return 4;
    return 0;
}
]=])

set(_consumer_configure_args
    -S "${_source}"
    -B "${_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DCMAKE_BUILD_TYPE=${_config}")
if(_config_lower STREQUAL "debug")
    list(APPEND _consumer_configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()
if(PULP_PARENT_OSX_ARCHITECTURES)
    list(APPEND _consumer_configure_args
        "-DCMAKE_OSX_ARCHITECTURES=${PULP_PARENT_OSX_ARCHITECTURES}")
endif()

set(_consumer_cxx_flags "${PULP_PARENT_CXX_FLAGS}")
set(_consumer_linker_flags "${PULP_PARENT_EXE_LINKER_FLAGS}")
# Installed static libraries retain the producer's coverage or sanitizer
# runtime references, so the external consumer must carry matching flags.
if(PULP_PARENT_INSTRUMENTATION_CXX_FLAGS)
    string(APPEND _consumer_cxx_flags
        " ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}")
endif()
if(PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS)
    string(APPEND _consumer_linker_flags
        " ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
endif()
list(APPEND _consumer_configure_args
    "-DCMAKE_CXX_FLAGS=${_consumer_cxx_flags}"
    "-DCMAKE_EXE_LINKER_FLAGS=${_consumer_linker_flags}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_consumer_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "Installed native-surface consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config "${_config}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed native-surface consumer build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

set(_consumer "${_build}/consumer")
if(WIN32)
    set(_consumer "${_build}/${_config}/consumer.exe")
endif()
execute_process(
    COMMAND "${_consumer}"
    RESULT_VARIABLE _run_result
    OUTPUT_VARIABLE _run_output
    ERROR_VARIABLE _run_error)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR
        "Installed native-surface consumer failed (${_run_result})\n"
        "${_run_output}\n${_run_error}")
endif()
