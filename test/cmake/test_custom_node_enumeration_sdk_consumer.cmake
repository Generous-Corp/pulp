cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/custom-node-enumeration-sdk-consumer")
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
        "Custom-node SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

file(WRITE "${_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpCustomNodeEnumerationConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Pulp REQUIRED COMPONENTS host)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Pulp::host)
]=])

file(WRITE "${_source}/main.cpp" [=[
#include <pulp/host/signal_graph.hpp>

#include <utility>
#include <vector>

std::vector<pulp::host::CustomNodeTypeMetadata> detached_snapshot() {
    pulp::host::SignalGraph graph;
    pulp::host::CustomNodeType type;
    type.type_id = "installed.consumer.node";
    type.version = 7;
    type.num_input_ports = 1;
    type.num_output_ports = 2;
    type.default_name = "Installed Consumer";
    type.lowerable = true;
    type.create = [] { return static_cast<void*>(new int{0}); };
    type.destroy = [](void* instance) { delete static_cast<int*>(instance); };
    type.process_instance = [](void*, auto&, const auto&, int) {};
    type.baked_params.push_back({71, 0.0f, 2.0f, 1.0f});
    type.process_instance_baked_param =
        [](void*, auto&, const auto&, int, const auto&) {};
    if (!graph.register_custom_node_type(std::move(type))) return {};
    return graph.custom_node_types();
}

int main() {
    const auto metadata = detached_snapshot();
    if (metadata.size() != 1) return 1;
    const auto& row = metadata.front();
    if (row.type_id != "installed.consumer.node" || row.version != 7 ||
        row.num_input_ports != 1 || row.num_output_ports != 2 ||
        row.default_name != "Installed Consumer" || !row.lowerable ||
        row.baked_params.size() != 1 || row.baked_params[0].id != 71) {
        return 2;
    }
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
        "Installed custom-node consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config "${_config}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed custom-node consumer build failed (${_build_result})\n"
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
        "Installed custom-node consumer failed (${_run_result})\n"
        "${_run_output}\n${_run_error}")
endif()
