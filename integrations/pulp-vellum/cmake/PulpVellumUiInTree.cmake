# PulpVellumUiInTree.cmake: Pulp's UI target names served from an installed
# Vellum SDK inside Pulp's own build tree.
#
# An installed SDK built with PULP_SDK_UI_FROM_VELLUM serves Pulp::view,
# Pulp::canvas, Pulp::render, Pulp::design-import, Pulp::view-audio and
# Pulp::view-tooling from Vellum (PulpVellumUiCompat.cmake, read by
# PulpConfig). PULP_UI_FROM_VELLUM_IN_TREE does the same for the source tree:
# Pulp's own UI directories are left out and the same names, plus their
# lower-case pulp:: aliases, come from the same mapping, so in-tree consumers
# link what an installed-SDK consumer links. It is off by default and changes
# nothing while off.
#
# The directories it leaves out are listed once, in
# PULP_VELLUM_IN_TREE_SKIPPED_UI_DIRS, each with the reason, so the list can
# only shrink or grow through a reviewed edit.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumUiCompat.cmake")

# <dir>|<reason>. Vellum provides what each directory provides.
set(PULP_VELLUM_IN_TREE_SKIPPED_UI_DIRS
    "core/canvas|Vellum::MirrorCanvas serves pulp::canvas"
    "core/view|Vellum::MirrorView_core and its script and import layers serve pulp::view; the editor bridge lives in core/editor_bridge"
    "core/timeline_view|its views build on pulp::view-core, which this mode does not define"
)

# The SDK targets the skipped directories define (or, for
# pulp-standalone-native, that link Pulp::view-native, which stays undefined in
# a Vellum-backed tree because no Vellum view is free of a script engine).
set(PULP_VELLUM_IN_TREE_UI_TARGETS
    pulp-canvas pulp-design-ir pulp-view-core pulp-view-tooling pulp-view-platform
    pulp-view-native pulp-view pulp-view-script pulp-design-import
    pulp-design-import-audio-pack pulp-view-webview pulp-view-webview-backend
    pulp-standalone-native)
# The header subsystems those directories install.
set(PULP_VELLUM_IN_TREE_UI_HEADER_SUBSYSTEMS canvas view)

# Sets `out` to TRUE when the in-tree Vellum mode leaves `dir` out.
function(pulp_vellum_in_tree_skips dir out)
    set(_skip FALSE)
    if(PULP_UI_FROM_VELLUM_IN_TREE)
        foreach(_entry IN LISTS PULP_VELLUM_IN_TREE_SKIPPED_UI_DIRS)
            string(REPLACE "|" ";" _fields "${_entry}")
            list(GET _fields 0 _dir)
            if(_dir STREQUAL dir)
                set(_skip TRUE)
                list(GET _fields 1 _reason)
                set_property(GLOBAL APPEND PROPERTY PULP_VELLUM_IN_TREE_SKIPPED "${dir}")
                message(STATUS "Pulp UI from Vellum (in tree): skipping ${dir}: ${_reason}")
            endif()
        endforeach()
    endif()
    set(${out} ${_skip} PARENT_SCOPE)
endfunction()

# Defines the Vellum-backed UI names for the whole source tree. Call it from the
# top-level directory, after integrations/, so the IMPORTED targets it defines
# are visible to every directory and the Pulp integration packages exist.
function(pulp_vellum_in_tree_define_ui_targets)
    if(TARGET pulp-view-core)
        message(FATAL_ERROR
            "PULP_UI_FROM_VELLUM_IN_TREE serves the UI names from Vellum, but "
            "pulp-view-core is defined in this tree: Pulp's own view and Vellum's "
            "cannot both back pulp::view")
    endif()
    # The mapping keys on the package names an installed SDK exports; in the
    # source tree those are aliases of the targets the build defines.
    foreach(_pair IN ITEMS
            "pulp-vellum|vellum" "pulp-vellum-audio|vellum-audio"
            "pulp-editor-bridge|editor-bridge" "pulp-gpu-device|gpu-device"
            "pulp-gpu-audio|gpu-audio")
        string(REPLACE "|" ";" _fields "${_pair}")
        list(GET _fields 0 _target)
        list(GET _fields 1 _name)
        if(TARGET "${_target}" AND NOT TARGET "Pulp::${_name}")
            add_library("Pulp::${_name}" ALIAS "${_target}")
        endif()
    endforeach()
    pulp_define_vellum_ui_compat_targets()
    # Every name must be Vellum-backed, and the lower-case alias in-tree
    # consumers spell must be that same target, not a leftover of Pulp's own.
    foreach(_name IN ITEMS canvas render design-import view view-audio view-tooling)
        if(NOT _name IN_LIST PULP_SDK_VELLUM_UI_COMPAT_TARGETS)
            message(FATAL_ERROR
                "PULP_UI_FROM_VELLUM_IN_TREE: Pulp::${_name} is not Vellum-backed. "
                "The Vellum SDK must export its backing targets, and "
                "PULP_ENABLE_PULP_VELLUM and PULP_ENABLE_PULP_VELLUM_AUDIO must be ON.")
        endif()
        get_target_property(_aliased "pulp::${_name}" ALIASED_TARGET)
        if(NOT _aliased STREQUAL "Pulp::${_name}")
            message(FATAL_ERROR
                "PULP_UI_FROM_VELLUM_IN_TREE: pulp::${_name} resolves to '${_aliased}', "
                "not the Vellum-backed Pulp::${_name}")
        endif()
        get_target_property(_links "Pulp::${_name}" INTERFACE_LINK_LIBRARIES)
        message(STATUS "Pulp UI from Vellum (in tree): pulp::${_name} -> ${_links}")
    endforeach()
    set(PULP_SDK_VELLUM_UI_COMPAT_TARGETS "${PULP_SDK_VELLUM_UI_COMPAT_TARGETS}" PARENT_SCOPE)
endfunction()

# Removes the UI targets the skipped directories would have defined from the
# SDK export list `list_var`. A listed name that is still a target is refused:
# Pulp's own UI and Vellum's must not both be exported.
function(pulp_vellum_in_tree_drop_ui_targets list_var)
    set(_targets ${${list_var}})
    foreach(_target IN LISTS PULP_VELLUM_IN_TREE_UI_TARGETS)
        if(TARGET "${_target}")
            message(FATAL_ERROR
                "PULP_UI_FROM_VELLUM_IN_TREE: ${_target} is defined in this tree, "
                "but this mode serves the UI from Vellum and must not export Pulp's own")
        endif()
        list(REMOVE_ITEM _targets "${_target}")
    endforeach()
    set(${list_var} "${_targets}" PARENT_SCOPE)
endfunction()

# Removes the header subsystems of the skipped directories from `list_var`.
function(pulp_vellum_in_tree_drop_ui_headers list_var)
    set(_subsystems ${${list_var}})
    list(REMOVE_ITEM _subsystems ${PULP_VELLUM_IN_TREE_UI_HEADER_SUBSYSTEMS})
    set(${list_var} "${_subsystems}" PARENT_SCOPE)
endfunction()
