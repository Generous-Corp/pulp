cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/sample-kernel-sdk-consumer")
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
        "Sample-kernel SDK staging failed (${_install_result})\n"
        "${_install_output}\n${_install_error}")
endif()

file(WRITE "${_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpSampleKernelConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Pulp REQUIRED COMPONENTS host)
add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE Pulp::host)
]=])

file(WRITE "${_source}/main.cpp" [=[
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

#include <cstdint>
#include <type_traits>

using namespace pulp::host;

static_assert(SampleKernelDescriptor::kAbiVersion == 1);
static_assert(static_cast<std::uint8_t>(SampleKernelConfigKind::Invalid) == 0);
static_assert(std::is_same_v<SampleKernelResetFn, void (*)(void*) noexcept>);

void scalar_copy(void*, const PreparedSampleKernelConfig&,
                 const SampleFrameContext&, const float* input,
                 float* output) noexcept {
    output[0] = input[0];
}

int main() {
    SignalGraph graph;
    auto edit = graph.begin_prepared_topology_edit();
    CustomNodeType custom;
    custom.type_id = "sdk.consumer.scalar";
    custom.num_input_ports = 1;
    custom.num_output_ports = 1;
    SampleKernelDescriptor scalar;
    scalar.type_id = custom.type_id;
    scalar.num_input_ports = 1;
    scalar.num_output_ports = 1;
    scalar.authored_config_kind = SampleKernelConfigKind::None;
    scalar.process = scalar_copy;
    scalar.metadata.category = "test";
    if (!edit->register_custom_node_type(custom, scalar)) return 1;
    if (!register_builtin_sample_region_types(*edit)) return 1;
    if (edit->custom_node_type_count() != 8) return 2;
    if (edit->prepare(48000.0, 64) !=
        SignalGraph::PreparedTopologyEdit::Result::Prepared) return 3;
    if (edit->commit() !=
        SignalGraph::PreparedTopologyEdit::Result::Committed) return 4;
    const auto* delay = graph.sample_kernel_type("pulp.core.unit-delay", 1);
    if (!delay || delay->state_size != sizeof(float) ||
        delay->state_alignment != alignof(float)) return 5;
    if (!graph.sample_kernel_type("sdk.consumer.scalar", 1)) return 6;
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
        "Installed sample-kernel consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_build}" --config "${_config}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "Installed sample-kernel consumer build failed (${_build_result})\n"
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
        "Installed sample-kernel consumer failed (${_run_result})\n"
        "${_run_output}\n${_run_error}")
endif()
