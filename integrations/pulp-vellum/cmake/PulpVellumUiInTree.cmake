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
# A consumer that names a skipped target outright is reported by the scan
# below. A consumer behind a configure-time if(TARGET <UI name>) guard would
# instead switch itself off, so those guards outside the skipped directories
# are listed here with what each does in this mode:
#   tools/cli (pulp_cli_force_load_archives)  refuses: dropping the force-load
#       would link the CLI with undefined symbols.
#   CMakeLists.txt, the lite editor objects   off by design: they replace members
#       of Pulp's own canvas and view archives, which this mode does not build.
#   tools/cmake/PulpJsBytecode.cmake          takes Pulp::view first, which is the
#       Vellum-backed name here.
#   integrations/pulp-native/examples/metal-probe and
#   integrations/pulp-vellum-components/tests  test-only checks of Pulp's own
#       view archive; nothing to check when it is not built.
# Guards in test/ and examples/ belong to the plan rows that port those trees.
#
# The directories it leaves out are listed once, in
# PULP_VELLUM_IN_TREE_SKIPPED_UI_DIRS, each with the reason, so the list can
# only shrink or grow through a reviewed edit.

include_guard(GLOBAL)

include("${CMAKE_CURRENT_LIST_DIR}/PulpVellumUiCompat.cmake")

# <dir>|<reason>. Vellum provides what each directory provides.
set(PULP_VELLUM_IN_TREE_SKIPPED_UI_DIRS
    "core/canvas|Vellum::MirrorCanvas serves pulp::canvas"
    "core/view|Vellum::MirrorView_core and its script and import layers serve pulp::view, and the editor bridge lives in core/editor_bridge"
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
# top-level directory, after integrations/, so the Pulp integration packages
# exist. The names are IMPORTED GLOBAL, so directories configured earlier (all
# of core/) resolve them at generate time.
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
    # Every core/ directory is configured before this call, so the names must be
    # global for consumers there to resolve them at generate time.
    set(_PULP_VELLUM_UI_COMPAT_GLOBAL TRUE)
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
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}"
        CALL _pulp_vellum_in_tree_report_unported "${CMAKE_SOURCE_DIR}")
    set(PULP_SDK_VELLUM_UI_COMPAT_TARGETS "${PULP_SDK_VELLUM_UI_COMPAT_TARGETS}" PARENT_SCOPE)
endfunction()

# A name Pulp's own UI directories define: pulp-<x>, pulp::<x> or Pulp::<x> for
# a canvas, design-ir, design-import, view or timeline-view target.
set(_PULP_VELLUM_IN_TREE_UI_NAME
    "^(pulp-|pulp::|Pulp::)(canvas|design-ir|design-import|view|timeline-view)(-[a-z0-9-]+)?$")

# Reports, once the whole tree is configured, every target that still names a
# UI target this mode does not define, as
#   Pulp UI from Vellum (in tree): unported <target> -> <name>
# It reads link libraries, include directories and compile definitions, raw
# names and generator expressions alike, so it sees what CMake's own generate
# errors do not: a plain name such as pulp-canvas (which CMake would pass to
# the linker as -lpulp-canvas) and every missing name of a target, not only
# the first.
function(_pulp_vellum_in_tree_report_unported dir)
    _pulp_vellum_directories("${dir}" _dirs)
    set(_found "")
    foreach(_dir IN LISTS _dirs)
        get_property(_targets DIRECTORY "${_dir}" PROPERTY BUILDSYSTEM_TARGETS)
        foreach(_target IN LISTS _targets)
            foreach(_property IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES
                    INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES
                    COMPILE_DEFINITIONS INTERFACE_COMPILE_DEFINITIONS
                    LINK_OPTIONS INTERFACE_LINK_OPTIONS)
                get_target_property(_value "${_target}" ${_property})
                if(NOT _value)
                    continue()
                endif()
                foreach(_item IN LISTS _value)
                    _pulp_vellum_link_tokens("${_item}" _tokens)
                    foreach(_token IN LISTS _tokens)
                        if(_token MATCHES "${_PULP_VELLUM_IN_TREE_UI_NAME}"
                                AND NOT TARGET "${_token}")
                            list(APPEND _found "${_target} -> ${_token}")
                        endif()
                    endforeach()
                endforeach()
            endforeach()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _found)
    list(SORT _found)
    foreach(_entry IN LISTS _found)
        message(STATUS "Pulp UI from Vellum (in tree): unported ${_entry}")
    endforeach()
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
