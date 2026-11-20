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
file(WRITE "${FIXTURE_DIR}/src/module.cpp" "int vellum_package_fixture_module() { return 0; }\n")
file(WRITE "${FIXTURE_DIR}/src/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\nproject(VellumPackage CXX)\n"
    "add_library(pulp-vellum INTERFACE)\nadd_library(pulp::vellum ALIAS pulp-vellum)\n"
    "add_library(Pulp::vellum ALIAS pulp-vellum)\n"
    "add_library(vellum-dylib SHARED IMPORTED)\n"
    "set_target_properties(vellum-dylib PROPERTIES IMPORTED_LOCATION \"${FIXTURE_DIR}/libvellum-fixture.dylib\")\n"
    "add_library(vellum-static STATIC IMPORTED)\n"
    "set_target_properties(vellum-static PROPERTIES IMPORTED_LOCATION \"${FIXTURE_DIR}/libvellum-fixture.a\" "
    "INTERFACE_LINK_LIBRARIES \"\$<LINK_ONLY:vellum-dylib>\")\n"
    "add_library(vellum-view INTERFACE)\nadd_library(Vellum::MirrorView_core ALIAS vellum-view)\n"
    "target_link_libraries(vellum-view INTERFACE vellum-static)\n"
    "include(\"${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake\")\n"
    "include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpEditorPackage.cmake\")\n"
    "pulp_add_vellum_editor_package(pulp::vellum-editor VIEW_HEADER root.hpp VIEW_TYPE test::RootView)\n"
    "get_target_property(F pulp::vellum-editor PULP_EDITOR_FACTORY_FUNCTION)\n"
    "get_target_property(H pulp::vellum-editor PULP_EDITOR_FACTORY_HEADER)\n"
    "if(NOT F STREQUAL \"pulp::vellum::generated::pulp_vellum_editor\")\n message(FATAL_ERROR \"wrong factory: \${F}\")\nendif()\n"
    "if(NOT H)\n message(FATAL_ERROR \"missing generated header\")\nendif()\n"
    "add_library(plugin INTERFACE)\ntarget_link_libraries(plugin INTERFACE pulp::vellum-editor)\n"
    "_pulp_resolve_ui_package(Plugin pulp::vellum-editor PF PH)\n"
    "if(NOT PF OR NOT PH)\n message(FATAL_ERROR \"UI package did not resolve\")\nendif()\n"
    # The package names the shared libraries in its link closure, reached
    # through a static library's LINK_ONLY edge, and nothing static.
    "get_target_property(R pulp::vellum-editor PULP_EDITOR_RUNTIME_LIBRARIES)\n"
    "if(NOT R STREQUAL \"vellum-dylib\")\n message(FATAL_ERROR \"runtime libraries: \${R}\")\nendif()\n"
    # A plug-in module stages them and finds them through @loader_path only.
    "add_library(Plugin_CLAP MODULE module.cpp)\n"
    "set(PULP_Plugin_UI_PACKAGE pulp::vellum-editor)\n"
    "_pulp_stage_ui_package_runtime(Plugin Plugin_CLAP)\n"
    "get_target_property(B Plugin_CLAP BUILD_WITH_INSTALL_RPATH)\n"
    "get_target_property(P Plugin_CLAP INSTALL_RPATH)\n"
    "get_target_property(S Plugin_CLAP PULP_UI_PACKAGE_RUNTIME_LIBRARIES)\n"
    "if(APPLE)\n set(O @loader_path)\nelse()\n set(O \"\$ORIGIN\")\nendif()\n"
    "if(NOT B OR NOT P STREQUAL O OR NOT S STREQUAL \"vellum-dylib\")\n"
    " message(FATAL_ERROR \"module staging: rpath-from-install=\${B} rpath=\${P} staged=\${S}\")\nendif()\n"
    "add_library(Plain_CLAP MODULE module.cpp)\n"
    "_pulp_stage_ui_package_runtime(Plain Plain_CLAP)\n"
    "get_target_property(B Plain_CLAP BUILD_WITH_INSTALL_RPATH)\n"
    "if(B)\n message(FATAL_ERROR \"a plug-in without a UI package was restaged\")\nendif()\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/src" -B "${FIXTURE_DIR}/build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Vellum editor package configure failed:\n${output}${error}")
endif()
message(STATUS "Vellum editor package configure passed, and its runtime libraries stage beside a module")

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

# Installed SDK: find_package(Pulp) has already imported Pulp::vellum from
# PulpTargets.cmake. The helper gives it the in-tree spelling and links it.
file(MAKE_DIRECTORY "${FIXTURE_DIR}/installed-consumer")
file(WRITE "${FIXTURE_DIR}/installed-consumer/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)\nproject(VellumInstalled CXX)\n"
    "add_library(Pulp::vellum INTERFACE IMPORTED)\n"
    "add_library(Vellum::MirrorView_core INTERFACE IMPORTED)\n"
    "include(\"${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake\")\n"
    "get_target_property(A pulp::vellum ALIASED_TARGET)\n"
    "if(NOT A STREQUAL \"Pulp::vellum\")\n message(FATAL_ERROR \"pulp::vellum aliases \${A}\")\nendif()\n"
    "pulp_add_vellum_editor_package(InstalledEditor VIEW_HEADER root.hpp VIEW_TYPE test::RootView)\n"
    "get_target_property(L InstalledEditor INTERFACE_LINK_LIBRARIES)\n"
    "if(NOT \"Pulp::vellum\" IN_LIST L OR NOT \"Vellum::MirrorView_core\" IN_LIST L)\n"
    " message(FATAL_ERROR \"the installed editor package links: \${L}\")\nendif()\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${FIXTURE_DIR}/installed-consumer"
    -B "${FIXTURE_DIR}/installed-consumer-build"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "installed-SDK Vellum editor package configure failed:\n${output}${error}")
endif()
message(STATUS "Vellum editor package uses the installed Pulp::vellum")

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
