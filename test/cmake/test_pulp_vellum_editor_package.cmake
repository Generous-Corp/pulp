if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()
file(MAKE_DIRECTORY "${FIXTURE_DIR}/src")
file(WRITE "${FIXTURE_DIR}/src/root.hpp" [=[
#pragma once
namespace pulp::format { class EditorFactory; }
namespace test { struct RootView {}; }
namespace pulp::vellum { template <typename T> pulp::format::EditorFactory* editor_factory(); }
]=])
file(WRITE "${FIXTURE_DIR}/src/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\nproject(VellumPackage CXX)\n"
    "add_library(pulp-vellum INTERFACE)\nadd_library(pulp::vellum ALIAS pulp-vellum)\n"
    "add_library(vellum-view INTERFACE)\nadd_library(Vellum::MirrorView_core ALIAS vellum-view)\n"
    "include(\"${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake\")\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpEditorPackage.cmake\")\n"
    "pulp_add_vellum_editor_package(pulp::vellum-editor VIEW_HEADER root.hpp VIEW_TYPE test::RootView)\n"
    "get_target_property(F pulp::vellum-editor PULP_EDITOR_FACTORY_FUNCTION)\n"
    "get_target_property(H pulp::vellum-editor PULP_EDITOR_FACTORY_HEADER)\n"
    "if(NOT F STREQUAL \"pulp::vellum::generated::pulp_vellum_editor\")\n message(FATAL_ERROR \"wrong factory: \${F}\")\nendif()\n"
    "if(NOT H)\n message(FATAL_ERROR \"missing generated header\")\nendif()\n"
    "add_library(plugin INTERFACE)\ntarget_link_libraries(plugin INTERFACE pulp::vellum-editor)\n"
    "_pulp_resolve_ui_package(Plugin pulp::vellum-editor PF PH)\n"
    "if(NOT PF OR NOT PH)\n message(FATAL_ERROR \"UI package did not resolve\")\nendif()\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/src" -B "${FIXTURE_DIR}/build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Vellum editor package configure failed:\n${output}${error}")
endif()
message(STATUS "Vellum editor package configure passed")

# Without Vellum's view layer (the pinned v0.2.0 SDK), the helper must stop
# with a clear message instead of a generate-time missing-target error.
file(MAKE_DIRECTORY "${FIXTURE_DIR}/noview")
file(WRITE "${FIXTURE_DIR}/noview/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\nproject(VellumNoView CXX)\n"
    "add_library(pulp-vellum INTERFACE)\nadd_library(pulp::vellum ALIAS pulp-vellum)\n"
    "include(\"${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake\")\n"
    "pulp_add_vellum_editor_package(pulp::vellum-editor VIEW_HEADER root.hpp VIEW_TYPE test::RootView)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/noview" -B "${FIXTURE_DIR}/noview-build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "Vellum editor package configured without Vellum::MirrorView_core")
endif()
string(REGEX REPLACE "[ \t\r\n]+" " " error_flat "${error}")
if(NOT error_flat MATCHES "needs Vellum::MirrorView_core")
    message(FATAL_ERROR "missing-view error was not the helper's message:\n${output}${error}")
endif()
message(STATUS "Vellum editor package refuses an SDK without the view layer")

# An installed SDK that did not build the optional Pulp bridge must fail with
# the same actionable explanation instead of generating an unresolved target.
file(MAKE_DIRECTORY "${FIXTURE_DIR}/nopulp")
file(WRITE "${FIXTURE_DIR}/nopulp/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\nproject(VellumNoPulp CXX)\n"
    "add_library(Vellum::MirrorView_core INTERFACE IMPORTED)\n"
    "include(\"${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake\")\n"
    "pulp_add_vellum_editor_package(pulp::vellum-editor VIEW_HEADER root.hpp VIEW_TYPE test::RootView)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/nopulp" -B "${FIXTURE_DIR}/nopulp-build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "Vellum editor package configured without pulp-vellum")
endif()
string(REGEX REPLACE "[ \t\r\n]+" " " error_flat "${error}")
if(NOT error_flat MATCHES "optional pulp-vellum integration")
    message(FATAL_ERROR "missing-pulp error was not the helper's message:\n${output}${error}")
endif()
message(STATUS "Vellum editor package refuses an SDK without the Pulp bridge")
