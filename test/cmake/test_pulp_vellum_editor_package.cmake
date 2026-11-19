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
