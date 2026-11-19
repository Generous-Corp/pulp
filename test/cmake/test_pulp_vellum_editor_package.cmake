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

# Without Vellum's view layer, the helper must stop
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

# Installed SDK: pulp::vellum comes from PulpVellumTargets.cmake beside the
# helper, which may load only once find_package(Vellum) has defined its targets.
set(_installed "${FIXTURE_DIR}/installed/lib/cmake/Pulp")
file(MAKE_DIRECTORY "${_installed}")
file(COPY "${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake"
    DESTINATION "${_installed}")
file(WRITE "${_installed}/PulpVellumTargets.cmake"
    "add_library(Pulp::vellum INTERFACE IMPORTED)\n"
    "set_target_properties(Pulp::vellum PROPERTIES INTERFACE_LINK_LIBRARIES Vellum::Value)\n")
file(MAKE_DIRECTORY "${FIXTURE_DIR}/installed-consumer")
file(WRITE "${FIXTURE_DIR}/installed-consumer/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\nproject(VellumInstalled CXX)\n"
    "add_library(vellum-value INTERFACE)\nadd_library(Vellum::Value ALIAS vellum-value)\n"
    "add_library(vellum-view INTERFACE)\nadd_library(Vellum::MirrorView_core ALIAS vellum-view)\n"
    "include(\"${_installed}/PulpVellumEditorPackage.cmake\")\n"
    "if(NOT TARGET pulp::vellum)\n message(FATAL_ERROR \"installed pulp::vellum was not loaded\")\nendif()\n"
    "get_target_property(A pulp::vellum ALIASED_TARGET)\n"
    "if(NOT A STREQUAL \"Pulp::vellum\")\n message(FATAL_ERROR \"pulp::vellum aliases \${A}\")\nendif()\n"
    "pulp_add_vellum_editor_package(InstalledEditor VIEW_HEADER root.hpp VIEW_TYPE test::RootView)\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/installed-consumer"
    -B "${FIXTURE_DIR}/installed-consumer-build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed-SDK Vellum editor package configure failed:\n${output}${error}")
endif()
message(STATUS "Vellum editor package loads the installed pulp::vellum")

file(MAKE_DIRECTORY "${FIXTURE_DIR}/installed-novellum")
file(WRITE "${FIXTURE_DIR}/installed-novellum/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\nproject(VellumInstalledNoVellum CXX)\n"
    "include(\"${_installed}/PulpVellumEditorPackage.cmake\")\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/installed-novellum"
    -B "${FIXTURE_DIR}/installed-novellum-build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(result EQUAL 0)
    message(FATAL_ERROR "installed pulp::vellum loaded before find_package(Vellum)")
endif()
string(REGEX REPLACE "[ \t\r\n]+" " " error_flat "${error}")
if(NOT error_flat MATCHES "call find_package\\(Vellum\\) before including it")
    message(FATAL_ERROR "missing-Vellum error was not the helper's message:\n${output}${error}")
endif()
message(STATUS "Vellum editor package refuses to load before find_package(Vellum)")
