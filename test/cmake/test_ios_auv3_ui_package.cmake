# pulp_add_ios_auv3(... UI <package>) at configure level. Each case is its own
# fixture project, with the heavyweight iOS target replaced by a stub, and is
# expected to configure (or to fail with a named message).
if(NOT DEFINED PULP_SOURCE_DIR OR NOT DEFINED FIXTURE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR and FIXTURE_DIR are required")
endif()
file(REMOVE_RECURSE "${FIXTURE_DIR}")

function(_run_case name body expect_ok expect_regex)
    set(dir "${FIXTURE_DIR}/${name}")
    file(MAKE_DIRECTORY "${dir}/src")
    file(WRITE "${dir}/src/main.cpp" "int main() { return 0; }\n")
    file(WRITE "${dir}/src/CMakeLists.txt" "cmake_minimum_required(VERSION 3.24)
project(IosAuv3Ui LANGUAGES CXX)
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpControlShipping.cmake\")
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpAuv3.cmake\")
include(\"${PULP_SOURCE_DIR}/tools/cmake/PulpEditorPackage.cmake\")
add_library(pulp-format-fixture INTERFACE)
set(_PULP_FORMAT_TARGET pulp-format-fixture)
set(PULP_IOS TRUE)
function(_pulp_metadata_require_fourcc)
endfunction()
function(_pulp_add_auv3 target)
    add_executable(\${target}_AUv3 main.cpp)
    _pulp_attach_control_shipping(\${target} \${target}_AUv3 AUv3Extension)
endfunction()
add_library(good-ui INTERFACE)
set_target_properties(good-ui PROPERTIES
    PULP_EDITOR_FACTORY_FUNCTION \"fixture::make_factory\"
    PULP_EDITOR_FACTORY_HEADER \"fixture_factory.hpp\")
macro(add_ext)
    pulp_add_ios_auv3(NAME Ext BUNDLE_ID com.pulp.test.ext MANUFACTURER Pulp
        MANUFACTURER_CODE Pulp SUBTYPE_CODE Fxtr VERSION 1.0.0 \${ARGN})
endmacro()
${body}
")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -S "${dir}/src" -B "${dir}/build"
        RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expect_ok AND NOT result EQUAL 0)
        message(FATAL_ERROR "case ${name}: expected to configure:\n${out}${err}")
    endif()
    if(NOT expect_ok AND result EQUAL 0)
        message(FATAL_ERROR "case ${name}: expected a refusal but it configured:\n${out}")
    endif()
    # CMake wraps long messages; compare on one line.
    string(REGEX REPLACE "[ \n]+" " " flat "${out}${err}")
    if(expect_regex AND NOT flat MATCHES "${expect_regex}")
        message(FATAL_ERROR "case ${name}: output does not match '${expect_regex}':\n${out}${err}")
    endif()
endfunction()

# A UI package attaches to the AUv3 target: property, generated hook source, link.
_run_case(with_ui "
add_ext(UI good-ui)
get_target_property(pkg Ext_AUv3 PULP_UI_PACKAGE)
if(NOT pkg STREQUAL \"good-ui\")
    message(FATAL_ERROR \"PULP_UI_PACKAGE is [\${pkg}]\")
endif()
get_target_property(srcs Ext_AUv3 SOURCES)
if(NOT \"\${srcs}\" MATCHES \"pulp_editor_package/Ext.cpp\")
    message(FATAL_ERROR \"hook source not attached: \${srcs}\")
endif()
get_target_property(libs Ext_AUv3 LINK_LIBRARIES)
if(NOT \"good-ui\" IN_LIST libs)
    message(FATAL_ERROR \"package not linked: \${libs}\")
endif()
file(READ \"\${CMAKE_CURRENT_BINARY_DIR}/pulp_editor_package/Ext.cpp\" hook)
if(NOT hook MATCHES \"fixture::make_factory\")
    message(FATAL_ERROR \"hook does not call the package factory\")
endif()
message(STATUS \"CASE-OK with_ui\")
" TRUE "CASE-OK with_ui")

# Without UI the extension is built exactly as before: nothing attached.
_run_case(without_ui "
add_ext()
get_target_property(pkg Ext_AUv3 PULP_UI_PACKAGE)
if(pkg)
    message(FATAL_ERROR \"unexpected PULP_UI_PACKAGE [\${pkg}]\")
endif()
if(EXISTS \"\${CMAKE_CURRENT_BINARY_DIR}/pulp_editor_package\")
    message(FATAL_ERROR \"a hook source was generated without UI\")
endif()
get_target_property(libs Ext_AUv3 LINK_LIBRARIES)
if(\"good-ui\" IN_LIST libs)
    message(FATAL_ERROR \"a UI package was linked without UI\")
endif()
message(STATUS \"CASE-OK without_ui\")
" TRUE "CASE-OK without_ui")

# UI none behaves like no UI.
_run_case(ui_none "
add_ext(UI none)
if(EXISTS \"\${CMAKE_CURRENT_BINARY_DIR}/pulp_editor_package\")
    message(FATAL_ERROR \"UI none generated a hook source\")
endif()
message(STATUS \"CASE-OK ui_none\")
" TRUE "CASE-OK ui_none")

# A UI that is not a target is refused, never silently dropped.
_run_case(missing_target "add_ext(UI no-such-package)" FALSE
    "pulp_add_ios_auv3\\(Ext\\): UI 'no-such-package' is not a target")

# A package that needs shared runtime libraries cannot load in an app extension.
_run_case(runtime_libraries "
set_property(TARGET good-ui PROPERTY PULP_EDITOR_RUNTIME_LIBRARIES some-shared-lib)
add_ext(UI good-ui)
" FALSE "cannot load loose shared libraries")

# A Vellum package on iOS with no iOS slice of the Vellum SDK says so.
_run_case(vellum_no_ios_slice "
include(\"${PULP_SOURCE_DIR}/integrations/pulp-vellum/cmake/PulpVellumEditorPackage.cmake\")
pulp_add_vellum_editor_package(Ext_VellumEditor VIEW_HEADER v.hpp VIEW_TYPE t::V)
" FALSE "has no iOS slice")
