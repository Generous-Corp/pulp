# Installed-SDK consumer of the audio displays.
#
# Stages `cmake --install` of this build, then configures and builds a project
# outside the source tree against the prefix with find_package(Pulp). The
# consumer design-imports a meter node and must get a real Meter, after the
# one explicit registration call the installed SDK documents (the build-tree
# convenience shim is not exported). The installed view-audio archive and the
# consumer image must both carry register_audio_widgets.
cmake_minimum_required(VERSION 3.24)

if(NOT DEFINED PULP_BUILD_DIR)
    message(FATAL_ERROR "PULP_BUILD_DIR is required")
endif()

set(_fixture_root "${PULP_BUILD_DIR}/view-audio-sdk-consumer")
set(_prefix "${_fixture_root}/prefix")
set(_consumer_source "${_fixture_root}/src")
set(_consumer_build "${_fixture_root}/build")
file(REMOVE_RECURSE "${_fixture_root}")
file(MAKE_DIRECTORY "${_consumer_source}")

set(_producer_config Release)
if(PULP_PARENT_BUILD_TYPE)
    set(_producer_config "${PULP_PARENT_BUILD_TYPE}")
endif()
string(TOLOWER "${_producer_config}" _producer_config_lower)

execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${PULP_BUILD_DIR}"
            --prefix "${_prefix}" --config "${_producer_config}"
    RESULT_VARIABLE _install_result
    OUTPUT_VARIABLE _install_output
    ERROR_VARIABLE _install_error)
if(NOT _install_result EQUAL 0)
    message(FATAL_ERROR
        "view-audio SDK staging failed (${_install_result})\n${_install_output}\n${_install_error}")
endif()

file(GLOB_RECURSE _installed_view_audio_archives
    "${_prefix}/lib*/*pulp-view-audio.a" "${_prefix}/lib*/*pulp-view-audio.lib")
if(NOT _installed_view_audio_archives)
    message(FATAL_ERROR "the installed SDK has no pulp-view-audio archive under ${_prefix}")
endif()
list(GET _installed_view_audio_archives 0 _installed_view_audio_archive)

file(WRITE "${_consumer_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.24)
project(PulpViewAudioSdkConsumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Pulp CONFIG REQUIRED COMPONENTS view view-audio design-import)
add_executable(pulp-view-audio-sdk-consumer main.cpp)
target_link_libraries(pulp-view-audio-sdk-consumer PRIVATE Pulp::view)
]=])

file(WRITE "${_consumer_source}/main.cpp" [=[
#include <pulp/view/data_display.hpp>
#include <pulp/view/design_import.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/register_audio_widgets.hpp>
#include <pulp/view/view.hpp>

#include <cstdio>
#include <string>

namespace {
pulp::view::View* find_anchor(pulp::view::View& root, const std::string& anchor) {
    if (root.anchor_id() == anchor) return &root;
    for (std::size_t i = 0; i < root.child_count(); ++i)
        if (auto* hit = find_anchor(*root.child_at(i), anchor)) return hit;
    return nullptr;
}
} // namespace

int main() {
    using namespace pulp::view;
    if (has_data_display("pulp.level")) {
        std::fprintf(stderr, "a display was registered before main(); the installed SDK must not ship the shim\n");
        return 2;
    }
    register_audio_widgets();

    DesignIR ir;
    ir.root.type = "frame";
    ir.root.stable_anchor_id = "panel";
    ir.root.style.width = 120.0f;
    ir.root.style.height = 120.0f;
    IRNode meter;
    meter.type = "frame";
    meter.stable_anchor_id = "output-meter";
    meter.style.width = 24.0f;
    meter.style.height = 100.0f;
    meter.audio_widget = AudioWidgetType::meter;
    ir.root.children.push_back(meter);

    auto root = build_native_view_tree(ir, ir.asset_manifest);
    if (!root) return 3;
    auto* view = find_anchor(*root, "output-meter");
    if (!view) return 4;
    const auto type = ViewInspector::type_name(*view);
    if (type != "Meter") {
        std::fprintf(stderr, "imported meter is a %s, not a Meter\n", type.c_str());
        return 5;
    }
    if (!dynamic_cast<LevelDisplay*>(view)) return 6;
    std::printf("installed SDK: imported meter is a Meter\n");
    return 0;
}
]=])

set(_consumer_configure_args
    -S "${_consumer_source}"
    -B "${_consumer_build}"
    "-DCMAKE_PREFIX_PATH=${_prefix}"
    "-DPulp_DIR=${_prefix}/lib/cmake/Pulp"
    "-DCMAKE_BUILD_TYPE=${_producer_config}")
