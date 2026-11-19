# Generate a concrete pulp_add_plugin UI package for a plug-in root View type.
function(pulp_add_vellum_editor_package package)
    cmake_parse_arguments(PARSE_ARGV 1 _vellum_pkg "" "VIEW_HEADER;VIEW_TYPE" "VIEW_INCLUDE_DIRS")
    if(NOT _vellum_pkg_VIEW_HEADER OR NOT _vellum_pkg_VIEW_TYPE)
        message(FATAL_ERROR "${package}: VIEW_HEADER and VIEW_TYPE are required")
    endif()
    if(NOT TARGET Vellum::MirrorView_core)
        message(FATAL_ERROR "${package}: pulp_add_vellum_editor_package needs "
            "Vellum::MirrorView_core, which this Vellum SDK does not provide "
            "(the v0.2.0 release lacks it; use a Vellum SDK that ships the view layer)")
    endif()
    if(TARGET Pulp::vellum)
        set(_pulp_vellum_target Pulp::vellum)
    elseif(TARGET pulp::vellum)
        set(_pulp_vellum_target pulp::vellum)
    else()
        message(FATAL_ERROR "${package}: pulp_add_vellum_editor_package needs "
            "the Pulp SDK's optional pulp-vellum integration; configure Pulp with "
            "PULP_ENABLE_PULP_VELLUM=ON and install that SDK")
    endif()
    if(package MATCHES "::")
        string(REPLACE "::" "-" _package_target "${package}")
    else()
        set(_package_target "${package}")
    endif()
    if(TARGET "${package}" OR TARGET "${_package_target}")
        message(FATAL_ERROR "Vellum editor package target '${package}' already exists")
    endif()
    add_library("${_package_target}" INTERFACE)
    if(package MATCHES "::")
        add_library("${package}" ALIAS "${_package_target}")
    endif()

    string(MAKE_C_IDENTIFIER "${_package_target}" _package_id)
    set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/pulp-vellum/editor-packages")
    file(MAKE_DIRECTORY "${_generated_dir}")
    set(_generated_header "${_generated_dir}/${_package_id}.hpp")
    file(WRITE "${_generated_header}"
"#pragma once\n#include <pulp/vellum/editor.hpp>\n#include <${_vellum_pkg_VIEW_HEADER}>\n\nnamespace pulp::vellum::generated {\ninline ::pulp::format::EditorFactory* ${_package_id}() {\n    return ::pulp::vellum::editor_factory<${_vellum_pkg_VIEW_TYPE}>();\n}\n} // namespace pulp::vellum::generated\n")
    target_include_directories("${_package_target}" INTERFACE
        "${CMAKE_CURRENT_BINARY_DIR}" ${_vellum_pkg_VIEW_INCLUDE_DIRS})
    target_link_libraries("${_package_target}" INTERFACE
        ${_pulp_vellum_target} Vellum::MirrorView_core)
    set_target_properties("${_package_target}" PROPERTIES
        PULP_EDITOR_FACTORY_FUNCTION "pulp::vellum::generated::${_package_id}"
        PULP_EDITOR_FACTORY_HEADER "pulp-vellum/editor-packages/${_package_id}.hpp")
    unset(_pulp_vellum_target)
endfunction()
