# PulpVellumUiCompat.cmake: Vellum-backed compatibility targets for the Pulp
# UI names an installed SDK consumer links.
#
# Downstream plug-ins link Pulp::view, Pulp::canvas, Pulp::render,
# Pulp::view-native and Pulp::design-import. Vellum owns the implementations
# behind those names and installs them as its mirror targets. For every name
# this SDK does not export from Pulp's own code, pulp_define_vellum_ui_compat_targets()
# defines an INTERFACE IMPORTED target of the same name that links the
# matching Vellum targets, plus the lower-case pulp:: alias the in-tree build
# provides. A name Pulp still exports is left alone, so an SDK that ships its
# own UI libraries is unchanged.
#
# The caller loads the Vellum package first. A name whose Vellum backing target
# is missing stays undefined, so a consumer fails at generate time on that name
# instead of linking an empty interface.
#
# PULP_SDK_VELLUM_UI_COMPAT_TARGETS lists, in the caller's scope, the names this
# defined. It is empty when Pulp exports every name itself.

include_guard(GLOBAL)

function(_pulp_vellum_ui_compat_define name)
    cmake_parse_arguments(PARSE_ARGV 1 _compat "" "" "REQUIRES;OPTIONAL")
    set(_target "Pulp::${name}")
    if(TARGET "${_target}")
        return()
    endif()
    foreach(_dependency IN LISTS _compat_REQUIRES)
        if(NOT TARGET "${_dependency}")
            return()
        endif()
    endforeach()
    set(_links ${_compat_REQUIRES})
    foreach(_dependency IN LISTS _compat_OPTIONAL)
        if(TARGET "${_dependency}")
            list(APPEND _links "${_dependency}")
        endif()
    endforeach()
    add_library("${_target}" INTERFACE IMPORTED)
    set_target_properties("${_target}" PROPERTIES
        INTERFACE_LINK_LIBRARIES "${_links}"
        INTERFACE_COMPILE_FEATURES cxx_std_20
        PULP_UI_PROVIDER vellum)
    if(NOT TARGET "pulp::${name}")
        add_library("pulp::${name}" ALIAS "${_target}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY _PULP_VELLUM_UI_COMPAT_DEFINED "${name}")
endfunction()

# Pulp::vellum is the Pulp-owned editor bridge (format-core plus the Vellum
# parameter ports). It rides along with the view names when the SDK exports
# it, because those are the names a plug-in editor links.
function(pulp_define_vellum_ui_compat_targets)
    set_property(DIRECTORY PROPERTY _PULP_VELLUM_UI_COMPAT_DEFINED "")
    _pulp_vellum_ui_compat_define(canvas
        REQUIRES Vellum::MirrorCanvas)
    _pulp_vellum_ui_compat_define(render
        REQUIRES Vellum::MirrorRender_2d)
    _pulp_vellum_ui_compat_define(design-import
        REQUIRES Vellum::MirrorDesign_import)
    _pulp_vellum_ui_compat_define(view-native
        REQUIRES Vellum::MirrorView_core
        OPTIONAL Pulp::vellum)
    # The umbrella keeps the importer and the scripted view, as Pulp::view
    # does in an SDK built with them.
    _pulp_vellum_ui_compat_define(view
        REQUIRES Vellum::MirrorView_core Vellum::MirrorDesign_import
        OPTIONAL Vellum::MirrorView_script Pulp::vellum)
    get_property(_defined DIRECTORY PROPERTY _PULP_VELLUM_UI_COMPAT_DEFINED)
    set(PULP_SDK_VELLUM_UI_COMPAT_TARGETS "${_defined}" PARENT_SCOPE)
endfunction()