if(_producer_config_lower STREQUAL "debug")
    list(APPEND _consumer_configure_args -DPULP_ALLOW_DEBUG_SDK=ON)
endif()
if(PULP_PARENT_OSX_ARCHITECTURES)
    list(APPEND _consumer_configure_args
        "-DCMAKE_OSX_ARCHITECTURES=${PULP_PARENT_OSX_ARCHITECTURES}")
endif()
set(_consumer_cxx_flags "${PULP_PARENT_CXX_FLAGS}")
set(_consumer_linker_flags "${PULP_PARENT_EXE_LINKER_FLAGS}")
if(PULP_PARENT_INSTRUMENTATION_CXX_FLAGS)
    string(APPEND _consumer_cxx_flags " ${PULP_PARENT_INSTRUMENTATION_CXX_FLAGS}")
endif()
if(PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS)
    string(APPEND _consumer_linker_flags " ${PULP_PARENT_INSTRUMENTATION_LINKER_FLAGS}")
endif()
string(STRIP "${_consumer_cxx_flags}" _consumer_cxx_flags)
string(STRIP "${_consumer_linker_flags}" _consumer_linker_flags)
if(PULP_PARENT_SANITIZER)
    list(APPEND _consumer_configure_args "-DPULP_SANITIZER=${PULP_PARENT_SANITIZER}")
endif()
if(_consumer_cxx_flags)
    list(APPEND _consumer_configure_args "-DCMAKE_CXX_FLAGS=${_consumer_cxx_flags}")
endif()
if(_consumer_linker_flags)
    list(APPEND _consumer_configure_args "-DCMAKE_EXE_LINKER_FLAGS=${_consumer_linker_flags}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" ${_consumer_configure_args}
    RESULT_VARIABLE _configure_result
    OUTPUT_VARIABLE _configure_output
    ERROR_VARIABLE _configure_error)
if(NOT _configure_result EQUAL 0)
    message(FATAL_ERROR
        "view-audio SDK consumer configure failed (${_configure_result})\n"
        "${_configure_output}\n${_configure_error}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${_consumer_build}"
            --config "${_producer_config}" --parallel 2
    RESULT_VARIABLE _build_result
    OUTPUT_VARIABLE _build_output
    ERROR_VARIABLE _build_error)
if(NOT _build_result EQUAL 0)
    message(FATAL_ERROR
        "view-audio SDK consumer build failed (${_build_result})\n"
        "${_build_output}\n${_build_error}")
endif()

set(_executable_suffix "")
if(WIN32)
    set(_executable_suffix ".exe")
endif()
set(_executable "${_consumer_build}/pulp-view-audio-sdk-consumer${_executable_suffix}")
if(NOT EXISTS "${_executable}")
    set(_executable
        "${_consumer_build}/${_producer_config}/pulp-view-audio-sdk-consumer${_executable_suffix}")
endif()
execute_process(COMMAND "${_executable}"
    RESULT_VARIABLE _run_result OUTPUT_VARIABLE _run_output ERROR_VARIABLE _run_error)
if(NOT _run_result EQUAL 0)
    message(FATAL_ERROR
        "view-audio SDK consumer exited ${_run_result}\n${_run_output}\n${_run_error}")
endif()
message(STATUS "${_run_output}")

# nm is the instrument for the archive and image checks; MSVC builds have none.
if(PULP_NM AND NOT WIN32)
    foreach(_binary "${_installed_view_audio_archive}" "${_executable}")
        execute_process(COMMAND "${PULP_NM}" "${_binary}"
            RESULT_VARIABLE _nm_result OUTPUT_VARIABLE _symbols ERROR_VARIABLE _nm_error)
        if(NOT _nm_result EQUAL 0 OR _symbols STREQUAL "")
            message(FATAL_ERROR "nm could not read ${_binary}: ${_nm_error}")
        endif()
        if(NOT _symbols MATCHES "4pulp4view22register_audio_widgetsEv")
            message(FATAL_ERROR "register_audio_widgets is not in ${_binary}")
        endif()
        if(NOT _symbols MATCHES "4pulp4view21register_data_display")
            message(FATAL_ERROR "nm output for ${_binary} lacks the display registry; the match is not reading it")
        endif()
    endforeach()
    message(STATUS "register_audio_widgets is in ${_installed_view_audio_archive} and ${_executable}")
endif()
