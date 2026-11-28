#[[
Contract test for the generated standalone panel registrar
(_pulp_add_standalone_panel_registrar in tools/cmake/PulpAppTargets.cmake).

An integration that ships in the SDK declares its panel provider as the
exported target property PULP_STANDALONE_PANEL_PROVIDER. Two halves:

  1. The property survives `install(EXPORT)`: a throwaway library that sets it
     (with EXPORT_PROPERTIES, as pulp-vellum-audio does) is installed, and the
     exported targets file must carry it.
  2. A consumer that only has the imported target, and never runs the
     integration's CMakeLists.txt, announces it the way PulpConfig.cmake does
     (_pulp_announce_standalone_panel_provider) and still gets a registrar that
     includes the provider's header and calls its function. A consumer with no
     provider must get no registrar.

  -DPULP_SOURCE_DIR  the source tree
  -DFIXTURE_DIR      scratch directory, removed and recreated
]]
foreach(_input PULP_SOURCE_DIR FIXTURE_DIR)
    if(NOT DEFINED ${_input} OR "${${_input}}" STREQUAL "")
        message(FATAL_ERROR "${_input} is required")
    endif()
endforeach()
set(_app_targets "${PULP_SOURCE_DIR}/tools/cmake/PulpAppTargets.cmake")
if(NOT EXISTS "${_app_targets}")
    message(FATAL_ERROR "missing ${_app_targets}")
endif()
file(REMOVE_RECURSE "${FIXTURE_DIR}")

function(_run name)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "${name} failed (${_rc}):\n${_out}${_err}")
    endif()
endfunction()

# Half 1: the exporting side.
set(_exporter "${FIXTURE_DIR}/exporter")
file(MAKE_DIRECTORY "${_exporter}")
file(WRITE "${_exporter}/provider.cpp" "int provider_symbol() { return 1; }\n")
file(WRITE "${_exporter}/CMakeLists.txt" [[
cmake_minimum_required(VERSION 3.24)
project(RegistrarExporter CXX)
add_library(pulp-fake-panels STATIC provider.cpp)
set_target_properties(pulp-fake-panels PROPERTIES EXPORT_NAME fake-panels
    PULP_STANDALONE_PANEL_PROVIDER "fake/panel.hpp|fake::install_panel"
    EXPORT_PROPERTIES PULP_STANDALONE_PANEL_PROVIDER)
install(TARGETS pulp-fake-panels EXPORT FakeTargets ARCHIVE DESTINATION lib)
install(EXPORT FakeTargets NAMESPACE Pulp:: DESTINATION lib/cmake/Fake)
]])
_run("exporter configure" "${CMAKE_COMMAND}" -S "${_exporter}" -B "${_exporter}/build")
_run("exporter build" "${CMAKE_COMMAND}" --build "${_exporter}/build")
_run("exporter install" "${CMAKE_COMMAND}" --install "${_exporter}/build" --prefix "${FIXTURE_DIR}/prefix")
file(READ "${FIXTURE_DIR}/prefix/lib/cmake/Fake/FakeTargets.cmake" _exported)
if(NOT _exported MATCHES "PULP_STANDALONE_PANEL_PROVIDER \"fake/panel.hpp\\|fake::install_panel\"")
    message(FATAL_ERROR "the exported targets file does not carry the provider property:\n${_exported}")
endif()

# Half 2: the consuming side, against the imported target only.
function(_consume dir declares expect_registrar)
    set(_consumer "${FIXTURE_DIR}/${dir}")
    file(MAKE_DIRECTORY "${_consumer}")
    file(WRITE "${_consumer}/main.cpp" "int main() { return 0; }\n")
    if(declares)
        set(_import [[
include("${FAKE_PREFIX}/lib/cmake/Fake/FakeTargets.cmake")
# PulpConfig.cmake aliases each imported target to its lower-case name.
add_library(pulp::fake-panels INTERFACE IMPORTED)
_pulp_announce_standalone_panel_provider(Pulp::fake-panels)
]])
    else()
        set(_import "")
    endif()
    file(WRITE "${_consumer}/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)
project(RegistrarConsumer CXX)
include(\"${_app_targets}\")
${_import}
add_executable(app main.cpp)
_pulp_add_standalone_panel_registrar(app)
")
    _run("${dir} configure" "${CMAKE_COMMAND}" -S "${_consumer}" -B "${_consumer}/build"
        -DFAKE_PREFIX=${FIXTURE_DIR}/prefix)
    set(_generated "${_consumer}/build/app_standalone_panel_registrar.cpp")
    if(expect_registrar)
        if(NOT EXISTS "${_generated}")
            message(FATAL_ERROR "${dir}: no registrar was generated")
        endif()
        file(READ "${_generated}" _text)
        foreach(_needle "#include <fake/panel.hpp>" "fake::install_panel();"
                "void install_standalone_panels()")
            string(FIND "${_text}" "${_needle}" _at)
            if(_at EQUAL -1)
                message(FATAL_ERROR "${dir}: registrar lacks '${_needle}':\n${_text}")
            endif()
        endforeach()
    elseif(EXISTS "${_generated}")
        message(FATAL_ERROR "${dir}: a registrar was generated with no provider declared")
    endif()
endfunction()

_consume(consumer-declared TRUE TRUE)
_consume(consumer-none FALSE FALSE)
message(STATUS "standalone-panel-registrar: ok")
