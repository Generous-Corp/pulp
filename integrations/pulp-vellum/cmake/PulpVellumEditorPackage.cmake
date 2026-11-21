# Generate a concrete pulp_add_plugin UI package for a plug-in root View type.
#
#
# In-tree, integrations/pulp-vellum defines pulp::vellum. An installed SDK
# exports it as Pulp::vellum through PulpTargets.cmake, which find_package(Pulp)
# loads after finding Vellum; the lowercase alias keeps one spelling working in
# both.
if(TARGET Pulp::vellum AND NOT TARGET pulp::vellum)
    add_library(pulp::vellum ALIAS Pulp::vellum)
endif()

# The shared libraries in `root`'s link closure: the Vellum dylibs an editor
# loads at run time. Static and interface targets are walked, not listed.
function(_pulp_vellum_runtime_libraries out_var root)
    set(_libraries "")
    set(_seen "")
    set(_pending "${root}")
    while(_pending)
        list(POP_FRONT _pending _item)
        if(_item MATCHES "^\\$<LINK_ONLY:(.+)>$")
            set(_item "${CMAKE_MATCH_1}")
        endif()
        if(NOT TARGET "${_item}")
            continue()
        endif()
        get_target_property(_aliased "${_item}" ALIASED_TARGET)
        if(_aliased)
            set(_item "${_aliased}")
        endif()
        if(_item IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_item}")
        get_target_property(_type "${_item}" TYPE)
        if(_type STREQUAL "SHARED_LIBRARY")
            list(APPEND _libraries "${_item}")
        endif()
        get_target_property(_links "${_item}" INTERFACE_LINK_LIBRARIES)
        if(_links)
            list(APPEND _pending ${_links})
        endif()
    endwhile()
    set(${out_var} "${_libraries}" PARENT_SCOPE)
endfunction()

function(pulp_add_vellum_editor_package package)
    cmake_parse_arguments(PARSE_ARGV 1 _vellum_pkg "" "VIEW_HEADER;VIEW_TYPE" "VIEW_INCLUDE_DIRS;SOURCES")
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
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}" ${_vellum_pkg_VIEW_INCLUDE_DIRS})
    target_link_libraries("${_package_target}" INTERFACE
        ${_pulp_vellum_target} Vellum::MirrorView_core)
    # View sources compile in each target that links the package, where
    # Vellum's include paths are present; the plug-in's audio core never sees
    # them.
    foreach(_source IN LISTS _vellum_pkg_SOURCES)
        cmake_path(ABSOLUTE_PATH _source BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            NORMALIZE OUTPUT_VARIABLE _source_path)
        target_sources("${_package_target}" INTERFACE "${_source_path}")
    endforeach()
    # The editor loads Vellum's dylibs; each plug-in module carries copies so
    # the bundle does not depend on where the Vellum SDK was installed.
    _pulp_vellum_runtime_libraries(_runtime_libraries "${_package_target}")
    set_target_properties("${_package_target}" PROPERTIES
        PULP_EDITOR_FACTORY_FUNCTION "pulp::vellum::generated::${_package_id}"
        PULP_EDITOR_FACTORY_HEADER "pulp-vellum/editor-packages/${_package_id}.hpp"
        PULP_EDITOR_RUNTIME_LIBRARIES "${_runtime_libraries}")
    unset(_pulp_vellum_target)
endfunction()

# Built-in package for UI_SCRIPT. Keep its script library out of other Vellum editors.
function(_pulp_vellum_scripted_package context out_var)
    if(TARGET Pulp::vellum-scripted)
        set(_scripted Pulp::vellum-scripted)
    elseif(TARGET pulp::vellum-scripted)
        set(_scripted pulp::vellum-scripted)
    else()
        message(FATAL_ERROR
            "${context}: UI_ENGINE vellum requires PULP_ENABLE_PULP_VELLUM=ON "
            "and a Vellum SDK with Vellum::MirrorView_script")
    endif()
    if(NOT TARGET pulp-vellum-scripted-package)
        add_library(pulp-vellum-scripted-package INTERFACE)
        target_link_libraries(pulp-vellum-scripted-package INTERFACE "${_scripted}")
        _pulp_vellum_runtime_libraries(_runtime_libraries Vellum::MirrorView_script)
        set_target_properties(pulp-vellum-scripted-package PROPERTIES
            PULP_EDITOR_FACTORY_FUNCTION "pulp::vellum::scripted_editor_factory"
            PULP_EDITOR_FACTORY_HEADER "pulp/vellum/scripted_editor.hpp"
            PULP_EDITOR_RUNTIME_LIBRARIES "${_runtime_libraries}"
            PULP_VELLUM_SCRIPTED_EDITOR TRUE)
    endif()
    set(${out_var} pulp-vellum-scripted-package PARENT_SCOPE)
endfunction()
