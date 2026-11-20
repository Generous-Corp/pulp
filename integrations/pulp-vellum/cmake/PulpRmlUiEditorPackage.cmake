# Generate an RmlUi editor package backed by Vellum's renderer-neutral native host.
function(pulp_add_rmlui_editor_package package)
    cmake_parse_arguments(PARSE_ARGV 1 _rmlui_pkg ""
        "DOCUMENT;STYLESHEET" "ASSET_DIRS")
    if(NOT _rmlui_pkg_DOCUMENT OR NOT _rmlui_pkg_STYLESHEET)
        message(FATAL_ERROR "${package}: pulp_add_rmlui_editor_package needs DOCUMENT and STYLESHEET")
    endif()
    if(NOT EXISTS "${_rmlui_pkg_DOCUMENT}" OR IS_DIRECTORY "${_rmlui_pkg_DOCUMENT}")
        message(FATAL_ERROR "${package}: RmlUi document is not a regular file: ${_rmlui_pkg_DOCUMENT}")
    endif()
    if(NOT EXISTS "${_rmlui_pkg_STYLESHEET}" OR IS_DIRECTORY "${_rmlui_pkg_STYLESHEET}")
        message(FATAL_ERROR "${package}: RmlUi stylesheet is not a regular file: ${_rmlui_pkg_STYLESHEET}")
    endif()
    foreach(_root IN LISTS _rmlui_pkg_ASSET_DIRS)
        if(NOT IS_DIRECTORY "${_root}")
            message(FATAL_ERROR "${package}: RmlUi asset root is not a directory: ${_root}")
        endif()
    endforeach()
    if(TARGET "${package}")
        message(FATAL_ERROR "RmlUi editor package target '${package}' already exists")
    endif()

    if(NOT TARGET VellumRmlUi::Core OR NOT TARGET VellumRmlUi::SoftRenderer OR
       NOT TARGET VellumRmlUi::NativeHost)
        find_package(VellumRmlUi CONFIG REQUIRED COMPONENTS Core SoftRenderer NativeHost)
    endif()
    if(package MATCHES "::")
        string(REPLACE "::" "-" _package_target "${package}")
    else()
        set(_package_target "${package}")
    endif()
    add_library("${_package_target}" INTERFACE)
    if(package MATCHES "::")
        add_library("${package}" ALIAS "${_package_target}")
    endif()

    string(MAKE_C_IDENTIFIER "${_package_target}" _package_id)
    set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/pulp-vellum/rmlui-editor-packages")
    file(MAKE_DIRECTORY "${_generated_dir}")
    set(_generated_header "${_generated_dir}/${_package_id}.hpp")
    set(_document "${_rmlui_pkg_DOCUMENT}")
    set(_stylesheet "${_rmlui_pkg_STYLESHEET}")
    string(REPLACE "\\" "\\\\" _document "${_document}")
    string(REPLACE "\\" "\\\\" _stylesheet "${_stylesheet}")
    set(_roots "")
    foreach(_root IN LISTS _rmlui_pkg_ASSET_DIRS)
        string(REPLACE "\\" "\\\\" _root "${_root}")
        string(APPEND _roots "        \"${_root}\",\n")
    endforeach()
    file(WRITE "${_generated_header}"
"#pragma once
#include <pulp/vellum/rmlui_editor.hpp>

namespace pulp::vellum::rmlui::generated {
inline ::pulp::format::EditorFactory* ${_package_id}() {
    return ::pulp::vellum::rmlui::editor_factory(
        ::pulp::vellum::rmlui::PackageAssets{
            \"${_document}\",\n            \"${_stylesheet}\",\n            {\n${_roots}            }});
}
} // namespace pulp::vellum::rmlui::generated
")
    target_include_directories("${_package_target}" INTERFACE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries("${_package_target}" INTERFACE
        pulp::vellum VellumRmlUi::NativeHost VellumRmlUi::Core VellumRmlUi::SoftRenderer)
    set_target_properties("${_package_target}" PROPERTIES
        PULP_EDITOR_FACTORY_FUNCTION "pulp::vellum::rmlui::generated::${_package_id}"
        PULP_EDITOR_FACTORY_HEADER "pulp-vellum/rmlui-editor-packages/${_package_id}.hpp"
        PULP_RMLUI_DOCUMENT "${_rmlui_pkg_DOCUMENT}"
        PULP_RMLUI_STYLESHEET "${_rmlui_pkg_STYLESHEET}"
        PULP_RMLUI_ASSET_DIRS "${_rmlui_pkg_ASSET_DIRS}")
endfunction()
